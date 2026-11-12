#include "metax_memory.h"
#include "perftest_gdrcopy_loader.c"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MC_SUCCESS 0
#define MC_MEMCPY_HOST_TO_DEVICE 1
#define MC_MEMCPY_DEVICE_TO_HOST 2

typedef int (*mc_set_device_fn)(int);
typedef int (*mc_malloc_fn)(void **, size_t);
typedef int (*mc_free_fn)(void *);
typedef int (*mc_memcpy_fn)(void *, const void *, size_t, int);
typedef int (*mxc_init_fn)(void);
typedef int (*mxkw_export_dmabuf_fn)(void *, uint64_t, int *, uint64_t *);

struct metax_api {
	void *runtime;
	void *mxkw;
	mc_set_device_fn set_device;
	mc_malloc_fn malloc_device;
	mc_free_fn free_device;
	mc_memcpy_fn memcpy_sync;
	mxkw_export_dmabuf_fn export_dmabuf;
	int loaded;
};

static struct metax_api api;
static char last_error[256];

/* Optional CPU mapping of GPU allocations. The flag path falls back to
 * mcMemcpy unless PERFTEST_ALLOW_MACA_GDRAPI=1 and mapping succeeds. */
struct metax_bar_map {
    void *gpu, *raw, *cpu;
    size_t bytes;
    perftest_gdr_mh_t handle;
    struct metax_bar_map *next;
};
static struct metax_bar_map *bar_maps;

static void metax_try_bar_map(void *gpu, size_t bytes)
{
    const char *e = getenv("PERFTEST_ALLOW_MACA_GDRAPI");
    struct metax_bar_map *m;
    if (!e || strcmp(e, "1") != 0) return;
    m = calloc(1, sizeof(*m));
    if (!m) { fprintf(stderr, "MetaX BAR: allocation failed; using mcMemcpy\n"); return; }
    if (!getenv("PERFTEST_GDRCOPY_LIB"))
        (void)setenv("PERFTEST_GDRCOPY_LIB", "/opt/maca-3.9.0/lib/libgdrapi.so.2", 0);
    if (perftest_gdrcopy_map(gpu, bytes, &m->raw, &m->cpu, &m->handle)) {
        fprintf(stderr, "MetaX BAR: mapping unavailable; using mcMemcpy\n");
        perftest_gdrcopy_loader_fini();
        free(m);
        return;
    }
    m->gpu = gpu; m->bytes = bytes; m->next = bar_maps; bar_maps = m;
    fprintf(stderr, "MetaX BAR: flag load/store enabled (gpu=%p cpu=%p size=%zu)\n",
            gpu, m->cpu, bytes);
}

static volatile unsigned char *metax_bar_address(const void *gpu)
{
    const uintptr_t p = (uintptr_t)gpu;
    struct metax_bar_map *m;
    for (m = bar_maps; m; m = m->next) {
        uintptr_t base = (uintptr_t)m->gpu;
        if (p >= base && p - base < m->bytes)
            return (volatile unsigned char *)m->cpu + (p - base);
    }
    return NULL;
}

static void metax_remove_bar_map(void *gpu)
{
    struct metax_bar_map **link = &bar_maps;
    while (*link) {
        struct metax_bar_map *m = *link;
        if (m->gpu == gpu) {
            *link = m->next;
            (void)perftest_gdrcopy_unmap(m->raw, m->bytes, m->handle);
            perftest_gdrcopy_loader_fini();
            free(m);
            return;
        }
        link = &m->next;
    }
}

static void set_error(const char *operation, int status)
{
	snprintf(last_error, sizeof(last_error), "%s failed (status=%d)",
		 operation, status);
}

static void *open_first(const char *const *candidates)
{
	int i;
	void *handle;

	for (i = 0; candidates[i] != NULL; ++i) {
		handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
		if (handle != NULL)
			return handle;
	}
	return NULL;
}

static int load_api(void)
{
	static const char *const runtime_candidates[] = {
		"/opt/maca-3.9.0/lib/libmcruntime.so",
		"/opt/maca-3.9.0/lib/libmcruntime.so.1",
		"/opt/maca-3.9.0/lib/libmacart.so",
		"libmcruntime.so", "libmcruntime.so.1", "libmacart.so", NULL
	};
	static const char *const mxkw_candidates[] = {
		"/opt/maca-3.9.0/lib/libmxkw.so.1.0.6",
		"/opt/maca-3.9.0/lib/libmxkw.so.1",
		"/opt/maca-3.9.0/lib/libmxkw.so",
		"libmxkw.so.1", "libmxkw.so", NULL
	};
	mxc_init_fn mxc_init;
	int status;

	if (api.loaded)
		return 0;

	api.runtime = open_first(runtime_candidates);
	if (api.runtime == NULL) {
		snprintf(last_error, sizeof(last_error),
			 "cannot load MetaX MACA runtime: %s", dlerror());
		return -1;
	}
	api.set_device = (mc_set_device_fn)dlsym(api.runtime, "mcSetDevice");
	api.malloc_device = (mc_malloc_fn)dlsym(api.runtime, "mcMalloc");
	api.free_device = (mc_free_fn)dlsym(api.runtime, "mcFree");
	api.memcpy_sync = (mc_memcpy_fn)dlsym(api.runtime, "mcMemcpy");
	if (!api.set_device || !api.malloc_device || !api.free_device ||
	    !api.memcpy_sync) {
		snprintf(last_error, sizeof(last_error),
			 "MetaX runtime is missing a required mc* symbol");
		return -1;
	}

	api.mxkw = open_first(mxkw_candidates);
	if (api.mxkw == NULL) {
		snprintf(last_error, sizeof(last_error),
			 "cannot load MetaX mxkw library: %s", dlerror());
		return -1;
	}
	mxc_init = (mxc_init_fn)dlsym(api.mxkw, "mxc_init");
	if (mxc_init != NULL) {
		status = mxc_init();
		if (status != 0) {
			set_error("mxc_init", status);
			return -1;
		}
	}
	api.export_dmabuf = (mxkw_export_dmabuf_fn)dlsym(
		api.mxkw, "mxkwExportDMABufHandle");
	if (api.export_dmabuf == NULL) {
		snprintf(last_error, sizeof(last_error),
			 "libmxkw is missing mxkwExportDMABufHandle");
		return -1;
	}
	api.loaded = 1;
	return 0;
}

