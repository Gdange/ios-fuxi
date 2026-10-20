/* Fuxi A740 staged GPU/GMU probe (lane-gpu, milestones 1 and 2).
 *
 * Single translation unit #included by probe.c before ud_setup(), so these
 * statics share probe.c's helpers (delay(), the volatile memcpy) and the USB
 * report path. Request 0x56 runs a gated, bisectable sequence and returns a
 * fixed 128-word report. Nothing here is reached unless the host asks for it,
 * and every stage runs only if the previous one proved its prerequisite, so a
 * single boot photographs the whole chain and a bad MMIO (which resets the
 * SoC) pins the blame on one stage.
 *
 * The loader runs with the MMU and caches OFF and VA==PA, so every "window"
 * below is just a physical base; accesses are naturally uncached Device-nGnRE
 * which is what the GPU/GMU bring-up wants. All register maths is ported from
 * the pinned Linux msm/adreno + qcom clk drivers (ianchb/sm8550-mainline
 * 42f3b40c) and cross-checked against UniXNU-sheng's GpuPowerBoard.h; every
 * address carries its Fuxi-verified source. Facts vs. guesses: see
 * docs-lanes/gpu.md.
 */

/* ---- Fuxi-verified physical bases (research/device/probe-09/live.dtb) ---- */
#define GPU_GCC_BASE      0x00100000ULL   /* gcc kalama-v2 @100000 (clock-controller) */
#define GPU_GCC_GPU_PAGE  (GPU_GCC_BASE + 0x71000) /* gcc-kalama: gpu branch page */
#define GPU_GCC_VOTE_PAGE (GPU_GCC_BASE + 0x52000) /* gcc gpll0 GPU vote register */
#define GPU_GPUCC_BASE    0x03d90000ULL   /* gpucc @3d90000 size 0xa000 (qcom,kalama-gpucc) */
#define GPU_SMMU_BASE     0x03da0000ULL   /* kgsl-smmu@3da0000 size 0x40000 (adreno-smmu) */
#define GPU_REG_BASE      0x03d00000ULL   /* kgsl-3d0@3d00000 size 0x40000 */
#define GPU_GMU_CORE      0x03d6a000ULL   /* GMU core (upstream/sheng base; Fuxi node gmu@3d68000/0x37000 contains it) */
#define GPU_RSCC_BASE     0x03d50000ULL   /* gmu rscc */
#define GPU_CXMISC_BASE   0x03d9e000ULL   /* gpu cx_misc / cx_mem */
#define GPU_CMDDB_BASE    0x81c60000ULL   /* reserved-memory aop_cmd_db_region@81c60000 reg .. 0x20000 (no-map) */
#define GPU_CMDDB_BYTES   0x20000ULL

/* GCC GPU branch offsets within the 0x71000 page (gcc-sm8550.c / gcc-kalama.c). */
#define GCC_GPU_CFG_AHB      0x004   /* gcc_gpu_cfg_ahb_clk (critical; hwcg bit1 -> CLK_OFF unreliable, use enable bit0) */
#define GCC_GPU_MEMNOC_GFX   0x010   /* gcc_gpu_memnoc_gfx_clk (voted) */
#define GCC_GPU_SNOC_DVM_GFX 0x018
#define GCC_DDRSS_GPU_AXI    0x154   /* gcc_ddrss_gpu_axi_clk 0x71154 */
#define CBCR_CLK_OFF (1u << 31)
#define CBCR_ENABLE  (1u << 0)
#define GPLL0_VOTES  ((1u << 15) | (1u << 16))   /* gcc_gpu_gpll0_clk_src / _div_clk_src in the 0x52000 page */

/* GPUCC offsets (gpucc-sm8550.c, confirmed against the Fuxi DTB node layout). */
#define CC_PLL0        0x0000
#define CC_PLL1        0x1000
#define PLL_MODE       0x0
#define PLL_OPMODE     0x4
#define PLL_L_VAL      0x10
#define PLL_USER_CTL   0x18
#define PLL_LOCK_DET   (1u << 31)
#define PLL_OUTCTRL    (1u << 0)
#define PLL_RESET_N    (1u << 2)
#define PLL_OUT_MASK   0x7u
#define PLL_RUN        1u
#define CC_CX_GDSCR    0x9108   /* gpu_cc_cx_gdsc.gdscr */
#define CC_GX_GDSCR    0x905c   /* gpu_cc_gx_gdsc.gdscr */
#define CC_CX_GDS_HW   0x953c   /* gpu_cc_cx_gdsc.gds_hw_ctrl (VOTABLE: poll PWR_ON here) */
#define CC_AHB_CLK     0x911c   /* gpu_cc_ahb_clk */
#define CC_CX_GMU_CLK  0x913c   /* gpu_cc_cx_gmu_clk */
#define CC_SMMU_VOTE   0x7000   /* gpu_cc_hlos1_vote_gpu_smmu_clk */
#define CC_CXO_AON     0x9004   /* gpu_cc_cxo_aon_clk */
#define CC_DEMET       0x900c   /* gpu_cc_demet_clk */
#define CC_CXO_CLK     0x9144
#define CC_HUB_CX_INT  0x9148
#define CC_GMU_RCGR    0x9318   /* gpu_cc_gmu_clk_src cmd_rcgr; CFG at +4 */
#define CC_HUB_RCGR    0x93ec   /* gpu_cc_hub_clk_src */
#define GDSC_PWR_ON       (1u << 31)
#define GDSC_SW_COLLAPSE  (1u << 0)
#define GDSC_RETAIN_MEM   (1u << 14)
#define GDSC_RETAIN_PERIPH (1u << 13)
#define GDSC_RETAIN_FF    (1u << 11)
#define RCG_UPDATE   (1u << 0)
#define RCG_ROOT_EN  (1u << 1)
#define RCG_ROOT_OFF (1u << 31)
#define RCG_CFG_CLEAR (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))

/* ARM SMMUv2 global space 0 (IHI 0062D). adreno_smmu: qsmmu-v500 + adreno-smmu. */
#define SMMU_SCR0  0x0
#define SMMU_ID0   0x20
#define SMMU_ID1   0x24
#define SMMU_ID2   0x28
#define SMMU_GFSR  0x48
#define SMMU_SMR(n)  (0x800 + 4 * (n))
#define SMMU_S2CR(n) (0xc00 + 4 * (n))

/* GPU + GMU register dword offsets (a6xx.xml / a6xx_gmu.xml, A7XX variants).
 * The GMU core block sits SHENG_GMU_CORE_GPU_DELTA=0x6a000 bytes above GPU reg
 * base, so GMU dword D is absolute GPU_GMU_CORE + (D*4 - 0x6a000). */
#define GMU_DWORD(d) (GPU_GMU_CORE + ((uint64_t)(d) * 4 - 0x6a000))
#define GMUD_CM3_SYSRESET   0x1f800
#define GMUD_HFI_CTRL_STATUS 0x1f980
#define GMUD_SPTPRAC_STATUS 0x1f8d0   /* GMU_SPTPRAC_PWR_CLK_STATUS; see stage10 mask below */
#define RSCC_SEQ_BUSY_DRV0  0x101     /* rscc dword */
#define CXMISC_TCM_RET_CNTL 0x39      /* cx_misc dword */
#define GPU_RBBM_STATUS     0x210     /* gpu reg dword */

/* ---- report pages (each 128 u32 = the 512-byte control-IN cap) ---- */
#define GPU_REPORT_WORDS 128
static uint32_t gpu_report[GPU_REPORT_WORDS];
static unsigned gpu_stage;   /* furthest stage entered, for a post-mortem photo */
static void g_enter(unsigned stage) { gpu_stage = stage; gpu_report[2] = stage; }

