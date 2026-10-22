/* NativeAdrenoABI platform bridge. All operations serialize with presents. */
#ifndef FUXI_GPU_ABI_H
#define FUXI_GPU_ABI_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Managed 16K host page with Normal WB or NC cache policy; osfmk owns pmap APIs. */
bool fuxi_adreno_host_page_ok(uint64_t pa);
/* 0 success, 1 ring busy, -1 disabled/drained, -2 failed DMA drain.
 * poll also returns -3 on watchdog timeout/drained, -4 timeout/quarantined. */
int fuxi_gpu_abi_map(uint64_t iova, uintptr_t source, uint64_t bytes,
    unsigned permissions, unsigned writeback, unsigned shareability);
/* Persistent kernel mapping of a prepared task descriptor; lifetime through unmap. */
int fuxi_gpu_abi_map_import(uint64_t iova, uintptr_t source, uint64_t bytes, unsigned permissions);
int fuxi_gpu_abi_unmap(uint64_t iova, uint64_t bytes);
int fuxi_gpu_abi_submit(const uint64_t *iovas, const uint64_t *sizes,
    unsigned count, uint64_t *fence);
int fuxi_gpu_abi_poll(uint64_t fence);
/* CP always-on ticks: 0 success, -1 not ready, -2 MMIO error, -3 rollover timeout. */
int fuxi_gpu_abi_timestamp(uint64_t *ticks);
/* Monotonic boot-wide counts remain readable after permanent GPU failure. */
int fuxi_gpu_abi_status(uint64_t *faults, uint64_t *suspends);
int fuxi_gpu_abi_available(void);
void fuxi_gpu_abi_publish(void);
#ifdef __cplusplus
}
#endif
#endif
