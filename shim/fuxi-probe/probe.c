/* Fuxi standalone DWC3 probe. Control-endpoint state machine adapted from
 * ios-sweet patches 0073/0074 (dipa-studio). No filesystem or UFS access.
 * USB register base and clock offsets verified against Fuxi's stock DTB and
 * LineageOS gcc-kalama.c. PHY setup is inherited from ABL, not reprogrammed.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define printf(...) ((void)0)
#define rsr(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
static void *memcpy(void *dst, const void *src, size_t n)
{
 volatile uint8_t *d=dst; const volatile uint8_t *s=src;
 while(n--) *d++=*s++;
 return dst;
}
static void delay(unsigned us)
{
 uint64_t until=rsr("cntpct_el0")+rsr("cntfrq_el0")*us/1000000;
 while(rsr("cntpct_el0")<until) __asm__ volatile("yield");
}
static uint32_t usb_rd(uintptr_t base, uint32_t off)
{
 uint32_t v=*(volatile uint32_t *)(base+off);
 __asm__ volatile("dmb oshld" ::: "memory");
 return v;
}
static uint8_t dma_area[0x4000] __attribute__((aligned(0x4000)));
static uintptr_t probe_fdt;
static uint32_t probe_fdt_size;
static struct {
 uint64_t magic, version, midr, mpidr, mmfr0, mmfr1, current_el, cntfrq, sctlr;
 uint64_t fdt_address, fdt_size, gsnpsid, gctl_before, dctl_before, dsts_before;
 uint64_t gcc_master, gcc_cfg, configured, speed;
} probe_info;
_Static_assert(sizeof(probe_info)==19*8, "USB report format");
#define GCTL            0xc110
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEPCMDPAR2(n)   (0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)   (0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)   (0xc808 + 0x10 * (n))
#define DEPCMD(n)       (0xc80c + 0x10 * (n))

/*
 * Qualcomm wrapper registers ("QSCRATCH", core base + 0xf8800). The
 * controller only connects (pull-up on D+) while it sees a valid VBUS
 * session; Linux dwc3-qcom forces that in software when the charger
 * reports a cable (dwc3_qcom_vbus_override_enable).
 */
#define QSCRATCH_PA             0x0a6f8800
#define QSCRATCH_HS_PHY_CTRL    0x10
#define   UTMI_OTG_VBUS_VALID   (1u << 20)
#define   SW_SESSVLD_SEL        (1u << 28)
#define QSCRATCH_SS_PHY_CTRL    0x30
#define   LANE0_PWR_PRESENT     (1u << 24)

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)

#define DEPCMD_SETEPCONFIG      1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL         4
#define DEPCMD_STARTTRANSFER    6
#define DEPCMD_DEPSTARTCFG      9
#define DEPCMD_CMDACT           (1u << 10)

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

/* DMA area layout */
#define UD_EVT          0x0000          /* event buffer, 4 KiB */
#define UD_EVT_SIZE     0x1000
#define UD_TRB_OUT      0x1000          /* ep0 OUT TRB */
#define UD_TRB_IN       0x1010          /* ep0 IN TRB */
#define UD_SETUP        0x1040          /* 8-byte setup packet */
#define UD_DATA         0x1100          /* ep0 data stage, 512 bytes */
#define UD_SIZE         0x4000

static struct {
	uintptr_t dwc;
	uintptr_t dma;
	uint64_t    dma_pa;
	uint32_t    evt_pos;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	bool        data_in;        /* data stage direction */
	uint32_t    events;
} ud;

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
dwc_rd(uint32_t off)
{
	return usb_rd(ud.dwc, off);
}

static void ud_sync(uint32_t off, uint32_t len)
{
 (void)off; (void)len;
 /* Linux boot ABI: MMU/data cache off; DMA sees the same physical memory. */
 __asm__ volatile("dsb sy" ::: "memory");
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			uint32_t status = (v >> 12) & 0xf;
			if (status) {
				printf("SWEET USB: ep%u cmd 0x%x status %u\n", ep, cmd & 0xf, status);
				return -1;
			}
			return 0;
		}
		delay(1);
	}
	printf("SWEET USB: ep%u cmd 0x%x timeout\n", ep, cmd & 0xf);
	return -1;
}

/* One TRB on physical endpoint ep (0 = ep0 OUT, 1 = ep0 IN). */
static void
ud_start(unsigned ep, uint32_t buf, uint32_t len, uint32_t ctl)
{
	uint32_t trb = ep ? UD_TRB_IN : UD_TRB_OUT;
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	ud_sync(trb, 16);
	if (len) {
		ud_sync(buf, len);
	}
	uint64_t tpa = ud.dma_pa + trb;
	ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
}