static uint32_t g_rd(uint64_t base, uint32_t off)
{
 uint32_t v = *(volatile uint32_t *)(uintptr_t)(base + off);
 __asm__ volatile("dmb oshld" ::: "memory");
 return v;
}
static void g_wr(uint64_t base, uint32_t off, uint32_t v)
{
 __asm__ volatile("dmb oshst" ::: "memory");
 *(volatile uint32_t *)(uintptr_t)(base + off) = v;
 __asm__ volatile("dsb osh" ::: "memory");
}
static int g_clk_on(uint32_t cbcr) { return (cbcr & CBCR_CLK_OFF) == 0; }
static int g_gdsc_on(uint32_t gdscr) { return (gdscr & GDSC_PWR_ON) != 0; }

/* lucid_ole config (gpucc-sm8550.c): write L/alpha and the fixed config words,
 * leave OUTCTRL clear, OPMODE STANDBY, assert RESET_N. Zero values are skipped.
 * `pll` is the PLL's offset within the GPUCC window (CC_PLL0 / CC_PLL1). */
static void g_pll_configure(uint32_t pll, uint32_t l, uint32_t alpha)
{
 static const uint32_t off[] = {0x10,0x14,0x20,0x24,0x28,0x18,0x1c,0x2c,0x30,0x34,0x38};
 const uint32_t val[] = {l | 0x44u<<16 | 0x44u<<24, alpha, 0x20485699u, 0x00182261u,
                         0x82aa299cu, 0, 5, 0, 3, 0x9000, 0x34};
 for (unsigned i = 0; i < sizeof off / sizeof off[0]; i++)
  if (val[i]) g_wr(GPU_GPUCC_BASE, pll + off[i], val[i]);
 uint32_t mode = g_rd(GPU_GPUCC_BASE, pll + PLL_MODE);
 g_wr(GPU_GPUCC_BASE, pll + PLL_MODE, mode & ~PLL_OUTCTRL);
 g_wr(GPU_GPUCC_BASE, pll + PLL_OPMODE, 0);
 mode = g_rd(GPU_GPUCC_BASE, pll + PLL_MODE);
 g_wr(GPU_GPUCC_BASE, pll + PLL_MODE, mode | PLL_RESET_N);
}
/* clk_rcg2_shared_force_enable_clear: ROOT_EN, wait ROOT_OFF clear, CFG, UPDATE. 0 ok / 1 timeout. */
static int g_rcg_set(uint32_t rcgr, uint32_t sel, uint32_t div)
{
 uint32_t cmd = g_rd(GPU_GPUCC_BASE, rcgr);
 g_wr(GPU_GPUCC_BASE, rcgr, cmd | RCG_ROOT_EN);
 unsigned i;
 for (i = 0; i < 500; i++) { if (!(g_rd(GPU_GPUCC_BASE, rcgr) & RCG_ROOT_OFF)) break; delay(1); }
 if (i == 500) return 1;
 uint32_t cfg = g_rd(GPU_GPUCC_BASE, rcgr + 4);
 g_wr(GPU_GPUCC_BASE, rcgr + 4, (cfg & ~RCG_CFG_CLEAR) | sel << 8 | div);
 g_wr(GPU_GPUCC_BASE, rcgr, g_rd(GPU_GPUCC_BASE, rcgr) | RCG_UPDATE);
 for (i = 0; i < 500; i++) { if (!(g_rd(GPU_GPUCC_BASE, rcgr) & RCG_UPDATE)) break; delay(1); }
 if (i == 500) return 1;
 g_wr(GPU_GPUCC_BASE, rcgr, g_rd(GPU_GPUCC_BASE, rcgr) & ~RCG_ROOT_EN);
 return 0;
}
static void g_set_bit0(uint64_t base, uint32_t off)
{
 uint32_t v = g_rd(base, off);
 if (!(v & CBCR_ENABLE)) g_wr(base, off, v | CBCR_ENABLE);
}

/* little-endian cmd-db accessors over the already-copied snapshot */
static uint16_t cdb16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t cdb32(const uint8_t *p) { return cdb16(p) | (uint32_t)cdb16(p + 2) << 16; }

/* The staged sequence. `limit` (USB wIndex) is the last stage allowed;
 * the report records the actual outcome of each stage it reached. */
/* Milestone 2: Linux 6.12 a6xx_gmu.c/a6xx_hfi.c, ARM SMMUv2.
 * Fixed scratch in the loader-only XNU RAM window. Never part of .bss.
 * Firmware and tables must survive until reset once CM3 has been released.
 */
#define G_MEM       0x8f0000000ULL
#define G_MEM_SIZE  0x02000000u
#define G_IC_PA     (G_MEM + 0x100000)
#define G_DATA_PA   (G_MEM + 0x1100000)
#define G_DUMMY_VA  0x60000000u
#define G_LOG_VA    0x60002000u
#define G_HFI_VA    0x60006000u
#define G_DEBUG_VA  0x60400000u
#define G_LOG_PA    (G_DATA_PA + 0x2000)
#define G_HFI_PA    (G_DATA_PA + 0x6000)
#define G_DEBUG_PA  (G_DATA_PA + 0xa000)
#define G_IC_SIZE   0xffc000u
#define G_PAGES     6
#define G_STAGE_8A  0x8au
static uint32_t gpu_extra[G_PAGES - 1][GPU_REPORT_WORDS];
static unsigned gpu_gmu_started; /* monotonic: also covers failed init/ACK */
static unsigned gpu_m2_attempted, gpu_s8_ok, gpu_s9_attempted, gpu_s9_ok, gpu_s10_attempted;
static unsigned g_bank, g_slot, g_asid, g_stride, g_cbbase, g_ncb, g_nsmr, g_pt_next;
static unsigned gpu_snapshot_valid;
static uint8_t gpu_cmddb[GPU_CMDDB_BYTES] __attribute__((aligned(8)));

/* All new GMU offsets are relative to the upstream GMU core resource,
 * unlike the GPU-relative dword offsets in milestone 1. */
