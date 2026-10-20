/*
 * FUXI WLAN (lane-wlan milestones 4/5/6): WCN7850 power-up, PCIe0 link, BAR0
 * assignment and a read-only MHI/BHI register dump from XNU.
 * Gated by boot-arg fuxiwlan (default 0 = do nothing):
 *   fuxiwlan=1      run all 28 steps
 *   fuxiwlan=2..28  stop after that step (bisecting a hang or reset)
 * Steps 17..28 probe DMA safety and load rootfs firmware through BHI/BHIe.
 * No interrupts, channels or QMI are enabled; see docs-lanes/wlan-bt.md M6.
 *
 * Steps 1..13 are the loader's milestone-3 sequence, compiled from an exact
 * copy of shim/fuxi-probe/wlan.c (bsd/dev/wlan.c, FUXI_WLAN_XNU). Every write
 * there is documented against Linux and the live DT in that file and in
 * docs-lanes/wlan-bt.md section 7; it passed on the phone (Gen2 x2 link,
 * endpoint 17cb:1107, BAR0 64-bit non-prefetchable 2 MiB).
 * This file supplies the loader helpers (usb_rd/ts_wr/ts_gpio/rh_send and
 * the fault-contained accessors) over kernel mappings, then adds:
 *  14  outbound iATU region 1, type MEM, for the live-DT ranges entry
 *      <0x02000000 0 0x60300000 0 0x60300000 0 0x3d00000>
 *      (dw_pcie_prog_outbound_atu; unrolled region n at ATU base + n*0x200);
 *      root-port memory window 0x60400000-0x605fffff, empty prefetchable
 *      window, command IO|MEM|MASTER|SERR (dw_pcie_setup_rc / pci bridge
 *      window setup).
 *  15  endpoint BAR0 = 0x60400000 (2 MiB aligned, inside the window),
 *      BAR1 (upper half) = 0, command |= MEMORY only (no bus master).
 *  16  read MHI registers at BAR0 (drivers/bus/mhi/common.h: MHIREGLEN 0x00,
 *      MHIVER 0x08, MHICFG 0x10, CHDBOFF 0x18, ERDBOFF 0x20, BHIOFF 0x28,
 *      BHIEOFF 0x2c, DEBUGOFF 0x30, MHICTRL 0x38, MHISTATUS 0x48) and the
 *      BHI block at BHIOFF (BHIVERSION 0x00/0x04, EXECENV 0x28, STATUS 0x2c,
 *      ERRCODE 0x30, ERRDBG1..3 0x34..0x3c). BHI_SERIALNU is never read.
 *      ath12k (mhi.c) points mhi_ctrl->regs at BAR0, so the MHI block is at
 *      offset 0.
 * All device accesses go through fuxi_gpu_access (synchronous aborts are
 * recovered); after a fault normal writes are refused. M6 cleanup permits
 * containment and undo of only our proven-owned SMMU/RC slots.
 */
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>

extern ppnum_t pmap_find_phys(pmap_t, addr64_t);

extern kern_return_t thread_terminate(thread_t);
extern kern_return_t pmap_enter_options_addr(pmap_t, vm_map_offset_t,
    uint64_t, vm_prot_t, vm_prot_t, unsigned, boolean_t, unsigned, void *);
/* Fault-contained aligned access, osfmk/arm64/machine_routines_asm.s. */
extern unsigned fuxi_gpu_access(uintptr_t, uint64_t *, unsigned, unsigned);
void fuxi_wlan_init(void);

#define rsr(r) ({ uint64_t rsr_v_; __asm__ volatile("mrs %0, " r : "=r"(rsr_v_)); rsr_v_; })
#define delay fw_delay
static void fw_delay(unsigned us) { delay_for_interval(us, NSEC_PER_USEC); }

/* ---- kernel mappings (16 KiB aligned; same scheme as fuxi_adreno.c:
 * kernel_map VA only, fallible page-table expansion, never the 8 MiB
 * io_map submap). Kept for the boot; M5 partial/failure paths assert PERST. */
struct fw_window { uint64_t pa, size; uintptr_t va; };
static struct fw_window fw_windows[] = {
	{0x00100000, 0x80000, 0},   /* GCC: votes, GDSCs, PCIe0 RCGs/CBCRs/BCRs */
	{0x00898000, 0x4000, 0},    /* QUP2 SE6 (BT UART), gated reads only */
	{0x008c0000, 0x4000, 0},    /* QUP2 wrapper */
	{0x01c00000, 0x8000, 0},    /* PCIe0 PARF 0x1c00000 + QMP PHY 0x1c06000 */
	{0x01fd4000, 0x4000, 0},    /* TCSR clkref 0x1fd5100 */
	{0x15000000, 0x100000, 0},  /* apps SMMU: GR0/GR1 + runtime-bounded CBs */
	{0x60480000, 0x4000, 0},    /* EP internal register window (TCSR version) */
	{0x0f14c000, 0x18000, 0},   /* TLMM tiles gpio76..96 */
	{0x17a20000, 0x4000, 0},    /* apps RSC DRV2 + TCS */
	{0x60000000, 0x4000, 0},    /* PCIe0 DBI, ELBI 0xf20, iATU 0x1000 */
	{0x60100000, 0x4000, 0},    /* config window (iATU region 0, CFG0) */
	{0x60400000, 0x4000, 0},    /* endpoint BAR0, first 16 KiB (MHI/BHI) */
	{0x81c60000, 0x20000, 0},   /* cmd-db (reserved DDR) */
};
#define FW_NWIN (sizeof(fw_windows) / sizeof(fw_windows[0]))
static uint64_t fw_mapped_bytes;

static kern_return_t
fw_map_window(struct fw_window *w)
{
	vm_offset_t va = 0;
	if (!w->size || (w->pa & PAGE_MASK) || (w->size & PAGE_MASK) || w->va) {
		return KERN_INVALID_ARGUMENT;
	}
	kern_return_t kr = kmem_alloc(kernel_map, &va, w->size,
	    KMA_VAONLY | KMA_KOBJECT | KMA_NOPAGEWAIT, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS) {
		return kr;
	}
	for (vm_size_t off = 0; off < w->size; off += PAGE_SIZE) {
		kr = pmap_enter_options_addr(kernel_pmap, va + off, w->pa + off,
		    VM_PROT_READ | VM_PROT_WRITE, VM_PROT_NONE, VM_WIMG_IO, TRUE,
		    PMAP_OPTIONS_NOWAIT, NULL);
		if (kr != KERN_SUCCESS) {
			pmap_remove(kernel_pmap, va, va + w->size);
			kmem_free(kernel_map, va, w->size);
			return kr;
		}
	}
	w->va = va;
	fw_mapped_bytes += w->size;
	return KERN_SUCCESS;
}

/* ---- fault-contained 32-bit access by physical address ---- */
static unsigned fw_faults, fw_unmapped, fw_refused, fw_logged;
static uint32_t fw_fault_esr;
static uint64_t fw_fault_far;

static uintptr_t
fw_va(uint64_t pa)
{
	for (unsigned i = 0; i < FW_NWIN; i++) {
		struct fw_window *w = &fw_windows[i];
		if (w->va && pa >= w->pa && pa - w->pa <= w->size - 4) {
			return w->va + (uintptr_t)(pa - w->pa);
		}
	}
	return 0;
}

static unsigned
fw_access(uint64_t pa, uint64_t *v, unsigned wr)
{
	uintptr_t va = fw_va(pa);
	if (!va || (pa & 3)) {
		if (fw_unmapped++ < 4) {
			printf("FUXI WLAN: no window for PA %#llx\n", pa);
		}
		fw_faults++;
		return 1;
	}
	if (wr && fw_faults) {
		if (fw_refused++ < 4) {
			printf("FUXI WLAN: write PA %#llx refused after a fault\n", pa);
		}
		return 1;
	}
	unsigned bad = fuxi_gpu_access(va, v, wr, 0);
	if (bad) {
		fw_fault_esr = (uint32_t)rsr("esr_el1");
		fw_fault_far = pa;
		fw_faults++;
		if (fw_logged++ < 8) {
			printf("FUXI WLAN: access fault PA %#llx %s ESR %#x\n", pa,
			    wr ? "write" : "read", fw_fault_esr);
		}
	}
	return bad;
}

