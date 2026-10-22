/*
 * FUXI WLAN (lane-wlan milestones 4/5/6): WCN7850 power-up, PCIe0 link, BAR0
 * assignment and a read-only MHI/BHI register dump from XNU.
 * Gated by boot-arg fuxiwlan (default 0 = do nothing):
 *   fuxiwlan=1      run all 28 steps
 *   fuxiwlan=2..28  stop after that step (bisecting a hang or reset)
 *   fuxiwlanoldiova=1  use M6f's IOVA pool, retaining the M6g SMMU profile
 *   fuxiwlanclk=1  add missing PCIe fabric clock votes (default: observe)
 *   fuxiwlanicc=0  disable M6i BCM votes (default 1 = enabled)
 *   fuxiwlaniccbw=0..8  optional KIWI CNSS bandwidth row (default 0)
 *   fuxiwlaniboff=1  disable stale inbound iATU windows before BHI (opt-in)
 *   fuxiwlanprobe=unmapped|pattern|civac|nc  one BHI experiment, then contain
 *   fuxiwlanaspm=1  retain inherited ASPM (revert download safeguard)
 *   fuxiwlanpre=qfprom|warm|reset|wake|l1ss  pre-BHI experiment
 *   fuxiwlanfw=stock  separate caller-supplied stock image, no fallback
 *   fuxiwlanbhi=full  M6l whole-file BHI size experiment
 *   fuxiwlanvec=early|rddm  M6l TX staging or RDDM RX experiment
 *   fuxiwlancnss=pdc  M6n AOP WLAN PDC setup before rails (default off)
 *   fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix  M6o opt-ins
 *   fuxiwlanseq=cyclerails  also cycle shared rails (froze the SoC once)
 * Steps 17..28 probe DMA safety and load rootfs firmware through BHI/BHIe.
 * Completion is polled; M6o can enable MSI to a RAM sink. No channels/QMI;
 * see docs-lanes/wlan-bt.md M6.
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
#include <sys/random.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
/* osfmk/vm/pmap.h declares these under MACH_KERNEL_PRIVATE only; bsd/ cannot
 * see them, but they are in the same kernel image (M6k fuxiwlanprobe=nc). */
extern unsigned int pmap_cache_attributes(ppnum_t pn);
extern void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);

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
	{0x60480000, 0x80000, 0},   /* EP sliding 512-KiB window: version + bounded BL logs */
	{0x0f14c000, 0x18000, 0},   /* TLMM tiles gpio76..96 */
	{0x17a20000, 0x4000, 0},    /* apps RSC DRV2 + TCS */
	{0x60000000, 0x4000, 0},    /* PCIe0 DBI, ELBI 0xf20, iATU 0x1000 */
	{0x60100000, 0x4000, 0},    /* config window (iATU region 0, CFG0) */
	{0x60400000, 0x4000, 0},    /* endpoint BAR0, first 16 KiB (MHI/BHI) */
	{0x81c60000, 0x20000, 0},   /* cmd-db (reserved DDR) */
	{0x0c300000, 0x4000, 0},    /* opt-in M6n AOSS QMP; descriptor bounds 0x400 */
	{0x00408000, 0x4000, 0},    /* opt-in M6n IPCC MPROC SEND_ID */
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

/* M6o link-down guard, defined with the cycle below. */
static bool wl_link_refused(uint64_t pa);