static uint32_t gm_rd(unsigned reg) { return g_rd(GPU_GMU_CORE, reg * 4); }
static void gm_wr(unsigned reg, uint32_t val) { g_wr(GPU_GMU_CORE, reg * 4, val); }
static void g_wr64(uint64_t at, uint64_t val)
{
 __asm__ volatile("dmb oshst" ::: "memory");
 *(volatile uint64_t *)(uintptr_t)at = val;
 __asm__ volatile("dsb sy" ::: "memory");
}
static void g_zero(uint64_t at, unsigned bytes)
{
 for (unsigned i = 0; i < bytes; i += 8) *(volatile uint64_t *)(uintptr_t)(at + i) = 0;
 __asm__ volatile("dsb sy" ::: "memory");
}
static unsigned g_cb(unsigned b) { return g_cbbase + b * g_stride; }
static void g_live(void)
{
 uint32_t *e = gpu_extra[0];
 e[4] = gpu_gmu_started;
 e[84] = gm_rd(0x5192); e[85] = gm_rd(0x9305); e[86] = gm_rd(0x9313);
 e[87] = g_rd(GPU_SMMU_BASE, SMMU_GFSR);
 if (gpu_m2_attempted && e[15]) {
  unsigned cb = g_cb(g_bank);
  e[88] = g_rd(GPU_SMMU_BASE, cb + 0x58);
  e[89] = g_rd(GPU_SMMU_BASE, cb + 0x60);
  e[90] = g_rd(GPU_SMMU_BASE, cb + 0x64);
  e[91] = g_rd(GPU_SMMU_BASE, cb + 0x68);
  e[92] = g_rd(GPU_SMMU_BASE, cb + 0x6c);
 }
 if (gpu_s8_ok) {
  e[93] = g_rd(G_HFI_PA, 24 + 40); e[94] = g_rd(G_HFI_PA, 24 + 44);
  e[95] = g_rd(G_HFI_PA, 72 + 40); e[96] = g_rd(G_HFI_PA, 72 + 44);
 }
}
static int g_snapshot(void)
{
 uint32_t *e = gpu_extra[0];
 uint32_t id0 = g_rd(GPU_SMMU_BASE, SMMU_ID0), id1 = g_rd(GPU_SMMU_BASE, SMMU_ID1);
 uint32_t id2 = g_rd(GPU_SMMU_BASE, SMMU_ID2);
 g_stride = (id1 >> 31) ? 65536 : 4096;
 g_cbbase = (2u << ((id1 >> 28) & 7)) * g_stride;
 g_ncb = id1 & 255; g_nsmr = id0 & 255;
 e[5] = g_nsmr; e[6] = g_ncb; e[7] = g_stride; e[8] = g_cbbase;
 /* Bound every access to Fuxi's 256K resource and report capacity. */
 if (!g_ncb || g_ncb > 24 || !g_nsmr || g_nsmr > 32 ||
     g_cbbase >= 0x40000 || g_ncb * g_stride > 0x40000 - g_cbbase ||
     !(id0 & (1u << 30)) || !(id0 & (1u << 27)) ||
     !(id2 & (1u << 12)) || ((id2 >> 4) & 15) < 1 ||
     (g_rd(GPU_SMMU_BASE, SMMU_SCR0) & 1)) { e[2] = 0x801; return 0; }
 for (unsigned i = 0; i < g_nsmr; i++) {
  gpu_extra[1][i * 2] = g_rd(GPU_SMMU_BASE, SMMU_SMR(i));
  gpu_extra[1][i * 2 + 1] = g_rd(GPU_SMMU_BASE, SMMU_S2CR(i));
 }
 for (unsigned b = 0; b < g_ncb; b++) {
  uint32_t *d = &gpu_extra[2 + b / 8][(b % 8) * 16];
  unsigned cb = g_cb(b);
  /* Spare words in the stream page retain implementation context controls. */
  gpu_extra[1][64 + b] = g_rd(GPU_SMMU_BASE, cb + 4); /* ACTLR */
  gpu_extra[1][88 + b] = g_rd(GPU_SMMU_BASE, cb + 0x34); /* CONTEXTIDR */
  d[0] = g_rd(GPU_SMMU_BASE, g_stride + b * 4);
  d[1] = g_rd(GPU_SMMU_BASE, g_stride + 0x800 + b * 4);
  static const unsigned offs[14] = {0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c,0x58,0x60,0x64,0x68,0x6c};
  for (unsigned i = 0; i < 14; i++) d[2 + i] = g_rd(GPU_SMMU_BASE, cb + offs[i]);
 }
 e[9] = g_rd(GPU_SMMU_BASE, SMMU_GFSR);
 gpu_snapshot_valid = 1; e[1] = 1;
 return 1;
}
static uint64_t g_new_table(void)
{
 if (g_pt_next >= 256) return 0;
 return G_MEM + 4096 * g_pt_next++;
}
/* 32-bit IOVA, 4K granules: L1 root -> L2 -> L3 pages. All leafs NC,
 * inner-shareable (MAIR index 0 = 0x44), AF=1. Privileged fixed regions
 * match MSM_BO_MAP_PRIV; log/HFI permit unprivileged GMU accesses. */
