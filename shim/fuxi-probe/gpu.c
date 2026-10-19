/* Fuxi A740 GPU power / clock staged probe (lane-gpu, milestone 1).
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
#define GMUD_SPTPRAC_STATUS 0x1f8d0   /* GMU_SPTPRAC_PWR_CLK_STATUS (A7XX): GX off = bits0,1 clear */
#define RSCC_SEQ_BUSY_DRV0  0x101     /* rscc dword */
#define CXMISC_TCM_RET_CNTL 0x39      /* cx_misc dword */
#define GPU_RBBM_STATUS     0x210     /* gpu reg dword */

/* ---- report layout (128 u32 = 512 bytes, the control-IN cap) ---- */
#define GPU_REPORT_WORDS 128
static uint32_t gpu_report[GPU_REPORT_WORDS];
static unsigned gpu_stage;   /* furthest stage entered, for a post-mortem photo */

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

/* The staged sequence. `limit` (host st[4], 1..7) is the last stage allowed;
 * the report records the actual outcome of each stage it reached. */
static uint8_t gpu_cmddb[GPU_CMDDB_BYTES] __attribute__((aligned(8)));
static void gpu_probe(unsigned limit)
{
 uint32_t *r = gpu_report;
 for (int i = 0; i < GPU_REPORT_WORDS; i++) r[i] = 0;
 if (!limit) return;
 if (limit > 7) limit = 7;
 r[0] = 0xa740;          /* magic */
 r[1] = limit;

 /* ---- Stage 1: read-only snapshot (always safe: same GCC family the UFS /
  * touch probes already read; GPUCC/SMMU/GPU reads are each gated). ---- */
 gpu_stage = 1;
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
 gpu_stage = 2;
 if (!(cfg_ahb & CBCR_ENABLE)) { g_wr(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB, cfg_ahb | CBCR_ENABLE); r[26] = 1; }
 cfg_ahb = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB);
 r[27] = cfg_ahb;
 if (!(cfg_ahb & CBCR_ENABLE)) return;   /* nothing beyond is readable */
 if (limit < 3) return;

 /* ---- Stage 3: CX GDSC + GPUCC AHB + HLOS SMMU vote (gdsc_enable mirror). ---- */
 gpu_stage = 3;
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
 gpu_stage = 4;
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
 if (limit < 5) return;

 /* ---- Stage 5: GMU clock snapshot (read-only; a6xx_gmu_resume expects XO). ---- */
 gpu_stage = 5;
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
 gpu_stage = 6;
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
 gpu_stage = 7;
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
 /* r[83..90] = gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV, CN0 addresses (0 = absent). */
}
