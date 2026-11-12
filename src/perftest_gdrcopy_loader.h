/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.
 *
 * GDRCopy (libgdrapi) thin loader: dlopen at runtime, no build-time link
 * against libgdrapi. Used so peermem/GDR write_lat can CPU-poll GPU BAR
 * last-byte flags without cuMemcpyDtoH in the hot path.
 *
 * Runtime needs: gdrdrv.ko (or dmabuf mmap backend) + libgdrapi.so,
 * Tesla/Quadro GPU with GPUDirect RDMA. Disable with PERFTEST_DISABLE_GDRCOPY=1.
 */

#ifndef PERFTEST_GDRCOPY_LOADER_H
#define PERFTEST_GDRCOPY_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle fields matching gdrapi.h layout (gdr_mh_t.h). */
typedef struct perftest_gdr_mh {
    unsigned long h;
} perftest_gdr_mh_t;

int  perftest_gdrcopy_loader_init(void);
void perftest_gdrcopy_loader_fini(void);
int  perftest_gdrcopy_is_loaded(void);

/*
 * Pin + map a GPU allocation into CPU VA via BAR.
 *   gpu_va/size: device pointer range (prefer 64KB-aligned, as peermem does)
 *   out_map_raw: pointer returned by gdr_map (for unmap)
 *   out_cpu_base: CPU pointer corresponding to gpu_va (map + page offset)
 *   out_mh: pin handle for unpin
 * Returns 0 on success.
 */
int  perftest_gdrcopy_map(void *gpu_va, size_t size,
                          void **out_map_raw, void **out_cpu_base,
                          perftest_gdr_mh_t *out_mh);
int  perftest_gdrcopy_unmap(void *map_raw, size_t size, perftest_gdr_mh_t mh);

#ifdef __cplusplus
}
#endif

#endif /* PERFTEST_GDRCOPY_LOADER_H */
