#ifndef FUXI_ADRENO_H
#define FUXI_ADRENO_H
/* Private synchronous kernel copy API. Caller pins/retains source until return.
 * -1: failed bus drain, caller must quarantine the prepared descriptor forever.
 *  0: CPU fallback, 1: completed GPU fence. No userspace submit interface. */
int fuxi_gpu_ready(void);
void fuxi_gpu_start(uint64_t framebuffer, unsigned width, unsigned height, unsigned pitch);
int fuxi_gpu_copy(uintptr_t source, uint64_t length, unsigned pitch,
    unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned width, unsigned height);
#endif