/* Loader-helper equivalents for bsd/dev/wlan.c. */
#define probe_fault_esr fw_fault_esr
#define probe_fault_far fw_fault_far
#define TS_TLMM 0x0f100000UL
#define RH_DRV_PA 0x17a20000UL
#define RH_TCS_PA 0x17a20d00UL

static uint32_t
usb_rd(uintptr_t base, uint32_t off)
{
	uint64_t v = 0;
	if (fw_access((uint64_t)base + off, &v, 0)) {
		return 0xffffffffu;
	}
	return (uint32_t)v;
}

static void
ts_wr(uintptr_t base, uint32_t off, uint32_t value)
{
	uint64_t v = value;
	__asm__ volatile("dmb oshst" ::: "memory");
	(void)fw_access((uint64_t)base + off, &v, 1);
}

static uint32_t
fw_safe_rd32(uint64_t pa, uint32_t *faulted)
{
	uint64_t v = 0;
	*faulted = fw_access(pa, &v, 0);
	return (uint32_t)v;
}

static unsigned
fw_safe_wr32(uint64_t pa, uint32_t value)
{
	uint64_t v = value;
	return fw_access(pa, &v, 1);
}
#define probe_safe_rd32 fw_safe_rd32
#define probe_safe_wr32 fw_safe_wr32

/* TLMM CTL: pull [1:0], func [5:2], drive [8:6] = mA/2-1, OE [9]; as probe.c. */
static void
ts_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	uintptr_t c = TS_TLMM + 0x1000u * n;
	if (out >= 0) {
		ts_wr(c, 4, out ? 2u : 0u);
	}
	ts_wr(c, 0, (usb_rd(c, 0) & ~0x3ffu) | pull | (func << 2) | ((ma / 2 - 1) << 6) |
	    (out >= 0 ? 0x200u : 0u));
}

/* One write-with-response command in ACTIVE TCS t (RSC v3 layout,
 * rpmh_rsc_reg_offset_ver_3_0); identical to probe.c rh_send. No other XNU
 * code drives the fuxi apps RSC. 0 on proven completion; on timeout the TCS
 * is left quarantined and wlan.c stops at that step. */
#define RH_STRIDE 0x2a0u
#define RH_WAIT   0x20u
#define RH_CTRL   0x24u
#define RH_IDLE   0x28u
#define RH_EN     0x2cu
#define RH_MSGID  0x34u
#define RH_ADDR   0x38u
#define RH_DATA   0x3cu
#define RH_CSTAT  0x40u
static uint32_t rh_rd(uint32_t off) { return usb_rd(RH_TCS_PA, off); }
static void
rh_wr(uint32_t off, uint32_t v)
{
	ts_wr(RH_TCS_PA, off, v);
	__asm__ volatile("dsb osh" ::: "memory");
}
static uint32_t
rh_send(unsigned t, uint32_t addr, uint32_t data)
{
	uint32_t b = t * RH_STRIDE, bit = 1u << t;
	if (rh_rd(b + RH_IDLE) == 0 || rh_rd(b + RH_CTRL) || rh_rd(b + RH_EN) ||
	    rh_rd(b + RH_WAIT) || (rh_rd(4) & bit) || fw_faults) {
		return 1;
	}
	uint32_t irq_en = rh_rd(0);
	rh_wr(0, irq_en & ~bit);
	rh_wr(b + RH_MSGID, 8u | (1u << 8) | (1u << 16));
	rh_wr(b + RH_ADDR, addr);
	rh_wr(b + RH_DATA, data);
	rh_wr(b + RH_WAIT, 1);
	rh_wr(b + RH_EN, 1);
	rh_wr(b + RH_CTRL, 0);
	rh_wr(b + RH_CTRL, 1u << 16);
	rh_wr(b + RH_CTRL, (1u << 16) | (1u << 24));
	uint32_t st = 0;
	for (unsigned us = 0; us < 100000; us++) {
		st = rh_rd(b + RH_CSTAT);
		if (rh_rd(b + RH_IDLE) && (st & 0x10100) == 0x10100) {
			break;
		}
		fw_delay(1);
	}
	if ((st & 0x10100) != 0x10100) {
		return 2 | (st << 12);
	}
	rh_wr(b + RH_CTRL, 1u << 16);
	rh_wr(b + RH_CTRL, 0);
	rh_wr(b + RH_EN, 0);
	rh_wr(b + RH_WAIT, 0);
	rh_wr(8, bit);
	rh_wr(0, (rh_rd(0) & ~bit) | (irq_en & bit));
	return 0;
}

#define FUXI_WLAN_XNU 1
#include "wlan.c"

/* ---- milestone 4 ---- */
#define WL4_MAGIC   0x574c0004u
#define WL4_STEPS   16u
#define WL_ATU1     0x60001200ull  /* unrolled outbound region 1 */
#define WL_MEM_LO   0x60300000u    /* live DT ranges, 32-bit non-prefetchable */
#define WL_MEM_HI   0x63ffffffu
#define WL_BAR0     0x60400000u
#define WL_BAR0_END 0x605fffffu
#define WL_EPCFG    0x60100000ull
static uint32_t wl4_report[64];

static const char *
wl_mhi_state(uint32_t s)
{
	static const char *n[8] = {"RESET", "READY", "M0", "M1", "M2", "M3", "M3_FAST", "BHI"};
	return s < 8 ? n[s] : (s == 0xff ? "SYS_ERR" : "?");
}

static const char *
wl_mhi_ee(uint32_t e)
{
	static const char *n[8] = {"PBL", "SBL", "AMSS", "RDDM", "WFW", "PTHRU", "EDL", "FP"};
	return e < 8 ? n[e] : "?";
}

