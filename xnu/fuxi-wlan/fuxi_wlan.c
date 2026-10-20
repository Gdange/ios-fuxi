/*
 * FUXI WLAN (lane-wlan milestone 4): WCN7850 power-up, PCIe0 link, BAR0
 * assignment and a read-only MHI/BHI register dump from XNU.
 * Gated by boot-arg fuxiwlan (default 0 = do nothing):
 *   fuxiwlan=1      run all 16 steps
 *   fuxiwlan=2..16  stop after that step (bisecting a hang or reset)
 * No firmware is loaded, no MHI state change is requested, the endpoint's
 * bus mastering stays off and no interrupt is enabled.
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
 * recovered); after the first fault every later write is refused.
 */
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

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
 * io_map submap). Kept for the boot: the endpoint stays decoded. */
struct fw_window { uint64_t pa, size; uintptr_t va; };
static struct fw_window fw_windows[] = {
	{0x00100000, 0x80000, 0},   /* GCC: votes, GDSCs, PCIe0 RCGs/CBCRs/BCRs */
	{0x00898000, 0x4000, 0},    /* QUP2 SE6 (BT UART), gated reads only */
	{0x008c0000, 0x4000, 0},    /* QUP2 wrapper */
	{0x01c00000, 0x8000, 0},    /* PCIe0 PARF 0x1c00000 + QMP PHY 0x1c06000 */
	{0x01fd4000, 0x4000, 0},    /* TCSR clkref 0x1fd5100 */
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
			e = wl4_step(s);
			wl3_report[124] += wl3_faults;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		/* A fault in a plain (usb_rd/ts_wr) access also ends the run. */
		if (!e && fw_faults != faults0) {
			e = 0xfd;
		}
		r4[2] = s;
		r4[40 + s] = e ? (s << 8 | (unsigned)e) : 0x1000u | s;
		printf("FUXI WLAN: step %u %s code %#x (%llu ms, faults %u)\n", s, e ? "FAILED" : "ok",
		    r4[40 + s], ns / 1000000ull, fw_faults);
		if (!e && s <= WL3_STEPS) {
			fw_m3_summary(s);
		}
		if (e) {
			r4[3] = s << 8 | (unsigned)e;
			printf("FUXI WLAN: stopped at step %u reason %u ESR %#x FAR %#llx; reboot to retry\n",
			    s, e, fw_fault_esr, fw_fault_far);
			goto out;
		}
	}
	printf("FUXI WLAN: %u steps done; no firmware loaded, bus master off\n", limit);
out:
	/* Shared GCC vote registers (touch and GPU also read-modify-write
	 * 0x52008/0x52000 without a common lock): show the final values. */
	printf("FUXI WLAN: GCC votes 0x52000 %#x 0x52008 %#x gdsc-collapse 0x52020 %#x\n",
	    usb_rd(WL_GCC, 0x52000), usb_rd(WL_GCC, 0x52008), usb_rd(WL_GCC, 0x52020));
	fw_dump("m3", wl3_report, 128);
	fw_dump("m4", wl4_report, 64);
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
	unsigned limit = (arg == 1 || arg > (int)WL4_STEPS) ? WL4_STEPS : (unsigned)arg;
	printf("FUXI WLAN: enabled (fuxiwlan=%d), WCN7850 power-up + PCIe0 + MHI read, limit %u\n",
	    arg, limit);
	if (kernel_thread_start(fw_thread, (void *)(uintptr_t)limit, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("FUXI WLAN: kernel_thread_start failed\n");
	}
}
#endif /* FUXI */