static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	ud_start(0, UD_SETUP, 8, TRBCTL_SETUP);
}

static void
ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/* Descriptors. VID 0x1209 / PID 0x0001 = pid.codes test ID, not a product. */
static const uint8_t ud_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t ud_cfg_desc[18] = {
	9, 2, 18, 0, 1, 1, 0, 0x80, 250,                /* config: 1 interface, 500 mA */
	9, 4, 0, 0, 0, 0xff, 0x00, 0x00, 0,             /* vendor interface, no endpoints */
};
static uint32_t
ud_string(unsigned idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;   /* en-US */
		return 4;
	}
	if (idx > 3) {
		return 0;
	}
	const char *str = idx == 1 ? "fuxi-ios" : idx == 2 ? "Fuxi hardware probe" : "probe";
	uint32_t n = 2;
	for (; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void
ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	bool ok = true;

	printf("SWEET USB: setup %02x %02x %04x len %u\n", type, req, value, length);
	ud.data_in = (type & 0x80) != 0;
	if (type == 0xc0 && (req == 0x40 || req == 0x41)) {
        uint32_t offset = (uint32_t)value | ((uint32_t)st[4] << 16) | ((uint32_t)st[5] << 24);
        const uint8_t *src = req == 0x40 ? (const uint8_t *)&probe_info : (const uint8_t *)probe_fdt;
        uint32_t size = req == 0x40 ? sizeof(probe_info) : probe_fdt_size;
        n = length > 512 ? 512 : length;
        if (offset > size || n > size - offset) ok = false;
        else memcpy(data, src + offset, n);
    } else if ((type & 0x60) != 0) {
        ok = false;
	} else if (req == 6) {          /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: memcpy(data, ud_dev_desc, sizeof(ud_dev_desc)); n = sizeof(ud_dev_desc); break;
		case 2: memcpy(data, ud_cfg_desc, sizeof(ud_cfg_desc)); n = sizeof(ud_cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = false; break;
		}
	} else if (req == 5) {          /* SET_ADDRESS: takes effect now (DWC3 handles the timing) */
		uint32_t dcfg = dwc_rd(DCFG) & ~(0x7fu << 3);
		dwc_wr(DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
		printf("SWEET USB: address %u\n", value & 0x7f);
	} else if (req == 9) {          /* SET_CONFIGURATION */
		probe_info.configured = value;
	} else if (req == 8) {          /* GET_CONFIGURATION */
		data[0] = 1; n = 1;
	} else if (req == 0) {          /* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11) {         /* SET_INTERFACE */
	} else {
		ok = false;
	}
	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;    /* status stage on XferNotReady */
		return;
	}
	ud.state = EP0_DATA;
	if (ud.data_in) {
		if (n > length) {
			n = length;
		}
		ud_start(1, UD_DATA, n, TRBCTL_DATA);
	} else {
		ud_start(0, UD_DATA, 64, TRBCTL_DATA);  /* OUT data is not used here */
	}
}

static void
ud_event(uint32_t ev)
{
	if (ev & 1) {                   /* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {        /* USB reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			probe_info.configured = 0;
		} else if (type == 2) { /* connect done */
			uint32_t speed = dwc_rd(DSTS) & 7;
            probe_info.speed = speed;
			/* ep0 max packet: 64 at high/full speed (modify action) */
			ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (0u << 25), 0);
			ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (1u << 25), 0);
			printf("SWEET USB: connected, speed %u\n", speed);
		} else if (type == 0) {
			printf("SWEET USB: disconnect\n");
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned status = (ev >> 12) & 0xf;
	if (ep > 1) {
		return;
	}
	/*
	 * Control transfer state machine (Linux dwc3 ep0.c): SETUP completes ->
	 * request handled; the data stage (if any) is started right away; the
	 * status stage is started when the controller reports XferNotReady
	 * with status "control status"; its completion re-arms SETUP.
	 */
	if (type == 1) {                /* XferComplete */
		switch (ud.state) {
		case EP0_SETUP:
			ud_sync(UD_SETUP, 8);
			ud_setup();
			break;
		case EP0_DATA:
			ud.state = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
		return;
	}
	if (type == 3 && status == 2) { /* XferNotReady, control status phase */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.state = EP0_STATUS;
			ud_start(ep, UD_DATA, 0, ctl);
		}
	}
}

static void usb_poll(void)
{
 uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
 if (count > UD_EVT_SIZE || (count & 3)) return;
 ud_sync(UD_EVT, UD_EVT_SIZE);
 for (uint32_t done = 0; done < count; done += 4) {
  uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
  ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
  ud.events++;
  ud_event(ev);
 }
 if (count) dwc_wr(GEVNTCOUNT, count);
}