/* 0 on success or a nonzero reason. */
static int
wl4_step(unsigned step)
{
	uint32_t *r = wl4_report, *m3 = wl3_report;
	switch (step) {
	case 14: { /* MEM outbound + root-port window */
		wl_pwr(WL_ATU1 + 0x08, WL_MEM_LO); wl_pwr(WL_ATU1 + 0x0c, 0);
		wl_pwr(WL_ATU1 + 0x10, WL_MEM_HI); wl_pwr(WL_ATU1 + 0x20, 0);
		wl_pwr(WL_ATU1 + 0x14, WL_MEM_LO); wl_pwr(WL_ATU1 + 0x18, 0);
		wl_pwr(WL_ATU1 + 0x00, 0);         /* type MEM */
		wl_pwr(WL_ATU1 + 0x04, 1u << 31);  /* enable */
		unsigned i;
		for (i = 0; i < 5; i++) {
			if (wl_prd(WL_ATU1 + 0x04) & (1u << 31)) {
				break;
			}
			delay(9000);
		}
		r[4] = wl_prd(WL_ATU1 + 0x04);
		r[5] = wl_prd(WL_ATU1 + 0x08);
		r[6] = wl_prd(WL_ATU1 + 0x10);
		r[7] = wl_prd(WL_ATU1 + 0x14);
		/* type-1 header: memory base/limit [15:4]/[31:20] = addr[31:20] */
		wl_pwr(WL_DBI + 0x20, ((WL_BAR0_END >> 16) & 0xfff0u) << 16 | ((WL_BAR0 >> 16) & 0xfff0u));
		wl_pwr(WL_DBI + 0x24, 0x0000fff0u); /* prefetchable window empty */
		wl_pwr(WL_DBI + 0x28, 0);
		wl_pwr(WL_DBI + 0x2c, 0);
		wl_pwr(WL_DBI + 0x04, (wl_prd(WL_DBI + 0x04) & 0xffffu) | 0x107u);
		r[8] = wl_prd(WL_DBI + 0x20);
		r[9] = wl_prd(WL_DBI + 0x24);
		r[10] = wl_prd(WL_DBI + 0x04);
		printf("FUXI WLAN: iATU1 MEM %#x-%#x -> %#x ctrl2 %#x; RC window %#x pref %#x cmd/status %#x\n",
		    r[5], r[6], r[7], r[4], r[8], r[9], r[10]);
		if (wl3_faults) {
			return 1;
		}
		if (!(r[4] & (1u << 31)) || r[5] != WL_MEM_LO || r[6] != WL_MEM_HI) {
			return 2;
		}
		return (r[10] & 6u) == 6u ? 0 : 3;
	}
	case 15: { /* BAR0 assignment */
		uint32_t rb = m3[87], size = ~(rb & ~0xfu) + 1u;
		r[11] = rb;
		r[12] = size;
		/* step 13 must have found a 64-bit non-prefetchable memory BAR0 of
		 * at most 2 MiB with an all-ones upper half (sizes below 4 GiB). */
		if ((rb & 0xfu) != 0x4u || !size || size > 0x200000u || (size & (size - 1)) ||
		    m3[88] != 0xffffffffu) {
			printf("FUXI WLAN: BAR0 readback %#x/%#x unexpected; not assigning\n", rb, m3[88]);
			return 1;
		}
		uint32_t cmd = wl_prd(WL_EPCFG + 0x04);
		r[13] = cmd;
		wl_pwr(WL_EPCFG + 0x04, cmd & 0xffffu & ~7u);   /* decode off while moving */
		wl_pwr(WL_EPCFG + 0x10, WL_BAR0);
		wl_pwr(WL_EPCFG + 0x14, 0);
		r[14] = wl_prd(WL_EPCFG + 0x10);
		r[15] = wl_prd(WL_EPCFG + 0x14);
		wl_pwr(WL_EPCFG + 0x04, (cmd & 0xffffu & ~7u) | 2u); /* MEMORY only */
		r[16] = wl_prd(WL_EPCFG + 0x04);
		printf("FUXI WLAN: BAR0 size %#x assigned %#llx (readback %#x %#x) cmd/status %#x -> %#x\n",
		    size, (uint64_t)r[15] << 32 | (r[14] & ~0xfu), r[14], r[15], cmd, r[16]);
		if (wl3_faults) {
			return 2;
		}
		if ((r[14] & ~0xfu) != WL_BAR0 || r[15] != 0) {
			return 3;
		}
		return (r[16] & 7u) == 2u ? 0 : 4;
	}
	case 16: { /* MHI + BHI, read-only */
		static const uint16_t mo[10] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x2c, 0x30, 0x38, 0x48};
		static const uint16_t bo[8] = {0x00, 0x04, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c};
		for (unsigned i = 0; i < 10; i++) {
			r[20 + i] = wl_prd((uint64_t)WL_BAR0 + mo[i]);
		}
		if (wl3_faults) {
			return 1;
		}
		uint32_t cfg = r[22], st = r[29], ctrl = r[28], bhi = r[25];
		printf("FUXI WLAN: MHI REGLEN %#x MHIVER %#x MHICFG %#x (nhwer %u ner %u nhwch %u nch %u)\n",
		    r[20], r[21], cfg, cfg >> 24, (cfg >> 16) & 0xffu, (cfg >> 8) & 0xffu, cfg & 0xffu);
		printf("FUXI WLAN: MHI CHDBOFF %#x ERDBOFF %#x BHIOFF %#x BHIEOFF %#x DEBUGOFF %#x\n",
		    r[23], r[24], r[25], r[26], r[27]);
		printf("FUXI WLAN: MHICTRL %#x (state %u, reset %u) MHISTATUS %#x (state %s, ready %u, syserr %u)\n",
		    ctrl, (ctrl >> 8) & 0xffu, (ctrl >> 1) & 1u, st, wl_mhi_state((st >> 8) & 0xffu),
		    st & 1u, (st >> 2) & 1u);
		if (r[20] == 0xffffffffu && r[21] == 0xffffffffu) {
			return 2;               /* BAR not decoded / link down */
		}
		if (bhi < 0x40 || bhi > 0x4000u - 0x40u || (bhi & 3)) {
			printf("FUXI WLAN: BHIOFF %#x outside the mapped 16 KiB; BHI not read\n", bhi);
			return 3;
		}
		for (unsigned i = 0; i < 8; i++) {
			r[30 + i] = wl_prd((uint64_t)WL_BAR0 + bhi + bo[i]);
		}
		printf("FUXI WLAN: BHI version %u.%u EXECENV %#x (%s) STATUS %#x (%u) ERRCODE %#x ERRDBG %#x %#x %#x\n",
		    r[31], r[30], r[32], wl_mhi_ee(r[32]), r[33], r[33] >> 30, r[34], r[35], r[36], r[37]);
		/* Root-port status after BAR traffic (received master/target abort). */
		r[38] = wl_prd(WL_DBI + 0x04);
		r[39] = wl_prd(WL_EPCFG + 0x04);
		printf("FUXI WLAN: RC cmd/status %#x EP cmd/status %#x\n", r[38], r[39]);
		return wl3_faults ? 4 : 0;
	}
	}
	return 0xff;
}

/* ---- milestone 5: polling only, no channel start or QMI ----
 * Register/layout references (independent implementation): Linux MHI common.h,
 * host/boot.c + pm.c, arm-smmu.h; OpenBSD if_qwz_pci.c READY/FBC sequence.
 * M6 uses owned stage-1 translation of low IOVAs to wired XNU pages.
 */
#define WL5_STEPS 28u
#define WL_SMMU 0x15000000ull
#define WL_CHUNK 0x80000u
#define WL_FW_MAX 0xf00000u
#define WL_SEQ_MASK 0x3fffffffu
static uint32_t wl5_report[128];
static uint32_t wl_bhi, wl_bhie, wl_fw_size, wl_seq;
static const char *wl_fw_path;
static bool wl_mhi_valid;
struct wl_dma { vm_offset_t va; vm_size_t size; uint64_t pa, iova; };
static struct wl_dma wl_image, wl_vectors, wl_contexts, wl_tables;
#define WL_IOVA 0x10000000ull
#define WL_IOVA_END 0x11000000ull
static uint64_t wl_iova_next = WL_IOVA, wl_cb_base, wl_gr1;
static unsigned wl_slot, wl_cb, wl_smr_count, wl_cb_count, wl_pgshift;
static uint32_t wl_old_smr, wl_old_s2cr, wl_old_sctlr;
static uint32_t wl_route_cfg, wl_route_entry;
static unsigned wl_route_hash;
static bool wl_owned, wl_enabled, wl_route_owned;
static int wl_contain_wr(uint64_t, uint32_t);
static int wl_smmu_fault(unsigned);
/* Packed MHI contexts are 44 bytes; the old research note's 40 was wrong. */
struct wl_context {
	uint32_t cfg, type, index;
	uint64_t base, len, rp, wp;
} __attribute__((packed));
_Static_assert(sizeof(struct wl_context) == 44, "MHI context stride");
struct wl_vector { uint64_t addr, size; };
_Static_assert(sizeof(struct wl_vector) == 16, "BHIe vector stride");
/* Within a single 16 KiB page: 128 disabled channels, two event contexts,
 * command context, 512-byte control ring, 4 KiB data ring, 2 KiB command ring.
 * Ring bases are aligned to their lengths. No transfer ring is created. */
#define WL_ECTX 0x1800u
#define WL_CCTX 0x1900u
#define WL_ER0  0x1a00u
#define WL_ER1  0x2000u
#define WL_CR   0x3000u
_Static_assert(128 * sizeof(struct wl_context) <= WL_ECTX, "channel array");
_Static_assert(WL_CR + 2048 <= 16384, "context page");