static int g_map(unsigned va, uint64_t pa, unsigned size, unsigned unpriv)
{
 for (unsigned off = 0; off < size; off += 4096) {
  unsigned v = va + off;
  volatile uint64_t *l1 = (volatile uint64_t *)(uintptr_t)G_MEM;
  unsigned a = v >> 30;
  if (!l1[a]) { uint64_t t = g_new_table(); if (!t) return 0; l1[a] = t | 3; }
  volatile uint64_t *l2 = (volatile uint64_t *)(uintptr_t)(l1[a] & ~0xfffull);
  unsigned b = (v >> 21) & 511;
  if (!l2[b]) { uint64_t t = g_new_table(); if (!t) return 0; l2[b] = t | 3; }
  volatile uint64_t *l3 = (volatile uint64_t *)(uintptr_t)(l2[b] & ~0xfffull);
  unsigned c = (v >> 12) & 511;
  if (l3[c]) return 0;
  l3[c] = (pa + off) | 3 | (3u << 8) | (1u << 10) | (unpriv ? (1u << 6) : 0);
 }
 __asm__ volatile("dsb sy" ::: "memory");
 return 1;
}
static int g_range(unsigned addr, unsigned size, unsigned base, unsigned bytes)
{ return addr >= base && addr - base < bytes && size <= bytes - (addr - base); }
/* Return a physical destination, or zero for an unmapped address. */
static uint64_t g_fw_dest(unsigned addr, unsigned size)
{
 if (g_range(addr, size, 0, 0x4000)) return GPU_GMU_CORE + 0xc00 * 4 + addr;
 if (g_range(addr, size, 0x10004000, 0x4000)) return GPU_GMU_CORE + 0x1c00 * 4 + addr - 0x10004000;
 if (g_range(addr, size, 0x4000, G_IC_SIZE)) return G_IC_PA + addr - 0x4000;
 if (g_range(addr, size, G_DUMMY_VA, 0x2000)) return G_DATA_PA + addr - G_DUMMY_VA;
 return 0;
}
static int g_firmware(unsigned load)
{
#if defined(FUXI_XNU_LOADER)
 extern const uint8_t _gpu_fw_start[], _gpu_fw_end[];
 const uint8_t *fw = _gpu_fw_start;
 size_t bytes = (uintptr_t)_gpu_fw_end - (uintptr_t)_gpu_fw_start;
#else
 const uint8_t *fw = 0; size_t bytes = 0;
#endif
 uint32_t *e = gpu_extra[0]; e[17] = bytes;
 if (!bytes) { e[2] = 0x802; return 0; }
 size_t at = 0; unsigned blocks = 0, prealloc = 0, total = 0;
 while (at < bytes) {
  if (bytes - at < 16) { e[2] = 0x803; return 0; }
  unsigned addr = cdb32(fw + at), size = cdb32(fw + at + 4);
  unsigned extent = size ? size : cdb32(fw + at + 12);
  uint64_t dst = g_fw_dest(addr, extent);
  if ((addr & 3) || (size & 3) || size > bytes - at - 16 || !dst || !extent) {
   e[2] = 0x803; e[21] = addr; e[22] = size; return 0;
  }
  if (size) {
   blocks++; total += size;
   if (load) for (unsigned i = 0; i < size; i += 4) {
    uint32_t word = cdb32(fw + at + 16 + i);
    g_wr(dst, i, word);
    if (g_rd(dst, i) != word) { e[2] = 0x80a; e[21] = addr + i; return 0; }
   }
  } else prealloc++;
  at += 16 + size;
 }
 e[18] = blocks; e[19] = prealloc; e[20] = total;
 if (!blocks) { e[2] = 0x803; return 0; }
 return 1;
}
static int g_select_context(void)
{
 uint32_t *e = gpu_extra[0]; unsigned used = 0;
 g_slot = g_nsmr;
 for (unsigned i = 0; i < g_nsmr; i++) {
  unsigned smr = gpu_extra[1][2*i], s2cr = gpu_extra[1][2*i+1];
  if (!(smr & (1u << 31))) { if (g_slot == g_nsmr) g_slot = i; continue; }
  unsigned mask = (smr >> 16) & 0x7fff;
  if (((smr ^ 5) & ~mask & 0x7fff) == 0) { e[2] = 0x804; return 0; }
  if (((s2cr >> 16) & 3) == 0) {
   unsigned bank = s2cr & 255;
   if (bank < g_ncb) used |= 1u << bank;
  }
 }
 g_bank = g_ncb;
 unsigned s2banks = (g_rd(GPU_SMMU_BASE, SMMU_ID1) >> 16) & 255;
 for (unsigned b = s2banks ? s2banks : 1; b < g_ncb; b++)
  if (!(used & (1u << b)) && !(gpu_extra[2+b/8][b%8*16+2] & 1)) { g_bank = b; break; }
 if (g_slot == g_nsmr || g_bank == g_ncb) { e[2] = 0x805; return 0; }
 /* ASIDs are shared across banks; avoid every enabled bank's two TTBR ASIDs. */
 for (g_asid = 1; g_asid < 256; g_asid++) {
  int used_asid = 0;
  for (unsigned b = 0; b < g_ncb; b++) {
   uint32_t *d = &gpu_extra[2+b/8][b%8*16];
   if ((d[2] & 1) && ((d[5] >> 16) == g_asid || (d[7] >> 16) == g_asid)) used_asid = 1;
  }
  if (!used_asid) break;
 }
 if (g_asid == 256) { e[2] = 0x805; return 0; }
 e[10] = g_slot; e[11] = g_bank; e[12] = g_asid;
 return 1;
}
static int g_program_context(void)
{
 uint32_t *e = gpu_extra[0]; unsigned cb = g_cb(g_bank);
 /* An unused, disabled bank only; never alter GR0 or unrelated streams. */
 if (g_rd(GPU_SMMU_BASE, cb) & 1) { e[2] = 0x805; return 0; }
 uint64_t ttbr = G_MEM | ((uint64_t)g_asid << 48);
 g_wr(GPU_SMMU_BASE, g_stride + 0x800 + g_bank * 4, 1);
 g_wr(GPU_SMMU_BASE, g_stride + g_bank * 4, 0x1f300);
 /* 36-bit PA, AArch64 ASID, TTBR0=32-bit IOVA, TTBR1 disabled.
  * Table walks are NC/outer-shareable, coherent with uncached CPU writes. */
 g_wr(GPU_SMMU_BASE, cb + 0x10, 0x38011);
 g_wr(GPU_SMMU_BASE, cb + 0x30, 0x80802020);
 g_wr64(GPU_SMMU_BASE + cb + 0x20, ttbr);
 g_wr64(GPU_SMMU_BASE + cb + 0x28, (uint64_t)g_asid << 48);
 g_wr(GPU_SMMU_BASE, cb + 0x38, 0xff44);
 g_wr(GPU_SMMU_BASE, cb + 0x3c, 0);
 g_wr(GPU_SMMU_BASE, cb + 0x58, 0xffffffff);
 e[15] = 1;
 g_wr(GPU_SMMU_BASE, cb + 0x610, g_asid);
 g_wr(GPU_SMMU_BASE, cb + 0x7f0, 0);
 unsigned i;
 for (i = 0; i < 1000; i++) {
  e[16] = g_rd(GPU_SMMU_BASE, cb + 0x7f4);
  if (!(e[16] & 1)) break;
  delay(1);
 }
 if (i == 1000) { e[2] = 0x808; return 0; }
 e[23] = g_rd(GPU_SMMU_BASE, g_stride + g_bank * 4);
 e[24] = g_rd(GPU_SMMU_BASE, g_stride + 0x800 + g_bank * 4);
 e[25] = g_rd(GPU_SMMU_BASE, cb + 0x10); e[26] = g_rd(GPU_SMMU_BASE, cb + 0x30);
 e[27] = g_rd(GPU_SMMU_BASE, cb + 0x20); e[28] = g_rd(GPU_SMMU_BASE, cb + 0x24);
 e[29] = g_rd(GPU_SMMU_BASE, cb + 0x38);
 if (e[23] != 0x1f300 || e[24] != 1 || (e[25] & ~0x60u) != 0x38011 ||
     e[26] != 0x80802020 || e[27] != (uint32_t)ttbr || e[28] != (uint32_t)(ttbr >> 32) ||
     e[29] != 0xff44 || g_rd(GPU_SMMU_BASE, cb + 0x28) ||
     g_rd(GPU_SMMU_BASE, cb + 0x2c) != (g_asid << 16) || g_rd(GPU_SMMU_BASE, cb + 0x3c)) {
  e[2] = 0x809; return 0;
 }
 g_wr(GPU_SMMU_BASE, cb, 0x1067); /* faults reported, no interrupt controller needed */
 e[30] = g_rd(GPU_SMMU_BASE, cb);
 if ((e[30] & ~0x1000u) != 0x67) { e[2] = 0x809; return 0; }
 g_wr(GPU_SMMU_BASE, SMMU_S2CR(g_slot), g_bank); /* translate, PRIVCFG=default */
 g_wr(GPU_SMMU_BASE, SMMU_SMR(g_slot), 0x80000005);
 e[31] = g_rd(GPU_SMMU_BASE, SMMU_SMR(g_slot));
 e[32] = g_rd(GPU_SMMU_BASE, SMMU_S2CR(g_slot));
 if (e[31] != 0x80000005 || e[32] != g_bank) { e[2] = 0x809; return 0; }
 return 1;
}
static int g_memory_load(void)
{
 uint32_t *e = gpu_extra[0];
 if (gm_rd(0x5000) != 1 || g_rd(GPU_CXMISC_BASE, 0x39*4) ||
     g_rd(GPU_RSCC_BASE, 0x101*4) || (gm_rd(0x5180) & 1)) { e[2] = 0x806; return 0; }
 if (!g_firmware(0) || !g_select_context()) return 0;
 extern char _head[], _end[];
 uint64_t image = (uintptr_t)_head, end = (uintptr_t)_end;
 uint64_t fdt = probe_fdt;
 if ((image < G_MEM + G_MEM_SIZE && G_MEM < end) ||
     (fdt < G_MEM + G_MEM_SIZE && G_MEM < fdt + probe_fdt_size)) { e[2] = 0x807; return 0; }
 g_zero(G_MEM, 0x100000); g_zero(G_IC_PA, G_IC_SIZE); g_zero(G_DATA_PA, 0x11000);
 g_pt_next = 1;
 if (!g_map(0x4000, G_IC_PA, G_IC_SIZE, 0) || !g_map(G_DUMMY_VA, G_DATA_PA, 0x2000, 0) ||
     !g_map(G_LOG_VA, G_LOG_PA, 0x4000, 1) || !g_map(G_HFI_VA, G_HFI_PA, 0x4000, 1) ||
     !g_map(G_DEBUG_VA, G_DEBUG_PA, 0x7000, 0)) { e[2] = 0x80b; return 0; }
 e[13] = (uint32_t)G_MEM; e[14] = G_MEM >> 32; e[33] = g_pt_next;
 /* Linux fw_start's FAL veto + retention precede the TCM firmware writes. */
 gm_wr(0x50f1, 1); gm_wr(0x50f0, 1); g_wr(GPU_CXMISC_BASE, 0x39*4, 1);
 if (!g_program_context() || !g_firmware(1)) return 0;
 if (g_rd(GPU_SMMU_BASE, g_cb(g_bank) + 0x58) & 0xc00001feu) { e[2] = 0x80c; return 0; }
 e[2] = 1; gpu_s8_ok = 1;
 return 1;
}
/* Linux has two queues, not three: command id0 and response id1. */
static void g_hfi_init(void)
{
 g_zero(G_HFI_PA, 0x4000);
 static const uint32_t table[6] = {0,120,6,12,2,2}; /* size is bytes in Linux implementation */
 for (unsigned i = 0; i < 6; i++) g_wr(G_HFI_PA, i*4, table[i]);
 for (unsigned q = 0; q < 2; q++) {
  unsigned h = 24 + q*48;
  g_wr(G_HFI_PA, h, 1); g_wr(G_HFI_PA, h+4, G_HFI_VA + (q+1)*4096);
  g_wr(G_HFI_PA, h+8, (10u<<8)|q); g_wr(G_HFI_PA, h+12, 1024);
  g_wr(G_HFI_PA, h+24, 1); g_wr(G_HFI_PA, h+28, 1); g_wr(G_HFI_PA, h+32, 1);
 }
}
/* Return cmd-db aux data only after checking both the header and data extents. */
static const uint8_t *g_aux(const char *name, unsigned *count)
{
 for (unsigned s = 0; s < 8; s++) {
  const uint8_t *h = gpu_cmddb + 8 + s*16;
  unsigned n = cdb16(h+6), at = 144 + cdb16(h+2), data = 144 + cdb16(h+4);
  if (!cdb16(h)) continue;
  if (at > GPU_CMDDB_BYTES || n > (GPU_CMDDB_BYTES-at)/24) return 0;
  for (unsigned i = 0; i < n; i++) {
   const uint8_t *p = gpu_cmddb + at + i*24;
   unsigned j; for (j = 0; j < 8; j++) if (p[j] != (uint8_t)name[j]) break;
   if (j != 8) continue;
   unsigned len = cdb16(p+20), off = data + cdb16(p+22);
   if (!len || (len & 1) || off > GPU_CMDDB_BYTES || len > GPU_CMDDB_BYTES-off || len > 512) return 0;
   *count = len/2; return gpu_cmddb + off;
  }
 }
 return 0;
}
static int g_arc_vote(const char *primary, const char *secondary, unsigned level, uint32_t *vote)
{
 unsigned np = 0, ns = 0;
 const uint8_t *p = g_aux(primary, &np), *s = g_aux(secondary, &ns);
 if (!p || !s) return 0;
 unsigned pi, si = 0;
 for (pi = 0; pi < np; pi++) if (cdb16(p+pi*2) >= level) break;
 if (pi == np) return 0;
 for (unsigned j = 0; j < ns; j++) {
  if (cdb16(s+j*2) >= level) { si = j; break; }
  if (cdb16(s+j*2)) si = j;
 }
 *vote = (uint32_t)cdb16(p+pi*2)<<16 | si<<8 | pi;
 return 1;
}
/* e[40..59]: five command/ACK records {sent, response, returned, error}.
 * Poll shared queue indices as well as interrupt status; reject malformed or
 * unsolicited packets within one bounded deadline (no unbounded drain loop). */