static unsigned
fw_access(uint64_t pa, uint64_t *v, unsigned wr)
{
	if (wl_link_refused(pa)) {
		fw_faults++;
		return 1;
	}
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

/* ACTIVE TCS transport shared by the existing WLAN regulator votes and M6i.
 * RSC v3: TCS stride 672, command stride 24 (rpmh-rsc.c). No other XNU
 * code drives the fuxi apps RSC. Never touch inherited busy TCS state; an
 * uncertain transaction is quarantined for the session. */
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
struct wl_rh_cmd { uint32_t addr, data, wait; };
static bool rh_poisoned;
static uint32_t
rh_send_request(unsigned t, const struct wl_rh_cmd *cmd, unsigned n, bool regulator)
{
	/* DRV2 has three ACTIVE TCSs; preserve all sleep/wake/other slots. */
	if (t >= 3 || !n || n > 16 || rh_poisoned || fw_faults) { return 1; }
	uint32_t b = t * RH_STRIDE, bit = 1u << t;
	uint32_t cfg = usb_rd(RH_DRV_PA, 0xc);
	if (((usb_rd(RH_DRV_PA, 0) >> 16) & 255) != 3 ||
	    ((cfg >> 12) & 63) < 3 || ((cfg >> 27) & 31) < n ||
	    rh_rd(b + RH_IDLE) == 0 || rh_rd(b + RH_CTRL) || rh_rd(b + RH_EN) ||
	    rh_rd(b + RH_WAIT) || (rh_rd(4) & bit) || fw_faults) { return 1; }
	uint32_t irq_en = rh_rd(0), wait = 0;
	if (fw_faults) { return 1; }
	rh_poisoned = true;
	rh_wr(0, irq_en & ~bit);
	for (unsigned i = 0; i < n; i++) {
		uint32_t c = b + i * 24;
		rh_wr(c + RH_MSGID, 8u | (1u << 16) | (!regulator || cmd[i].wait ? 1u << 8 : 0));
		rh_wr(c + RH_ADDR, cmd[i].addr);
		rh_wr(c + RH_DATA, cmd[i].data);
		if (cmd[i].wait) { wait |= 1u << i; }
	}
	rh_wr(b + RH_WAIT, wait);
	rh_wr(b + RH_EN, (1u << n) - 1);
	bool programmed = !(rh_rd(0) & bit) && rh_rd(b + RH_WAIT) == wait && rh_rd(b + RH_EN) == (1u << n) - 1;
	for (unsigned i = 0; i < n; i++) {
		uint32_t c = b + i * 24;
		if (rh_rd(c + RH_ADDR) != cmd[i].addr || rh_rd(c + RH_DATA) != cmd[i].data ||
		    rh_rd(c + RH_MSGID) != (8u | (1u << 16) | (!regulator || cmd[i].wait ? 1u << 8 : 0))) { programmed = false; }
	}
	if (!programmed || fw_faults) { return 2; }
	rh_wr(b + RH_CTRL, 0);
	rh_wr(b + RH_CTRL, 1u << 16);
	if (rh_rd(b + RH_CTRL) != (1u << 16) || fw_faults) { return 2; }
	rh_wr(b + RH_CTRL, (1u << 16) | (1u << 24));
	bool done = false;
	for (unsigned us = 0; us < 100000 && !fw_faults; us++) {
		/* Device (M6j): BCM commands reach ISSUED|COMPLETED with the TCS idle
		 * while IRQ_STATUS stays 0 (its IRQ is masked here), and after the
		 * first BCM request regulator requests stop raising it too. Every
		 * request completes on idle plus per-command status. */
		done = rh_rd(b + RH_IDLE) != 0;
		for (unsigned i = 0; i < n; i++) {
			uint32_t mask = 0x100u | (cmd[i].wait ? 0x10000u : 0);
			if ((rh_rd(b + i * 24 + RH_CSTAT) & mask) != mask) { done = false; }
		}
		if (done) { break; }
		fw_delay(1);
	}
	if (!done || fw_faults) {
		if (!regulator) {
			uint32_t irq = rh_rd(4), ctrl = rh_rd(b + RH_CTRL), idle = rh_rd(b + RH_IDLE);
			for (unsigned i = 0; i < n; i++) {
				uint32_t c = b + i * 24;
				uint32_t msgid = rh_rd(c + RH_MSGID), status = rh_rd(c + RH_CSTAT);
				printf("FUXI WLAN: M6j RPMh timeout TCS %u cmd %u addr %#x data %#x MSGID %#x CMD_STATUS %#x IRQ_STATUS %#x CONTROL %#x IDLE %#x faults %u\n",
				    t, i, cmd[i].addr, cmd[i].data, msgid, status, irq, ctrl, idle, fw_faults);
			}
		}
		return 2;
	}
	rh_wr(b + RH_CTRL, 1u << 16);
	rh_wr(b + RH_CTRL, 0);
	rh_wr(b + RH_EN, 0);
	rh_wr(b + RH_WAIT, 0);
	rh_wr(8, bit);
	rh_wr(0, (rh_rd(0) & ~bit) | (irq_en & bit));
	if (fw_faults || rh_rd(b + RH_CTRL) || rh_rd(b + RH_EN) ||
	    rh_rd(b + RH_WAIT) || (rh_rd(4) & bit) || !rh_rd(b + RH_IDLE) || fw_faults) { return 3; }
	rh_poisoned = false;
	return 0;
}
static uint32_t
rh_send_batch(unsigned t, const struct wl_rh_cmd *cmd, unsigned n)
{
	/* BCM requests wait for the TCS IRQ and request responses on all cmds. */
	return rh_send_request(t, cmd, n, false);
}
static uint32_t
rh_send(unsigned t, uint32_t addr, uint32_t data)
{
	const struct wl_rh_cmd cmd = {addr, data, 1};
	return rh_send_request(t, &cmd, 1, true);
}

static bool wl_m6o_parf(void);
static int wl_m6o_host(unsigned);
#define FUXI_WLAN_XNU 1
#include "wlan.c"

/* ---- milestone 6i: fixed SM8550 PCIe0 ICC clients, ACTIVE session votes ----
 * Graph, aggregation and packet format: Linux v6.18 sm8550.c, bcm-voter.c,
 * icc-rpmh.c, cmd-db.c and tcs.h; derivation / CNSS audit: docs/wlan-m6i.md.
 * No BCM address is MMIO and no address/unit/width/VCD is hardcoded here.
 * This is the WLAN client's vote only, not an aggregate of other XNU clients.
 */
#define WL_ICC_N 6u
struct wl_bcm {
	const char *name; bool mask; /* Linux enable_mask BCM: no unit/width use */
	uint32_t addr, unit, width, vcd, version, x, y, data;
};
static struct wl_bcm wl_bcms[WL_ICC_N] = {
	{.name = "SN7"}, {.name = "SH1", .mask = true}, {.name = "SH0"},
	{.name = "MC0"}, {.name = "ACV", .mask = true}, {.name = "CN0", .mask = true}
};
static unsigned wl_icc_acks;
static bool wl_icc_ready, wl_icc_failed, wl_icc_configured;
static int wl_icc_enabled = 1, wl_icc_bw;

static bool
wl_cdb_range(uint32_t at, uint32_t n)
{
	return at <= WL_CMDDB_BYTES && n <= WL_CMDDB_BYTES - at;
}
static uint32_t
wl_cdb32(uint32_t off)
{
	return wl_cdb16(off) | wl_cdb16(off + 2) << 16;
}
static int
wl_bcm_lookup(struct wl_bcm *bcm)
{
	/* Same existing reserved-DDR mapping and byte accessors as WLAN rails.
	 * Header v1, 8 slave headers, 24-byte entries; all offsets relative to
	 * byte 144. Validate complete extents before fetching entry/aux bytes. */
	if (fw_faults || wl_cdb32(0) != 1 || wl_cdb32(4) != 0x0c0330db ||
	    (wl_cdb32(140) & 1)) { return 1; }
	unsigned matches = 0;
	for (unsigned h = 8; h < 136; h += 16) {
		unsigned slave = wl_cdb16(h);
		if (!slave) { break; }
		uint32_t at = 144 + wl_cdb16(h + 2), aux = 144 + wl_cdb16(h + 4);
		unsigned n = wl_cdb16(h + 6);
		if (slave < 3 || slave > 5 || !wl_cdb_range(at, n * 24) || !wl_cdb_range(aux, 0)) { return 2; }
		for (unsigned i = 0; i < n; i++) {
			uint32_t e = at + i * 24;
			unsigned j = 0;
			for (; j < 8 && bcm->name[j]; j++) {
				if (wl_cdb8(e + j) != (uint8_t)bcm->name[j]) { break; }
			}
			if (j < 8 && bcm->name[j]) { continue; }
			/* Compare the entire zero-padded key, not just a prefix. */
			for (; j < 8 && !wl_cdb8(e + j); j++) { }
			if (j != 8) { continue; }
			uint32_t addr = wl_cdb32(e + 16), len = wl_cdb16(e + 20);
			uint32_t data = aux + wl_cdb16(e + 22);
			if (slave != 5 || (addr >> 16) != 5 || (addr & 3) || len < 8 ||
			    !wl_cdb_range(data, len)) { return 3; }
			bcm->addr = addr; bcm->unit = wl_cdb32(data);
			bcm->width = wl_cdb16(data + 4); bcm->vcd = wl_cdb8(data + 6);
			bcm->version = wl_cdb16(h + 8);
			if (!bcm->mask && (!bcm->unit || !bcm->width)) { return 4; } /* device: ACV width 0, VCD 12 */
			matches++;
		}
	}
	return fw_faults || matches != 1 ? 5 : 0;
}

/* Linux bcm_div floors except a nonzero value below the divisor becomes 1.
 * Do not substitute DIV_ROUND_UP: both normalization stages matter. */
static uint64_t
wl_bcm_div(uint64_t value, uint32_t divisor)
{
	return value && value < divisor ? 1 : value / divisor;
}
static uint32_t
wl_bcm_encode(uint64_t x, uint64_t y, bool commit)
{
	uint32_t valid = !!(x || y);
	if (x > 0x3fff) { x = 0x3fff; }
	if (y > 0x3fff) { y = 0x3fff; }
	return (commit ? 1u << 30 : 0) | valid << 29 | (uint32_t)x << 14 | (uint32_t)y;
}
static void
wl_bcm_bandwidth(struct wl_bcm *b, uint32_t avg, uint32_t peak, unsigned buswidth, unsigned channels)
{
	uint64_t x = wl_bcm_div((uint64_t)avg * b->width, buswidth * channels);
	uint64_t y = wl_bcm_div((uint64_t)peak * b->width, buswidth);
	x = wl_bcm_div(x * 1000, b->unit); y = wl_bcm_div(y * 1000, b->unit);
	b->x = (uint32_t)MIN(x, 0x3fff); b->y = (uint32_t)MIN(y, 0x3fff);
}
static void
wl_icc_configure(void)
{
	if (wl_icc_configured) { return; }
	wl_icc_configured = true;
	(void)PE_parse_boot_argn("fuxiwlanicc", &wl_icc_enabled, sizeof(wl_icc_enabled));
	(void)PE_parse_boot_argn("fuxiwlaniccbw", &wl_icc_bw, sizeof(wl_icc_bw));
	if ((wl_icc_enabled != 0 && wl_icc_enabled != 1) || wl_icc_bw < 0 || wl_icc_bw > 8) {
		wl_icc_failed = true;
	}
	printf("FUXI WLAN: M6i ICC fuxiwlanicc=%d fuxiwlaniccbw=%d default=1 retained ACTIVE session votes; CNSS download auto-vote=none\n",
	    wl_icc_enabled, wl_icc_bw);
}
static int
wl_icc_vote(uint32_t peak, bool download)
{
	/* KIWI (0x1107), two paths / nine rows, kB/s: path1 PCIe->GEM,
	 * path2 GEM->EBI. Optional experiment; CNSS2 does not automatically
	 * select a nonzero row during PBL download in the audited revision. */
	static const uint32_t cnss[][3] = {
		{0, 0, 0}, {2250, 1600000, 2188800}, {7500, 1600000, 2188800},
		{30000, 1600000, 2188800}, {100000, 1600000, 2188800},
		{175000, 6448000, 6220800}, {312500, 6448000, 8368000},
		{587500, 8528000, 12800000}, {7500, 3200000, 2188800}
	};
	wl_icc_configure();
	if (wl_icc_failed || fw_faults) { return 1; }
	if (!wl_icc_enabled) { return 0; }
	/* Resolve every dependency before the first vote. Missing or malformed
	 * metadata never leads to guessed addresses or partial lookup votes. */
	if (!wl_icc_ready) {
		for (unsigned i = 0; i < WL_ICC_N; i++) {
			struct wl_bcm *b = &wl_bcms[i];
			int e = wl_bcm_lookup(b);
			printf("FUXI WLAN: M6i ICC %s cmd-db addr %#x unit %u width %u VCD %u aux-version %#x lookup %d\n",
			    b->name, b->addr, b->unit, b->width, b->vcd, b->version, e);
			if (e) { wl_icc_failed = true; return 2; }
			for (unsigned j = 0; j < i; j++) {
				if (b->addr == wl_bcms[j].addr) { wl_icc_failed = true; return 2; }
			}
		}
		wl_icc_ready = true;
	}
	unsigned row = download ? (unsigned)wl_icc_bw : 0;
	uint32_t avg = cnss[row][0], p1 = cnss[row][1], p2 = cnss[row][2];
	if (p1 < peak) { p1 = peak; }
	if (p2 < peak) { p2 = peak; }
	printf("FUXI WLAN: M6i ICC %s pcie-mem 0/%u cpu-pcie 0/1 CNSS row %u path1 %u/%u path2 %u/%u kBps\n",
	    download ? "link/download" : "pretrain", peak, row, avg, cnss[row][1], avg, cnss[row][2]);
	wl_bcm_bandwidth(&wl_bcms[0], avg, p1, 16, 1); /* SN7 qns_pcie_mem_noc */
	wl_bcms[1].x = 0; wl_bcms[1].y = 1;          /* SH1 mask, both paths */
	wl_bcm_bandwidth(&wl_bcms[2], avg, p2, 16, 4); /* SH0 qns_llcc */
	wl_bcm_bandwidth(&wl_bcms[3], avg, p2, 4, 4);  /* MC0 ebi */
	wl_bcms[4].x = 0; wl_bcms[4].y = 8;          /* ACV mask */
	wl_bcms[5].x = 1; wl_bcms[5].y = 1;          /* CN0 mask + keepalive */
	unsigned order[WL_ICC_N];
	for (unsigned i = 0; i < WL_ICC_N; i++) {
		unsigned j = i;
		while (j && wl_bcms[order[j - 1]].vcd > wl_bcms[i].vcd) {
			order[j] = order[j - 1]; j--;
		}
		order[j] = i;
	}
	/* M6j serializes one whole VCD per request. Linux v6.18 can pack
	 * multiple VCDs; see docs/wlan-m6i.md for this deliberate restriction.
	 * All commands request responses; only the last gets commit/wait. */
	struct wl_rh_cmd cmd[WL_ICC_N];
	for (unsigned i = 0; i < WL_ICC_N; i++) {
		struct wl_bcm *b = &wl_bcms[order[i]];
		bool commit = i + 1 == WL_ICC_N || b->vcd != wl_bcms[order[i + 1]].vcd;
		b->data = wl_bcm_encode(b->x, b->y, commit);
		cmd[i] = (struct wl_rh_cmd){b->addr, b->data, commit};
	}
	unsigned groups = 0;
	for (unsigned first = 0; first < WL_ICC_N;) {
		unsigned end = first + 1;
		while (end < WL_ICC_N && !cmd[end - 1].wait) { end++; }
		uint32_t e = rh_send_batch(0, cmd + first, end - first);
		for (unsigned i = first; i < end; i++) {
			struct wl_bcm *b = &wl_bcms[order[i]];
			printf("FUXI WLAN: M6i ICC %s addr %#x vote %u/%u data %#x VCD %u commit %u ack %s error %#x\n",
			    b->name, b->addr, b->x, b->y, b->data, b->vcd, cmd[i].wait, e ? "unconfirmed" : "yes", e);
		}
		unsigned vcd = wl_bcms[order[first]].vcd;
		if (!e) { groups++; }
		printf("FUXI WLAN: M6j ICC VCD %u cmds %u ack %s error %#x acked_groups %u\n",
		    vcd, end - first, e ? "unconfirmed" : "yes", e, groups);
		if (e) { wl_icc_failed = true; return 3; }
		first = end;
	}
	wl_icc_acks++;
	return 0;
}
static int
wl_icc_step(unsigned step)
{
	if (step == 2) { return wl_icc_vote(250000, false); }
	if (step != 12) { return 0; }
	wl_icc_configure();
	if (wl_icc_failed) { return 1; }
	if (!wl_icc_enabled) { return 0; }
	/* Step 11's capability walk / link read has succeeded. Validate it
	 * again before accepting a negotiated rate; never lower to zero. */
	uint32_t link = usb_rd(WL_DBI, wl3_report[67] + 0x10);
	unsigned speed = (link >> 16) & 15, width = (link >> 20) & 63;
	static const uint32_t rate[] = {0, 250000, 500000, 984500};
	if (fw_faults || !(link & (1u << 29)) || !width || width > 2 || speed < 1 || speed > 3) {
		wl_icc_failed = true; return 4;
	}
	return wl_icc_vote(rate[speed] * width, true);
}
/* ---- end milestone 6i ---- */

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
 * M6 uses owned stage-1 translation of WLAN IOVAs to wired XNU pages.
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
struct wl_dma { vm_offset_t va; vm_size_t size; uint64_t pa, iova; bool nc; };
static struct wl_dma wl_image, wl_vectors, wl_contexts, wl_tables;
static struct wl_dma wl_rddm, wl_rxvectors, wl_msi_sink;
#define WL_RDDM_SIZE 0x780000u /* ath12k WCN7850; crash sink only, never dumped */
static unsigned wl_pre, wl_full, wl_vec, wl_fw;
static unsigned wl_cnss;
static bool wl_pdc_attempted;
static unsigned wl_pdc_acks;
static const char *const wl_cnss_names[] = {"off", "pdc"};
static bool wl_variant_invalid;
static const char *const wl_pre_names[] = {"off", "qfprom", "warm", "reset", "wake", "l1ss"};
static const char *const wl_fw_names[] = {"off", "stock"};
static const char *const wl_vec_names[] = {"off", "early", "rddm"};

static unsigned
wl_variant_arg(const char *name, const char *const *names, unsigned count)
{
	char arg[16] = {0};
	if (!PE_parse_boot_argn(name, arg, sizeof(arg))) { return 0; }
	arg[sizeof(arg) - 1] = 0;
	for (unsigned i = 0; i < count; i++) {
		if (!strcmp(arg, names[i])) { return i; }
	}
	wl_variant_invalid = true;
	return 0;
}

/* Independent default-off sequence deltas; register facts: docs/wlan-m6o.md. */
enum { WL_O_CYCLE=1, WL_O_PARF=2, WL_O_DBI=4, WL_O_EQ=8,
    WL_O_HOSTIRQ=16, WL_O_MSI=32, WL_O_POR=64, WL_O_PREFIX=128,
    WL_O_CYCLERAILS=256 };
static unsigned wl_seq_flags;
static bool wl_cycle_attempted, wl_seq_requested;
static unsigned wl_msi_cap, wl_pm_cap;
static const char *const wl_seq_names[] = {"cycle", "parf", "dbi", "eq", "hostirq", "msi", "por", "prefix", "cyclerails"};
static bool wl_m6o_parf(void) { return !!(wl_seq_flags & WL_O_PARF); }
static bool wl_m6o_early(void) { return !!(wl_seq_flags & (WL_O_CYCLE | WL_O_MSI)); }
static void
wl_seq_configure(void)
{
	char arg[96] = {0};
	wl_seq_flags = 0; wl_seq_requested = false;
	if (!PE_parse_boot_argn("fuxiwlanseq", arg, sizeof(arg))) { return; }
	wl_seq_requested = true;
	/* A sentinel prevents a truncated comma-list from becoming valid. */
	if (arg[sizeof(arg)-1]) { wl_variant_invalid = true; return; }
	if (!strcmp(arg, "off")) { return; }
	char *part = arg;
	while (*part) {
		char *end = part;
		while (*end && *end != ',') { end++; }
		bool more = *end == ','; *end = 0;
		unsigned bit = 0;
		for (unsigned i = 0; i < sizeof(wl_seq_names)/sizeof(wl_seq_names[0]); i++) {
			if (!strcmp(part, wl_seq_names[i])) { bit = 1u << i; break; }
		}
		if (!bit || (wl_seq_flags & bit)) { wl_variant_invalid = true; break; }
		wl_seq_flags |= bit;
		if (!more) { break; }
		part = end + 1;
		if (!*part) { wl_variant_invalid = true; }
	}
	if (!*arg) { wl_variant_invalid = true; }
	/* Imply cycle only after duplicate detection, independent of list order. */
	if (wl_seq_flags & WL_O_CYCLERAILS) { wl_seq_flags |= WL_O_CYCLE; }
}

static bool wl_variant_active(void) { return wl_pre || wl_full || wl_vec || wl_fw || wl_cnss || wl_seq_flags; }
static unsigned wl_bhi_bytes(void) { return wl_full ? wl_fw_size : WL_CHUNK; }
enum wl_probe_mode { WL_PROBE_OFF, WL_PROBE_UNMAPPED, WL_PROBE_PATTERN,
    WL_PROBE_CIVAC, WL_PROBE_NC, WL_PROBE_INVALID };
static enum wl_probe_mode wl_probe;
static bool wl_probe_fired;
static uint64_t wl_perst_done, wl_link_done;
static const char *const wl_probe_names[] = {"off", "unmapped", "pattern", "civac", "nc", "invalid"};

static void
wl_probe_configure(void)
{
	char arg[16] = {0};
	const char *const sizes[] = {"off", "full"};
	wl_variant_invalid = false;
	wl_pre = wl_variant_arg("fuxiwlanpre", wl_pre_names, 6);
	wl_fw = wl_variant_arg("fuxiwlanfw", wl_fw_names, 2);
	wl_full = wl_variant_arg("fuxiwlanbhi", sizes, 2);
	wl_vec = wl_variant_arg("fuxiwlanvec", wl_vec_names, 3);
	wl_cnss = wl_variant_arg("fuxiwlancnss", wl_cnss_names, 2);
	wl_seq_configure();
	if ((wl_seq_flags & WL_O_PREFIX) && (wl_full || wl_vec == 1)) { wl_variant_invalid = true; }
	wl_probe = WL_PROBE_OFF; wl_probe_fired = false;
	if (PE_parse_boot_argn("fuxiwlanprobe", arg, sizeof(arg))) {
		arg[sizeof(arg) - 1] = 0;
		wl_probe = WL_PROBE_INVALID;
		for (unsigned i = 0; i < WL_PROBE_INVALID; i++) {
			if (!strcmp(arg, wl_probe_names[i])) { wl_probe = (enum wl_probe_mode)i; break; }
		}
	}
	if (wl_variant_active() && (wl_probe == WL_PROBE_UNMAPPED || wl_probe == WL_PROBE_PATTERN)) {
		wl_variant_invalid = true;
	}
	int retain = 0;
	(void)PE_parse_boot_argn("fuxiwlanaspm", &retain, sizeof(retain));
	if (wl_pre == 5 && retain == 1) { wl_variant_invalid = true; }
	printf("FUXI WLAN: M6o seq mask %#x valid=%u (default off; stop/contain at 22)\n",
	    wl_seq_flags, !wl_variant_invalid);
	printf("FUXI WLAN: M6l pre=%s bhi=%s vec=%s valid=%u (variants stop at 22)\n",
	    wl_pre_names[wl_pre], sizes[wl_full], wl_vec_names[wl_vec], !wl_variant_invalid);
	printf("FUXI WLAN: M6k probe=%s (one BHI doorbell; diagnostic modes stop at 22)\n", wl_probe_names[wl_probe]);
	printf("FUXI WLAN: M6m fw=%s l1ss=%u valid=%u (stock means supplied input, not verified provenance)\n",
	    wl_fw_names[wl_fw], wl_pre == 5, !wl_variant_invalid);
	printf("FUXI WLAN: M6n cnss=%s valid=%u (PDC before step 1; stop at 22)\n",
	    wl_cnss_names[wl_cnss], !wl_variant_invalid);
}
#define WL_CNSS_IOVA 0xa0000000ull
#define WL_CNSS_IOVA_END 0xb0000000ull
#define WL_LEGACY_IOVA 0x10000000ull
#define WL_LEGACY_IOVA_END 0x11000000ull
_Static_assert((WL_CNSS_IOVA >> 30) == ((WL_CNSS_IOVA_END - 1) >> 30) &&
    (WL_LEGACY_IOVA >> 30) == ((WL_LEGACY_IOVA_END - 1) >> 30), "one L2 per pool");
_Static_assert(!((WL_CNSS_IOVA | WL_CNSS_IOVA_END | WL_LEGACY_IOVA |
    WL_LEGACY_IOVA_END) & ((1ull << 21) - 1)), "2-MiB pool alignment");
static uint64_t wl_iova_base = WL_CNSS_IOVA, wl_iova_end = WL_CNSS_IOVA_END;
static uint64_t wl_iova_next = WL_CNSS_IOVA, wl_cb_base, wl_gr1;
/* Xiaomi SM8550 arm-smmu + io-pgtable-fast: coherent walk = ISH/WB-WA;
 * fastmap cached leaves = OSH + MAIR 0xff. See docs-lanes/wlan-bt.md, M6g.
 * Keep EPD1/T1SZ/TG1 legal, TTBR0 4-KiB granule and 32-bit input. */
#define WL_CB_TCR (0x80a00000u | (3u << 12) | (1u << 10) | (1u << 8) | 32u)
/* HUPCF=1, CFCFG=0 (stall-disable), CFRE=0 (no-CFRE), M=1.
 * ASIDPNE/AFE/TRE retained as optional Linux-style hints. CFIE stays off:
 * this lane polls FSR and has no context-fault IRQ handler. Coherent SCTLR
 * bypass overrides only apply with M=0, when we keep bus mastering off. */
#define WL_CB_SCTLR 0x1107u
#define WL_DMA_PTE 0x60000000000e63ull /* XN, nG, AF, OSH, RW, NS, AttrIdx 0 */
static unsigned wl_slot, wl_cb, wl_smr_count, wl_cb_count, wl_pgshift;
static uint32_t wl_old_smr, wl_old_s2cr, wl_old_sctlr;
#define WL_CB_PROBE_MAX 8u
static bool wl_cb_unusable[128];
static unsigned wl_smmu_mismatches;
static uint32_t wl_route_cfg;
static unsigned wl_route_hash;
static bool wl_owned, wl_enabled, wl_route_owned, wl_route_installed;
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

/* Select once before the WLAN thread allocates anything. The experiment
 * changes only IOVAs; both boots use the CNSS coherency/fault profile. */
static void
wl_iova_configure(void)
{
	wl_probe_configure();
	int legacy = 0;
	(void)PE_parse_boot_argn("fuxiwlanoldiova", &legacy, sizeof(legacy));
	wl_iova_base = legacy == 1 ? WL_LEGACY_IOVA : WL_CNSS_IOVA;
	wl_iova_end = legacy == 1 ? WL_LEGACY_IOVA_END : WL_CNSS_IOVA_END;
	wl_iova_next = wl_iova_base;
	printf("FUXI WLAN: M6g IOVA %s pool [%#llx,%#llx) fuxiwlanoldiova=%d\n",
	    legacy == 1 ? "legacy" : "cnss", wl_iova_base, wl_iova_end, legacy);
}

static void
wl_sleep(unsigned ms)
{
	/* Yield this dedicated thread; root mount and other drivers must run. */
	assert_wait_timeout((event_t)&wl5_report, THREAD_UNINT, ms, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Independent QMP polling implementation. The only kernel QMP writer here is
 * this single WLAN thread; shim audio.c runs in a separate boot environment.
 * Public protocol/DT facts and pinned source references: docs/wlan-m6n.md.
 * Never overwrite a busy slot, clear an unacknowledged packet, or retry a
 * partially applied PDC table. Acknowledgement means consumption, not a
 * voltage readback. No security/fuse state is written. */
#define WL_AOSS_QMP 0x0c300000ull
#define WL_AOSS_IPCC 0x00408000ull

static int
wl_qmp_wait(unsigned offset, uint32_t expected)
{
	for (unsigned ms = 0; ms <= 1000; ms++) {
		uint32_t value = wl_prd(WL_AOSS_QMP + offset);
		if (fw_faults) { return 1; }
		if (value == expected) { return 0; }
		if (ms < 1000) { wl_sleep(1); }
	}
	printf("FUXI WLAN: M6n QMP timeout offset %#x expected %#x\n", offset, expected);
	return 1;
}

static int
wl_qmp_kick(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	if (fw_faults) { return 1; }
	wl_pwr(WL_AOSS_IPCC + 0xc, 0); /* AOP client 0 / QMP signal 0 */
	return fw_faults ? 1 : 0;
}

static int
wl_cnss_pdc(void)
{
	if (!wl_cnss) { return 0; }
	if (wl_variant_invalid || fw_faults || wl_pdc_attempted) { return 1; }
	wl_pdc_attempted = true;
	wl_pdc_acks = 0;
	if (wl_prd(WL_AOSS_QMP) != 0x4d41494c || wl_prd(WL_AOSS_QMP + 4) != 1) { return 2; }
	unsigned offset = wl_prd(WL_AOSS_QMP + 0x38), bytes = wl_prd(WL_AOSS_QMP + 0x34);
	if (fw_faults || (offset & 3) || offset < 0x40 || offset >= 0x400 ||
	    bytes < 68 || bytes > 0x400 - offset || wl_prd(WL_AOSS_QMP + offset)) { return 3; }
	uint32_t link = wl_prd(WL_AOSS_QMP + 0x28), channel = wl_prd(WL_AOSS_QMP + 0x30);
	if (fw_faults) { return 3; }
	if (link != 0xffff || channel != 0xffff) {
		uint32_t remote = wl_prd(WL_AOSS_QMP + 0xc);
		if (fw_faults || (remote && remote != 0xffff && remote != 0xffff0000)) { return 4; }
		wl_pwr(WL_AOSS_QMP + 0x10, remote);
		wl_pwr(WL_AOSS_QMP + 0x24, 0xffff);
		if (wl_qmp_kick() || wl_qmp_wait(0x28, 0xffff)) { return 4; }
		wl_pwr(WL_AOSS_QMP + 0x2c, 0xffff);
		if (wl_qmp_kick() || wl_qmp_wait(0x14, 0xffff)) { return 4; }
		wl_pwr(WL_AOSS_QMP + 0x18, 0xffff);
		if (wl_qmp_kick() || wl_qmp_wait(0x30, 0xffff)) { return 4; }
	}
	if (wl_prd(WL_AOSS_QMP + 0xc) != 0xffff || wl_prd(WL_AOSS_QMP + 0x14) != 0xffff ||
	    fw_faults) { return 4; }
	/* KIWI autonomous power settings, encoded as protocol data, not driver
	 * code imported from CNSS. RF voltage pairs first, then route MX to BB,
	 * enable BB PDC and set its MX/CX up/down values. */
	const char *const rf[] = {"s4e", "s4g", "s6g"};
	const unsigned volts[][2] = {{916, 612}, {1316, 944}, {1864, 1820}, {976, 800}, {940, 420}};
	for (unsigned i = 0; i < 13; i++) {
		char msg[64] = {0};
		int n;
		if (i < 6 || i >= 9) {
			unsigned row = i < 6 ? i / 2 : 3 + (i - 9) / 2;
			unsigned down = i < 6 ? i & 1 : (i - 9) & 1;
			n = snprintf(msg, sizeof(msg), "{class: wlan_pdc, ss: %s, res: %s.v, %sval: %u}",
			    i < 6 ? "rf" : "bb", i < 6 ? rf[row] : row == 3 ? "s2g" : "s5g",
			    down ? "dwn" : "up", volts[row][down]);
		} else {
			n = snprintf(msg, sizeof(msg), "{class: wlan_pdc, ss: %s, res: %s, enable: %u}",
			    i == 8 ? "bb" : "rf", i == 6 ? "s2g.m" : i == 7 ? "s2g.v" : "pdc", i == 8 ? 1u : 0u);
		}
		if (n <= 0 || (unsigned)n >= sizeof(msg) || fw_faults ||
		    wl_prd(WL_AOSS_QMP + offset)) { return 5; }
		printf("FUXI WLAN: M6n PDC %u/13 %s\n", i + 1, msg);
		for (unsigned word = 0; word < 16; word++) {
			uint32_t value = 0;
			for (unsigned b = 0; b < 4; b++) { value |= (uint32_t)(uint8_t)msg[4 * word + b] << (8 * b); }
			if (fw_faults) { return 5; }
			wl_pwr(WL_AOSS_QMP + offset + 4 + 4 * word, value);
		}
		if (fw_faults) { return 5; }
		__asm__ volatile("dsb sy" ::: "memory");
		wl_pwr(WL_AOSS_QMP + offset, 64);
		uint32_t length = wl_prd(WL_AOSS_QMP + offset); /* drain posted writes */
		if (fw_faults || (length != 64 && length != 0) || wl_qmp_kick() || wl_qmp_wait(offset, 0)) { return 6; }
		wl_pdc_acks++;
	}
	printf("FUXI WLAN: M6n PDC consumed %u/13 before WLAN rails; AOP policy persists until reconfigured\n", wl_pdc_acks);
	return 0;
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
	if (!bytes || d->size < bytes || wl_iova_next < wl_iova_base ||
	    wl_iova_next >= wl_iova_end || d->size > wl_iova_end - wl_iova_next) { return 1; }
	kern_return_t kr = kmem_alloc(kernel_map, &d->va, d->size,
	    KMA_DATA | KMA_ZERO | KMA_NOPAGEWAIT, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS) {
		printf("FUXI WLAN: wired DMA allocation failed %d\n", kr); return 1;
	}
	for (vm_size_t off = 0; off < d->size; off += PAGE_SIZE) {
		uint64_t pa = wl_phys(d->va + off);
		if (!pa || pa >= (1ull << 36)) { return 2; }
		if (d == &wl_image && wl_probe == WL_PROBE_NC &&
		    pmap_cache_attributes((ppnum_t)(pa >> PAGE_SHIFT)) != VM_WIMG_DEFAULT) { return 3; }
	}
	if (d == &wl_image && wl_probe == WL_PROBE_NC) {
		/* Change ALL CPU aliases through pmap, including the physical aperture.
		 * WCOMB is Normal NC (MAIR 0x44); Device mappings can fault memcpy.
		 * Do this before VFS writes the image, restore only after containment. */
		d->nc = true;
		for (vm_size_t off = 0; off < d->size; off += PAGE_SIZE) {
			ppnum_t pn = (ppnum_t)(wl_phys(d->va + off) >> PAGE_SHIFT);
			pmap_set_cache_attributes(pn, VM_WIMG_WCOMB);
			if (pmap_cache_attributes(pn) != VM_WIMG_WCOMB) { return 3; }
		}
		printf("FUXI WLAN: M6k image CPU mapping Normal-NC WCOMB all aliases bytes %#llx\n", (uint64_t)d->size);
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

/* Explicit architectural clean+invalidate to PoC, regardless of CB attributes.
 * CTR_EL0.DminLine is log2(words); never assume an Apple cache-line size. */
static void
wl_dma_civac(struct wl_dma *d)
{
	uint64_t ctr = 0;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	vm_size_t line = (vm_size_t)4 << ((ctr >> 16) & 15);
	for (vm_size_t off = 0; off < d->size; off += line) {
		vm_offset_t va = d->va + off;
		__asm__ volatile("dc civac, %0" :: "r"(va) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
wl_probe_publish(void)
{
	if (wl_probe != WL_PROBE_CIVAC) { return; }
	wl_dma_civac(&wl_image); wl_dma_civac(&wl_vectors);
	wl_dma_civac(&wl_contexts); wl_dma_civac(&wl_tables);
	if (wl_rddm.va) { wl_dma_civac(&wl_rddm); wl_dma_civac(&wl_rxvectors); }
	printf("FUXI WLAN: M6k DC CIVAC + DSB SY image/vectors/contexts/tables before BHI doorbell\n");
}

static void
wl_dma_free(struct wl_dma *d)
{
	if (d->va) {
		if (d->nc) {
			for (vm_size_t off = 0; off < d->size; off += PAGE_SIZE) {
				pmap_set_cache_attributes((ppnum_t)(wl_phys(d->va + off) >> PAGE_SHIFT), VM_WIMG_DEFAULT);
			}
		}
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

/* The only caller is the dedicated bring-up thread. Save/restore the dynamic
 * window for every access, including fault paths; never leave a log window set.
 * CNSS pci.c/reg.h: WINDOW_SHIFT=19, mask=0x3f, start=0x80000. */
static int
wl_soc_read(uint32_t off, uint32_t *val)
{
	*val = ~0u;
	if (fw_faults || (off & 3) || off >= 0x02000000) { return 1; }
	uint32_t saved = wl_mrd(0x310c), win = (saved & ~0x3fu) | 0x40000000 | (off >> 19);
	if (saved == ~0u || fw_faults) { return 1; }
	wl_mwr(0x310c, win);
	int e = wl_mrd(0x310c) != win || fw_faults;
	if (!e) { *val = wl_prd(WL_BAR0 + 0x80000ull + (off & 0x7ffff)); }
	/* Containment writes remain possible after a latched synchronous fault. */
	if (wl_contain_wr(WL_BAR0 + 0x310c, saved) || wl_mrd(0x310c) != saved) { e = 1; }
	return e || fw_faults;
}

/* Independent register transactions; behavioral references and chip-specific
 * addresses are linked in docs/wlan-m6l.md. Preserve/verify the dynamic window
 * even on an exception. No raw arbitrary-address boot argument is accepted. */
static int
wl_soc_write(uint32_t off, uint32_t value, bool verify)
{
	if (fw_faults || (off & 3) || off >= 0x02000000) { return 1; }
	uint32_t saved = wl_mrd(0x310c), win = (saved & ~0x3fu) | 0x40000000 | (off >> 19);
	if (saved == ~0u || fw_faults) { return 1; }
	wl_mwr(0x310c, win);
	int e = wl_mrd(0x310c) != win || fw_faults;
	if (!e) {
		uint64_t pa = WL_BAR0 + 0x80000ull + (off & 0x7ffff);
		wl_pwr(pa, value);
		e = fw_faults || (verify && wl_prd(pa) != value);
	}
	if (wl_contain_wr(WL_BAR0 + 0x310c, saved) || wl_mrd(0x310c) != saved) { e = 1; }
	printf("FUXI WLAN: M6l SOC write [%#x] %#x %s window restore checked\n", off, value, e ? "FAILED" : "ok");
	return e || fw_faults;
}

static int
wl_pre_bhi(void)
{
	if (!wl_pre) { return 0; }
	if (wl_probe_fired || fw_faults || wl_prd(WL_EPCFG) != 0x110717cb ||
	    ((wl5_report[16] >> 8) & 15) != 2 || (wl_prd(WL_EPCFG + 4) & 4) ||
	    wl_mrd(wl_bhi + 0x28) != 0) { return 1; }
	uint32_t v, cookie, cause;
	printf("FUXI WLAN: M6l pre-BHI %s begin BME off EE PBL\n", wl_pre_names[wl_pre]);
	if (wl_pre == 4) {
		wl_mwr(0x3004, 1); wl_sleep(5);
		if (fw_faults || wl_mrd(0x3004) != 1) { return 1; }
	}
	if (wl_pre == 3) {
		if (wl_soc_read(0x01e081b0, &v) || v == ~0u) { return 1; }
		if (v != 0x111 && wl_soc_write(0x01e081b0, 0x111, true)) { return 1; }
		/* WCN7850 corrected hot-reset register, NOT v6.6's QCN9274 address. */
		if (wl_soc_read(0x01e40304, &v) || v == ~0u ||
		    wl_soc_write(0x01e40304, v | 0x10, true)) { return 1; }
		wl_sleep(5);
		if (wl_soc_write(0x01e08228, ~0u, false)) { return 1; } /* W1C */
	}
	if (wl_pre == 1 || wl_pre == 3) {
		if (wl_soc_read(0x01f8031c, &v) || v == ~0u) { return 1; }
		unsigned mask = wl_pre == 1 ? 5 : 4; /* CNSS vs ath12k */
		printf("FUXI WLAN: M6l QFPROM before %#x clear mask %#x\n", v, mask);
		if (wl_soc_write(0x01f8031c, v & ~mask, true)) { return 1; }
	}
	if (wl_pre == 3) {
		const unsigned clear[] = {0x3c, 0x44, 0x70, 0x78};
		for (unsigned i = 0; i < 4; i++) {
			wl_mwr(wl_bhie + clear[i], 0);
			if (wl_mrd(wl_bhie + clear[i]) || fw_faults) { return 1; }
		}
	}
	if (wl_pre == 2 || wl_pre == 3) {
		if (wl_soc_read(0x01f80500, &cookie) || cookie == ~0u ||
		    wl_soc_read(0x01f80504, &v) || v == ~0u) { return 1; }
		printf("FUXI WLAN: M6l warm cookie %#x entry %#x\n", cookie, v);
		wl_sleep(10);
		if (wl_soc_write(0x01f80504, 0, true)) { return 1; }
		wl_sleep(10);
		if (wl_soc_read(0x01f8060c, &cause) || cause == ~0u) { return 1; } /* read-clear */
		printf("FUXI WLAN: M6l warm entry cleared reset cause %#x (read-clear)\n", cause);
	}
	if (wl_pre == 3) {
		v = wl_mrd(0x3008);
		if (v == ~0u || fw_faults) { return 1; }
		wl_mwr(0x3008, v | 1); wl_sleep(10);
		/* Always try to release our reset even if the intervening read faults. */
		if (wl_contain_wr(WL_BAR0 + 0x3008, v & ~1u)) { return 1; }
		wl_sleep(10);
		if (wl_mrd(0x3008) == ~0u || (wl_mrd(0x3008) & 1) || fw_faults) { return 1; }
		wl_mwr(0x38, 2); wl_sleep(10);
		for (unsigned i = 0; i < 40 && (wl_mrd(0x38) & 2) && !fw_faults; i++) { wl_sleep(25); }
		if (fw_faults || (wl_mrd(0x38) & 2) || (wl_mrd(0x48) & 0xff04) ||
		    wl_mrd(0x28) != wl_bhi || wl_mrd(0x2c) != wl_bhie) { return 1; }
	}
	if (fw_faults || wl_mrd(wl_bhi + 0x28) != 0 || (wl_prd(WL_EPCFG + 4) & 4)) { return 1; }
	printf("FUXI WLAN: M6l pre-BHI %s complete BME off EE PBL\n", wl_pre_names[wl_pre]);
	return 0;
}

/* CNSS KIWI bootloader diagnostics only, <=27 words, no arbitrary SRAM dump.
 * In PBL the DBG tuple may still be the input DMA address/size, not an SBL log. */
static void
wl_boot_logs(void)
{
	uint32_t ee = wl_mrd(wl_bhi + 0x28);
	if (fw_faults || (ee != 0 && ee != 1)) {
		printf("FUXI WLAN: M6f boot logs skipped EE %#x faults %u\n", ee, fw_faults); return;
	}
	const uint32_t regs[] = {0x01b000f8, 0x01e22b34, 0x01a10008, 0x01e0e238, 0x01e0e23c};
	const char *names[] = {"TCSR_PBL_LOGGING", "PBL_WLAN_BOOT_CFG", "KIWI_PBL_BOOTSTRAP_STATUS",
	    "PCIE_BHI_ERRDBG2", "PCIE_BHI_ERRDBG3"};
	uint32_t v[5];
	for (unsigned i = 0; i < 5; i++) {
		if (wl_soc_read(regs[i], &v[i])) {
			printf("FUXI WLAN: M6f boot log access failed reg %#x; window restored/containment attempted\n", regs[i]); return;
		}
		printf("FUXI WLAN: M6f %s [%#x] %#x\n", names[i], regs[i], v[i]);
	}
	for (unsigned i = 0; i < 10; i++) {
		uint32_t value, addr = 0x01403d98 + 4 * i;
		if (wl_soc_read(addr, &value)) { return; }
		printf("FUXI WLAN: M6f PBL log [%#x] %#x\n", addr, value);
	}
	uint32_t start = v[3], size = v[4];
	/* Clamp like CNSS, but require word alignment and subtraction-safe bounds. */
	if (size > 48) { size = 48; }
	if (start < 0x01400000 || start >= 0x01800000 || (start & 3) ||
	    !size || (size & 3) || size > 0x01800000 - start) {
		printf("FUXI WLAN: M6f SBL log skipped invalid SRAM tuple %#x/%#x (runtime DBG %#x/%#x)\n",
		    start, v[4], wl_mrd(wl_bhi + 0x38), wl_mrd(wl_bhi + 0x3c)); return;
	}
	for (unsigned i = 0; i < size; i += 4) {
		uint32_t value;
		if (wl_soc_read(start + i, &value)) { return; }
		printf("FUXI WLAN: M6f SBL log [%#x] %#x\n", start + i, value);
	}
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

enum {
	WL_DIAG_STATUS_ONES = 1, WL_DIAG_BAR_LOST = 2, WL_DIAG_CONFIG_LOST = 4,
	WL_DIAG_LINK_LOST = 8, WL_DIAG_DECODE_CHANGED = 16, WL_DIAG_PERST = 32,
	WL_DIAG_NON_D0 = 64, WL_DIAG_PBL_ERROR = 128, WL_DIAG_MMIO_FAULT = 256,
	WL_DIAG_CAP_INVALID = 512,
	WL_DIAG_TRANSPORT = WL_DIAG_BAR_LOST | WL_DIAG_CONFIG_LOST | WL_DIAG_LINK_LOST |
	    WL_DIAG_DECODE_CHANGED | WL_DIAG_PERST | WL_DIAG_NON_D0 | WL_DIAG_MMIO_FAULT
};

/* M6e diagnostics are read-only. Missing capabilities use ~0, malformed
 * lists set a separate flag; neither is mistaken for a D3/AER indication. */
static unsigned
wl_diag_cap(uint64_t cfg, unsigned id, bool ext, uint32_t *flags)
{
	unsigned p = ext ? 0x100 : wl_prd(cfg + 0x34) & 255;
	for (unsigned n = 0; p && n < 48; n++) {
		if ((p & 3) || p < (ext ? 0x100u : 0x40u) || p > (ext ? 0xffcu : 0xfcu)) { break; }
		uint32_t v = wl_prd(cfg + p);
		if (v == ~0u) { break; }
		if (!v) { return 0; }
		if ((v & (ext ? 0xffffu : 255u)) == id) {
			unsigned bytes = ext ? 0x34 : id == 0x10 ? 0x14 : 8;
			if (p + bytes > (ext ? 0x1000u : 0x100u)) { break; }
			return p;
		}
		p = ext ? (v >> 20) & 0xfff : (v >> 8) & 255;
		if (!p) { return 0; }
	}
	if (p) { *flags |= WL_DIAG_CAP_INVALID; }
	return 0;
}

static uint32_t
wl_bhi_diag(unsigned sample)
{
	uint32_t *r = wl5_report, f = 0;
	wl5_status(22); /* raw STATUS, ERRCODE, ERRDBG1..3, EE, MHI, EP cmd */
	r[108] = 0x4d366500; /* M6e extension, r[109..123] is latest sample */
	r[110] = wl_prd(WL_PARF + 0x1b0); /* LTSSM */
	r[111] = wl_prd(WL_DBI + 0xf28); /* ELBI_SYS_STTS (ELBI + 8) */
	unsigned cap = wl_diag_cap(WL_DBI, 0x10, false, &f);
	r[112] = cap ? wl_prd(WL_DBI + cap + 0x10) : ~0u;
	r[113] = wl_prd(WL_DBI + 4);
	r[114] = wl_prd(WL_EPCFG);
	r[115] = wl_prd(WL_EPCFG + 0x10); r[116] = wl_prd(WL_EPCFG + 0x14);
	r[117] = r[118] = r[119] = r[120] = r[121] = r[122] = ~0u;
	if (r[114] != ~0u && r[114]) {
		cap = wl_diag_cap(WL_EPCFG, 1, false, &f);
		if (cap) { r[117] = wl_prd(WL_EPCFG + cap + 4); }
		cap = wl_diag_cap(WL_EPCFG, 1, true, &f);
		if (cap) {
			r[121] = wl_prd(WL_EPCFG + cap + 4);
			r[122] = wl_prd(WL_EPCFG + cap + 0x10);
		}
	}
	cap = wl_diag_cap(WL_DBI, 1, true, &f);
	if (cap) {
		r[118] = wl_prd(WL_DBI + cap + 4); r[119] = wl_prd(WL_DBI + cap + 0x10);
		r[120] = wl_prd(WL_DBI + cap + 0x30);
	}
	r[123] = wl_prd(TS_TLMM + 0x1000ull * 94 + 4);
	uint32_t len = wl_mrd(0), cfg = wl_mrd(0x10);
	if (r[67] == ~0u) { f |= WL_DIAG_STATUS_ONES; }
	if (len == ~0u || cfg == ~0u || r[64] == ~0u) { f |= WL_DIAG_BAR_LOST; }
	if (r[114] != 0x110717cb) { f |= WL_DIAG_CONFIG_LOST; }
	if (r[110] == ~0u || r[111] == ~0u || !(r[111] & 0x400) ||
	    r[112] == ~0u || !(r[112] & (1u << 29))) { f |= WL_DIAG_LINK_LOST; }
	if (r[73] == ~0u || !(r[73] & 2) || (r[115] & ~15u) != WL_BAR0 || r[116]) { f |= WL_DIAG_DECODE_CHANGED; }
	if (r[123] == ~0u || !(r[123] & 2)) { f |= WL_DIAG_PERST; }
	if (r[117] != ~0u && (r[117] & 3)) { f |= WL_DIAG_NON_D0; }
	if (fw_faults) { f |= WL_DIAG_MMIO_FAULT; }
	/* All-ones STATUS alone is ambiguous. Require independent transport and
	 * PBL/error evidence before calling it a device-reported error. */
	if (!(f & WL_DIAG_TRANSPORT) && r[64] == 0 && (r[67] >> 30) == 3 && r[68] && r[68] != ~0u) { f |= WL_DIAG_PBL_ERROR; }
	r[109] = f;
	printf("FUXI WLAN: M6e BHI sample %u STATUS raw %#x field %u ERRCODE %#x ERRDBG %#x/%#x/%#x EXECENV %#x REGLEN %#x MHICFG %#x flags %#x: %s\n",
	    sample, r[67], r[67] >> 30, r[68], r[69], r[70], r[71], r[64], len, cfg, f,
	    (f & WL_DIAG_TRANSPORT) ? "transport inaccessible/changed" : (f & WL_DIAG_PBL_ERROR) ? "PBL error evidence" : "transport readable; BHI result unclassified");
	printf("FUXI WLAN: M6e link sample %u PARF_LTSSM %#x ELBI_SYS_STTS %#x RC_LNKCTLSTA %#x RCcmd/status %#x EPid %#x EPcmd/status %#x BAR0 %#x/%#x PMCSR %#x PERST_IO %#x\n",
	    sample, r[110], r[111], r[112], r[113], r[114], r[73], r[115], r[116], r[117], r[123]);
	printf("FUXI WLAN: M6e AER sample %u RC uncor/cor/root %#x/%#x/%#x EP uncor/cor %#x/%#x (absent/unreadable = all-ones)\n",
	    sample, r[118], r[119], r[120], r[121], r[122]);
	return f;
}

/* ath12k disables ASPM during download. Preserve Link Status W1C bits by
 * writing only the low control half. M6m additionally tests disabling all
 * four L1SS enables (PCI-PM as well as ASPM), without PHY workarounds. */
static int
wl_download_aspm(void)
{
	int retain = 0;
	(void)PE_parse_boot_argn("fuxiwlanaspm", &retain, sizeof(retain));
	if (wl_pre == 5 && (retain == 1 || fw_faults ||
	    wl_prd(WL_EPCFG) != 0x110717cb || (wl_prd(WL_EPCFG + 4) & 4))) { return 3; }
	const uint64_t cfg[] = {WL_EPCFG, WL_DBI};
	for (unsigned i = 0; i < 2; i++) {
		uint32_t flags = 0;
		unsigned cap = wl_diag_cap(cfg[i], 0x10, false, &flags);
		unsigned l1ss = wl_diag_cap(cfg[i], 0x1e, true, &flags);
		if (flags || fw_faults || !cap) { return 1; }
		uint32_t old = wl_prd(cfg[i] + cap + 0x10) & 0xffff;
		uint32_t want = retain == 1 ? old : old & ~3u;
		if (old != want) { wl_pwr(cfg[i] + cap + 0x10, want); }
		uint32_t now = wl_prd(cfg[i] + cap + 0x10) & 0xffff;
		printf("FUXI WLAN: M6k %s ASPM LNKCTL %#x -> %#x L1SS_CTL1 %#x retained %u\n",
		    i ? "RC" : "EP", old, now, l1ss ? wl_prd(cfg[i] + l1ss + 8) : 0, retain == 1);
		if (fw_faults || now != want) { return 2; }
		if (wl_pre == 5) {
			if (!l1ss) {
				printf("FUXI WLAN: M6m %s L1SS absent\n", i ? "RC" : "EP");
				continue;
			}
			uint32_t before = wl_prd(cfg[i] + l1ss + 8);
			if (before == ~0u || fw_faults) { return 3; }
			uint32_t disabled = before & ~0xfu;
			if (before != disabled) { wl_pwr(cfg[i] + l1ss + 8, disabled); }
			uint32_t after = wl_prd(cfg[i] + l1ss + 8);
			printf("FUXI WLAN: M6m %s L1SS_CTL1 %#x -> %#x mask 0xf BME off\n",
			    i ? "RC" : "EP", before, after);
			if (after != disabled || fw_faults) { return 3; }
		}
	}
	return 0;
}

/* Linux qcom_pcie_config_sid_1_9_0: CRC8/MSB over big-endian BDF,
 * BDF[31:16] | (SID - first iommu-map SID)[15:8] | NEXT[7:0].
 * PCIe0's DT base is 0x1400, so offsets 0/1 mean SIDs 0x1400/0x1401.
 * RID 0 hashes to 0 and encodes as zero; RID 0x100 hashes to 0x15.
 * Own the whole table only while bypassed and with no other user evident. */
#define WL_RC_ENTRY 0x01000100u
static unsigned
wl_rc_hash(uint16_t rid)
{
	unsigned hash = 0;
	for (unsigned byte = 0; byte < 2; byte++) {
		hash ^= (rid >> (byte ? 0 : 8)) & 255;
		for (unsigned bit = 0; bit < 8; bit++) {
			hash = ((hash << 1) ^ ((hash & 0x80) ? 7 : 0)) & 255;
		}
	}
	return hash;
}

/* M6h upstream DMA audit. Sources and limits: docs/wlan-m6h.md.
 * The DT iATU aperture is 0x1000 bytes: eight unrolled 0x200-byte pairs,
 * inbound at +0x100. Read only within that aperture, including unused slots;
 * do not probe implementation by writing all-ones as Linux detection does.
 * Snapshots never clear PCI status/AER, nor alter route/ATU/clock state. */
#define WL_IB_SLOTS 8u
#define WL_IB(n) (WL_DBI + 0x1100ull + (uint64_t)(n) * 0x200)
#define WL_GCC_PA 0x00100000ull
static const uint32_t wl_fabric_cb[] = {
	0x1003c, 0x10030, 0x6b02c, 0x6b020, 0x6b01c, 0x1004c, 0x10028
};

static void
wl_host_diag(const char *phase)
{
	uint32_t flags = 0, cs = wl_prd(WL_DBI + 4), sec = wl_prd(WL_DBI + 0x1c) >> 16;
	unsigned aer = wl_diag_cap(WL_DBI, 1, true, &flags);
	printf("FUXI WLAN: M6h %s RC cmd/status %#x BME %u abort STA/RTA/RMA %u/%u/%u SEC %#x abort STA/RTA/RMA %u/%u/%u MEM %#x PREF %#x upper %#x/%#x AER unc/root %#x/%#x capflags %#x AXI_RESP %#x/%#x\n",
	    phase, cs, (cs >> 2) & 1, (cs >> 27) & 1, (cs >> 28) & 1, (cs >> 29) & 1,
	    sec, (sec >> 11) & 1, (sec >> 12) & 1, (sec >> 13) & 1,
	    wl_prd(WL_DBI + 0x20), wl_prd(WL_DBI + 0x24), wl_prd(WL_DBI + 0x28), wl_prd(WL_DBI + 0x2c),
	    aer ? wl_prd(WL_DBI + aer + 4) : ~0u, aer ? wl_prd(WL_DBI + aer + 0x30) : ~0u,
	    flags, wl_prd(WL_DBI + 0x818), wl_prd(WL_DBI + 0x81c));
	printf("FUXI WLAN: M6h %s PARF SYS %#x PM %#x PHY %#x MHI_CLK %#x BYPASS %u HALT v1/v2 %#x/%#x Q2A %#x LTSSM %#x TYPE %#x NOSNOOP %#x\n",
	    phase, wl_prd(WL_PARF), wl_prd(WL_PARF + 0x20), wl_prd(WL_PARF + 0x40),
	    wl_prd(WL_PARF + 0x174), (wl_prd(WL_PARF + 0x174) >> 4) & 1,
	    wl_prd(WL_PARF + 0x178), wl_prd(WL_PARF + 0x1a8), wl_prd(WL_PARF + 0x1ac),
	    wl_prd(WL_PARF + 0x1b0), wl_prd(WL_PARF + 0x1000), wl_prd(WL_PARF + 0x3d4));
	printf("FUXI WLAN: M6h %s PARF ADDR DBI/SLV v1 %#x/%#x DBI v2 %#x:%08x SLV v2 %#x:%08x ATU %#x:%08x BDF_TRANSLATE %#x\n",
	    phase, wl_prd(WL_PARF + 0x168), wl_prd(WL_PARF + 0x16c),
	    wl_prd(WL_PARF + 0x354), wl_prd(WL_PARF + 0x350),
	    wl_prd(WL_PARF + 0x35c), wl_prd(WL_PARF + 0x358),
	    wl_prd(WL_PARF + 0x638), wl_prd(WL_PARF + 0x634), wl_prd(WL_PARF + 0x24c));
	unsigned hash = wl_rc_hash(0x100), nonzero = 0, mismatch = 0;
	uint32_t entry = 0, root = 0, digest = 2166136261u;
	for (unsigned i = 0; i < 256; i++) {
		uint32_t v = wl_prd(WL_PARF + 0x2000 + 4 * i);
		if (i == hash) { entry = v; }
		if (!i) { root = v; }
		nonzero += !!v;
		mismatch += v != (i == hash ? WL_RC_ENTRY : 0);
		digest = (digest ^ v) * 16777619u;
	}
	printf("FUXI WLAN: M6h %s SID RID 0x100 DTbase 0x1400 cfg %#x offset %#x hash %#x entry %#x BDF %#x SIDoff %#x NEXT %#x root %#x nonzero %u mismatch %u digest %#x\n",
	    phase, wl_prd(WL_PARF + 0x2c00), wl_prd(WL_PARF + 0x234), hash, entry,
	    entry >> 16, (entry >> 8) & 255, entry & 255, root, nonzero, mismatch, digest);
	uint32_t ib[WL_IB_SLOTS], enabled = 0;
	for (unsigned i = 0; i < WL_IB_SLOTS; i++) {
		ib[i] = wl_prd(WL_IB(i) + 4);
		if (ib[i] & (1u << 31)) { enabled |= 1u << i; }
	}
	printf("FUXI WLAN: M6h %s iATU IB slots 0..7 enabled %#x ctrl2 %#x/%#x/%#x/%#x/%#x/%#x/%#x/%#x IOVA %#llx\n",
	    phase, enabled, ib[0], ib[1], ib[2], ib[3], ib[4], ib[5], ib[6], ib[7], wl_image.iova);
	for (unsigned i = 0; i < WL_IB_SLOTS; i++) {
		if (!(enabled & (1u << i))) { continue; }
		uint64_t p = WL_IB(i);
		printf("FUXI WLAN: M6h %s iATU IB%u ctrl1/2 %#x/%#x BARmatch %u base %#x:%08x limit %#x:%08x target %#x:%08x\n",
		    phase, i, wl_prd(p), ib[i], (ib[i] >> 30) & 1,
		    wl_prd(p + 0x0c), wl_prd(p + 8), wl_prd(p + 0x20), wl_prd(p + 0x10),
		    wl_prd(p + 0x18), wl_prd(p + 0x14));
	}
	uint32_t cb[7], off = 0, ungated = 0;
	for (unsigned i = 0; i < 7; i++) {
		cb[i] = wl_prd(WL_GCC_PA + wl_fabric_cb[i]);
		if (cb[i] & (1u << 31)) { off |= 1u << i; }
		if ((cb[i] & (1u << 31)) && !(cb[i] & 2)) { ungated |= 1u << i; }
	}
	printf("FUXI WLAN: M6h %s GCC vote0/8 %#x/%#x CB aggre/cnoc/master/slave/q2a/ddrss/cfg %#x/%#x/%#x/%#x/%#x/%#x/%#x OFF %#x OFF_no_HWCG %#x\n",
	    phase, wl_prd(WL_GCC_PA + 0x52000), wl_prd(WL_GCC_PA + 0x52008),
	    cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], off, ungated);
	unsigned cap = wl_diag_cap(WL_DBI, 0x10, false, &flags);
	uint32_t link = cap ? wl_prd(WL_DBI + cap + 0x10) : 0;
	unsigned speed = (link >> 16) & 15, width = (link >> 20) & 63;
	unsigned peak = speed == 1 ? 250000 : speed == 2 ? 500000 : speed == 3 ? 984500 : 0;
	/* No BCM register reads: cmd-db gives RPMh command addresses, not MMIO.
	 * Firmware/other clients' aggregated votes cannot be inferred from GCC. */
	printf("FUXI WLAN: M6h %s ICC XNU_batches=%u Linux pcie-mem init 0/250000 link 0/%u kBps cpu-pcie 0/1 kBps Gen%u x%u BCM SN7/SH1/SH0/MC0/ACV/CN0 firmware_votes=unknown faults %u\n",
	    phase, wl_icc_acks, peak * width, speed, width, fw_faults);
}

/* SM8550 gcc clock drivers: existing M3 votes cover master/slave/Q2A,
 * aggregation and DDRSS; missing CNOC SF is vote8 bit6, ANOC config is
 * vote0 bit20. Add only those shared votes; never remove another client's
 * vote. Exactly 1 opts in. No PARF recipe from a different IP revision. */
static int
wl_fabric_prepare(void)
{
	int clocks = 0, iboff = 0;
	(void)PE_parse_boot_argn("fuxiwlanclk", &clocks, sizeof(clocks));
	(void)PE_parse_boot_argn("fuxiwlaniboff", &iboff, sizeof(iboff));
	printf("FUXI WLAN: M6h fixes fuxiwlanclk=%d fuxiwlaniboff=%d (only 1 enables)\n", clocks, iboff);
	if (fw_faults || !wl_route_installed || (wl_prd(WL_EPCFG + 4) & 4)) { return 1; }
	if (clocks == 1) {
		uint32_t v0 = wl_prd(WL_GCC_PA + 0x52000), v8 = wl_prd(WL_GCC_PA + 0x52008);
		wl_pwr(WL_GCC_PA + 0x52000, v0 | (1u << 20));
		wl_pwr(WL_GCC_PA + 0x52008, v8 | (1u << 6));
		uint32_t a0 = wl_prd(WL_GCC_PA + 0x52000), a8 = wl_prd(WL_GCC_PA + 0x52008);
		printf("FUXI WLAN: M6h clock votes %#x/%#x -> %#x/%#x retained for boot\n", v0, v8, a0, a8);
		if (fw_faults || !(a0 & (1u << 20)) || !(a8 & (1u << 6))) { return 2; }
		for (unsigned i = 0; i < 2; i++) {
			uint32_t off = i ? 0x10030 : 0x10028, v = 0;
			unsigned n;
			for (n = 0; n < 500; n++) {
				v = wl_prd(WL_GCC_PA + off);
				if (fw_faults || !(v & (1u << 31)) || (v & 2)) { break; }
				fw_delay(1);
			}
			printf("FUXI WLAN: M6h clock CB %#x value %#x polls %u\n", off, v, n);
			if (fw_faults || n == 500) { return 3; }
		}
	}
	if (iboff == 1) {
		for (unsigned i = 0; i < WL_IB_SLOTS; i++) {
			uint64_t p = WL_IB(i) + 4;
			uint32_t v = wl_prd(p);
			if (fw_faults) { return 4; }
			if (!(v & (1u << 31))) { continue; }
			wl_pwr(p, v & ~(1u << 31));
			uint32_t after = wl_prd(p);
			printf("FUXI WLAN: M6h iATU disable IB%u ctrl2 %#x -> %#x\n", i, v, after);
			if (fw_faults || after != (v & ~(1u << 31))) { return 4; }
		}
	}
	return !!fw_faults;
}

/* Read-only capability walk: never disable an existing user to claim a table. */
static int
wl_rc_irqs_busy(uint64_t cfg)
{
	unsigned p = wl_prd(cfg + 0x34) & 255;
	for (unsigned i = 0; p && i < 48; i++) {
		if (p < 0x40 || p > 0xfc || (p & 3)) { return 1; }
		uint32_t cap = wl_prd(cfg + p);
		if (((cap & 255) == 5 && (cap & (1u << 16))) ||
		    ((cap & 255) == 0x11 && (cap & (1u << 31)))) { return 1; }
		p = (cap >> 8) & 255;
	}
	return p || fw_faults;
}

static int
wl_rc_fail(unsigned reason, const char *name)
{
	wl5_report[25] = reason;
	printf("FUXI WLAN: RC route refused: %s (route reason %u; step 17 reason 5)\n", name, reason);
	return (int)reason;
}

static int
wl_rc_check(void)
{
	if (!wl_route_installed || !wl_route_owned ||
	    wl_prd(WL_PARF + 0x2c00) != (wl_route_cfg & ~1u) || wl_prd(WL_PARF + 0x234)) { return 1; }
	for (unsigned i = 0; i < 256; i++) {
		if (wl_prd(WL_PARF + 0x2000 + 4 * i) != (i == wl_route_hash ? WL_RC_ENTRY : 0)) { return 1; }
	}
	return !!fw_faults;
}

static int
wl_rc_route(void)
{
	wl_route_cfg = wl_prd(WL_PARF + 0x2c00);
	uint32_t offset = wl_prd(WL_PARF + 0x234);
	wl_route_hash = wl_rc_hash(0x100);
	uint32_t entry = wl_prd(WL_PARF + 0x2000 + 4 * wl_route_hash);
	wl5_report[22] = wl_route_cfg; wl5_report[23] = offset; wl5_report[24] = entry;
	printf("FUXI WLAN: RC RID 0x100 SID cfg %#x offset %#x hash %#x entry %#x\n",
	    wl_route_cfg, offset, wl_route_hash, entry);
	if (fw_faults || offset || wl_route_cfg == ~0u) { return wl_rc_fail(1, "RC config/offset inaccessible or unsupported"); }
	if (!(wl_route_cfg & 1)) { return wl_rc_fail(2, "RC table in use (bypass clear)"); }
	if (wl_rc_irqs_busy(WL_DBI) || wl_rc_irqs_busy(WL_EPCFG) || (wl_prd(WL_EPCFG + 4) & 4)) {
		return wl_rc_fail(3, "MSI/MSI-X or endpoint bus master active, or capability chain unproven");
	}
	/* DWC supports up to eight blocks of 32 MSI vectors. All must be idle. */
	for (unsigned i = 0; i < 8; i++) {
		if (wl_prd(WL_DBI + 0x828 + 12 * i)) { return wl_rc_fail(3, "RC MSI vectors enabled"); }
	}
	if (fw_faults || wl_prd(WL_PARF + 0x2c00) != wl_route_cfg) { return wl_rc_fail(1, "RC config changed before claim"); }
	wl_route_owned = true; /* includes partial scrub/install; never restore garbage */
	for (unsigned i = 0; i < 256; i++) { wl_pwr(WL_PARF + 0x2000 + 4 * i, 0); }
	for (unsigned i = 0; i < 256; i++) {
		if (wl_prd(WL_PARF + 0x2000 + 4 * i) || fw_faults) { return wl_rc_fail(4, "RC table zero readback"); }
	}
	printf("FUXI WLAN: M6b RC bypassed table zeroed 256 entries, readback verified; MSI idle\n");
	/* Root RID 0 -> offset 0 is already installed by the verified zero at slot 0. */
	wl_pwr(WL_PARF + 0x2000 + 4 * wl_route_hash, WL_RC_ENTRY);
	if (wl_prd(WL_PARF + 0x2000 + 4 * wl_route_hash) != WL_RC_ENTRY ||
	    wl_prd(WL_PARF + 0x2000) || fw_faults) { return wl_rc_fail(5, "RC mapping readback"); }
	/* Enable lookup last, preserving every other CFG bit. */
	wl_pwr(WL_PARF + 0x2c00, wl_route_cfg & ~1u);
	if (wl_prd(WL_PARF + 0x2c00) != (wl_route_cfg & ~1u) || fw_faults) { return wl_rc_fail(5, "RC bypass clear readback"); }
	wl_route_installed = true;
	printf("FUXI WLAN: M6b RC route installed: RID 0 -> DT SID 0x1400 offset 0 slot 0; RID 0x100 -> DT SID 0x1401 offset 1 slot %#x entry %#x; bypass clear last\n", wl_route_hash, WL_RC_ENTRY);
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
	wl_smmu_mismatches = 0;
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
	    ((r[6] >> 16) & 255) > wl_cb_count || ((r[6] >> 24) & 3) == 2 || ((r[7] >> 8) & 15) > 5 ||
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

/* SMMUv2 IHI0062D.c 10.2 / 16.5: masks for our Non-secure AArch64
 * stage-1 format. Ignore undefined/RO fields, not failures of writable bits.
 * MMU-500 integrations can hardwire TCR2.TBI[1:0] to one (0x60).
 * SCTLR.AFE/TRE are UNK/SBOP in long-descriptor formats. ASIDPNE is an
 * optional hint (MMU-500 integrations can read zero); unique ASIDs are checked
 * across all enabled banks, so correctness never relies on that hint.
 * ACTLR is untouched. */
#define WL_CB_SCTLR_MASK 0x7fdfc7f9u /* no reserved [31,21,13,11], ASIDPNE, AFE/TRE */
#define WL_CB_TCR_MASK   0xffffffbfu /* AArch64: only bit 6 reserved */
#define WL_CB_TTBR_LO_MASK 0xffffffe0u /* TnSZ=32, 4-KiB granule: x=5 */
#define WL_CB_MAIR_MASK  0xffffffffu /* all eight attribute bytes are writable */

static uint32_t
wl_tcr2_mask(void)
{
	uint32_t mask = 0x38017; /* SEP, AS, PASize[2:0]; no TBI or Secure NSCFG */
	if (wl5_report[7] & (1u << 28)) { mask |= 0x300; } /* HADS */
	unsigned hafdbs = (wl5_report[6] >> 24) & 3;
	if (hafdbs & 1) { mask |= 0x400; } /* HA */
	if (hafdbs == 3) { mask |= 0x800; } /* HD */
	return mask;
}

static uint32_t
wl_ttbr_hi_mask(void)
{
	static const unsigned bits[] = {32, 36, 40, 42, 44, 48};
	unsigned ias = wl5_report[7] & 15, oas = (wl5_report[7] >> 4) & 15;
	/* Probe already bounded both encodings; S1/S2-bypass uses the smaller. */
	return 0xffff0000u | (uint32_t)(((1ull << bits[MIN(ias, oas)]) - 1) >> 32);
}

/* Offsets include GR1/CB page selection. Log the complete raw read even on
 * success, so ignored bits remain visible. W1C uses its own expected value;
 * command registers use completion polling, never equality-to-write checks. */
static int
wl_smmu_verify(const char *name, uint64_t pa, uint32_t written,
    uint32_t readback, uint32_t expected, uint32_t mask)
{
	bool mismatch = fw_faults || (readback & mask) != (expected & mask);
	if (!mismatch || wl_smmu_mismatches++ < 16) {
		printf("FUXI WLAN: M6d SMMU %s CB %u %s offset %#x wrote %#x read %#x mask %#x faults %u\n",
		    mismatch ? "mismatch" : "readback", wl_cb, name,
		    (unsigned)(pa - WL_SMMU), written, readback, mask, fw_faults);
	}
	return mismatch;
}

static int
wl_smmu_wr(const char *name, uint64_t pa, uint32_t v, uint32_t mask)
{
	wl_pwr(pa, v);
	uint32_t readback = wl_prd(pa);
	return wl_smmu_verify(name, pa, v, readback, v, mask);
}

/* Return 0 only for a presently disabled, unreferenced stage-1 candidate with
 * an unused ASID. 1 skips that bank; -1 aborts on shared ownership/access change.
 * Re-read the entire stream table before EVERY probe, not just the saved slot.
 * This driver never performs Linux's destructive global reset/quirk probes. */
static int
wl_smmu_candidate(unsigned candidate, uint32_t *ctl)
{
	if (fw_faults || wl_prd(WL_SMMU) != wl5_report[4] || wl_rc_check() ||
	    (wl_prd(WL_EPCFG + 4) & 4) ||
	    wl_prd(WL_SMMU + 0x800 + 4 * wl_slot) != wl_old_smr ||
	    wl_prd(WL_SMMU + 0xc00 + 4 * wl_slot) != wl_old_s2cr) { return -1; }
	bool referenced = false, asid_used = false;
	for (unsigned i = 0; i < wl_smr_count; i++) {
		uint32_t smr = wl_prd(WL_SMMU + 0x800 + 4 * i);
		uint32_t s2cr = wl_prd(WL_SMMU + 0xc00 + 4 * i);
		if (fw_faults || smr == ~0u || s2cr == ~0u ||
		    ((smr & 0x80000000u) &&
		    (((smr ^ 0x1401) & ~((smr >> 16) & 0x7fff) & 0xffff) == 0))) { return -1; }
		if ((s2cr & 255) == candidate) { referenced = true; }
	}
	*ctl = wl_prd(wl_cb_addr(candidate));
	for (unsigned i = 0; i < wl_cb_count; i++) {
		if (wl_prd(wl_cb_addr(i)) & 1) {
			if ((wl_prd(wl_cb_addr(i) + 0x24) >> 16) == candidate ||
			    (wl_prd(wl_cb_addr(i) + 0x2c) >> 16) == candidate) { asid_used = true; }
		}
	}
	if (fw_faults) { return -1; }
	return referenced || asid_used || *ctl == ~0u || (*ctl & 1) ||
	       candidate < ((wl5_report[6] >> 16) & 255) || wl_cb_unusable[candidate];
}

/* Zero TCR2 is a legal, non-enabling write, independent of ASID/TTBR format.
 * CONTEXTIDR does not hold the ASID in long-descriptor formats; choose
 * the configuration register we must program anyway.
 * A masked-zero readback is only a filter (RAZ/WI could pass); all configuration and
 * GR1 readbacks remain mandatory. Never retry a failure AFTER claiming a CB. */
static int
wl_smmu_claim_candidate(void)
{
	unsigned attempts = 0;
	for (unsigned i = wl_cb; i < wl_cb_count && attempts < WL_CB_PROBE_MAX; i++) {
		uint32_t ctl;
		int state = wl_smmu_candidate(i, &ctl);
		if (state < 0) { return 3; }
		if (state) { continue; }
		wl_cb = i;
		printf("FUXI WLAN: M6d CB probe %u/%u CB %u SCTLR %#x M=0 unreferenced; TCR2 zero\n",
		    ++attempts, WL_CB_PROBE_MAX, i, ctl);
		if (wl_smmu_wr("CB_TCR2 probe", wl_cb_addr(i) + 0x10, 0, wl_tcr2_mask())) {
			wl_cb_unusable[i] = true;
			printf("FUXI WLAN: M6d CB %u unusable; probe failed%s\n", i, fw_faults ? "; MMIO fault, abort" : "; next candidate");
			if (fw_faults) { return 6; }
			continue;
		}
		uint32_t now;
		if (wl_smmu_candidate(i, &now) || now != ctl) { return 3; }
		wl_old_sctlr = ctl;
		wl5_report[9] = i;
		printf("FUXI WLAN: M6d CB %u probe accepted; claim pending full CB/GR1 readbacks\n", i);
		return 0;
	}
	printf("FUXI WLAN: M6d CB probe exhausted %u candidates (limit %u); no bank claimed\n", attempts, WL_CB_PROBE_MAX);
	return 6;
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

/* 4-KiB AArch64 stage-1 tables: L1 root, L2, one L3 per 2 MiB of the pool.
 * Only allocated DMA buffers get leaves; page tables never get DMA mappings.
 * Both selectable pools fit within one L1 entry and are 2-MiB aligned. */
static uint64_t
wl_pt_pa(unsigned table)
{
	return wl_phys(wl_tables.va + table * 4096);
}

/* Reverse PA lookup is restricted to our wired allocations. Never dereference
 * an untrusted table PA via phystokv or invent physical contiguity at 16 KiB. */
static vm_offset_t
wl_owned_alias(uint64_t pa, unsigned bytes, bool tables_only)
{
	struct wl_dma *bufs[] = { &wl_tables, &wl_image, &wl_vectors, &wl_contexts, &wl_rddm, &wl_rxvectors, &wl_msi_sink };
	for (unsigned i = 0; i < (tables_only ? 1u : 7u); i++) {
		struct wl_dma *d = bufs[i];
		for (vm_size_t off = 0; d->va && off < d->size; off += PAGE_SIZE) {
			uint64_t base = wl_phys(d->va + off);
			if (pa >= base && pa - base < PAGE_SIZE && bytes <= PAGE_SIZE - (pa - base)) {
				return d->va + off + (vm_offset_t)(pa - base);
			}
		}
	}
	return 0;
}

static uint64_t
wl_hash_bytes(uint64_t h, const uint8_t *p, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++) { h = (h ^ p[i]) * 0x100000001b3ull; }
	return h;
}

static int
wl_bhi_mapping_check(void)
{
	const uint64_t mask = 0x0000000ffffff000ull, seed = 0xcbf29ce484222325ull;
	unsigned bytes = wl_bhi_bytes();
	uint64_t source = wl_hash_bytes(seed, (const uint8_t *)wl_image.va, bytes), mapped = seed;
	/* flush_dcache64 is clean+invalidate to PoC on this ARM64 target.
	 * Force the physical-page hash to reload after the source hash. */
	wl_dma_clean(&wl_image); wl_dma_clean(&wl_tables);
	uint64_t root = ((uint64_t)wl_prd(wl_cb_addr(wl_cb) + 0x24) << 32) | wl_prd(wl_cb_addr(wl_cb) + 0x20);
	unsigned pages = 0, bad = 0;
	/* Walk the live CB TTBR, not fixed virtual indexes used by the builder. */
	if ((root & mask) != wl_pt_pa(0) || fw_faults) { bad = 1; }
	for (unsigned off = 0; !bad && off < bytes; off += 4096) {
		uint64_t iova = wl_image.iova + off, table = root & mask, leaf = 0;
		const unsigned shifts[] = {30, 21, 12};
		for (unsigned level = 0; level < 3; level++) {
			vm_offset_t alias = wl_owned_alias(table, 4096, true);
			if (!alias) { bad = 2; break; }
			uint64_t desc = ((const uint64_t *)alias)[(iova >> shifts[level]) & 511];
			if (level == 2 && wl_probe == WL_PROBE_UNMAPPED) {
				if (desc) { bad = 6; }
				break;
			}
			if ((desc & 3) != 3) { bad = 3; break; }
			table = desc & mask;
			if (level == 2) { leaf = table; }
		}
		if (bad) { break; }
		if (wl_probe == WL_PROBE_UNMAPPED) { pages++; continue; }
		vm_offset_t alias = wl_owned_alias(leaf, 4096, false);
		if (!alias) { bad = 4; break; }
		mapped = wl_hash_bytes(mapped, (const uint8_t *)alias, MIN(4096, bytes - off)); pages++;
		if (leaf != wl_phys(wl_image.va + off)) { bad = 5; }
	}
	int e = bad || pages != (bytes + 4095) / 4096 || (wl_probe != WL_PROBE_UNMAPPED && source != mapped);
	if (wl_probe == WL_PROBE_UNMAPPED) {
		printf("FUXI WLAN: M6k hole IOVA %#llx+%#x invalid leaves %u/128 %s reason %u (live CB walk)\n",
		    wl_image.iova, WL_CHUNK, pages, e ? "FAIL" : "VERIFIED", bad);
		return e;
	}
	printf("FUXI WLAN: M6f CB %u BHI IOVA %#llx+%#x FNV1a64 source %016llx walk %016llx pages %u/%u %s reason %u (CPU view)\n",
	    wl_cb, wl_image.iova, bytes, source, mapped, pages, (bytes + 4095) / 4096, e ? "FAIL" : "MATCH", bad);
	return e;
}

static int
wl_smmu_populate(void)
{
	unsigned l3_count = (unsigned)((wl_iova_end - wl_iova_base) >> 21);
	uint64_t *root = (uint64_t *)wl_tables.va;
	uint64_t *l2 = (uint64_t *)(wl_tables.va + 4096);
	root[wl_iova_base >> 30] = wl_pt_pa(1) | 3;
	for (unsigned i = 0; i < l3_count; i++) {
		l2[((wl_iova_base >> 21) & 511) + i] = wl_pt_pa(2 + i) | 3;
	}
	struct wl_dma *bufs[] = { &wl_image, &wl_vectors, &wl_contexts, &wl_rddm, &wl_rxvectors, &wl_msi_sink };
	for (unsigned i = 0; i < 6; i++) {
		struct wl_dma *d = bufs[i];
		if (d == &wl_image && wl_probe == WL_PROBE_UNMAPPED) { continue; }
		for (vm_size_t off = 0; off < d->size; off += 4096) {
			uint64_t addr = d->iova + off;
			if (addr < wl_iova_base || addr >= wl_iova_end) { return 2; }
			unsigned n = (unsigned)((addr - wl_iova_base) >> 21);
			uint64_t *l3 = (uint64_t *)(wl_tables.va + (2 + n) * 4096);
			l3[(addr >> 12) & 511] = wl_phys(d->va + off) | WL_DMA_PTE;
		}
	}
	wl_dma_clean(&wl_tables);
	return fw_faults ? 1 : 0;
}

static int
wl_smmu_enable(void)
{
	if (wl_rc_check()) { return 3; }
	if (wl_enabled) {
		/* M6o installed a fault-only/sink mapping before MASTER. Add only
		 * owned buffers, publish leaves, invalidate cached faults, then prepare. */
		if (wl_smmu_fault(20) || fw_faults ||
		    wl_prd(WL_SMMU+0x800+4*wl_slot) != 0x80001401 ||
		    wl_prd(WL_SMMU+0xc00+4*wl_slot) != wl_cb ||
		    (wl_prd(wl_cb_addr(wl_cb))&WL_CB_SCTLR_MASK) != (WL_CB_SCTLR&WL_CB_SCTLR_MASK)) { return 5; }
		return wl_smmu_populate() || wl_smmu_sync(false) || wl_smmu_fault(20) ? 5 : 0;
	}
	unsigned l3_count = (unsigned)((wl_iova_end - wl_iova_base) >> 21);
	if (wl_dma_alloc(&wl_tables, (2 + l3_count) * 4096)) { return 1; }
	if (wl_smmu_populate()) { return 2; }
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
	if (fw_faults || wl_prd(WL_SMMU) != wl5_report[4] || wl_rc_check()) { return 3; }
	int claim = wl_smmu_claim_candidate();
	if (claim) { return claim; }
	uint64_t cb = wl_cb_addr(wl_cb), ttbr = wl_pt_pa(0) | ((uint64_t)wl_cb << 48);
	wl_owned = true; /* includes partial setup: abort must disable and sync */
	/* IRPTNDX is not required storage in v2; VMID16 is RES0 when disabled. */
	bool vmid16 = (wl5_report[4] & (1u << 31)) && (wl5_report[7] & (1u << 15));
	uint32_t cbar_mask = vmid16 ? 0x00ffff00 : 0x00ffffff;
	uint32_t cba2r_mask = vmid16 ? 0xffff0001 : 1; /* MONC ignored Non-secure */
	uint32_t ttbr_hi_mask = wl_ttbr_hi_mask();
	if (wl_smmu_wr("CB_SCTLR disable", cb, 0, WL_CB_SCTLR_MASK) ||
	    wl_smmu_wr("GR1_CBA2R", wl_gr1 + 0x800 + 4 * wl_cb, 1, cba2r_mask) || /* VA64 */
	    wl_smmu_wr("GR1_CBAR", wl_gr1 + 4 * wl_cb, 0x1f300, cbar_mask) || /* S1, S2 bypass */
	    wl_smmu_wr("CB_TCR2", cb + 0x10, 0x38011, wl_tcr2_mask()) || /* SEP upstream, AS, 36-bit PA */
	    wl_smmu_wr("CB_TCR", cb + 0x30, WL_CB_TCR, WL_CB_TCR_MASK) ||
	    wl_smmu_wr("CB_TTBR0_HI", cb + 0x24, (uint32_t)(ttbr >> 32), ttbr_hi_mask) ||
	    wl_smmu_wr("CB_TTBR0_LO", cb + 0x20, (uint32_t)ttbr, WL_CB_TTBR_LO_MASK) ||
	    wl_smmu_wr("CB_TTBR1_HI", cb + 0x2c, wl_cb << 16, ttbr_hi_mask) ||
	    wl_smmu_wr("CB_TTBR1_LO", cb + 0x28, 0, WL_CB_TTBR_LO_MASK) ||
	    wl_smmu_wr("CB_MAIR0", cb + 0x38, 0xff, WL_CB_MAIR_MASK) ||
	    wl_smmu_wr("CB_MAIR1", cb + 0x3c, 0, WL_CB_MAIR_MASK)) { return 4; }
	/* FSR is W1C only for our newly owned bank; preserve later DMA faults. */
	wl_pwr(cb + 0x58, 0xc00001fe);
	if (wl_smmu_verify("CB_FSR W1C", cb + 0x58, 0xc00001fe,
	    wl_prd(cb + 0x58), 0, 0xc00001fe)) { return 5; }
	if (wl_smmu_fault(20) || wl_smmu_sync(false) ||
	    wl_smmu_wr("CB_SCTLR enable", cb, WL_CB_SCTLR, WL_CB_SCTLR_MASK) ||
	    wl_smmu_wr("GR0_S2CR", WL_SMMU + 0xc00 + 4 * wl_slot, wl_cb, ~0u) ||
	    wl_smmu_wr("GR0_SMR", WL_SMMU + 0x800 + 4 * wl_slot, 0x80001401, ~0u)) { return 5; }
	wl_enabled = true;
	printf("FUXI WLAN: SMMU owned stage1 SMR %u CB %u TTBR %#llx IOVA %#llx-%#llx readback verified\n",
	    wl_slot, wl_cb, wl_pt_pa(0), wl_iova_base,
	    wl_iova_next - 1);
	printf("FUXI WLAN: M6g CB %u coherent walk TCR %#x IRGN0=1 ORGN0=1 SH0=3; DMA MAIR0 %#x SH=2; SCTLR %#x HUPCF=1 CFCFG=0 CFRE=0 CFIE=0\n",
	    wl_cb, wl_prd(cb + 0x30), wl_prd(cb + 0x38), wl_prd(cb));
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
	uint32_t cb_sid = wl_prd(wl_gr1 + 0x400 + 4 * wl_cb); /* CBFRSYNRA */
	wl5_report[80] = fsr; wl5_report[81] = lo; wl5_report[82] = hi;
	wl5_report[83] = syn; wl5_report[84] = syn1;
	wl5_report[85] = global; wl5_report[86] = sid;
	if (step >= 17 && step <= 28) { wl5_report[96 + step - 17] = fsr; }
	printf("FUXI WLAN: M6 step %u CB %u FSR %#x FAR %#x:%08x FSYNR %#x/%#x GFSR %#x GFSYNR1 %#x\n", step, wl_cb, fsr, hi, lo, syn, syn1, global, sid);
	printf("FUXI WLAN: M6k step %u CBFRSYNRA %#x SID %#x\n", step, cb_sid, cb_sid & 0xffff);
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
		uint64_t cb = wl_cb_addr(wl_cb);
		if (wl_contain_wr(cb, 0)) { return 1; }
		unsigned before = fw_faults;
		uint32_t readback = wl_prd(cb);
		printf("FUXI WLAN: M6d SMMU undo CB %u CB_SCTLR offset %#x wrote 0 read %#x mask %#x faults %u\n",
		    wl_cb, (unsigned)(cb - WL_SMMU), readback, WL_CB_SCTLR_MASK, fw_faults);
		if ((readback & WL_CB_SCTLR_MASK) || fw_faults != before) { return 1; }
		if (wl_smmu_sync(true)) { return 1; }
		if (wl_contain_wr(smr, wl_old_smr) || wl_prd(smr) != wl_old_smr) { return 1; }
		wl_enabled = wl_owned = false;
		printf("FUXI WLAN: SMMU undo SMR invalid, S2CR restored, CB disabled, TLBI synced\n");
	}
	if (wl_route_owned) {
		/* Restore bypass before touching any owned entry, including partial setup. */
		if (wl_contain_wr(WL_PARF + 0x2c00, wl_route_cfg) || wl_prd(WL_PARF + 0x2c00) != wl_route_cfg) { return 1; }
		wl_route_installed = false;
		for (unsigned i = 0; i < 256; i++) {
			uint64_t pa = WL_PARF + 0x2000 + 4 * i;
			if (wl_contain_wr(pa, 0) || wl_prd(pa)) { return 1; }
		}
		wl_route_owned = false;
		printf("FUXI WLAN: M6b RC undo bypass restored first, 256 owned entries zeroed\n");
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

/* M6o is original polling glue, using only the cited register constants.
 * Journal only host configuration, never W1C/status, DMA doorbells or BME.
 * Banked equalization entries carry their selector for bounded reverse undo. */
struct wl_seq_undo { uint64_t pa; uint32_t old, mask; unsigned bank; };
static struct wl_seq_undo wl_seq_undo[96];
static unsigned wl_seq_undos, wl_eq_bank;
static bool wl_cycle_powered;
static uint32_t wl_msi_saved[5];
static bool wl_msi_pvm, wl_msi_64;
/* MSI capability layout: KIWI reports 32-bit addressing with per-vector
 * masking (cap 0x010a7005 on device), so data/mask follow the low address. */
static unsigned wl_msi_data_off(void) { return wl_msi_64 ? 12 : 8; }
static unsigned wl_msi_mask_off(void) { return wl_msi_64 ? 16 : 12; }
static unsigned wl_msi_words(void) { return wl_msi_mask_off() / 4 + (wl_msi_pvm ? 1 : 0); }

static int
wl_m6o_write(const char *what, uint64_t pa, uint32_t value, uint32_t mask)
{
	printf("FUXI WLAN: M6o begin %s offset %#llx value %#x mask %#x\n", what, pa, value, mask);
	uint32_t old = wl_prd(pa);
	if (fw_faults || (old == ~0u && !(wl_msi_pvm && pa == WL_EPCFG+wl_msi_cap+wl_msi_mask_off())) || wl_seq_undos >= 96) { return 1; }
	uint32_t v = (old & ~mask) | (value & mask);
	/* PCI command/control shares a dword with W1C status. */
	if ((pa >= WL_EPCFG && pa < WL_EPCFG + 0x1000 && mask == 0xffff) ||
	    pa == WL_EPCFG+4 || pa == WL_DBI+0x78 || (wl_pm_cap && pa == WL_EPCFG+wl_pm_cap+4)) { v &= 0xffff; }
	if (wl_pm_cap && pa == WL_EPCFG+wl_pm_cap+4) { v &= ~0x8000u; } /* PME status W1C */
	bool host = (pa >= WL_PARF && pa < WL_PARF + 0x4000) ||
	    (pa >= WL_DBI && pa < WL_DBI + 0x1000);
	/* Cycle link control is contained by PERST, not restored to running. */
	if (host && pa != WL_PARF + 0x1b0 && pa != WL_DBI + 0xf24) {
		wl_seq_undo[wl_seq_undos++] = (struct wl_seq_undo){pa, old, mask,
		    pa == WL_DBI + 0x8a8 || pa == WL_DBI + 0x8ac || (pa == WL_DBI+0x890 && mask == 1) ? wl_eq_bank : 0};
	}
	wl_pwr(pa, v);
	uint32_t rb = wl_prd(pa);
	printf("FUXI WLAN: M6o %s offset %#llx write %#x read %#x mask %#x\n", what, pa, v, rb, mask);
	/* PME_TURNOFF self-clears once the message is sent; PM_STTS is checked next. */
	if (pa == WL_DBI + 0xf24) { return fw_faults ? 1 : 0; }
	return fw_faults || (rb & mask) != (v & mask);
}

static int
wl_m6o_host(unsigned step)
{
	if (!wl_seq_flags) { return 0; }
	if (wl_variant_invalid || fw_faults) { return 1; }
	if (step == 8) {
		if ((wl_seq_flags & WL_O_PARF) &&
		    (wl_m6o_write("PARF DBI_BASE_ADDR", WL_PARF+0x350, 0, ~0u) ||
		     wl_m6o_write("PARF SYS_CTRL", WL_PARF, 0x365e, ~0u) ||
		     wl_m6o_write("PARF SLV_ADDR_SPACE_SIZE", WL_PARF+0x358, 0x04000000, ~0u))) { return 1; }
		if ((wl_seq_flags & WL_O_HOSTIRQ) &&
		    (wl_m6o_write("PARF INT_ALL_MASK zero", WL_PARF+0x22c, 0, ~0u) ||
		     wl_m6o_write("PARF INT_ALL_MASK events", WL_PARF+0x22c, 0x3fc0d202, ~0u))) { return 1; }
	}
	if (step == 10 && (wl_seq_flags & WL_O_DBI)) {
		if (wl_m6o_write("PARF CFG_BITS_3", WL_PARF+0x2c4, 0x100, 0x101) ||
		    wl_m6o_write("DBI LANE_SKEW", WL_DBI+0x714, 0x20, 0x20)) { return 1; }
	}
	if (step == 12 && (wl_seq_flags & WL_O_DBI)) {
		if (wl_m6o_write("DBI N_FTS", WL_DBI+0x70c, 0x8000, 0x8000) ||
		    wl_m6o_write("DBI AUX_CLK_FREQ", WL_DBI+0xb40, 20, ~0u)) { return 1; }
	}
	if (step != 11) { return 0; }
	if (wl_seq_flags & WL_O_EQ) {
		unsigned speed = wl_prd(WL_DBI+0x7c) & 15;
		if (fw_faults || speed < 3 || speed > 4) { return 1; }
		if (wl_m6o_write("DBI GEN3_GEN2_CTRL", WL_DBI+0x80c, 0x100, 0x1f00)) { return 1; }
		for (unsigned gen = 3; gen <= speed; gen++) {
			wl_eq_bank = gen;
			if (wl_m6o_write("DBI RATE_SHADOW_SEL", WL_DBI+0x890, (gen-3)<<24, 0x03000000) ||
			    wl_m6o_write("DBI EQ preset vector", WL_DBI+0x8a8, 0, 0x00ffff00) ||
			    wl_m6o_write("DBI ZRXDC", WL_DBI+0x890, 0, 1) ||
			    wl_m6o_write("DBI EQ FMDC", WL_DBI+0x8ac, 0, 0x1f)) { return 1; }
		}
		wl_eq_bank = 0;
		if (wl_m6o_write("DBI RO write enable", WL_DBI+0x8bc, 1, 1) ||
		    wl_m6o_write("DBI Gen3 core preset", WL_DBI+0x154, 0x55555555, ~0u) ||
		    (speed >= 4 && wl_m6o_write("DBI Gen4 core preset", WL_DBI+0x188, 0x55555555, ~0u)) ||
		    wl_m6o_write("DBI RO write disable", WL_DBI+0x8bc, 0, 1)) { return 1; }
	}
	if ((wl_seq_flags & WL_O_DBI) &&
	    wl_m6o_write("DBI MRRS 4096", WL_DBI+0x78, 0x5000, 0x7000)) { return 1; }
	return 0;
}

/* Bounded standard PCI capability walk; reject malformed/cyclic lists. */
static unsigned
wl_m6o_cap(unsigned id)
{
	unsigned p = wl_prd(WL_EPCFG+0x34) & 255;
	uint64_t seen = 0;
	while (p) {
		if (p < 0x40 || p > 0xfc || (p & 3) || (seen & (1ull << (p/4)))) { return 0; }
		seen |= 1ull << (p/4);
		uint32_t v = wl_prd(WL_EPCFG+p);
		if (fw_faults || v == ~0u) { return 0; }
		if ((v & 255) == id) { return p; }
		p = (v >> 8) & 255;
	}
	return 0;
}

static int
wl_m6o_wait(uint64_t pa, uint32_t mask, uint32_t expected, unsigned ms)
{
	printf("FUXI WLAN: M6o begin wait %#llx mask %#x expected %#x limit %u ms\n", pa, mask, expected, ms);
	for (unsigned i = 0; i <= ms; i++) {
		uint32_t v = wl_prd(pa);
		if (fw_faults || v == ~0u) { return 1; }
		if ((v & mask) == expected) {
			printf("FUXI WLAN: M6o wait %#llx read %#x mask %#x after %u ms\n", pa, v, mask, i);
			return 0;
		}
		if (i < ms) { wl_sleep(1); }
	}
	printf("FUXI WLAN: M6o wait %#llx mask %#x expected %#x timeout %u ms\n", pa, mask, expected, ms);
	return 1;
}

/* Explicit cyclerails only: the original rail cycle froze the SoC once.
 * These six votes may be shared; plain cycle and its undo never alter them. */
static int
wl_m6o_rails(bool on)
{
	if (!(wl_seq_flags & WL_O_CYCLERAILS)) { return 0; }
	static const unsigned idx[] = {0, 4, 2, 1, 5, 3};
	static const unsigned mv[] = {1800, 1000, 980, 950, 1900, 1350};
	for (unsigned n = 0; n < 6; n++) {
		unsigned i = on ? n : 5-n, addr = wl3_report[20+idx[i]];
		static const unsigned expected[] = {0x40a00, 0x44000, 0x40400, 0x40200, 0x40600, 0x40500};
		if (addr != expected[i] || fw_faults) { return 1; }
		unsigned e = 0;
		if (on) {
			printf("FUXI WLAN: M6o begin rail voltage addr %#x mV %u\n", addr, mv[i]);
			e = rh_send(0, addr, mv[i]);
			if (e) { return 1; }
		}
		printf("FUXI WLAN: M6o begin rail enable addr %#x value %u\n", addr+4, on ? 1u : 0u);
		e = rh_send(0, addr+4, on ? 1 : 0);
		printf("FUXI WLAN: M6o WLAN rail %#x %s mV %u completion %#x\n", addr, on ? "on" : "off", mv[i], e);
		if (e) { return 1; }
	}
	return 0;
}

struct wl_seq_config { unsigned off[13]; uint32_t value[13]; };
static int
wl_m6o_config_save(struct wl_seq_config *cfg)
{
	unsigned pc = wl_m6o_cap(0x10);
	if (!pc || pc > 0xcc || ((wl_prd(WL_EPCFG+pc)>>16)&15) < 2) { return 1; }
	const unsigned off[] = {4, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x3c,
	    pc+8, pc+0x10, pc+0x28, pc+0x30};
	for (unsigned i = 0; i < 13; i++) {
		cfg->off[i] = off[i]; cfg->value[i] = wl_prd(WL_EPCFG+off[i]);
		if (fw_faults || cfg->value[i] == ~0u) { return 1; }
	}
	return 0;
}

static int
wl_m6o_config_restore(const struct wl_seq_config *cfg)
{
	/* BARs and controls first, saved command last, always MASTER clear. */
	for (unsigned i = 1; i < 13; i++) {
		uint32_t mask = i >= 9 ? 0xffff : i == 8 ? 255 : i == 1 ? 0xffff : ~0u;
		if (wl_m6o_write("restore config", WL_EPCFG+cfg->off[i], cfg->value[i], mask)) { return 1; }
	}
	return wl_m6o_write("restore command", WL_EPCFG+4, (cfg->value[0]&~4u)|0x402, 0xffff);
}

/* A trained controller taken down without reset can hang the NoC on DBI.
 * Keep every PCIe0 aperture closed until core reset + PHY ready. Then permit
 * only local DBI (not ELBI/iATU/config/BAR) for the downstream pre-training writes.
 * Close that exception on every resume exit; L0 alone opens all apertures. */
static bool wl_link_off, wl_dbi_ready;

static bool
wl_link_refused(uint64_t pa)
{
	if (!wl_link_off || pa < 0x60000000ull || pa >= 0x64000000ull) { return false; }
	if (wl_dbi_ready && pa < WL_DBI + 0xf20) { return false; }
	printf("FUXI WLAN: PA %#llx refused while the PCIe link is down\n", pa);
	return true;
}

static int
wl_m6o_cycle_link(bool up)
{
	if (!up) {
		wl_link_off = true; wl_dbi_ready = false;
		printf("FUXI WLAN: M6o cycle link down dwell 1 ms, no DBI access\n");
		wl_sleep(1);
		printf("FUXI WLAN: M6o cycle PARF LTSSM %#x after link down\n", wl_prd(WL_PARF+0x1b0));
		return fw_faults ? 1 : 0;
	}
	uint32_t seen[2] = {0, 0}, lt = 0;
	unsigned ms;
	printf("FUXI WLAN: M6o cycle wait PARF LTSSM L0 before DBI\n");
	printf("FUXI WLAN: M6o begin wait %#llx mask 0x3f expected 0x11 limit 1000 ms\n", WL_PARF+0x1b0);
	for (ms = 0; ms <= 1000; ms++) {
		lt = wl_prd(WL_PARF+0x1b0);
		if (fw_faults || lt == ~0u) { break; }
		unsigned state = lt & 0x3f;
		seen[state / 32] |= 1u << (state % 32);
		if (state == 0x11) { break; }
		if (ms < 1000) { wl_sleep(1); }
	}
	bool l0 = !fw_faults && lt != ~0u && (lt & 0x3f) == 0x11;
	if (l0) {
		printf("FUXI WLAN: M6o wait %#llx read %#x mask 0x3f after %u ms\n", WL_PARF+0x1b0, lt, ms);
	} else if (!fw_faults) {
		printf("FUXI WLAN: M6o wait %#llx mask 0x3f expected 0x11 timeout 1000 ms\n", WL_PARF+0x1b0);
	}
	int bad = 1;
	if (l0) {
		wl_link_off = false;
		bad = wl_m6o_wait(WL_DBI+0x80, 1u<<29, 1u<<29, 100);
		if (bad) { wl_link_off = true; }
	}
	/* Diagnostics are PARF/PHY only, including timeout and access-fault exits. */
	uint32_t pm = wl_prd(WL_PARF+0x24), phy = wl_prd(WL_PHY+0x214);
	printf("FUXI WLAN: M6q cycle link %s after %u ms LTSSM %#x seen %#x/%#x PM_STTS %#x PHY_STATUS %#x\n",
	    !bad && !fw_faults ? "L0" : "FAILED", ms > 1000 ? 1000 : ms, lt, seen[0], seen[1], pm, phy);
	return bad || fw_faults;
}

/* Re-enter the enable path with PERST low and LTSSM off, not merely an
 * LTSSM toggle on a stale controller. No clock/GDSC/rail teardown is needed
 * for this reset/replay; all those votes remain owned by the initial path. */
static int
wl_m6q_resume(void)
{
	int bad = 1;
	printf("FUXI WLAN: M6q cycle begin PHY power-down and sw reset; no DBI access\n");
	wl_pwr(WL_PHY+0x200, 1); /* PCS SW_RESET */
	wl_pwr(WL_PHY+0x240, 0); /* downstream disable: power-down control */
	if (fw_faults) { goto out; }
	printf("FUXI WLAN: M6q cycle begin step 8 core reset/PARF, PERST held low\n");
	if (wl3_step(8) || fw_faults || wl3_faults) { goto out; }
	/* Reset may have changed LTSSM; keep its enable bit explicitly clear. */
	if (wl_m6o_write("cycle LTSSM off after core reset", WL_PARF+0x1b0, 0, 0x100)) { goto out; }
	printf("FUXI WLAN: M6q cycle begin step 9 PHY init/ready, reset hold 1 ms\n");
	int phy_bad = wl3_phy_init(1000);
	printf("FUXI WLAN: M6q cycle PHY init %s ready after %u us PHY_STATUS %#x\n",
	    phy_bad || fw_faults || wl3_faults ? "FAILED" : "done", wl3_report[62], wl3_report[63]);
	if (phy_bad || fw_faults || wl3_faults) { goto out; }
	/* Verify the boundary before allowing downstream's local DBI writes. */
	if ((wl_prd(WL_GCC_PA+0x6b000) & 1) || (wl_prd(WL_GCC_PA+0x6c01c) & 1) ||
	    (wl_prd(WL_PHY+0x214) & 0x40) || (wl_prd(WL_PHY+0x200) & 1) ||
	    (wl_prd(WL_PHY+0x240) & 3) != 3 || (wl_prd(WL_PHY+0x244) & 3) != 3 ||
	    (wl_prd(TS_TLMM+0x1000ull*94+4) & 2) || (wl_prd(WL_PARF+0x1b0) & 0x100) ||
	    fw_faults) { goto out; }
	printf("FUXI WLAN: M6q cycle core reset done + PHY ready; local DBI allowed, config/BAR blocked\n");
	wl_dbi_ready = true;
	printf("FUXI WLAN: M6q cycle begin PHY-ready EP latency 10 ms, PERST held low\n");
	wl_sleep(10); /* X ep-latency, K enable after PHY ready */
	if (wl_m6o_host(10) ||
	    wl_m6o_write("cycle PERST high", TS_TLMM+0x1000ull*94+4, 2, 2)) { goto out; }
	printf("FUXI WLAN: M6o cycle begin PERST high dwell 1 ms\n");
	wl_sleep(1); wl_perst_done = mach_absolute_time();
	if (wl_m6o_host(11)) { goto out; }
	wl_dbi_ready = false; /* pre-training exception ends before LTSSM enable */
	if (wl_m6o_write("cycle LTSSM resume", WL_PARF+0x1b0, 0x100, 0x100) ||
	    wl_m6o_cycle_link(true)) { goto out; }
	wl_link_done = mach_absolute_time();
	/* Core reset invalidates RC bus numbers/windows too. Reuse enumeration
	 * and MEM setup, without BAR sizing or changing the saved EP controls. */
	printf("FUXI WLAN: M6q cycle begin step 12 CFG0 and step 14 MEM/window restore\n");
	if (wl3_step(12) || wl4_step(14) || fw_faults || wl3_faults) { goto out; }
	bad = 0;
out:
	wl_dbi_ready = false;
	return bad;
}

static int
wl_m6o_cycle(void)
{
	if (wl_cycle_attempted || fw_faults || wl_prd(WL_EPCFG) != 0x110717cb) { return 1; }
	wl_cycle_attempted = true;
	unsigned pm = wl_m6o_cap(1);
	wl_pm_cap = pm;
	if (!pm || pm > 0xf8 || wl_disable_irqs()) { return 1; }
	struct wl_seq_config cfg;
	if (wl_m6o_write("cycle clear MASTER", WL_EPCFG+4, 0, 4) || wl_m6o_config_save(&cfg)) { return 1; }
	printf("FUXI WLAN: M6o cycle saved config after clear MASTER; disable function then D3hot\n");
	if (wl_m6o_write("cycle disable function", WL_EPCFG+4, 0x400, 0xffff) ||
	    wl_m6o_write("cycle D3hot", WL_EPCFG+pm+4, 3, 3)) { return 1; }
	wl_sleep(10); /* PCI D3hot transition, before PME_TURNOFF */
	if (wl_m6o_write("cycle PME_TURNOFF", WL_DBI+0xf24, 0x10, ~0u) ||
	    /* Downstream logs a missing L23_READY and powers off anyway. */
	    (wl_m6o_wait(WL_PARF+0x24, 0x20, 0x20, 100) && !fw_faults &&
	     (printf("FUXI WLAN: M6o cycle L23_READY not received, continuing as downstream\n"), 0))) { return 1; }
	/* Latch before attempting link-down writes, including partial/faulted ones. */
	wl_link_off = true; wl_dbi_ready = false;
	if (wl_m6o_write("cycle PERST low", TS_TLMM+0x1000ull*94+4, 0, 2) ||
	    wl_m6o_write("cycle LTSSM off", WL_PARF+0x1b0, 0, 0x100) ||
	    wl_m6o_cycle_link(false) ||
	    wl_m6o_write("cycle WLAN_EN low", TS_TLMM+0x1000ull*80+4, 0, 2)) { return 1; }
	wl_cycle_powered = true; /* WLAN_EN containment; partial rail votes only if selected */
	if (!(wl_seq_flags & WL_O_CYCLERAILS)) {
		printf("FUXI WLAN: M6o cycle retains all six rails; WLAN_EN only\n");
	}
	if ((wl_seq_flags & WL_O_CYCLERAILS) && wl_m6o_rails(false)) { return 1; }
	printf("FUXI WLAN: M6o cycle begin WLAN_EN low dwell 10 ms\n");
	wl_sleep(10);
	if (((wl_seq_flags & WL_O_CYCLERAILS) && wl_m6o_rails(true)) ||
	    wl_m6o_write("cycle WLAN_EN high", TS_TLMM+0x1000ull*80+4, 2, 2)) { return 1; }
	printf("FUXI WLAN: M6o cycle begin WLAN_EN high dwell 10 ms\n");
	wl_sleep(10); /* retained M6o WLAN_EN dwell; EP latency follows PHY ready */
	if (wl_m6q_resume()) { return 1; }
	printf("FUXI WLAN: M6o cycle begin endpoint ID check\n");
	if (wl_prd(WL_EPCFG) != 0x110717cb ||
	    wl_m6o_write("cycle D0", WL_EPCFG+pm+4, 0, 3)) { return 1; }
	printf("FUXI WLAN: M6o cycle begin D0 dwell 10 ms\n");
	wl_sleep(10);
	if (wl_m6o_write("cycle enable function", WL_EPCFG+4, 0x402, 0xffff)) { return 1; }
	if (wl_m6o_config_restore(&cfg)) { return 1; }
	printf("FUXI WLAN: M6o cycle begin BHIOFF check offset 0x28\n");
	if (wl_mrd(0x28) != wl_bhi || fw_faults) { return 1; }
	printf("FUXI WLAN: M6o cycle begin BHIEOFF check offset 0x2c\n");
	if (wl_mrd(0x2c) != wl_bhie || fw_faults) { return 1; }
	printf("FUXI WLAN: M6o cycle begin PBL EE check offset %#x\n", wl_bhi+0x28);
	if (wl_mrd(wl_bhi+0x28) != 0 || fw_faults) { return 1; }
	printf("FUXI WLAN: M6o cycle resumed D0/config; MASTER follows owned SMMU install; WLAON skipped (KIWI DT gate absent)\n");
	return 0;
}

/* Step 18 failed silently on device right after a successful cycle; name the stage. */
static int
wl_m6o_master_fail(const char *what)
{
	printf("FUXI WLAN: M6o power-up failed at %s (faults %u)\n", what, fw_faults);
	return 1;
}

static int
wl_m6o_master(void)
{
	struct wl_seq_config cfg;
	if (wl_m6o_config_save(&cfg)) { return wl_m6o_master_fail("config save"); }
	if (wl_seq_flags & WL_O_MSI) {
		unsigned p = wl_m6o_cap(5);
		uint32_t cap = p ? wl_prd(WL_EPCFG+p) : 0;
		/* >=32 messages; 32- or 64-bit addressing. Require room for addr/data. */
		if (!p || p > 0xe8 || ((cap>>17)&7) < 5 ||
		    wl_disable_irqs() || wl_dma_alloc(&wl_msi_sink, PAGE_SIZE)) {
			printf("FUXI WLAN: M6o MSI cap %#x at %#x sink %p\n", cap, p, (void *)wl_msi_sink.va);
			return wl_m6o_master_fail("MSI capability/IRQ disable/sink");
		}
		wl_msi_pvm = !!(cap & (1u<<24));
		wl_msi_64 = !!(cap & (1u<<23));
		if (!wl_msi_64 && (wl_msi_sink.iova >> 32)) {
			printf("FUXI WLAN: M6o MSI 32-bit capability but sink IOVA %#llx above 4 GiB\n", wl_msi_sink.iova);
			return wl_m6o_master_fail("MSI 32-bit sink address");
		}
		printf("FUXI WLAN: M6o MSI cap %#x at %#x %u-bit%s\n", cap, p, wl_msi_64 ? 64 : 32, wl_msi_pvm ? " per-vector mask" : "");
		for (unsigned i = 0; i < wl_msi_words(); i++) { wl_msi_saved[i] = wl_prd(WL_EPCFG+p+4*i); }
		wl_msi_saved[0] &= ~(1u<<16); /* cleanup never reenables inherited IRQs */
		if (fw_faults) { return wl_m6o_master_fail("MSI save fault"); }
		wl_msi_cap = p;
		wl_dma_clean(&wl_msi_sink);
	}
	if (wl_smmu_enable()) { return wl_m6o_master_fail("SMMU enable"); }
	if (wl_fabric_prepare()) { return wl_m6o_master_fail("fabric prepare"); }
	unsigned pm = wl_m6o_cap(1);
	wl_pm_cap = pm;
	if (!pm || pm > 0xf8 || wl_m6o_write("power-up D0", WL_EPCFG+pm+4, 0, 3) ||
	    wl_m6o_write("power-up enable MEMORY", WL_EPCFG+4, 0x402, 0xffff) ||
	    wl_m6o_config_restore(&cfg) || wl_download_aspm()) { return wl_m6o_master_fail("D0/config restore/ASPM"); }
	if (wl_seq_flags & WL_O_MSI) {
		unsigned p = wl_msi_cap;
		if ((wl_msi_64 && wl_m6o_write("MSI address high", WL_EPCFG+p+8, (uint32_t)(wl_msi_sink.iova>>32), ~0u)) ||
		    wl_m6o_write("MSI address low", WL_EPCFG+p+4, (uint32_t)wl_msi_sink.iova, ~0u) ||
		    wl_m6o_write("MSI data base", WL_EPCFG+p+wl_msi_data_off(), 0, 0xffff) ||
		    (wl_msi_pvm && wl_m6o_write("MSI unmask sink vectors", WL_EPCFG+p+wl_msi_mask_off(), 0, ~0u)) ||
		    wl_m6o_write("MSI 32 vectors enabled", WL_EPCFG+p, (5u<<20)|(1u<<16), 0x00710000)) { return 1; }
		printf("FUXI WLAN: M6o MSI 32 vectors (MHI 0..2) sink IOVA %#llx, polling; no ITS/GIC changes\n", wl_msi_sink.iova);
	}
	if (wl_m6o_write("MASTER before MHI prepare", WL_EPCFG+4, 0x406, 0xffff)) { return 1; }
	printf("FUXI WLAN: M6o D0/config valid MASTER on before contexts/RX prepare; unmapped DMA faults contained\n");
	return 0;
}

static int
wl_m6o_rx_clear(void)
{
	for (unsigned o = 0x60; o <= 0x78; o += 4) {
		wl_mwr(wl_bhie+o, 0);
		uint32_t rb = wl_mrd(wl_bhie+o);
		printf("FUXI WLAN: M6o prepare RX zero offset %#x write 0 read %#x\n", o, rb);
		if (rb || fw_faults) { return 1; }
	}
	return 0;
}

static int
wl_m6o_prepare(void)
{
	uint64_t addr = wl_rxvectors.iova;
	unsigned bytes = (WL_RDDM_SIZE/WL_CHUNK)*sizeof(struct wl_vector);
	uint32_t seq; read_random(&seq, sizeof(seq)); seq &= WL_SEQ_MASK; if (!seq) { seq = 1; }
	if (wl_m6o_write("prepare RX high", WL_BAR0+wl_bhie+0x64, addr>>32, ~0u) ||
	    wl_m6o_write("prepare RX low", WL_BAR0+wl_bhie+0x60, (uint32_t)addr, ~0u) ||
	    wl_m6o_write("prepare RX bytes", WL_BAR0+wl_bhie+0x68, bytes, ~0u) ||
	    wl_m6o_write("prepare RX random DB", WL_BAR0+wl_bhie+0x70, seq, WL_SEQ_MASK) ||
	    wl_m6o_write("POR INTVEC zero", WL_BAR0+wl_bhi+0x20, 0, ~0u)) { return 1; }
	uint32_t ee = wl_mrd(wl_bhi+0x28), st = wl_mrd(0x48);
	if (fw_faults || ee != 0 || st == ~0u) { return 1; }
	if (((st>>8)&255) == 0xff) {
		/* SYS_ERR only; never an unconditional cold reset. */
		wl_mwr(0x38, 2);
		unsigned i;
		for (i = 0; i <= 40; i++) {
			uint32_t ctrl = wl_mrd(0x38);
			if (fw_faults || ctrl == ~0u) { return 1; }
			if (!(ctrl & 2)) { break; }
			if (i < 40) { wl_sleep(25); }
		}
		if (i > 40 || wl_m6o_write("POR INTVEC after reset", WL_BAR0+wl_bhi+0x20, 0, ~0u) ||
		    wl_mrd(wl_bhi+0x28) != 0 || (wl_mrd(0x48)&0xff04) || fw_faults) { return 1; }
	}
	printf("FUXI WLAN: M6o prepare RX zero +60..78, high/low/size/random DB %#x; POR EE %#x state %#x INTVEC 0\n", seq, ee, st);
	return 0;
}

/* Called only after BME clear, before PERST makes config inaccessible. */
static int
wl_m6o_msi_undo(void)
{
	if (!wl_msi_cap) { return 0; }
	unsigned faults0 = fw_faults;
	uint64_t pa = WL_EPCFG+wl_msi_cap;
	if (wl_contain_wr(pa, wl_msi_saved[0]) ||
	    (wl_prd(pa)&0x00710000) != (wl_msi_saved[0]&0x00710000)) { return 1; }
	for (unsigned i = 1; i < wl_msi_words(); i++) {
		uint32_t mask = i == wl_msi_data_off() / 4 ? 0xffff : ~0u;
		if (wl_contain_wr(pa+4*i, wl_msi_saved[i]&mask) ||
		    (wl_prd(pa+4*i)&mask) != (wl_msi_saved[i]&mask)) { return 1; }
	}
	if (fw_faults != faults0) { return 1; }
	wl_msi_cap = 0;
	printf("FUXI WLAN: M6o MSI disabled, saved address/data restored before PERST\n");
	return 0;
}

static int
wl_m6o_undo(bool contained)
{
	if (!contained) { return 1; }
	unsigned faults0 = fw_faults;
	int bad = 0;
	while (wl_seq_undos) {
		struct wl_seq_undo *u = &wl_seq_undo[wl_seq_undos-1];
		if (u->bank) {
			uint32_t v = wl_prd(WL_DBI+0x890);
			if (v == ~0u || wl_contain_wr(WL_DBI+0x890, (v&~0x03000000u)|((u->bank-3)<<24)) ||
			    (wl_prd(WL_DBI+0x890)&0x03000000) != (u->bank-3)<<24) { return 1; }
		}
		uint32_t v = wl_prd(u->pa);
		uint32_t restore = (v&~u->mask)|(u->old&u->mask);
		if (u->pa == WL_DBI+0x78) { restore &= 0xffff; }
		if (v == ~0u || wl_contain_wr(u->pa, restore) ||
		    (wl_prd(u->pa)&u->mask) != (u->old&u->mask)) { bad = 1; break; }
		wl_seq_undos--;
	}
	if (wl_cycle_powered) {
		printf("FUXI WLAN: M6o cycle undo begin WLAN_EN low\n");
		bad |= wl_contain_wr(TS_TLMM+0x1000ull*80+4, 0);
		bad |= !!(wl_prd(TS_TLMM+0x1000ull*80+4)&2);
		if (wl_seq_flags & WL_O_CYCLERAILS) { bad |= wl_m6o_rails(false); }
		if (!bad) { wl_cycle_powered = false; }
	}
	bad |= fw_faults != faults0;
	printf("FUXI WLAN: M6o undo host entries pending %u WLAN power %s result %s\n", wl_seq_undos,
	    wl_cycle_powered ? "unconfirmed" : "contained", bad ? "unconfirmed; quarantine" : "done");
	return bad;
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
	unsigned read_bytes = (wl_seq_flags & WL_O_PREFIX) ? WL_CHUNK : wl_fw_size;
	if (wl_dma_alloc(&wl_image, read_bytes)) { e = 4; goto close; }
	int resid = 0;
	e = vn_rdwr(UIO_READ, vp, (caddr_t)wl_image.va, (int)read_bytes, 0,
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
	    phoff < ehsize || phoff > WL_CHUNK || (uint64_t)phsize * phnum > WL_CHUNK - phoff) { e = 6; goto close; }
	/* FBC prefix must contain ELF metadata and any Qualcomm hash segment.
	 * Other PT_LOAD ranges may be in the later BHIe transfer. No extraction. */
	unsigned hashes = 0;
	uint64_t hash_off = 0, hash_size = 0;
	for (unsigned i = 0; i < phnum; i++) {
		const uint8_t *ph = b + phoff + (uint64_t)i * phsize;
		uint32_t flags;
		memcpy(&flags, ph + (b[4] == 1 ? 24 : 4), 4);
		if ((flags & 0x07000000) != 0x02000000) { continue; }
		if (b[4] == 1) {
			uint32_t off, size;
			memcpy(&off, ph + 4, 4); memcpy(&size, ph + 16, 4);
			hash_off = off; hash_size = size;
		} else {
			memcpy(&hash_off, ph + 8, 8); memcpy(&hash_size, ph + 32, 8);
		}
		if (++hashes > 1 || !hash_size || hash_off > WL_CHUNK || hash_size > WL_CHUNK - hash_off) { e = 6; goto close; }
	}
	printf("FUXI WLAN: M6f FBC metadata fits prefix phend %#llx hash segments %u offset %#llx size %#llx\n",
	    phoff + (uint64_t)phsize * phnum, hashes, hash_off, hash_size);
	/* MBN v5 fields are lengths, not proof of authenticity. Do not print
	 * certificate contents, keys or device identity, or interpret other versions. */
	if (hashes && hash_size >= 40) {
		uint32_t h[10]; memcpy(h, b + hash_off, sizeof(h));
		if (h[1] == 5) {
			uint64_t used = (uint64_t)h[2] + h[3] + h[5] + h[7] + h[9];
			printf("FUXI WLAN: M6m MBNv5 code %#x QTI sig/cert %#x/%#x OEM sig/cert %#x/%#x bounds %s; authentication unverified\n",
			    h[5], h[2], h[3], h[7], h[9], used <= hash_size - 40 ? "fit" : "INVALID");
			if (used > hash_size - 40) { e = 6; goto close; }
		} else {
			printf("FUXI WLAN: M6m hash header version %u unparsed; authentication unverified\n", h[1]);
		}
	}
	if (wl_seq_flags & WL_O_PREFIX) {
		printf("FUXI WLAN: M6o prefix separate coherent allocation %#x bytes, file %u; full image/FBC vectors deferred beyond BHI\n", WL_CHUNK, wl_fw_size);
	}
	wl_dma_clean(&wl_image);
	wl5_report[17] = wl_fw_size; wl5_report[18] = (uint32_t)wl_image.iova;
	printf("FUXI WLAN: firmware %s bytes %u ELF%u DMA %#llx cache-clean\n", wl_fw_path, wl_fw_size, b[4] == 1 ? 32 : 64, wl_image.iova);
	printf("FUXI WLAN: M6e BHI layout file offset 0 ELF ident %02x%02x%02x%02x class %u phoff %#llx phnum %u first %#x bytes; no segment extraction\n",
	    b[0], b[1], b[2], b[3], b[4], phoff, phnum, WL_CHUNK);
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

static void
wl_probe_pattern(void)
{
	if (wl_probe != WL_PROBE_PATTERN) { return; }
	uint8_t *p = (uint8_t *)wl_image.va;
	for (vm_size_t i = 0; i < wl_image.size; i++) { p[i] = (uint8_t)(0xa5u ^ (i & 255)); }
	bzero(p, 64);
	memcpy(p, "\177ELF\377\377\0", 7); /* invalid class, endian and version */
	memcpy(p + 16, "FUXI-M6K-PATTERN", 16);
	printf("FUXI WLAN: M6k pattern first64 ELF class ff endian ff version 00 marker FUXI-M6K-PATTERN FNV1a64 %016llx\n",
	    wl_hash_bytes(0xcbf29ce484222325ull, p, WL_CHUNK));
}

/* Unlike normal wl_poll, continue observing after PBL's first error: a late
 * SMMU fault is the discriminant. Never clear FSR, retry, or proceed to READY.
 * The caller's existing abort contains DMA before any allocation is freed. */
static int
wl_probe_observe(void)
{
	uint32_t flags = wl_bhi_diag(0);
	for (unsigned i = 0; i < 200 && !fw_faults; i++) { wl_sleep(25); }
	flags |= wl_bhi_diag(1);
	(void)wl_smmu_fault(22);
	uint64_t cb = wl_cb_addr(wl_cb);
	uint32_t fsr = wl_prd(cb + 0x58);
	uint64_t far = ((uint64_t)wl_prd(cb + 0x64) << 32) | wl_prd(cb + 0x60);
	uint32_t sid = wl_prd(wl_gr1 + 0x400 + 4 * wl_cb) & 0xffff;
	const char *result = "no-CB-translation-fault";
	if (fsr & 2) {
		result = far >= wl_image.iova && far - wl_image.iova < wl_bhi_bytes() && sid == 0x1401 ?
		    "target-translation-fault" : "other-translation-fault";
	}
	if (fw_faults || (flags & WL_DIAG_TRANSPORT)) { result = "transport-inconclusive"; }
	printf("FUXI WLAN: M6k result probe=%s %s FSR %#x FAR %#llx SID %#x observation 5000 ms; stop/contain (not firmware success)\n",
	    wl_probe_names[wl_probe], result, fsr, far, sid);
	wl_host_diag("probe-completion");
	if (!fw_faults && !(flags & WL_DIAG_TRANSPORT)) { wl_boot_logs(); }
	if (wl_variant_active()) {
		if (wl_seq_flags) {
			printf("FUXI WLAN: M6o result seq mask %#x EE %#x STATUS %#x ERRCODE %#x; stop/contain at 22\n",
			    wl_seq_flags, wl_mrd(wl_bhi + 0x28), wl_mrd(wl_bhi + 0x2c), wl_mrd(wl_bhi + 0x30));
		}
		if (wl_cnss) {
			printf("FUXI WLAN: M6n result cnss=%s PDC consumed %u/13 EE %#x ERRCODE %#x; raw code, no security decode\n",
			    wl_cnss_names[wl_cnss], wl_pdc_acks, wl_mrd(wl_bhi + 0x28), wl_mrd(wl_bhi + 0x30));
		}
		printf("FUXI WLAN: M6m result fw=%s pre=%s bhi=%s vec=%s EE %#x ERRCODE %#x observation 5000 ms; stop/contain\n",
		    wl_fw_names[wl_fw], wl_pre_names[wl_pre], wl_full ? "full" : "off", wl_vec_names[wl_vec],
		    wl_mrd(wl_bhi + 0x28), wl_mrd(wl_bhi + 0x30));
		printf("FUXI WLAN: M6l result pre=%s bhi=%s vec=%s EE %#x ERRCODE %#x observation 5000 ms; stop/contain\n",
		    wl_pre_names[wl_pre], wl_full ? "full" : "off", wl_vec_names[wl_vec],
		    wl_mrd(wl_bhi + 0x28), wl_mrd(wl_bhi + 0x30));
		return 0x6c;
	}
	return 0x6b; /* deliberate diagnostic stop, including unexpected BHI success */
}

static int
wl5_step(unsigned s)
{
	switch (s) {
	case 17:
		if (wl_variant_invalid) { return 0x6c; }
		if (wl_cnss && wl_pdc_acks != 13) { return 0x6e; }
		if (wl_probe == WL_PROBE_INVALID) { return 0x6b; }
		/* M4 validated BHIOFF; independently validate all M5 block extents. */
		wl_bhi = wl4_report[25]; wl_bhie = wl4_report[26];
		if (wl_bhi < 0x40 || wl_bhi > 0x3fc0 || (wl_bhi & 3) ||
		    wl_bhie < 0x40 || wl_bhie > 0x3f80 || (wl_bhie & 3)) { return 4; }
		wl_mhi_valid = true;
		if ((wl_seq_flags & WL_O_CYCLE) && wl_m6o_cycle()) { return 0x6f; }
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
		if (wl_fw) {
			if (wl_prd(WL_EPCFG) != 0x110717cb || ((hw >> 8) & 15) != 2) { return 0x6c; }
			wl_fw_path = "/usr/local/share/fuxi/wlan/stock-amss20.bin";
			printf("FUXI WLAN: M6m stock input selected major 2; no fallback or provenance claim\n");
		}
		printf("FUXI WLAN: TCSR_SOC_HW_VERSION %#x major %u minor %u firmware %s (CNSS kiwi major-version rule)\n", hw, (hw >> 8) & 15, hw & 255, wl_fw_path);
		if (wl_pre_bhi()) { return 0x6c; }
		return wl_m6o_early() && wl_m6o_master() ? 0x6f : 0;
	}
	case 19: return wl_read_firmware();
	case 20: {
		if (!(wl_seq_flags & WL_O_MSI) && wl_disable_irqs()) { return 4; }
		if (wl_mrd(wl_bhi + 0x28) != 0 || (wl_mrd(0x38) & 2) ||
		    ((wl_mrd(0x48) & 0xff04) && !((wl_seq_flags & WL_O_POR) &&
		    ((wl_mrd(0x48)>>8)&255) == 0xff)) || ((wl_prd(WL_EPCFG + 4) & 4) != (wl_m6o_early() ? 4u : 0u))) { return 1; }
		unsigned n = (wl_seq_flags & WL_O_PREFIX) ? 0 : (wl_fw_size + WL_CHUNK - 1) / WL_CHUNK;
		if ((n && wl_dma_alloc(&wl_vectors, n * sizeof(struct wl_vector))) ||
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
		if ((wl_seq_flags & WL_O_POR) && wl_m6o_rx_clear()) { return 0x6f; }
		if (wl_vec == 2 || (wl_seq_flags & WL_O_POR)) {
			unsigned rxn = WL_RDDM_SIZE / WL_CHUNK;
			if (wl_dma_alloc(&wl_rddm, WL_RDDM_SIZE) ||
			    wl_dma_alloc(&wl_rxvectors, rxn * sizeof(struct wl_vector))) { return 2; }
			struct wl_vector *rx = (struct wl_vector *)wl_rxvectors.va;
			for (unsigned i = 0; i < rxn; i++) {
				rx[i].addr = wl_rddm.iova + i * WL_CHUNK; rx[i].size = WL_CHUNK;
			}
			wl_dma_clean(&wl_rddm); wl_dma_clean(&wl_rxvectors);
		}
		wl5_report[20] = (uint32_t)wl_vectors.iova; wl5_report[21] = (uint32_t)wl_contexts.iova;
		printf("FUXI WLAN: prepared %u vectors (%u bytes), 44-byte contexts, channels disabled; BME follows selected sequence\n", n, wl5_report[19]);
		if (fw_faults) { return 3; }
		int setup = wl_smmu_enable();
		if (!setup && (wl_seq_flags & WL_O_POR)) { setup = wl_m6o_prepare(); }
		return setup;
	}
	case 21: {
		if (wl_probe_fired) { return 0x6b; } /* never a second doorbell this boot */
		if (!wl_enabled || wl_smmu_fault(21) || wl_rc_check() ||
		    wl_prd(WL_SMMU) != wl5_report[4] ||
		    wl_prd(WL_SMMU + 0x800 + 4 * wl_slot) != 0x80001401 ||
		    wl_prd(WL_SMMU + 0xc00 + 4 * wl_slot) != wl_cb ||
		    wl_smmu_verify("CB_SCTLR pre-DMA", wl_cb_addr(wl_cb), WL_CB_SCTLR,
		    wl_prd(wl_cb_addr(wl_cb)), WL_CB_SCTLR, WL_CB_SCTLR_MASK) ||
		    (wl_prd(WL_PARF + 0x2c00) & 1) || wl_prd(WL_PARF + 0x234)) { return 6; }
		if (!wl_m6o_early() && wl_fabric_prepare()) {
			wl_host_diag("preflight-blocked");
			return 10;
		}
		if (!wl_m6o_early() && wl_download_aspm()) { return 11; }
		/* ath12k_mhi_clear_vector: undefined reset status is not a result.
		 * Only doorbells/status, no RDDM buffers and no retry. */
		const unsigned clear[] = {0x3c, 0x44, 0x70, 0x78};
		unsigned clear_count = (wl_seq_flags & WL_O_POR) ? 2 : 4;
		for (unsigned i = 0; i < clear_count; i++) { wl_mwr(wl_bhie + clear[i], 0); }
		for (unsigned i = 0; i < clear_count; i++) {
			if (wl_mrd(wl_bhie + clear[i]) || fw_faults) { return 9; }
		}
		printf("FUXI WLAN: M6f BHIe %s doorbells/status cleared; %s\n",
		    (wl_seq_flags & WL_O_POR) ? "TX (prepared RX retained)" : "TX/RX",
		    (wl_vec == 2 || (wl_seq_flags & WL_O_POR)) ? "RX buffers prepared" : "no RX buffers");
		if (wl_vec && !(wl_seq_flags & WL_O_POR && wl_vec == 2)) {
			bool rx = wl_vec == 2;
			uint32_t off = rx ? 0x60 : 0x2c;
			uint64_t addr = rx ? wl_rxvectors.iova : wl_vectors.iova;
			unsigned bytes = rx ? (WL_RDDM_SIZE / WL_CHUNK) * sizeof(struct wl_vector) : wl5_report[19];
			if (rx) {
				/* Clear complete RX setup, not only DB/status, as MHI prepare does. */
				for (unsigned o = 0x60; o <= 0x78; o += 4) { wl_mwr(wl_bhie + o, 0); }
			}
			wl_mwr64(wl_bhie + off, addr); wl_mwr(wl_bhie + off + 8, bytes);
			if (wl_mrd(wl_bhie + off) != (uint32_t)addr ||
			    wl_mrd(wl_bhie + off + 4) != (uint32_t)(addr >> 32) ||
			    wl_mrd(wl_bhie + off + 8) != bytes || fw_faults) { return 12; }
			if (rx) {
				uint32_t seq; read_random(&seq, sizeof(seq));
				seq &= WL_SEQ_MASK; if (!seq) { seq = 1; }
				wl_mwr(wl_bhie + 0x70, seq);
				if (wl_mrd(wl_bhie + 0x70) != seq || fw_faults) { return 12; }
			}
			printf("FUXI WLAN: M6l BHIe %s staged bytes %u before BHI; %s\n",
			    rx ? "RDDM RX" : "AMSS TX", bytes, rx ? "RX doorbell armed; sink only" : "TX doorbell remains zero");
		}
		wl_probe_pattern();
		if (!(wl_seq_flags & WL_O_POR)) { wl_mwr(wl_bhi + 0x20, 0); }
		wl_mwr(wl_bhi + 0x2c, 0);
		wl_mwr64(wl_bhi + 8, wl_image.iova); wl_mwr(wl_bhi + 0x10, wl_bhi_bytes());
		wl_dma_clean(&wl_image);
		if (wl_bhi_mapping_check()) { return 8; }
		/* Flush posted setup writes and verify the exact Linux BHI tuple before
		 * enabling bus mastering. No MMIO write to MHICTRL before BHI. */
		uint32_t hi = wl_mrd(wl_bhi + 0x0c), lo = wl_mrd(wl_bhi + 8);
		uint32_t size = wl_mrd(wl_bhi + 0x10), ivec = wl_mrd(wl_bhi + 0x20);
		uint32_t status = wl_mrd(wl_bhi + 0x2c), ee = wl_mrd(wl_bhi + 0x28);
		printf("FUXI WLAN: M6e BHI preflight addr %#x:%08x size %#x INTVEC %#x STATUS %#x EE %#x image/tables cache-clean + dsb sy\n",
		    hi, lo, size, ivec, status, ee);
		if (fw_faults) { return 1; }
		if (hi != (uint32_t)(wl_image.iova >> 32) || lo != (uint32_t)wl_image.iova ||
		    size != wl_bhi_bytes() || ivec || status || ee != 0 || (wl_image.iova & 4095)) { return 7; }
		/* Default enables BME here; M6o early-MASTER modes must retain it.
		 * INTx stays disabled; MSI either disabled or targets the owned sink. */
		if (wl_m6o_early() && !(wl_prd(WL_EPCFG+4)&4)) { return 0x6f; }
		if (wl_seq_flags & WL_O_MSI) {
			unsigned p = wl_msi_cap;
			if (!p || (wl_prd(WL_EPCFG+p)&0x00710000) != 0x00510000 ||
			    wl_prd(WL_EPCFG+p+4) != (uint32_t)wl_msi_sink.iova ||
			    (wl_msi_64 && wl_prd(WL_EPCFG+p+8) != (uint32_t)(wl_msi_sink.iova>>32)) ||
			    (wl_prd(WL_EPCFG+p+wl_msi_data_off())&0xffff) ||
			    (wl_msi_pvm && wl_prd(WL_EPCFG+p+wl_msi_mask_off()))) { return 0x6f; }
		}
		if (!wl_m6o_early()) { wl_pwr(WL_EPCFG + 4, (wl_prd(WL_EPCFG + 4) & 0xffff) | 0x404); }
		if ((wl_prd(WL_EPCFG + 4) & 0x406) != 0x406 || fw_faults) { return 2; }
		wl_host_diag("preflight");
		/* Step 14 sets RC BME already. Prove it survived; do not silently
		 * repair changed command state or emit the BHI doorbell after a fault. */
		if (fw_faults || !(wl_prd(WL_DBI + 4) & 4)) { return 10; }
		read_random(&wl_seq, sizeof(wl_seq));
		wl_seq &= WL_SEQ_MASK;
		if (!wl_seq) { wl_seq = 1; } /* bounded nonzero fallback */
		wl_probe_publish(); /* last CPU buffer operation before the doorbell */
		uint64_t now = mach_absolute_time(), perst_ns = 0, link_ns = 0;
		absolutetime_to_nanoseconds(now - wl_perst_done, &perst_ns);
		absolutetime_to_nanoseconds(now - wl_link_done, &link_ns);
		printf("FUXI WLAN: M6k BHI timing since PERST-step completion %llu ms since link-step completion %llu ms (PERST sample is 1 ms after deassert)\n",
		    perst_ns / 1000000ull, link_ns / 1000000ull);
		wl_probe_fired = true;
		wl_mwr(wl_bhi + 0x18, wl_seq);
		printf("FUXI WLAN: M6f BHI doorbell random seq %#x mask %#x bytes %#x bus master on\n", wl_seq, WL_SEQ_MASK, wl_bhi_bytes());
		return fw_faults ? 3 : 0;
	}
	case 22: {
		if (wl_probe != WL_PROBE_OFF || wl_variant_active()) { return wl_probe_observe(); }
		int e = wl_poll(0);
		uint32_t first = wl_bhi_diag(0);
		wl5_report[124] = wl5_report[67]; wl5_report[125] = wl5_report[68];
		wl5_report[126] = first; wl5_report[127] = (uint32_t)e;
		wl_sleep(10); /* diagnostic repeat only; never retry a failed transfer */
		uint32_t again = wl_bhi_diag(1);
		bool failed = e || ((first | again) & (WL_DIAG_PBL_ERROR | WL_DIAG_TRANSPORT)) ||
		    (wl5_report[67] >> 30) == 3 || wl5_report[64] == 3 || wl5_report[64] == 6 ||
		    (wl5_report[65] & 4) || ((wl5_report[65] >> 8) & 255) == 255 || (wl5_report[66] & 2);
		wl_host_diag(failed ? "failure" : "completion");
		if (e || ((first | again) & WL_DIAG_PBL_ERROR)) {
			if (!((first | again) & WL_DIAG_TRANSPORT)) { wl_boot_logs(); }
			else { printf("FUXI WLAN: M6f boot logs skipped transport flags %#x\n", first | again); }
		}
		if (fw_faults) { return e == 3 ? e : 5; }
		if ((first | again) & WL_DIAG_TRANSPORT) { return e == 3 ? e : 5; }
		/* A late error/EE loss in the repeat must not turn into step-22 OK. */
		if (!e && ((wl5_report[67] >> 30) == 3 || wl5_report[64] == 3 || wl5_report[64] == 6 ||
		    (wl5_report[65] & 4) || ((wl5_report[65] >> 8) & 255) == 255 || (wl5_report[66] & 2))) { return 1; }
		return e;
	}
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
	if (!va || wl_link_refused(pa)) { return 1; }
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
	if (!bm_bad && wl_seq_flags && wl_m6o_msi_undo()) { bm_bad = 1; }
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
	int seq_bad = wl_seq_flags ? wl_m6o_undo(!perst_bad) : 0;
	if (!bm_bad && !perst_bad && !seq_bad && !wl_smmu_undo()) {
		wl_dma_free(&wl_msi_sink); wl_dma_free(&wl_tables);
		wl_dma_free(&wl_rxvectors); wl_dma_free(&wl_rddm);
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
			e = s == 1 ? wl_cnss_pdc() : 0; /* before any WLAN power writes */
			if (s == 1 && e) {
				printf("FUXI WLAN: M6n PDC blocked before WLAN power, consumed %u/13 reason %u; reboot to retry\n",
				    wl_pdc_acks, (unsigned)e);
			}
			if (!e && s == 1 && wl_variant_invalid && wl_seq_requested) { e = 0x6f; }
			if (!e) { e = wl_icc_step(s); } /* failure blocks link training / BHI */
			if (!e) {
				wlan_m3(s); /* runs only step s: earlier ones are in wl3_done */
				e = (int)(wl3_report[3] & 0xffu);
				if (wl3_report[3]) { e = e ? e : 0xfe; }
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
			if (s == 10) { wl_perst_done = mach_absolute_time(); }
			if (s == 11) { wl_link_done = mach_absolute_time(); }
			fw_m3_summary(s);
		}
		if (e) {
			r4[3] = s << 8 | (unsigned)e;
			wl5_report[3] = r4[3];
			if (s >= 17 || (wl_seq_flags && s >= 8)) { wl5_abort(s); }
			printf("FUXI WLAN: stopped at step %u reason %u ESR %#x FAR %#llx; reboot to retry\n",
			    s, e, fw_fault_esr, fw_fault_far);
			goto out;
		}
	}
	printf("FUXI WLAN: %u steps done\n", limit);
	/* A deliberate partial M5 run also shuts down before releasing memory. */
	if ((limit >= 17 || (wl_seq_flags && limit >= 8)) && limit < WL5_STEPS) { wl5_abort(limit); }
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
	unsigned mapped = 0;
	(void)result;
	for (unsigned i = 0; i < FW_NWIN; i++) {
		if ((!wl_cnss || wl_variant_invalid) &&
		    (fw_windows[i].pa == WL_AOSS_QMP || fw_windows[i].pa == WL_AOSS_IPCC)) { continue; }
		kern_return_t kr = fw_map_window(&fw_windows[i]);
		if (kr != KERN_SUCCESS) {
			printf("FUXI WLAN: map PA %#llx bytes %#llx failed %d\n",
			    fw_windows[i].pa, fw_windows[i].size, kr);
			goto out;
		}
		mapped++;
	}
	printf("FUXI WLAN: mapped %llu bytes in %u windows; running steps 1..%u\n",
	    fw_mapped_bytes, mapped, limit);
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
	wl_iova_configure();
	printf("FUXI WLAN: enabled (fuxiwlan=%d), WCN7850 power-up + PCIe0 + MHI firmware, limit %u\n",
	    arg, limit);
	if (kernel_thread_start(fw_thread, (void *)(uintptr_t)limit, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("FUXI WLAN: kernel_thread_start failed\n");
	}
}
#endif /* FUXI */
