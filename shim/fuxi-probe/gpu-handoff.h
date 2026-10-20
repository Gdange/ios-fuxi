/* M6 cold handoff, shared by the loader and XNU. No firmware bytes here. */
#ifndef FUXI_GPU_HANDOFF_H
#define FUXI_GPU_HANDOFF_H
#define FG_RAM_BASE 0x8c0000000ULL
#define FG_RAM_SIZE 0x30000000ULL
#define FG_SCRATCH  0x8f0000000ULL
#define FG_SCRATCH_SIZE 0x02000000u
#define FG_HANDOFF  (FG_SCRATCH + 0x1800000)
#define FG_FW_BASE  (FG_HANDOFF + 0x4000)
#define FG_FW_SLOT  0x100000u
#define FG_MAGIC    0x36555047u /* GPU6 */
struct fg_handoff {
 uint32_t magic, version, bytes, reserved;
 uint64_t base, size;
 uint32_t fw_size[3], fw_hash[3]; /* GMU, SQE, zap; FNV1a detects corruption */
};
static inline uint32_t fg_hash(const uint8_t *p, unsigned n)
{
 uint32_t h = 2166136261u;
 for (unsigned i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
 return h;
}
#endif