static void
wl_sleep(unsigned ms)
{
	/* Yield this dedicated thread; root mount and other drivers must run. */
	assert_wait_timeout((event_t)&wl5_report, THREAD_UNINT, ms, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Wired allocations need neither physical contiguity nor low DDR. */
static uint64_t
wl_phys(vm_offset_t va)
{
	return ptoa_64(pmap_find_phys(kernel_pmap, va)) + (va & PAGE_MASK);
}

static int
wl_dma_alloc(struct wl_dma *d, vm_size_t bytes)
{
	d->size = round_page(bytes);
	if (!bytes || d->size > WL_IOVA_END - wl_iova_next) { return 1; }
	kern_return_t kr = kmem_alloc(kernel_map, &d->va, d->size,
	    KMA_DATA | KMA_ZERO | KMA_NOPAGEWAIT, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS) {
		printf("FUXI WLAN: wired DMA allocation failed %d\n", kr); return 1;
	}
	for (vm_size_t off = 0; off < d->size; off += PAGE_SIZE) {
		uint64_t pa = wl_phys(d->va + off);
		if (!pa || pa >= (1ull << 36)) { return 2; }
	}
	d->pa = wl_phys(d->va);
	d->iova = wl_iova_next; wl_iova_next += d->size;
	return 0;
}

static void
wl_dma_clean(struct wl_dma *d)
{
	for (vm_size_t off = 0; off < d->size; off += PAGE_SIZE) {
		flush_dcache64(wl_phys(d->va + off), PAGE_SIZE, TRUE);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
wl_dma_free(struct wl_dma *d)
{
	if (d->va) {
		kmem_free(kernel_map, d->va, d->size);
		bzero(d, sizeof(*d));
	}
}

static uint32_t wl_mrd(uint32_t off) { return wl_prd((uint64_t)WL_BAR0 + off); }
static void wl_mwr(uint32_t off, uint32_t v) { wl_pwr((uint64_t)WL_BAR0 + off, v); }
static void
wl_mwr64(uint32_t off, uint64_t v)
{
	wl_mwr(off + 4, (uint32_t)(v >> 32));
	wl_mwr(off, (uint32_t)v);
}

static void
wl5_status(unsigned step)
{
	if (!wl_mhi_valid) {
		return;
	}
	uint32_t *r = wl5_report;
	r[64] = wl_mrd(wl_bhi + 0x28); r[65] = wl_mrd(0x48);
	r[66] = wl_mrd(0x38); r[67] = wl_mrd(wl_bhi + 0x2c);
	r[68] = wl_mrd(wl_bhi + 0x30);
	for (unsigned i = 0; i < 3; i++) { r[69 + i] = wl_mrd(wl_bhi + 0x34 + 4 * i); }
	r[72] = wl_mrd(wl_bhie + 0x44); r[73] = wl_prd(WL_EPCFG + 4);
	printf("FUXI WLAN: M6 step %u EE %#x (%s) MHISTATUS %#x (%s ready %u) MHICTRL %#x BHI %#x ERRCODE %#x ERRDBG %#x/%#x/%#x BHIE %#x EPcmd %#x\n",
	    step, r[64], wl_mhi_ee(r[64]), r[65], wl_mhi_state((r[65] >> 8) & 255),
	    r[65] & 1, r[66], r[67], r[68], r[69], r[70], r[71], r[72], r[73]);
}

/* DT: PCIe0 qcom,smmu-sid-base=0x1400; RID 0x100 -> SID 0x1401,
 * length 1; no iommu-map-mask override, no wildcard SMMU mask (mask=0).
 * Own only a vacant endpoint lookup entry. Never rewrite a collision chain,
 * SID_OFFSET, a populated entry, or an enabled route belonging to another RID. */
static int
wl_rc_route(void)
{
	wl_route_cfg = wl_prd(WL_PARF + 0x2c00);
	uint32_t offset = wl_prd(WL_PARF + 0x234);
	unsigned hash = 0;
	for (unsigned byte = 0; byte < 2; byte++) {
		hash ^= byte ? 0 : 1;
		for (unsigned bit = 0; bit < 8; bit++) {
			hash = ((hash << 1) ^ ((hash & 0x80) ? 7 : 0)) & 255;
		}
	}
	wl_route_hash = hash;
	bool route = false, empty = true;
	unsigned matches = 0;
	for (unsigned i = 0; i < 256; i++) {
		uint32_t v = wl_prd(WL_PARF + 0x2000 + 4 * i);
		if (v) { empty = false; }
		if ((v >> 16) == 0x100) { matches++; }
	}
	unsigned next = hash;
	for (unsigned i = 0; i < 256; i++) {
		uint32_t v = wl_prd(WL_PARF + 0x2000 + 4 * next);
		if ((v >> 16) == 0x100) { route = ((v >> 8) & 255) == 1; wl_route_hash = next; break; }
		if (!(v & 255)) { break; }
		next = v & 255;
	}
	wl_route_entry = wl_prd(WL_PARF + 0x2000 + 4 * hash);
	wl5_report[22] = wl_route_cfg; wl5_report[23] = offset; wl5_report[24] = wl_route_entry;
	printf("FUXI WLAN: RC RID 0x100 SID cfg %#x offset %#x hash %#x entry %#x matches %u\n",
	    wl_route_cfg, offset, hash, wl_route_entry, matches);
	if (fw_faults || offset || wl_route_cfg == ~0u || matches > 1) { return 1; }
	if (route && !(wl_route_cfg & 1) && matches == 1) { return 0; }
	/* Reset RC: only install when the entire table is empty. */
	if (!empty) { return 2; }
	wl_route_owned = true;
	wl_pwr(WL_PARF + 0x2000 + 4 * hash, 0x01000100);
	if (wl_prd(WL_PARF + 0x2000 + 4 * hash) != 0x01000100 || fw_faults) { return 3; }
	wl_pwr(WL_PARF + 0x2c00, wl_route_cfg & ~1u);
	if (wl_prd(WL_PARF + 0x2c00) != (wl_route_cfg & ~1u) || fw_faults) { return 3; }
	printf("FUXI WLAN: RC owned empty RID slot %#x -> DT SID 0x1401 (mask 0)\n", hash);
	return 0;
}

static uint64_t
wl_cb_addr(unsigned cb)
{
	return wl_cb_base + ((uint64_t)cb << wl_pgshift);
}

/* Strictly read-only selection. Reserve CBs referenced by ANY S2CR, even an
 * invalid SMR or non-translating S2CR: conservative with firmware leftovers. */
static int
wl_smmu_probe(void)
{
	uint32_t *r = wl5_report;
	r[4] = wl_prd(WL_SMMU); r[5] = wl_prd(WL_SMMU + 0x20);
	r[6] = wl_prd(WL_SMMU + 0x24); r[7] = wl_prd(WL_SMMU + 0x28);
	wl_smr_count = r[5] & 255; wl_cb_count = r[6] & 255;
	wl_pgshift = (r[6] & (1u << 31)) ? 16 : 12;
	unsigned pages = 1u << (((r[6] >> 28) & 7) + 1);
	wl_gr1 = WL_SMMU + (1u << wl_pgshift);
	wl_cb_base = WL_SMMU + ((uint64_t)pages << wl_pgshift);
	printf("FUXI WLAN: M6 apps_smmu sCR0 %#x IDR0 %#x IDR1 %#x IDR2 %#x NUMSMRG %u NUMCB %u CBbase %#llx\n",
	    r[4], r[5], r[6], r[7], wl_smr_count, wl_cb_count, wl_cb_base);
	/* No EXID, global reconfiguration or guesses about unsupported formats. */
	if (fw_faults || r[4] == ~0u || r[5] == ~0u || r[6] == ~0u || r[7] == ~0u ||
	    (r[4] & 9) || (r[5] & 0x48000000) != 0x48000000 ||
	    !(r[7] & 0x1000) || !(r[7] & 15) || (r[7] & 15) > 5 ||
	    !((r[7] >> 4) & 15) || ((r[7] >> 4) & 15) > 5 ||
	    !wl_smr_count || !wl_cb_count || wl_cb_count > 128 ||
	    ((r[6] >> 16) & 255) > wl_cb_count || ((r[7] >> 8) & 15) > 5 ||
	    wl_cb_base + ((uint64_t)wl_cb_count << wl_pgshift) > WL_SMMU + 0x100000) { return 1; }
	bool referenced[128] = {false};
	wl_slot = wl_smr_count; wl_cb = wl_cb_count;
	for (unsigned i = 0; i < wl_smr_count; i++) {
		uint32_t smr = wl_prd(WL_SMMU + 0x800 + 4 * i);
		uint32_t s2cr = wl_prd(WL_SMMU + 0xc00 + 4 * i);
		if (smr == ~0u || s2cr == ~0u || fw_faults) { return 1; }
		if ((s2cr & 255) < wl_cb_count) { referenced[s2cr & 255] = true; }
		if (smr & 0x80000000u) {
			if (((0x1401 ^ smr) & ~((smr >> 16) & 0x7fff) & 0xffff) == 0) {
				printf("FUXI WLAN: SMMU endpoint collision slot %u SMR %#x S2CR %#x\n", i, smr, s2cr); return 2;
			}
		} else if (wl_slot == wl_smr_count && ((s2cr >> 16) & 3) >= 1 && ((s2cr >> 16) & 3) <= 2) {
			wl_slot = i; wl_old_smr = smr; wl_old_s2cr = s2cr;
		}
	}
	unsigned s2only = (r[6] >> 16) & 255;
	for (unsigned i = 0; i < wl_cb_count; i++) {
		uint32_t ctl = wl_prd(wl_cb_addr(i));
		if (ctl == ~0u) { return 3; }
		if (i >= s2only && !referenced[i] && !(ctl & 1) && wl_cb == wl_cb_count) {
			wl_cb = i; wl_old_sctlr = ctl;
		}
	}
	if (fw_faults || wl_slot == wl_smr_count || wl_cb == wl_cb_count) { return 3; }
	/* CB index is also the unique ASID, as in the generic arm-smmu driver.
	 * Refuse firmware configurations reusing this ASID in any enabled bank. */
	for (unsigned i = 0; i < wl_cb_count; i++) {
		if (wl_prd(wl_cb_addr(i)) & 1) {
			if ((wl_prd(wl_cb_addr(i) + 0x24) >> 16) == wl_cb ||
			    (wl_prd(wl_cb_addr(i) + 0x2c) >> 16) == wl_cb) { return 3; }
		}
	}
	if (fw_faults || (wl_prd(WL_EPCFG + 4) & 4)) { return 4; }
	printf("FUXI WLAN: SMMU free SMR %u old %#x/%#x CB %u SCTLR %#x unreferenced; SID 0x1401 mask 0\n",
	    wl_slot, wl_old_smr, wl_old_s2cr, wl_cb, wl_old_sctlr);
	wl5_report[8] = wl_slot; wl5_report[9] = wl_cb;
	return wl_rc_route() ? 5 : 0;
}

static int
wl_smmu_wr(uint64_t pa, uint32_t v)
{
	wl_pwr(pa, v);
	return fw_faults || wl_prd(pa) != v;
}

static int
wl_smmu_sync(bool cleanup)
{
	uint64_t cb = wl_cb_addr(wl_cb);
	if (cleanup) {
		if (wl_contain_wr(cb + 0x610, wl_cb) || wl_contain_wr(cb + 0x7f0, 0)) { return 1; }
	} else {
		wl_pwr(cb + 0x610, wl_cb); wl_pwr(cb + 0x7f0, 0);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (unsigned i = 0; i <= 100; i++) {
		uint32_t v = wl_prd(cb + 0x7f4);
		if (v == ~0u || (!cleanup && fw_faults)) { return 1; }
		if (!(v & 1)) { return 0; }
		if (i < 100) { wl_sleep(1); }
	}
	printf("FUXI WLAN: SMMU CB %u TLB sync timeout 100 ms\n", wl_cb);
	return 1;
}

/* 4-KiB AArch64 stage-1 tables: L1 root, L2, eight L3 tables for the 16-MiB
 * window. Tables themselves are never mapped into endpoint address space. */
static uint64_t
wl_pt_pa(unsigned table)
{
	return wl_phys(wl_tables.va + table * 4096);
}

static int
wl_smmu_enable(void)
{
	if (wl_dma_alloc(&wl_tables, 10 * 4096)) { return 1; }
	uint64_t *root = (uint64_t *)wl_tables.va;
	uint64_t *l2 = (uint64_t *)(wl_tables.va + 4096);
	root[0] = wl_pt_pa(1) | 3;
	for (unsigned i = 0; i < 8; i++) { l2[128 + i] = wl_pt_pa(2 + i) | 3; }
	struct wl_dma *bufs[] = { &wl_image, &wl_vectors, &wl_contexts };
	for (unsigned i = 0; i < 3; i++) {
		struct wl_dma *d = bufs[i];
		for (vm_size_t off = 0; off < d->size; off += 4096) {
			uint64_t addr = d->iova + off;
			if (addr < WL_IOVA || addr >= WL_IOVA_END) { return 2; }
			unsigned n = (unsigned)((addr - WL_IOVA) >> 21);
			uint64_t *l3 = (uint64_t *)(wl_tables.va + (2 + n) * 4096);
			/* RW unprivileged, AF, nG, NS, inner-shareable WB, XN. */
			l3[(addr >> 12) & 511] = wl_phys(d->va + off) | 0x60000000000f63ull;
		}
	}
	wl_dma_clean(&wl_tables);
	/* Re-prove vacancy after allocations/VFS sleeps, immediately before writes. */
	if (wl_prd(WL_SMMU + 0x800 + 4 * wl_slot) != wl_old_smr ||
	    wl_prd(WL_SMMU + 0xc00 + 4 * wl_slot) != wl_old_s2cr ||
	    wl_prd(wl_cb_addr(wl_cb)) != wl_old_sctlr) { return 3; }
	for (unsigned i = 0; i < wl_smr_count; i++) {
		uint32_t smr = wl_prd(WL_SMMU + 0x800 + 4 * i), s2cr = wl_prd(WL_SMMU + 0xc00 + 4 * i);
		if ((s2cr & 255) == wl_cb || ((smr & 0x80000000u) &&
		    (((smr ^ 0x1401) & ~((smr >> 16) & 0x7fff) & 0xffff) == 0))) { return 3; }
	}
	if (fw_faults || (wl_prd(WL_EPCFG + 4) & 4)) { return 3; }
	for (unsigned i = 0; i < wl_cb_count; i++) {
		if (wl_prd(wl_cb_addr(i)) & 1) {
			if ((wl_prd(wl_cb_addr(i) + 0x24) >> 16) == wl_cb ||
			    (wl_prd(wl_cb_addr(i) + 0x2c) >> 16) == wl_cb) { return 3; }
		}
	}
	if (fw_faults || wl_prd(WL_SMMU) != wl5_report[4]) { return 3; }
	uint64_t cb = wl_cb_addr(wl_cb), ttbr = wl_pt_pa(0) | ((uint64_t)wl_cb << 48);
	wl_owned = true; /* includes partial setup: abort must disable and sync */
	if (wl_smmu_wr(cb, 0) ||
	    wl_smmu_wr(wl_gr1 + 0x800 + 4 * wl_cb, 1) || /* VA64 */
	    wl_smmu_wr(wl_gr1 + 4 * wl_cb, 0x1f300) || /* S1, S2 bypass */
	    wl_smmu_wr(cb + 0x10, 0x38011) || /* SEP upstream, AS, 36-bit PA */
	    wl_smmu_wr(cb + 0x30, 0x803520) || /* EPD1, WB/ISH, 32-bit IOVA */
	    wl_smmu_wr(cb + 0x24, (uint32_t)(ttbr >> 32)) ||
	    wl_smmu_wr(cb + 0x20, (uint32_t)ttbr) ||
	    wl_smmu_wr(cb + 0x2c, wl_cb << 16) || wl_smmu_wr(cb + 0x28, 0) ||
	    wl_smmu_wr(cb + 0x38, 0xff) || wl_smmu_wr(cb + 0x3c, 0)) { return 4; }
	/* FSR is W1C only for our newly owned bank; preserve later DMA faults. */
	wl_pwr(cb + 0x58, 0xc00001fe);
	if (wl_smmu_fault(20) || wl_smmu_sync(false) ||
	    wl_smmu_wr(cb, 0x1027) || /* ASIDPNE, CFRE, AFE, TRE, M; IRQ/stall off */
	    wl_smmu_wr(WL_SMMU + 0xc00 + 4 * wl_slot, wl_cb) ||
	    wl_smmu_wr(WL_SMMU + 0x800 + 4 * wl_slot, 0x80001401)) { return 5; }
	wl_enabled = true;
	printf("FUXI WLAN: SMMU owned stage1 SMR %u CB %u TTBR %#llx IOVA %#llx-%#llx readback verified\n",
	    wl_slot, wl_cb, wl_pt_pa(0), WL_IOVA, wl_contexts.iova + wl_contexts.size - 1);
	printf("FUXI WLAN: DMA self-test: no standalone BHI host-write cookie; step 21/22 BHI transfer + CB faults is first endpoint proof\n");
	return 0;
}

static int
wl_smmu_fault(unsigned step)
{
	if (!wl_owned) { return 0; }
	uint64_t cb = wl_cb_addr(wl_cb);
	uint32_t fsr = wl_prd(cb + 0x58), lo = wl_prd(cb + 0x60), hi = wl_prd(cb + 0x64);
	uint32_t syn = wl_prd(cb + 0x68), syn1 = wl_prd(cb + 0x6c);
	uint32_t global = wl_prd(WL_SMMU + 0x48), sid = wl_prd(WL_SMMU + 0x54);
	wl5_report[80] = fsr; wl5_report[81] = lo; wl5_report[82] = hi;
	wl5_report[83] = syn; wl5_report[84] = syn1;
	wl5_report[85] = global; wl5_report[86] = sid;
	if (step >= 17 && step <= 28) { wl5_report[96 + step - 17] = fsr; }
	printf("FUXI WLAN: M6 step %u CB %u FSR %#x FAR %#x:%08x FSYNR %#x/%#x GFSR %#x GFSYNR1 %#x\n", step, wl_cb, fsr, hi, lo, syn, syn1, global, sid);
	return fw_faults || !!(fsr & 0xc00001fe) || ((global & 2) && (sid & 0xffff) == 0x1401);
}

/* Fault-contained cleanup is restricted to this driver's proven vacant slots.
 * BME off + PERST/drain are prerequisites in wl5_abort. Failure quarantines
 * tables and data; never free a table while a walker could still use it. */
static int
wl_smmu_undo(void)
{
	if (wl_owned) {
		uint64_t smr = WL_SMMU + 0x800 + 4 * wl_slot;
		uint64_t s2cr = WL_SMMU + 0xc00 + 4 * wl_slot;
		if (wl_contain_wr(smr, 0x1401) || (wl_prd(smr) & 0x80000000u)) { return 1; }
		if (wl_contain_wr(s2cr, wl_old_s2cr) || wl_prd(s2cr) != wl_old_s2cr) { return 1; }
		if (wl_contain_wr(wl_cb_addr(wl_cb), 0) || wl_prd(wl_cb_addr(wl_cb)) != 0) { return 1; }
		if (wl_smmu_sync(true)) { return 1; }
		if (wl_contain_wr(smr, wl_old_smr) || wl_prd(smr) != wl_old_smr) { return 1; }
		wl_enabled = wl_owned = false;
		printf("FUXI WLAN: SMMU undo SMR invalid, S2CR restored, CB disabled, TLBI synced\n");
	}
	if (wl_route_owned) {
		if (wl_contain_wr(WL_PARF + 0x2c00, wl_route_cfg) || wl_prd(WL_PARF + 0x2c00) != wl_route_cfg ||
		    wl_contain_wr(WL_PARF + 0x2000 + 4 * wl_route_hash, wl_route_entry) || wl_prd(WL_PARF + 0x2000 + 4 * wl_route_hash) != wl_route_entry) { return 1; }
		wl_route_owned = false;
	}
	return 0;
}

/* Disable both PCI message interrupt mechanisms, leaving the endpoint in
 * polling mode even if a previous boot stage left a capability enabled. */
static int
wl_disable_irqs(void)
{
	unsigned p = wl_prd(WL_EPCFG + 0x34) & 0xff;
	for (unsigned i = 0; p && i < 48; i++) {
		if (p < 0x40 || p > 0xfc || (p & 3)) { return 1; }
		uint32_t cap = wl_prd(WL_EPCFG + p), v = cap;
		if ((cap & 255) == 5) { v &= ~(1u << 16); } /* MSI enable */
		if ((cap & 255) == 0x11) { v = (v & ~(1u << 31)) | (1u << 30); } /* MSI-X enable/mask */
		if (v != cap) {
			wl_pwr(WL_EPCFG + p, v);
			if (wl_prd(WL_EPCFG + p) != v) { return 2; }
		}
		p = (cap >> 8) & 255;
	}
	return p || fw_faults ? 3 : 0;
}

static int
wl_read_firmware(void)
{
	vnode_t root = NULL, vp = NULL;
	vfs_context_t ctx = vfs_context_kernel();
	printf("FUXI WLAN: wait root <=120 s, firmware %s\n", wl_fw_path);
	for (unsigned i = 0; i <= 1200; i++) {
		root = vfs_rootvnode();
		if (root) { break; }
		if (i < 1200) { wl_sleep(100); }
	}
	if (!root) { return 1; }
	vnode_put(root);
	int e = vnode_open(wl_fw_path, FREAD | O_NOFOLLOW | O_NONBLOCK, 0, 0, &vp, ctx);
	if (e) {
		printf("FUXI WLAN: firmware open failed errno %d\n", e);
		return 2;
	}
	struct vnode_attr a;
	VATTR_INIT(&a); VATTR_WANTED(&a, va_data_size);
	e = vnode_getattr(vp, &a, ctx);
	if (e || vnode_vtype(vp) != VREG || !VATTR_IS_SUPPORTED(&a, va_data_size) ||
	    a.va_data_size < WL_CHUNK || a.va_data_size > WL_FW_MAX) {
		e = 3; goto close;
	}
	wl_fw_size = (uint32_t)a.va_data_size;
	if (wl_dma_alloc(&wl_image, wl_fw_size)) { e = 4; goto close; }
	int resid = 0;
	e = vn_rdwr(UIO_READ, vp, (caddr_t)wl_image.va, (int)wl_fw_size, 0,
	    UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (e || resid) {
		printf("FUXI WLAN: firmware read errno %d residual %d\n", e, resid);
		e = 5; goto close;
	}
	/* Qualcomm images need not be native ARM ELFs. Check ident, header size,
	 * version and bounded program table; do not reject vendor e_machine. */
	const uint8_t *b = (const uint8_t *)wl_image.va;
	uint16_t ehsize, phsize, phnum;
	uint32_t version; memcpy(&version, b + 20, 4);
	uint64_t phoff = 0;
	if (version != 1 || memcmp(b, "\177ELF", 4) || (b[4] != 1 && b[4] != 2) || b[5] != 1 || b[6] != 1) { e = 6; goto close; }
	if (b[4] == 1) {
		uint32_t p; memcpy(&p, b + 28, 4); phoff = p;
		memcpy(&ehsize, b + 40, 2); memcpy(&phsize, b + 42, 2); memcpy(&phnum, b + 44, 2);
	} else {
		memcpy(&phoff, b + 32, 8);
		memcpy(&ehsize, b + 52, 2); memcpy(&phsize, b + 54, 2); memcpy(&phnum, b + 56, 2);
	}
	if (ehsize != (b[4] == 1 ? 52 : 64) || !phnum || phsize != (b[4] == 1 ? 32 : 56) ||
	    phoff < ehsize || phoff > wl_fw_size || (uint64_t)phsize * phnum > wl_fw_size - phoff) { e = 6; goto close; }
	wl_dma_clean(&wl_image);
	wl5_report[17] = wl_fw_size; wl5_report[18] = (uint32_t)wl_image.iova;
	printf("FUXI WLAN: firmware %s bytes %u ELF%u DMA %#llx cache-clean\n", wl_fw_path, wl_fw_size, b[4] == 1 ? 32 : 64, wl_image.iova);
	e = 0;
close:
	(void)vnode_close(vp, FREAD, ctx);
	return e;
}

/* Poll modes: BHI completion, READY, SBL/M0, BHIe completion, mission/M0.
 * Error envs are never treated as success merely because EE >= SBL. */
static int
wl_poll(unsigned mode)
{
	for (unsigned i = 0; i <= 200; i++) {
		if (wl_owned && (wl_prd(wl_cb_addr(wl_cb) + 0x58) & 0xc00001fe)) { return 3; }
		uint32_t ee = wl_mrd(wl_bhi + 0x28), st = wl_mrd(0x48);
		uint32_t bs = wl_mrd(wl_bhi + 0x2c), es = wl_mrd(wl_bhie + 0x44);
		uint32_t ctrl = wl_mrd(0x38), state = (st >> 8) & 255;
		if (fw_faults || (st & 4) || state == 255 || (bs >> 30) == 3 ||
		    (mode >= 3 && (es >> 30) == 3) || ee == 3 || ee == 6 || ee == ~0u) { return 1; }
		if ((mode == 0 && ((bs >> 30) == 2 || ee == 1)) ||
		    (mode == 1 && !(ctrl & 2) && (st & 1)) ||
		    (mode == 2 && ee == 1 && state == 2) ||
		    (mode == 3 && (es >> 30) == 2 && (es & WL_SEQ_MASK) == wl_seq) ||
		    (mode == 4 && (ee == 2 || ee == 4 || ee == 7) && state == 2)) { return 0; }
		if (i < 200) { wl_sleep(25); }
	}
	printf("FUXI WLAN: poll mode %u timeout 5000 ms\n", mode);
	return 2;
}

static int
wl5_step(unsigned s)
{
	switch (s) {
	case 17:
		/* M4 validated BHIOFF; independently validate all M5 block extents. */
		wl_bhi = wl4_report[25]; wl_bhie = wl4_report[26];
		if (wl_bhi < 0x40 || wl_bhi > 0x3fc0 || (wl_bhi & 3) ||
		    wl_bhie < 0x40 || wl_bhie > 0x3f80 || (wl_bhie & 3)) { return 4; }
		wl_mhi_valid = true;
		return wl_smmu_probe();
	case 18: {
		uint32_t saved = wl_mrd(0x310c);
		wl_mwr(0x310c, 0x40000036); /* EP internal TCSR, NOT host TCSR */
		if (wl_mrd(0x310c) != 0x40000036) { return 1; }
		uint32_t hw = wl_prd((uint64_t)WL_BAR0 + 0x80000);
		wl_mwr(0x310c, saved);
		wl5_report[16] = hw;
		if (!hw || hw == ~0u || fw_faults) { return 2; }
		wl_fw_path = ((hw >> 8) & 15) == 2 ?
		    "/usr/local/share/fuxi/wlan/amss20.bin" : "/usr/local/share/fuxi/wlan/amss.bin";
		printf("FUXI WLAN: TCSR_SOC_HW_VERSION %#x major %u minor %u firmware %s (vendor filename inference)\n", hw, (hw >> 8) & 15, hw & 255, wl_fw_path);
		return 0;
	}
	case 19: return wl_read_firmware();
	case 20: {
		if (wl_disable_irqs()) { return 4; }
		if (wl_mrd(wl_bhi + 0x28) != 0 || (wl_mrd(0x38) & 2) ||
		    (wl_mrd(0x48) & 0xff04) || (wl_prd(WL_EPCFG + 4) & 4)) { return 1; }
		unsigned n = (wl_fw_size + WL_CHUNK - 1) / WL_CHUNK;
		if (wl_dma_alloc(&wl_vectors, n * sizeof(struct wl_vector)) ||
		    wl_dma_alloc(&wl_contexts, 16384)) { return 2; }
		struct wl_vector *v = (struct wl_vector *)wl_vectors.va;
		for (unsigned i = 0; i < n; i++) {
			unsigned off = i * WL_CHUNK;
			v[i].addr = wl_image.iova + off;
			v[i].size = MIN(WL_CHUNK, wl_fw_size - off);
		}
		struct wl_context *e = (struct wl_context *)(wl_contexts.va + WL_ECTX);
		for (unsigned i = 0; i < 2; i++) {
			e[i].type = 1; e[i].base = e[i].rp = wl_contexts.iova + (i ? WL_ER1 : WL_ER0);
			e[i].len = i ? 4096 : 512; e[i].wp = e[i].base + e[i].len - 16;
		}
		struct wl_context *c = (struct wl_context *)(wl_contexts.va + WL_CCTX);
		c->base = c->rp = c->wp = wl_contexts.iova + WL_CR; c->len = 2048;
		wl_dma_clean(&wl_contexts); wl_dma_clean(&wl_vectors);
		wl5_report[19] = n * sizeof(struct wl_vector);
		wl5_report[20] = (uint32_t)wl_vectors.iova; wl5_report[21] = (uint32_t)wl_contexts.iova;
		printf("FUXI WLAN: prepared %u vectors (%u bytes), 44-byte contexts, channels disabled; bus master off\n", n, wl5_report[19]);
		return fw_faults ? 3 : wl_smmu_enable();
	}
	case 21:
		if (!wl_enabled || wl_smmu_fault(21) ||
		    (wl_prd(WL_PARF + 0x2000 + 4 * wl_route_hash) & 0xffffff00) != 0x01000100 ||
		    wl_prd(WL_SMMU) != wl5_report[4] ||
		    wl_prd(WL_SMMU + 0x800 + 4 * wl_slot) != 0x80001401 ||
		    wl_prd(WL_SMMU + 0xc00 + 4 * wl_slot) != wl_cb ||
		    wl_prd(wl_cb_addr(wl_cb)) != 0x1027 ||
		    (wl_prd(WL_PARF + 0x2c00) & 1) || wl_prd(WL_PARF + 0x234)) { return 6; }
		for (unsigned o = 0x60; o <= 0x70; o += 4) { wl_mwr(wl_bhie + o, 0); }
		wl_mwr(wl_bhi + 0x20, 0); wl_mwr(wl_bhi + 0x2c, 0);
		wl_mwr64(wl_bhi + 8, wl_image.iova); wl_mwr(wl_bhi + 0x10, WL_CHUNK);
		wl_dma_clean(&wl_image);
		if (fw_faults) { return 1; }
		/* Only now, immediately before the first image doorbell, allow DMA.
		 * Disable legacy INTx; MSI/MSI-X remain unconfigured/disabled. */
		wl_pwr(WL_EPCFG + 4, (wl_prd(WL_EPCFG + 4) & 0xffff) | 0x404);
		if ((wl_prd(WL_EPCFG + 4) & 0x406) != 0x406 || fw_faults) { return 2; }
		wl_seq = ((uint32_t)mach_absolute_time() & WL_SEQ_MASK) | 1;
		wl_mwr(wl_bhi + 0x18, wl_seq);
		printf("FUXI WLAN: BHI doorbell seq %#x bytes %#x bus master on\n", wl_seq, WL_CHUNK);
		return fw_faults ? 3 : 0;
	case 22: return wl_poll(0);
	case 23: return wl_poll(1);
	case 24: {
		uint32_t erdb = wl_mrd(0x20);
		if ((erdb & 7) || erdb < 0x100 || erdb > 0x3ff0 ||
		    (wl_mrd(0x10) & 255) != 128) { return 1; }
		wl_mwr64(0x58, wl_contexts.iova); wl_mwr64(0x60, wl_contexts.iova + WL_ECTX);
		wl_mwr64(0x68, wl_contexts.iova + WL_CCTX);
		wl_mwr64(0x80, 0); wl_mwr64(0x88, 0xffffffffull);
		wl_mwr64(0x98, 0); wl_mwr64(0xa0, 0xffffffffull);
		wl_mwr(0x10, (wl_mrd(0x10) & 0xffff) | (2u << 16));
		wl_mwr64(erdb, wl_contexts.iova + WL_ER0 + 512 - 16);
		wl_mwr64(erdb + 8, wl_contexts.iova + WL_ER1 + 4096 - 16);
		return fw_faults ? 2 : 0;
	}
	case 25:
		wl_mwr(0x38, (wl_mrd(0x38) & ~0xff00u) | 0x200);
		return wl_poll(2);
	case 26:
		wl_dma_clean(&wl_image); wl_dma_clean(&wl_vectors);
		wl_mwr64(wl_bhie + 0x2c, wl_vectors.iova);
		wl_mwr(wl_bhie + 0x34, wl5_report[19]);
		wl_seq = (wl_seq + 1) & WL_SEQ_MASK; if (!wl_seq) { wl_seq = 1; }
		wl_mwr(wl_bhie + 0x3c, (wl_mrd(wl_bhie + 0x3c) & ~WL_SEQ_MASK) | wl_seq);
		printf("FUXI WLAN: BHIe doorbell seq %#x full image %u vector bytes %u\n", wl_seq, wl_fw_size, wl5_report[19]);
		return fw_faults ? 1 : 0;
	case 27: return wl_poll(3);
	case 28: {
		int e = wl_poll(4);
		if (!e) { printf("FUXI WLAN: M6 mission mode M0 confirmed (READY observed at step 23); no channels/QMI; DMA buffers retained\n"); }
		return e;
	}
	}
	return 0xff;
}

/* Cleanup is the only exception to the normal write-after-fault latch. These
 * containment/owned undo destinations were mapped before bring-up. Keep each access
 * fault-contained, and never clear the original fault counters. */
static int
wl_contain_wr(uint64_t pa, uint32_t value)
{
	uintptr_t va = fw_va(pa); uint64_t v = value;
	if (!va) { return 1; }
	__asm__ volatile("dsb sy" ::: "memory");
	return (int)fuxi_gpu_access(va, &v, 1, 0);
}

static void
wl5_abort(unsigned step)
{
	wl5_status(step);
	(void)wl_smmu_fault(step);
	int reset_bad = 0;
	if (wl_mhi_valid) {
		reset_bad = wl_contain_wr((uint64_t)WL_BAR0 + 0x38, 2);
		for (unsigned i = 0; !reset_bad && i <= 40; i++) {
			uint32_t ctrl = wl_mrd(0x38);
			if (ctrl == ~0u) { reset_bad = 1; break; }
			if (!(ctrl & 2)) { break; }
			if (i == 40) { reset_bad = 1; break; }
			wl_sleep(25);
		}
	}
	/* PCI command upper word is W1C status: always write only low 16 bits. */
	uint32_t cmd = wl_prd(WL_EPCFG + 4);
	int bm_bad = wl_contain_wr(WL_EPCFG + 4, cmd & 0xffff & ~4u);
	cmd = wl_prd(WL_EPCFG + 4); bm_bad |= cmd == ~0u || !!(cmd & 4);
	/* gpio94 was configured as an output at step 5; clear only its OUT bit. */
	uint64_t perst = TS_TLMM + 0x1000ull * 94 + 4;
	uint32_t io = wl_prd(perst);
	int perst_bad = wl_contain_wr(perst, io & ~2u);
	io = wl_prd(perst); perst_bad |= io == ~0u || !!(io & 2);
	__asm__ volatile("dsb sy" ::: "memory");
	wl_sleep(10); /* PERST assertion + outstanding transaction drain */
	printf("FUXI WLAN: abort step %u MHI reset %s, bus master off %s, PERST asserted %s\n", step,
	    reset_bad ? "unconfirmed" : "done", bm_bad ? "unconfirmed" : "yes", perst_bad ? "unconfirmed" : "yes");
	/* No DMA buffer is ever returned to the allocator while containment is
	 * unproven. This is a bounded leak on an MMIO fault, never a panic/UAF. */
	if (!bm_bad && !perst_bad && !wl_smmu_undo()) {
		wl_dma_free(&wl_tables);
		wl_dma_free(&wl_contexts); wl_dma_free(&wl_vectors); wl_dma_free(&wl_image);
		printf("FUXI WLAN: abort buffers freed\n");
	} else {
		printf("FUXI WLAN: abort buffers quarantined until reboot (containment or SMMU undo unproven)\n");
	}
}

static void
fw_dump(const char *tag, const uint32_t *r, unsigned words)
{
	for (unsigned i = 0; i < words; i += 8) {
		unsigned any = 0;
		for (unsigned j = 0; j < 8; j++) {
			any |= r[i + j];
		}
		if (any) {
			printf("FUXI WLAN: %s r[%3u] %08x %08x %08x %08x %08x %08x %08x %08x\n", tag, i,
			    r[i], r[i + 1], r[i + 2], r[i + 3], r[i + 4], r[i + 5], r[i + 6], r[i + 7]);
		}
	}
}

static void
fw_m3_summary(unsigned s)
{
	const uint32_t *r = wl3_report;
	switch (s) {
	case 1:
		printf("FUXI WLAN: cmd-db %#x %#x %#x %#x %#x %#x %#x %#x GPLL0 %#x\n",
		    r[20], r[21], r[22], r[23], r[24], r[25], r[26], r[27], r[28]);
		break;
	case 9:
		printf("FUXI WLAN: PHY ready after %u us pcs_status %#x\n", r[62], r[63]);
		break;
	case 11:
		printf("FUXI WLAN: link LTSSM %#x LNKCTL/STA %#x (gen%u x%u DLLLA %u) %u ms\n",
		    r[69], r[70], (r[70] >> 16) & 0xfu, (r[70] >> 20) & 0x3fu, (r[70] >> 29) & 1u, r[68]);
		break;
	case 12:
		printf("FUXI WLAN: endpoint id %#x class/rev %#x cmd/status %#x tries %u\n",
		    r[75], r[77], r[80], r[76]);
		break;
	case 13:
		printf("FUXI WLAN: BAR0 orig %#x/%#x sizing readback %#x/%#x\n", r[81], r[82], r[87], r[88]);
		break;
	}
}

static void
fw_run(unsigned limit)
{
	uint32_t *r4 = wl4_report;
	r4[0] = WL4_MAGIC;
	r4[1] = limit;
	wl5_report[0] = 0x574c0006; wl5_report[1] = limit;
	for (unsigned s = 1; s <= limit; s++) {
		uint64_t t0 = mach_absolute_time(), ns = 0;
		unsigned faults0 = fw_faults;
		int e;
		if (s <= WL3_STEPS) {
			wlan_m3(s);    /* runs only step s: earlier ones are in wl3_done */
			e = (int)(wl3_report[3] & 0xffu);
			if (wl3_report[3]) {
				e = e ? e : 0xfe;
			}
		} else {
			wl_mark(s);
			wl3_faults = 0;
			e = s <= WL4_STEPS ? wl4_step(s) : wl5_step(s);
			if (s > WL4_STEPS) {
				wl5_status(s);
				if (wl_smmu_fault(s) && !e) { e = 0xfc; }
			}
			wl3_report[124] += wl3_faults;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		/* A fault in a plain (usb_rd/ts_wr) access also ends the run. */
		if (!e && fw_faults != faults0) {
			e = 0xfd;
		}
		r4[2] = s;
		uint32_t code = e ? (s << 8 | (unsigned)e) : 0x1000u | s;
		if (s <= WL4_STEPS) { r4[40 + s] = code; }
		wl5_report[2] = s; wl5_report[32 + s] = code;
		printf("FUXI WLAN: step %u %s code %#x (%llu ms, faults %u)\n", s, e ? "FAILED" : "ok",
		    code, ns / 1000000ull, fw_faults);
		if (!e && s <= WL3_STEPS) {
			fw_m3_summary(s);
		}
		if (e) {
			r4[3] = s << 8 | (unsigned)e;
			wl5_report[3] = r4[3];
			if (s >= 17) { wl5_abort(s); }
			printf("FUXI WLAN: stopped at step %u reason %u ESR %#x FAR %#llx; reboot to retry\n",
			    s, e, fw_fault_esr, fw_fault_far);
			goto out;
		}
	}
	printf("FUXI WLAN: %u steps done\n", limit);
	/* A deliberate partial M5 run also shuts down before releasing memory. */
	if (limit >= 17 && limit < WL5_STEPS) { wl5_abort(limit); }
out:
	/* Shared GCC vote registers (touch and GPU also read-modify-write
	 * 0x52008/0x52000 without a common lock): show the final values. */
	printf("FUXI WLAN: GCC votes 0x52000 %#x 0x52008 %#x gdsc-collapse 0x52020 %#x\n",
	    usb_rd(WL_GCC, 0x52000), usb_rd(WL_GCC, 0x52008), usb_rd(WL_GCC, 0x52020));
	fw_dump("m3", wl3_report, 128);
	fw_dump("m4", wl4_report, 64);
	if (limit >= 17) { fw_dump("m6", wl5_report, 128); }
}

static void
fw_thread(void *arg, wait_result_t result)
{
	unsigned limit = (unsigned)(uintptr_t)arg;
	(void)result;
	for (unsigned i = 0; i < FW_NWIN; i++) {
		kern_return_t kr = fw_map_window(&fw_windows[i]);
		if (kr != KERN_SUCCESS) {
			printf("FUXI WLAN: map PA %#llx bytes %#llx failed %d\n",
			    fw_windows[i].pa, fw_windows[i].size, kr);
			goto out;
		}
	}
	printf("FUXI WLAN: mapped %llu bytes in %u windows; running steps 1..%u\n",
	    fw_mapped_bytes, (unsigned)FW_NWIN, limit);
	fw_run(limit);
out:
	printf("FUXI WLAN: thread exit\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
fuxi_wlan_init(void)
{
	static bool started;
	int arg = 0;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	if (!PE_parse_boot_argn("fuxiwlan", &arg, sizeof(arg)) || arg <= 0) {
		return;
	}
	unsigned limit = (arg == 1 || arg > (int)WL5_STEPS) ? WL5_STEPS : (unsigned)arg;
	printf("FUXI WLAN: enabled (fuxiwlan=%d), WCN7850 power-up + PCIe0 + MHI firmware, limit %u\n",
	    arg, limit);
	if (kernel_thread_start(fw_thread, (void *)(uintptr_t)limit, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("FUXI WLAN: kernel_thread_start failed\n");
	}
}
#endif /* FUXI */
