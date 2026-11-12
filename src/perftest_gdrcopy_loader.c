/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "perftest_gdrcopy_loader.h"

#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GDR_LOG(fmt, ...) \
    (void)fprintf(stderr, "[gdrcopy] %s: " fmt "\n", __func__, ##__VA_ARGS__)

/* Minimal ABI mirror of gdrapi.h (v2.x). */
typedef struct gdr *gdr_t;

typedef struct gdr_mh_s {
    unsigned long h;
} gdr_mh_t;

typedef struct gdr_info_v2 {
    uint64_t va;
    uint64_t mapped_size;
    uint32_t page_size;
    uint64_t tm_cycles;
    uint32_t cycles_per_ms;
    unsigned mapped : 1;
    unsigned wc_mapping : 1;
    int mapping_type; /* gdr_mapping_type_t */
} gdr_info_t;

typedef gdr_t (*pfn_gdr_open)(void);
typedef int (*pfn_gdr_close)(gdr_t g);
typedef int (*pfn_gdr_pin_buffer)(gdr_t g, unsigned long addr, size_t size,
                                  uint64_t p2p_token, uint32_t va_space,
                                  gdr_mh_t *handle);
typedef int (*pfn_gdr_unpin_buffer)(gdr_t g, gdr_mh_t handle);
typedef int (*pfn_gdr_get_info)(gdr_t g, gdr_mh_t handle, gdr_info_t *info);
typedef int (*pfn_gdr_map)(gdr_t g, gdr_mh_t handle, void **va, size_t size);
typedef int (*pfn_gdr_unmap)(gdr_t g, gdr_mh_t handle, void *va, size_t size);

static void *g_lib;
static gdr_t g_gdr;
static int g_loaded;
static int g_refcnt;

static pfn_gdr_open g_open;
static pfn_gdr_close g_close;
static pfn_gdr_pin_buffer g_pin_buffer;
static pfn_gdr_unpin_buffer g_unpin_buffer;
static pfn_gdr_get_info g_get_info;
static pfn_gdr_map g_map;
static pfn_gdr_unmap g_unmap;

static int env_disabled(void)
{
    const char *e = getenv("PERFTEST_DISABLE_GDRCOPY");

    return (e != NULL && e[0] == '1' && e[1] == '\0');
}

static int env_allow_maca_gdrapi(void)
{
    const char *e = getenv("PERFTEST_ALLOW_MACA_GDRAPI");

    return (e != NULL && e[0] == '1' && e[1] == '\0');
}

static int gdrapi_is_maca(void *lib, const char *hint)
{
    struct link_map *map = NULL;
    const char *path = hint;

    if (lib != NULL && dlinfo(lib, RTLD_DI_LINKMAP, &map) == 0 &&
        map != NULL && map->l_name != NULL && map->l_name[0] != '\0') {
        path = map->l_name;
    }
    /*
     * MXGDA: MACA ships a libgdrapi that exports the same gdr_* ABI
     * (gdr_open/gdr_pin_buffer/gdr_map/gdr_get_info_v2) plus an extra
     * mcMemRemap symbol. The heuristic below rejects it, which forces
     * perftest onto the mcMemcpy D2H fallback for every flag poll and
     * inflates write-latency by ~13us. Set PERFTEST_ALLOW_MACA_GDRAPI=1
     * to accept it instead and let the CPU map GPU memory directly.
     * Default (unset) keeps the original behaviour.
     */
    if (env_allow_maca_gdrapi()) {
        return 0;
    }
    if (path != NULL &&
        (strstr(path, "metax_lib") != NULL ||
         strstr(path, "/maca/") != NULL ||
         strstr(path, "/maca-") != NULL)) {
        return 1;
    }
    /* MACA gdrapi pulls mcMemRemap from libmcruntime; NVIDIA gdrcopy does not. */
    return (lib != NULL && dlsym(lib, "mcMemRemap") != NULL);
}

/*
 * MXGDA: MACA libgdrapi.so.2 calls mcMemRemap but does NOT declare
 * libmcruntime in its DT_NEEDED, so the symbol is unresolved at first use and
 * the process dies with "symbol lookup error: undefined symbol: mcMemRemap".
 * Pre-loading libmcruntime with RTLD_GLOBAL makes the symbol available.
 */
static void preload_mc_runtime(void)
{
    static const char *const candidates[] = {
        "/opt/maca/lib64/libmcruntime.so",
        "/opt/maca/lib/libmcruntime.so",
        "libmcruntime.so",
        NULL,
    };
    int i;

    if (!env_allow_maca_gdrapi()) {
        return;
    }
    for (i = 0; candidates[i] != NULL; i++) {
        void *h = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (h != NULL) {
            GDR_LOG("preloaded %s (mcMemRemap for MACA gdrapi)", candidates[i]);
            return;
        }
    }
    GDR_LOG("WARN: could not preload libmcruntime; MACA gdrapi will fail");
}

static void *dlopen_nvidia_gdrapi(const char *path, const char **opened_path)
{
    void *lib;

    preload_mc_runtime();
    (void)dlerror();
    lib = dlopen(path, RTLD_LAZY);
    if (lib == NULL) {
        return NULL;
    }
    if (gdrapi_is_maca(lib, path)) {
        GDR_LOG("skip MACA libgdrapi %s (mcMemRemap ABI, not NVIDIA gdrcopy)",
                path);
        dlclose(lib);
        return NULL;
    }
    if (opened_path != NULL) {
        *opened_path = path;
    }
    return lib;
}

static void *try_dlopen_gdrapi(const char **opened_path)
{
    /*
     * Default linker path often omits /usr/local/lib (common gdrcopy
     * prefix install). Try sonames first, then absolute fallbacks.
     */
    static const char *const candidates[] = {
        "libgdrapi.so.2",
        "libgdrapi.so",
        "/usr/local/lib/libgdrapi.so.2",
        "/usr/local/lib/libgdrapi.so",
        "/usr/local/lib64/libgdrapi.so.2",
        "/usr/local/lib64/libgdrapi.so",
        "/usr/lib64/libgdrapi.so.2",
        "/usr/lib64/libgdrapi.so",
        "/usr/lib/libgdrapi.so.2",
        "/usr/lib/libgdrapi.so",
        "/usr/lib/x86_64-linux-gnu/libgdrapi.so.2",
        "/usr/lib/x86_64-linux-gnu/libgdrapi.so",
        "/usr/lib/aarch64-linux-gnu/libgdrapi.so.2",
        "/usr/lib/aarch64-linux-gnu/libgdrapi.so",
        NULL,
    };
    const char *env = getenv("PERFTEST_GDRCOPY_LIB");
    void *lib;
    int i;

    if (env != NULL && env[0] != '\0') {
        lib = dlopen_nvidia_gdrapi(env, opened_path);
        if (lib != NULL) {
            return lib;
        }
        GDR_LOG("PERFTEST_GDRCOPY_LIB=%s not usable", env);
    }

    for (i = 0; candidates[i] != NULL; i++) {
        lib = dlopen_nvidia_gdrapi(candidates[i], opened_path);
        if (lib != NULL) {
            return lib;
        }
    }
    return NULL;
}

int perftest_gdrcopy_loader_init(void)
{
    const char *opened = NULL;

    if (env_disabled()) {
        GDR_LOG("disabled by PERFTEST_DISABLE_GDRCOPY=1");
        return -1;
    }
    if (g_loaded) {
        g_refcnt++;
        return 0;
    }

    g_lib = try_dlopen_gdrapi(&opened);
    if (g_lib == NULL) {
        GDR_LOG("FAIL: dlopen libgdrapi — install gdrcopy, or "
                "export PERFTEST_GDRCOPY_LIB=/usr/local/lib/libgdrapi.so.2 "
                "(and/or: echo /usr/local/lib > /etc/ld.so.conf.d/gdrcopy.conf "
                "&& ldconfig)");
        return -1;
    }

#define LOAD(sym, name)                                                        \
    do {                                                                       \
        g_##sym = (pfn_gdr_##sym)dlsym(g_lib, name);                           \
        if (g_##sym == NULL) {                                                 \
            GDR_LOG("FAIL: dlsym %s: %s", name, dlerror());                    \
            dlclose(g_lib);                                                    \
            g_lib = NULL;                                                      \
            return -1;                                                         \
        }                                                                      \
    } while (0)

    LOAD(open, "gdr_open");
    LOAD(close, "gdr_close");
    LOAD(pin_buffer, "gdr_pin_buffer");
    LOAD(unpin_buffer, "gdr_unpin_buffer");
    /* Prefer v2 info; fall back to historical gdr_get_info symbol. */
    g_get_info = (pfn_gdr_get_info)dlsym(g_lib, "gdr_get_info_v2");
    if (g_get_info == NULL) {
        g_get_info = (pfn_gdr_get_info)dlsym(g_lib, "gdr_get_info");
    }
    if (g_get_info == NULL) {
        GDR_LOG("FAIL: dlsym gdr_get_info(_v2): %s", dlerror());
        dlclose(g_lib);
        g_lib = NULL;
        return -1;
    }
    LOAD(map, "gdr_map");
    LOAD(unmap, "gdr_unmap");
#undef LOAD

    g_gdr = g_open();
    if (g_gdr == NULL) {
        GDR_LOG("FAIL: gdr_open (is gdrdrv loaded? try: lsmod | grep gdrdrv)");
        dlclose(g_lib);
        g_lib = NULL;
        return -1;
    }

    g_loaded = 1;
    g_refcnt = 1;
    GDR_LOG("OK: %s opened, gdr handle ready (BAR map for flag poll)",
            opened != NULL ? opened : "libgdrapi");
    return 0;
}

void perftest_gdrcopy_loader_fini(void)
{
    if (!g_loaded) {
        return;
    }
    if (g_refcnt > 1) {
        g_refcnt--;
        return;
    }
    if (g_gdr != NULL && g_close != NULL) {
        (void)g_close(g_gdr);
        g_gdr = NULL;
    }
    if (g_lib != NULL) {
        dlclose(g_lib);
        g_lib = NULL;
    }
    g_loaded = 0;
    g_refcnt = 0;
    g_open = NULL;
    g_close = NULL;
    g_pin_buffer = NULL;
    g_unpin_buffer = NULL;
    g_get_info = NULL;
    g_map = NULL;
    g_unmap = NULL;
}

int perftest_gdrcopy_is_loaded(void)
{
    return g_loaded;
}

int perftest_gdrcopy_map(void *gpu_va, size_t size, void **out_map_raw,
                         void **out_cpu_base, perftest_gdr_mh_t *out_mh)
{
    gdr_mh_t mh = { 0 };
    gdr_info_t info;
    void *map_va = NULL;
    uint64_t dptr;
    int rc;

    if (out_map_raw == NULL || out_cpu_base == NULL || out_mh == NULL ||
        gpu_va == NULL || size == 0) {
        return -1;
    }
    if (perftest_gdrcopy_loader_init() != 0) {
        return -1;
    }

    dptr = (uint64_t)(uintptr_t)gpu_va;
    rc = g_pin_buffer(g_gdr, (unsigned long)dptr, size, 0, 0, &mh);
    if (rc != 0) {
        GDR_LOG("FAIL: gdr_pin_buffer dptr=%p size=%zu rc=%d", gpu_va, size, rc);
        return -1;
    }

    rc = g_map(g_gdr, mh, &map_va, size);
    if (rc != 0 || map_va == NULL) {
        GDR_LOG("FAIL: gdr_map dptr=%p size=%zu rc=%d (need GPU-page align)",
                gpu_va, size, rc);
        (void)g_unpin_buffer(g_gdr, mh);
        return -1;
    }

    memset(&info, 0, sizeof(info));
    rc = g_get_info(g_gdr, mh, &info);
    if (rc != 0) {
        GDR_LOG("FAIL: gdr_get_info rc=%d", rc);
        (void)g_unmap(g_gdr, mh, map_va, size);
        (void)g_unpin_buffer(g_gdr, mh);
        return -1;
    }

    /*
     * MXGDA: MACA's gdr_get_info_v2 does not fill the NVIDIA-style
     * gdr_info_v2 layout. Raw dump of a real call shows only 16 bytes
     * written: va at offset 0, page_size at offset 12, and no mapped_size
     * field at all. The NVIDIA struct reads mapped_size from offset 8,
     * which is always 0, so the bounds check below can never pass.
     * Since gdr_map() already succeeded for exactly [dptr, dptr+size),
     * skip that check when the MACA experiment is enabled.
     */
    if (!env_allow_maca_gdrapi() &&
        (dptr < info.va || dptr + size > info.va + info.mapped_size)) {
        GDR_LOG("FAIL: dptr=%#llx size=%zu outside mapped [%#llx,+%#llx)",
                (unsigned long long)dptr, size,
                (unsigned long long)info.va,
                (unsigned long long)info.mapped_size);
        (void)g_unmap(g_gdr, mh, map_va, size);
        (void)g_unpin_buffer(g_gdr, mh);
        return -1;
    }

    /* Fail closed unless the returned mapping starts at this allocation.
     * Do not trust a different GPU ABI's mapped_size layout. */
    if (env_allow_maca_gdrapi() && info.va != dptr) {
        (void)g_unmap(g_gdr, mh, map_va, size);
        (void)g_unpin_buffer(g_gdr, mh);
        GDR_LOG("MACA mapping base mismatch; refusing CPU mapping");
        return -1;
    }
    *out_map_raw = map_va;
    *out_cpu_base = (char *)map_va + (size_t)(dptr - info.va);
    out_mh->h = mh.h;

    GDR_LOG("OK: dptr=%p -> cpu=%p map_raw=%p size=%zu info.va=%#llx "
            "mapped_size=%#llx page=%u wc=%u",
            gpu_va, *out_cpu_base, map_va, size,
            (unsigned long long)info.va,
            (unsigned long long)info.mapped_size, info.page_size,
            (unsigned)info.wc_mapping);
    return 0;
}

int perftest_gdrcopy_unmap(void *map_raw, size_t size, perftest_gdr_mh_t mh_in)
{
    gdr_mh_t mh;

    if (!g_loaded || g_gdr == NULL) {
        return -1;
    }
    mh.h = mh_in.h;
    if (map_raw != NULL && g_unmap != NULL) {
        (void)g_unmap(g_gdr, mh, map_raw, size);
    }
    if (g_unpin_buffer != NULL) {
        (void)g_unpin_buffer(g_gdr, mh);
    }
    return 0;
}