static int g_hfi_send(unsigned id, uint32_t *msg, unsigned words, unsigned record)
{
 uint32_t *e = gpu_extra[0], *rec = e + 40 + record*4;
 unsigned seq = record + 1, count = (words+3)&~3u;
 unsigned rd = g_rd(G_HFI_PA, 64), wr = g_rd(G_HFI_PA, 68);
 rec[0] = msg[0] = seq<<20 | words<<8 | id;
 if (words > 255 || rd >= 1024 || wr >= 1024 || (wr & 3) || ((rd-wr-1)&1023) < count) return 0x904;
 for (unsigned i = 0; i < count; i++) g_wr(G_HFI_PA+4096, ((wr+i)&1023)*4, i < words ? msg[i] : 0xfafafafa);
 g_wr(G_HFI_PA, 68, (wr+count)&1023);
 gm_wr(0x5194, 1);
 uint64_t until = rsr("cntpct_el0") + rsr("cntfrq_el0") / 20; /* 50ms total */
 do {
  unsigned intr = gm_rd(0x5192);
  if (intr & (1u<<23)) return 0x905;
  rd = g_rd(G_HFI_PA, 112); wr = g_rd(G_HFI_PA, 116);
  if (rd >= 1024 || wr >= 1024 || (rd & 3) || (wr & 3)) return 0x904;
  if (rd == wr) { delay(10); continue; }
  __asm__ volatile("dmb oshld" ::: "memory");
  unsigned hdr = g_rd(G_HFI_PA+8192, rd*4), n = (hdr>>8)&255;
  rec[1] = hdr;
  if (n < 3 || n > 19 || ((n+3)&~3u) > ((wr-rd)&1023)) return 0x904;
  rec[2] = g_rd(G_HFI_PA+8192, ((rd+1)&1023)*4);
  rec[3] = g_rd(G_HFI_PA+8192, ((rd+2)&1023)*4);
  g_wr(G_HFI_PA, 112, (rd+((n+3)&~3u))&1023);
  gm_wr(0x5191, 1);
  if ((hdr&255) == 100) return 0x905;
  if ((hdr&255) != 126 || ((hdr>>16)&15) != 1 || rec[2] != rec[0]) return 0x906;
  if (rec[3]) return 0x907;
  return 0;
 } while (rsr("cntpct_el0") < until);
 return 0x903;
}
static void g_rpmh_init(void)
{
 static const unsigned regs[] = {4,0xb,0xe,0xd,0x10,0xf,0x12,0x11,0x100,8,9,0xa,0x154,0x155,0x156,0x157,0x158};
 static const uint32_t vals[] = {1u<<24,1,0,0,0,0,0x80000021,0,0,0x4520,0x4510,0x4514,
  0xeaaae5a0,0xe1a1ebab,0xa2e0a581,0xecac82e2,0x0020edad};
 for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) g_wr(GPU_RSCC_BASE, regs[i]*4, vals[i]);
 /* A7xx PDC uCode is AOP-owned: only enable/configure, never replace it. */
 g_wr(0x0b280000, 0x1148*4, 0); g_wr(0x0b280000, 0x1140*4, 0x80000001);
}
static int g_poll(unsigned reg, uint32_t mask, uint32_t value, unsigned word)
{
 uint32_t *e = gpu_extra[0];
 for (unsigned i = 0; i < 1000; i++) {
  e[word] = gm_rd(reg);
  if ((e[word]&mask) == value) return 1;
  if (gm_rd(0x5192) & (1u<<23)) return 0;
  delay(10);
 }
 return 0;
}
static int g_start(void)
{
 uint32_t *e = gpu_extra[0], perf[59], bw[160];
 /* Volatile stores avoid compiler-emitted libc memset in a freestanding TU. */
 for (unsigned i = 0; i < 59; i++) ((volatile uint32_t *)perf)[i] = 0;
 for (unsigned i = 0; i < 160; i++) ((volatile uint32_t *)bw)[i] = 0;
 /* Ascending Linux frequency indices, including OFF at index 0. kHz. */
 static const uint32_t freq[10] = {0,220000,295000,348000,401000,475000,550000,615000,680000,719000};
 static const unsigned level[10] = {0,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0};
 static const uint32_t cfreq[3] = {0,200000,500000};
 static const unsigned clevel[3] = {0,0x40,0x80};
 static const char gfx[8] = "gfx.lvl", mx[8] = "mx.lvl", cx[8] = "cx.lvl", gmxc[8] = "gmxc.lvl";
 unsigned unused;
 const char *secondary = g_aux(gmxc, &unused) ? gmxc : mx;
 perf[1] = 10; perf[2] = 3;
 for (unsigned i = 0; i < 10; i++) {
  if (!g_arc_vote(gfx, secondary, level[i], &perf[3+i*3])) { e[3] = 0x908; return 0; }
  perf[4+i*3] = 0xffffffff; perf[5+i*3] = freq[i]; e[100+i] = perf[3+i*3];
 }
 for (unsigned i = 0; i < 3; i++) {
  if (!g_arc_vote(cx, mx, clevel[i], &perf[51+i*2])) { e[3] = 0x908; return 0; }
  perf[52+i*2] = cfreq[i]; e[110+i] = perf[51+i*2];
 }
 /* Full Linux bw struct is 160 words: 6 scalar + 6 + 12 + 8 + 128. */
 bw[1] = 1; bw[2] = 1; bw[3] = 3; bw[4] = 1; bw[5] = 7;
 bw[6] = gpu_report[90]; bw[12] = 0x40000000; bw[18] = 0x60000001;
 for (unsigned i = 0; i < 3; i++) {
  bw[24+i] = gpu_report[87+i]; bw[32+i] = 0x40000000;
  if (!bw[24+i]) { e[3] = 0x908; return 0; }
 }
 if (!bw[6]) { e[3] = 0x908; return 0; }
 if (gm_rd(0x5000) != 1 || g_rd(GPU_RSCC_BASE, 0x101*4) || (gm_rd(0x5180)&1)) { e[3] = 0x909; return 0; }
 g_hfi_init(); g_rpmh_init();
 gm_wr(0x9304, 0xffffffff); gm_wr(0x9306, 0xffffffff);
 gm_wr(0x5191, 0xffffffff); gm_wr(0x5193, 0xffffffff); /* polled, no GIC IRQ */
 gm_wr(0x501c, 0); gm_wr(0x5001, 2);
 gm_wr(0x5185, G_HFI_VA); gm_wr(0x5184, 1);
 gm_wr(0x9311, 0x80000000u | (0x32u<<18) | 0x8a0);
 gm_wr(0x502d, 0x4052); gm_wr(0x51cf, 0x7020100); gm_wr(0x51cd, G_LOG_VA | 3);
 gm_wr(0x4c0f, 1); gm_wr(0x4c00, 1); gm_wr(0x4c01, 1);
 e[34] = gm_rd(0x1c00+0xff8);
 uint32_t mask = e[34] <= 0x20010004 ? 0xffffffff : 0x1ff;
 e[35] = e[34] <= 0x20010004 ? 0xbabeface : 0x100;
 gm_wr(0x5000, 1); gm_wr(0x51ce, 0);
 gpu_gmu_started = 1; e[4] = 1; /* latch BEFORE releasing reset, including timeout */
 __asm__ volatile("dsb sy" ::: "memory");
 gm_wr(0x5000, 0);
 if (!g_poll(0x501c, mask, e[35], 36)) { e[3] = 0x901; return 0; }
 gm_wr(0x5186, 1);
 if (!g_poll(0x5180, 1, 1, 37)) { e[3] = 0x902; return 0; }
 e[38] = gm_rd(0x5181);
 int err = g_hfi_send(4, perf, 59, 0);
 if (!err) err = g_hfi_send(3, bw, 160, 1);
 uint32_t core[2] = {0}, start[1] = {0};
 if (!err) err = g_hfi_send(14, core, 2, 2);
 if (!err) err = g_hfi_send(10, start, 1, 3);
 if (err) { e[3] = err; return 0; }
 e[3] = 1; gpu_s9_ok = 1;
 return 1;
}
static void g_gx(void)
{
 uint32_t *e = gpu_extra[0];
 if (g_rd(GPU_SMMU_BASE, g_cb(g_bank) + 0x58) & 0xc00001feu) { e[60] = 0xa04; return; }
 uint32_t vote[4] = {0,1,1,0}; /* blocking, index1=220MHz, BW0 (only entry) */
 int err = g_hfi_send(30, vote, 4, 4);
 if (err) { e[60] = err; return; }
 gm_wr(0x5191, 1u<<31); gm_wr(0x5194, 1u<<30);
 int ack = g_poll(0x5192, 1u<<31, 1u<<31, 61);
 gm_wr(0x5191, 1u<<31);
 if (!ack) { gm_wr(0x5194, 1u<<31); e[60] = 0xa01; return; }
 /* Hold GPU_SET while checking GX and reading RBBM, just as Linux hw_init.
  * Linux XML GX_HM_OFF bits6/7 must be clear; also wait transition bits0/1. */
 if (!g_poll(0x50d0, 0xc3, 0, 62)) { gm_wr(0x5194, 1u<<31); e[60] = 0xa02; return; }
 e[63] = g_rd(GPU_GPUCC_BASE, CC_GX_GDSCR);
 if (!g_gdsc_on(e[63])) { gm_wr(0x5194, 1u<<31); e[60] = 0xa03; return; }
 e[64] = g_rd(GPU_REG_BASE, GPU_RBBM_STATUS*4); e[65] = 1;
 gm_wr(0x5194, 1u<<31); e[60] = 1;
}
static void gpu_m2(unsigned limit)
{
 if (!gpu_snapshot_valid && !g_snapshot()) return;
 if (limit == G_STAGE_8A) { g_live(); return; }
 if (!gpu_m2_attempted) { g_enter(8); gpu_m2_attempted = 1; g_memory_load(); }
 if (!gpu_s8_ok || limit < 9) { g_live(); return; }
 if (!gpu_s9_attempted) { g_enter(9); gpu_s9_attempted = 1; g_start(); }
 if (!gpu_s9_ok || limit < 10) { g_live(); return; }
 if (!gpu_s10_attempted) { g_enter(10); gpu_s10_attempted = 1; g_gx(); }
 g_live();
}

