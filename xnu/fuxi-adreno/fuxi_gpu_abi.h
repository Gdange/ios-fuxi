/* NativeAdrenoABI platform bridge. All operations serialize with presents. */
#ifndef FUXI_GPU_ABI_H
#define FUXI_GPU_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 0 success, 1 ring busy, -1 disabled/drained, -2 failed DMA drain.
 * poll also returns -3 on watchdog timeout/drained, -4 timeout/quarantined. */
int fuxi_gpu_abi_map(uint64_t iova, uintptr_t source, uint64_t bytes,
    unsigned permissions, unsigned writeback, unsigned shareability);
int fuxi_gpu_abi_unmap(uint64_t iova, uint64_t bytes);
int fuxi_gpu_abi_submit(const uint64_t *iovas, const uint64_t *sizes,
    unsigned count, uint64_t *fence);
int fuxi_gpu_abi_poll(uint64_t fence);
int fuxi_gpu_abi_available(void);
void fuxi_gpu_abi_publish(void);
#ifdef __cplusplus
}
#endif
#endif
