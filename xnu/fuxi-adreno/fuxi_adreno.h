#ifndef FUXI_ADRENO_H
#define FUXI_ADRENO_H
#define FG_CACHE_SLOTS 4u
/* Timings in mach absolute ticks, CPU uses scheduler runtime (excludes sleep).
 * Surface ownership uses SweetSurface's cache lock. When the ABI is
 * enabled, a second shared mutex serializes ring and SMMU control. */
struct fg_times {
 uint64_t pin, lookup, map, clean, submit, wait, unmap, cpu;
 unsigned attempts, hits, misses, sleeps, blocks, groups;
};
int fuxi_gpu_ready(void);
void fuxi_gpu_start(uint64_t framebuffer, unsigned width, unsigned height, unsigned pitch);
int fuxi_gpu_map(unsigned slot, uint64_t generation, uintptr_t source,
    uint64_t length, struct fg_times *t);
/* False means failed DMA drain: keep backing prepared/retained until reset. */
int fuxi_gpu_unmap(unsigned slot, struct fg_times *t);
/* copy: 2 submitted, 0 fallback, -1 quarantine. poll: 0 busy, 1 done,
 * -1 failed/drained, -2 failed/quarantined. Serialized by the surface lock. */
int fuxi_gpu_poll(struct fg_times *t);
int fuxi_gpu_copy(unsigned slot, uint64_t generation, uintptr_t source, uint64_t length,
    unsigned pitch, unsigned sx, unsigned sy, unsigned dx, unsigned dy,
    unsigned width, unsigned height, struct fg_times *t);
#endif