static int select_device(int device_id)
{
	int status;
	if (load_api())
		return -1;
	status = api.set_device(device_id);
	if (status != MC_SUCCESS) {
		set_error("mcSetDevice", status);
		return -1;
	}
	return 0;
}

int metax_memory_alloc(void **buffer, size_t bytes, int device_id)
{
	int status;
	if (!buffer || bytes == 0 || select_device(device_id))
		return -1;
	*buffer = NULL;
	status = api.malloc_device(buffer, bytes);
	if (status != MC_SUCCESS || *buffer == NULL) {
		set_error("mcMalloc", status);
		return -1;
	}
	metax_try_bar_map(*buffer, bytes);
	return 0;
}

int metax_memory_free(void *buffer, int device_id)
{
	int status;
	if (!buffer)
		return 0;
	if (select_device(device_id))
		return -1;
	metax_remove_bar_map(buffer);
	status = api.free_device(buffer);
	if (status != MC_SUCCESS) {
		set_error("mcFree", status);
		return -1;
	}
	return 0;
}

int metax_memory_init_buffer(void *buffer, size_t bytes, int device_id, int zero)
{
	unsigned char *host;
	uint32_t state = 0x6d657461U;
	size_t i;
	int status;

	if (!buffer || bytes == 0 || select_device(device_id))
		return -1;
	host = malloc(bytes);
	if (!host) {
		snprintf(last_error, sizeof(last_error),
			 "host setup allocation failed: %s", strerror(errno));
		return -1;
	}
	if (zero) {
		memset(host, 0, bytes);
	} else {
		for (i = 0; i < bytes; ++i) {
			state = state * 1664525U + 1013904223U;
			host[i] = (unsigned char)(state >> 24);
		}
	}
	status = api.memcpy_sync(buffer, host, bytes, MC_MEMCPY_HOST_TO_DEVICE);
	free(host);
	if (status != MC_SUCCESS) {
		set_error("mcMemcpy(H2D init)", status);
		return -1;
	}
	return 0;
}

int metax_memory_read_byte(const void *device_address, unsigned char *value,
			   int device_id)
{
	int status;
	volatile unsigned char *mapped;
	if (!device_address || !value) return -1;
	mapped = metax_bar_address(device_address);
	if (mapped) { *value = *mapped; return 0; }
	if (select_device(device_id)) return -1;
	status = api.memcpy_sync(value, device_address, 1,
				 MC_MEMCPY_DEVICE_TO_HOST);
	if (status != MC_SUCCESS) {
		set_error("mcMemcpy(D2H byte)", status);
		return -1;
	}
	return 0;
}

int metax_memory_write_byte(void *device_address, unsigned char value,
			    int device_id)
{
	int status;
	volatile unsigned char *mapped;
	if (!device_address) return -1;
	mapped = metax_bar_address(device_address);
	if (mapped) { *mapped = value; __sync_synchronize(); return 0; }
	if (select_device(device_id)) return -1;
	status = api.memcpy_sync(device_address, &value, 1,
				 MC_MEMCPY_HOST_TO_DEVICE);
	if (status != MC_SUCCESS) {
		set_error("mcMemcpy(H2D byte)", status);
		return -1;
	}
	return 0;
}

struct ibv_mr *metax_memory_register(struct ibv_pd *pd, void *buffer,
				     size_t bytes, int access)
{
	struct ibv_mr *mr;
	uint64_t offset = 0;
	int fd = -1;
	int status;
	int saved_errno;

	if (!pd || !buffer || bytes == 0 || load_api())
		return NULL;
	status = api.export_dmabuf(buffer, bytes, &fd, &offset);
	if (status != 0 || fd < 0) {
		set_error("mxkwExportDMABufHandle", status);
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	mr = ibv_reg_dmabuf_mr(pd, offset, bytes,
				(uint64_t)(uintptr_t)buffer, fd, access);
	saved_errno = errno;
	close(fd);
	if (!mr) {
		snprintf(last_error, sizeof(last_error),
			 "ibv_reg_dmabuf_mr failed: %s", strerror(saved_errno));
		errno = saved_errno;
		return NULL;
	}
	mr->addr = buffer;
	fprintf(stderr,
		"MetaX DMA-BUF MR registered: addr=%p length=%zu offset=%llu rkey=0x%x\n",
		buffer, bytes, (unsigned long long)offset, mr->rkey);
	return mr;
}

const char *metax_memory_error(void)
{
	return last_error[0] ? last_error : "unknown MetaX error";
}