static void
usb_start(uintptr_t dwc)
{
 ud.dwc = dwc;
 ud.dma = (uintptr_t)dma_area;
 ud.dma_pa = ud.dma;

	/* Device controller soft reset (PHY and global setup stay as ABL left them). */
	dwc_wr(DCTL, DCTL_CSFTRST);
	int i;
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++) {
		delay(1000);
	}
	printf("SWEET USB: soft reset %s after %d ms, GCTL 0x%08x DCFG 0x%08x\n",
	    (dwc_rd(DCTL) & DCTL_CSFTRST) ? "STUCK" : "done", i, dwc_rd(GCTL), dwc_rd(DCFG));
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		return;
	}

	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;

	/* High speed, address 0 */
	uint32_t dcfg = dwc_rd(DCFG) & ~(7u | (0x7fu << 3));
	dwc_wr(DCFG, dcfg | 0);
	/* Events: disconnect, USB reset, connect done */
	dwc_wr(DEVTEN, 0x7);

	/* Endpoint 0 OUT/IN: start config, control type, max packet 512 until connect done. */
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("SWEET USB: endpoint 0 setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, 0x3);
	ud_start_setup();

	uintptr_t qs = (uintptr_t)QSCRATCH_PA;
	if (qs != 0) {
		uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL), ss = usb_rd(qs, QSCRATCH_SS_PHY_CTRL);
		printf("SWEET USB: QSCRATCH HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x GENERAL_CFG 0x%08x\n",
		    hs, ss, usb_rd(qs, 0x08));
		*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL;
		*(volatile uint32_t *)(qs + QSCRATCH_SS_PHY_CTRL) = ss | LANE0_PWR_PRESENT;
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("SWEET USB: VBUS override on: HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x\n",
		    usb_rd(qs, QSCRATCH_HS_PHY_CTRL), usb_rd(qs, QSCRATCH_SS_PHY_CTRL));
	}

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++) {
		delay(1000);
	}
	printf("SWEET USB: running (DSTS 0x%08x after %d ms), DMA at 0x%llx\n", dwc_rd(DSTS), i, ud.dma_pa);

}

static __attribute__((noreturn)) void reboot(void)
{
 register uint64_t fn __asm__("x0")=0x84000009;
 __asm__ volatile("dsb sy; smc #0" : "+r"(fn) :: "x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
 for(;;) __asm__ volatile("wfe");
}
void probe_main(uintptr_t fdt)
{
 probe_info.magic=0x45424f5250495846ULL; /* little-endian FXIPROBE */
 probe_info.version=1;
 probe_info.midr=rsr("midr_el1"); probe_info.mpidr=rsr("mpidr_el1");
 probe_info.mmfr0=rsr("id_aa64mmfr0_el1"); probe_info.mmfr1=rsr("id_aa64mmfr1_el1");
 probe_info.current_el=rsr("CurrentEL"); probe_info.cntfrq=rsr("cntfrq_el0");
 probe_info.sctlr=rsr("sctlr_el1");
 /* Refuse DMA if ABL violates the Linux entry ABI. */
 if(probe_info.sctlr & 5) reboot();
 probe_fdt=fdt;
 if(fdt && !(fdt&3) && usb_rd(fdt,0)==0xedfe0dd0) {
  probe_fdt_size=__builtin_bswap32(usb_rd(fdt,4));
  if(probe_fdt_size<40 || probe_fdt_size>0x1000000) probe_fdt_size=0;
 }
 probe_info.fdt_address=fdt; probe_info.fdt_size=probe_fdt_size;
 probe_info.gcc_master=usb_rd(0x100000,0x39018);
 probe_info.gcc_cfg=usb_rd(0x100000,0x39088);
 if((probe_info.gcc_master|probe_info.gcc_cfg)&0x80000000) reboot();
 probe_info.gsnpsid=usb_rd(0xa600000,0xc120);
 if((probe_info.gsnpsid&0xffff0000)!=0x55330000 && (probe_info.gsnpsid&0xffff0000)!=0x33310000) reboot();
 probe_info.gctl_before=usb_rd(0xa600000,GCTL);
 probe_info.dctl_before=usb_rd(0xa600000,DCTL);
 probe_info.dsts_before=usb_rd(0xa600000,DSTS);
 usb_start(0xa600000);
 uint64_t until=rsr("cntpct_el0")+rsr("cntfrq_el0")*90;
 while(rsr("cntpct_el0")<until) usb_poll();
 reboot();
}