static void gpu_probe(unsigned limit)
{
 uint32_t *r = gpu_report;
 /* Once stage8 has touched tables or retention, never replay power/fw setup.
  * Stages8->9->10 may advance once; reads of old stages return the frozen report. */
 if (gpu_snapshot_valid) {
  if (limit >= 8) gpu_m2(limit);
  r[1] = limit; r[2] = gpu_stage; r[3] = gpu_gmu_started;
  return;
 }
 for (int i = 0; i < GPU_REPORT_WORDS; i++) r[i] = 0;
 if (!limit) return;
 if (limit != G_STAGE_8A && limit > 10) limit = 10;
 r[0] = 0xa740;          /* magic */
 r[1] = limit;
 gpu_extra[0][0] = 0xa7400002;

 /* ---- Stage 1: read-only snapshot (always safe: same GCC family the UFS /
  * touch probes already read; GPUCC/SMMU/GPU reads are each gated). ---- */
 g_enter(1);
 uint32_t cfg_ahb = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB);
 uint32_t memnoc  = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_MEMNOC_GFX);
 uint32_t dvm     = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_SNOC_DVM_GFX);
 uint32_t vote    = g_rd(GPU_GCC_VOTE_PAGE, 0);
 r[4] = cfg_ahb; r[5] = memnoc; r[6] = dvm; r[7] = vote;
 uint32_t cx_gdscr = 0, gx_gdscr = 0, cc_ahb = 0, cc_smmu = 0, cc_gmu = 0;
 int cc_read = 0;
 if (cfg_ahb & CBCR_ENABLE) {         /* GPUCC needs gcc_gpu_cfg_ahb_clk enabled */
  cx_gdscr = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
  gx_gdscr = g_rd(GPU_GPUCC_BASE, CC_GX_GDSCR);
  cc_ahb   = g_rd(GPU_GPUCC_BASE, CC_AHB_CLK);
  cc_smmu  = g_rd(GPU_GPUCC_BASE, CC_SMMU_VOTE);
  cc_gmu   = g_rd(GPU_GPUCC_BASE, CC_CX_GMU_CLK);
  cc_read = 1;
 }
 r[8] = cx_gdscr; r[9] = gx_gdscr; r[10] = cc_ahb; r[11] = cc_smmu; r[12] = cc_gmu;
 r[13] = cc_read;
 /* SMMU GR0 only if CX on and the HLOS vote clock runs. */
 if (cc_read && g_gdsc_on(cx_gdscr) && g_clk_on(cc_smmu)) {
  r[16] = g_rd(GPU_SMMU_BASE, SMMU_SCR0);
  r[17] = g_rd(GPU_SMMU_BASE, SMMU_ID0);
  r[18] = g_rd(GPU_SMMU_BASE, SMMU_ID1);
  r[19] = g_rd(GPU_SMMU_BASE, SMMU_ID2);
  r[20] = g_rd(GPU_SMMU_BASE, SMMU_GFSR);
  unsigned n = r[17] & 0xff, valid = 0, first = 0xffff;
  if (n > 128) n = 128;
  for (unsigned i = 0; i < n; i++) {
   uint32_t smr = g_rd(GPU_SMMU_BASE, SMMU_SMR(i));
   if (smr & (1u << 31)) { valid++; if (first == 0xffff) { first = i; r[22] = smr; r[23] = g_rd(GPU_SMMU_BASE, SMMU_S2CR(i)); } }
  }
  r[21] = valid | (first << 16);
  r[15] = 1;   /* smmu read */
 }
 /* GPU RBBM_STATUS only if CX+GX both on (GX-gated on A7XX). */
 if (cc_read && g_gdsc_on(cx_gdscr) && g_gdsc_on(gx_gdscr)) { r[24] = g_rd(GPU_REG_BASE, GPU_RBBM_STATUS * 4); r[14] = 1; }
 if (limit < 2) return;

 /* ---- Stage 2: the one write the gcc-sm8550 probe makes. ---- */
 g_enter(2);
 if (!(cfg_ahb & CBCR_ENABLE)) { g_wr(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB, cfg_ahb | CBCR_ENABLE); r[26] = 1; }
 cfg_ahb = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB);
 r[27] = cfg_ahb;
 if (!(cfg_ahb & CBCR_ENABLE)) return;   /* nothing beyond is readable */
 if (limit < 3) return;

 /* ---- Stage 3: CX GDSC + GPUCC AHB + HLOS SMMU vote (gdsc_enable mirror). ---- */
 g_enter(3);
 cx_gdscr = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
 r[28] = cx_gdscr;
 if (!g_gdsc_on(cx_gdscr)) {
  g_wr(GPU_GPUCC_BASE, CC_CX_GDSCR, cx_gdscr & ~GDSC_SW_COLLAPSE);
  delay(1);
  uint32_t st = 0; unsigned w;
  for (w = 0; w < 200; w++) { st = g_rd(GPU_GPUCC_BASE, CC_CX_GDS_HW); if (g_gdsc_on(st)) break; delay(10); }
  r[29] = st;
  if (g_gdsc_on(st)) {
   uint32_t v = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
   g_wr(GPU_GPUCC_BASE, CC_CX_GDSCR, v | GDSC_RETAIN_MEM | GDSC_RETAIN_PERIPH); delay(1);
   g_wr(GPU_GPUCC_BASE, CC_CX_GDSCR, v | GDSC_RETAIN_MEM | GDSC_RETAIN_PERIPH | GDSC_RETAIN_FF); delay(1);
  }
 }
 cx_gdscr = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
 r[30] = cx_gdscr;
 if (!g_gdsc_on(cx_gdscr)) return;
 g_set_bit0(GPU_GPUCC_BASE, CC_AHB_CLK);
 g_set_bit0(GPU_GPUCC_BASE, CC_SMMU_VOTE);
 { unsigned w; for (w = 0; w < 50; w++) { cc_smmu = g_rd(GPU_GPUCC_BASE, CC_SMMU_VOTE); if (g_clk_on(cc_smmu)) break; delay(10); } }
 r[31] = g_rd(GPU_GPUCC_BASE, CC_AHB_CLK);
 r[32] = cc_smmu;
 r[33] = g_clk_on(cc_smmu);
 if (limit < 4) return;

 /* ---- Stage 4: now-powered adreno SMMU GR0 + CB0/CB1 snapshot (read-only). ---- */
 g_enter(4);
 if (g_clk_on(cc_smmu)) {
  r[36] = g_rd(GPU_SMMU_BASE, SMMU_SCR0);
  r[37] = g_rd(GPU_SMMU_BASE, SMMU_ID0);
  r[38] = g_rd(GPU_SMMU_BASE, SMMU_ID1);
  r[39] = g_rd(GPU_SMMU_BASE, SMMU_ID2);
  uint32_t id1 = r[38];
  unsigned pageshift = (id1 >> 31) ? 16 : 12;
  uint64_t gr1 = 1ull << pageshift;
  uint64_t pages = 1ull << (((id1 >> 28) & 7) + 1);
  /* CB0 (GPU user) and CB1 context base + CBAR: read only, no translation touched. */
  for (unsigned b = 0; b < 2; b++) {
   uint64_t cb = (pages + b) * gr1;
   r[40 + b * 4 + 0] = g_rd(GPU_SMMU_BASE, (uint32_t)(gr1 + 4 * b));          /* CBAR */
   r[40 + b * 4 + 1] = g_rd(GPU_SMMU_BASE, (uint32_t)cb);                      /* SCTLR */
   r[40 + b * 4 + 2] = g_rd(GPU_SMMU_BASE, (uint32_t)(cb + 0x58));             /* FSR */
   r[40 + b * 4 + 3] = g_rd(GPU_SMMU_BASE, (uint32_t)(cb + 0x30));             /* TCR */
  }
  r[35] = 1;
 }
 if (!r[35] || limit < 5) return;

 /* ---- Stage 5: GMU clock snapshot (read-only; a6xx_gmu_resume expects XO). ---- */
 g_enter(5);
 uint32_t pll1_mode = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
 uint32_t pll1_op   = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_OPMODE);
 r[48] = pll1_mode; r[49] = pll1_op;
 r[50] = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_L_VAL);
 r[51] = g_rd(GPU_GPUCC_BASE, CC_GMU_RCGR);
 r[52] = g_rd(GPU_GPUCC_BASE, CC_GMU_RCGR + 4);
 r[53] = g_rd(GPU_GPUCC_BASE, CC_HUB_RCGR);
 r[54] = g_rd(GPU_GPUCC_BASE, CC_HUB_RCGR + 4);
 r[55] = g_rd(GPU_GPUCC_BASE, CC_CX_GMU_CLK);
 r[56] = g_rd(GPU_GPUCC_BASE, CC_CXO_CLK);
 r[57] = g_rd(GPU_GPUCC_BASE, CC_HUB_CX_INT);
 r[58] = g_rd(GPU_GPUCC_BASE, CC_DEMET);
 r[59] = g_rd(GPU_GCC_GPU_PAGE, GCC_DDRSS_GPU_AXI);
 r[60] = g_rd(GPU_GCC_VOTE_PAGE, 0);
 /* GMU core state words, only if CX on and AHB + cx_gmu clocks already run. */
 if (g_gdsc_on(cx_gdscr) && g_clk_on(g_rd(GPU_GPUCC_BASE, CC_AHB_CLK)) && g_clk_on(r[55])) {
  r[61] = g_rd(GMU_DWORD(GMUD_CM3_SYSRESET), 0);
  r[62] = g_rd(GPU_CXMISC_BASE, CXMISC_TCM_RET_CNTL * 4);
  r[63] = g_rd(GPU_RSCC_BASE, RSCC_SEQ_BUSY_DRV0 * 4);
  r[64] = g_rd(GMU_DWORD(GMUD_HFI_CTRL_STATUS), 0);
  r[79] = 1;   /* gmu core regs were readable in stage 5 */
 }
 if (limit < 6) return;

 /* ---- Stage 6: GMU clock bring-up (a6xx_gmu_resume + gpucc probe order). ---- */
 g_enter(6);
 /* 1. GPLL0 GPU votes */
 vote = g_rd(GPU_GCC_VOTE_PAGE, 0);
 if ((vote & GPLL0_VOTES) != GPLL0_VOTES) { g_wr(GPU_GCC_VOTE_PAGE, 0, vote | GPLL0_VOTES); delay(10); }
 r[66] = 1;
 /* 2/3. Configure + enable PLL1 (and configure PLL0) unless PLL1 already runs. */
 int pll1_running = (pll1_op & PLL_RUN) && (pll1_mode & PLL_OUTCTRL);
 if (!pll1_running) {
  uint32_t m0 = g_rd(GPU_GPUCC_BASE, CC_PLL0 + PLL_MODE), o0 = g_rd(GPU_GPUCC_BASE, CC_PLL0 + PLL_OPMODE);
  if (!((o0 & PLL_RUN) && (m0 & PLL_OUTCTRL))) g_pll_configure(CC_PLL0, 0x1e, 0xbaaa);
  g_pll_configure(CC_PLL1, 0x16, 0xeaaa);
  uint32_t v = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE, v | PLL_RESET_N);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_OPMODE, PLL_RUN);
  unsigned w; int locked = 0;
  for (w = 0; w < 1500; w++) { if (g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE) & PLL_LOCK_DET) { locked = 1; break; } delay(1); }
  r[67] = w;
  if (!locked) { r[65] = 3; return; }
  v = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_USER_CTL);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_USER_CTL, v | PLL_OUT_MASK);
  v = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE, v | PLL_OUTCTRL);
 }
 /* 4. hub_clk_src 200 MHz (GPLL0 sel5 div5)  5. gmu_clk_src 220 MHz (PLL1 sel3 div3) */
 if (g_rcg_set(CC_HUB_RCGR, 5, 5)) { r[65] = 4; return; }
 if (g_rcg_set(CC_GMU_RCGR, 3, 3)) { r[65] = 5; return; }
 /* 6. branches: CXO_AON, DEMET, AHB, CX_GMU, CXO, GCC axi, GCC memnoc, HUB_CX_INT; wait cx_gmu */
 g_set_bit0(GPU_GPUCC_BASE, CC_CXO_AON);
 g_set_bit0(GPU_GPUCC_BASE, CC_DEMET);
 g_set_bit0(GPU_GPUCC_BASE, CC_AHB_CLK);
 g_set_bit0(GPU_GPUCC_BASE, CC_CX_GMU_CLK);
 g_set_bit0(GPU_GPUCC_BASE, CC_CXO_CLK);
 g_set_bit0(GPU_GCC_GPU_PAGE, GCC_DDRSS_GPU_AXI);
 g_set_bit0(GPU_GCC_GPU_PAGE, GCC_GPU_MEMNOC_GFX);
 g_set_bit0(GPU_GPUCC_BASE, CC_HUB_CX_INT);
 { unsigned w; uint32_t cg = 0; int on = 0;
   for (w = 0; w < 500; w++) { cg = g_rd(GPU_GPUCC_BASE, CC_CX_GMU_CLK); if (g_clk_on(cg)) { on = 1; break; } delay(1); }
   r[68] = cg; r[69] = w; if (!on) { r[65] = 6; return; } }
 r[70] = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
 r[71] = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_OPMODE);
 r[72] = g_rd(GPU_GPUCC_BASE, CC_GMU_RCGR + 4) & 0x7ff;
 r[73] = g_rd(GPU_GPUCC_BASE, CC_HUB_RCGR + 4) & 0x7ff;
 /* GMU core now readable: cold-start preconditions (SYSRESET==1, TCM_RET==0, RSC idle, HFI not ready). */
 r[74] = g_rd(GMU_DWORD(GMUD_CM3_SYSRESET), 0);
 r[75] = g_rd(GPU_CXMISC_BASE, CXMISC_TCM_RET_CNTL * 4);
 r[76] = g_rd(GPU_RSCC_BASE, RSCC_SEQ_BUSY_DRV0 * 4);
 r[77] = g_rd(GMU_DWORD(GMUD_HFI_CTRL_STATUS), 0);
 r[65] = 7;   /* all six steps passed */
 if (limit < 7) return;

 /* ---- Stage 7: cmd-db read + parse (read-only DRAM; needed for GMU votes). ---- */
 g_enter(7);
 for (uint64_t i = 0; i < GPU_CMDDB_BYTES / 4; i++)
  { uint32_t w = g_rd(GPU_CMDDB_BASE, (uint32_t)(i * 4)); gpu_cmddb[i*4]=w; gpu_cmddb[i*4+1]=w>>8; gpu_cmddb[i*4+2]=w>>16; gpu_cmddb[i*4+3]=w>>24; }
 r[80] = cdb32(gpu_cmddb);        /* version */
 r[81] = cdb32(gpu_cmddb + 4);    /* magic (expect 0x0c0330db) */
 /* slave table: 16-byte rows from +8; resolve named resources' addresses. */
 static const char names[8][8] = {
  {'g','f','x','.','l','v','l',0},{'m','x','.','l','v','l',0,0},{'c','x','.','l','v','l',0,0},
  {'g','m','x','c','.','l','v','l'},{'S','H','0',0,0,0,0,0},{'M','C','0',0,0,0,0,0},
  {'A','C','V',0,0,0,0,0},{'C','N','0',0,0,0,0,0}};
 unsigned rows = 0; size_t heads[8] = {0}, sizes[8] = {0};
 for (unsigned i = 0; i < 8; i++) {
  const uint8_t *row = gpu_cmddb + 8 + 16 * i; unsigned id = cdb16(row), cnt = cdb16(row + 6);
  if (!id) break;
  heads[i] = 144u + cdb16(row + 2); sizes[i] = (size_t)cnt * 24; rows++;
 }
 r[82] = rows;
 for (unsigned i = 0; i < rows; i++)
  for (size_t at = heads[i]; at + 24 <= heads[i] + sizes[i] && at + 24 <= GPU_CMDDB_BYTES; at += 24) {
   const uint8_t *e = gpu_cmddb + at;
   for (unsigned nm = 0; nm < 8; nm++) {
    int eq = 1; for (unsigned c = 0; c < 8; c++) if (e[c] != (uint8_t)names[nm][c]) { eq = 0; break; }
    if (eq) r[83 + nm] = cdb32(e + 16);   /* resource address */
   }
  }
 if (limit >= 8 && r[65] == 7 && r[80] == 1 && r[81] == 0x0c0330db && r[82]) {
  g_enter(limit == G_STAGE_8A ? G_STAGE_8A : 8);
  gpu_m2(limit);
 }
 r[2] = gpu_stage; r[3] = gpu_gmu_started;
 /* r[83..90] = gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV, CN0 addresses (0 = absent). */
}
