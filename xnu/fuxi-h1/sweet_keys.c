/*
 * Minimal polled SWEET keys. Call once after the scheduler is running.
 * See hw-report.md: volume-up is PM6150L SID 4, NOT PM6150 SID 0.
 * PMIC access is READ ONLY: the sole MMIO write issues EXT_READL on an
 * observer channel. No PMIC config, IRQ acknowledge or reset writes exist.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi_bus.h"
#include "fuxi_key_state.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_key(int key, int down);
void sweet_keys_init(void);

#define KEYS_CORE_PA     0x0c440000u
#define KEYS_OBS_PA      0x0e600000u
#define KEYS_CNFG_PA     0x0c40a000u
#define KEYS_PON_RT      0x0810u
#define KEYS_GPIO_RT     0xc110u
#define KEYS_NO_APID     0xffffu

static vm_offset_t keys_core, keys_obs, keys_cnfg;
static uint16_t keys_pon_apid = KEYS_NO_APID, keys_gpio_apid = KEYS_NO_APID;
static uint32_t keys_version, keys_last_status;
static bool keys_started;
/* A late completion after timeout must never be mistaken for a new sample. */
static bool keys_timed_out[512];
static int keys_sleep_chan;

static uint32_t
keys_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* v5 can map one PPID more than once; prefer the apps-owned entry. */
static uint16_t
keys_find_apid(uint16_t ppid, uint32_t count)
{
	uint16_t found = KEYS_NO_APID;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t map = keys_rd(keys_core, 0x900u + 4u * i);
		if (map == 0 || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		uint32_t owner = keys_rd(keys_cnfg, 0x700u + 4u * i) & 7u;
		if (found == KEYS_NO_APID || owner == 0) {
			found = (uint16_t)i;
		}
	}
	return found;
}

static bool
keys_read_locked(uint16_t apid, uint16_t addr, uint8_t *out)
{
	if (apid == KEYS_NO_APID || keys_timed_out[apid]) {
		return false;
	}
	uint32_t off = 0x80u * apid; /* obs + EE(0)*0x10000 */
	uint32_t cmd = (1u << 27) | ((uint32_t)(addr & 0xffu) << 4);
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(keys_obs + off) = cmd; /* EXT_READL, one byte */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		keys_last_status = keys_rd(keys_obs, off + 0x08u);
		if (keys_last_status & 1u) {
			if (keys_last_status & 0x0eu) {
				return false;
			}
			*out = (uint8_t)keys_rd(keys_obs, off + 0x18u);
			return true;
		}
		delay(1);
	}
	keys_timed_out[apid] = true;
	return false;
}

static bool
keys_read(uint16_t apid, uint16_t addr, uint8_t *out)
{
	sweet_spmi_bus_lock();
	bool ok = keys_read_locked(apid, addr, out);
	sweet_spmi_bus_unlock();
	return ok;
}
static void
keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
keys_thread(void *arg, wait_result_t result)
{
	struct fuxi_key_state keys[3] = {0};
	unsigned errors = 0;
	(void)arg;
	(void)result;
	for (;;) {
		uint8_t pon = 0, gpio = 0;
		uint64_t t = mach_absolute_time();
		bool pok = keys_read(keys_pon_apid, KEYS_PON_RT, &pon);
		bool gok = keys_read(keys_gpio_apid, KEYS_GPIO_RT, &gpio);
		int now[3] = {!!(pon & 1u), !(gpio & 1u), !!(pon & 2u)};
		bool failed = (keys_pon_apid != KEYS_NO_APID && !pok) ||
		    (keys_gpio_apid != KEYS_NO_APID && !gok);
		if (fuxi_keys_failed(&errors, failed)) {
			for (int k = 0; k < 3; ++k) {
				if (keys[k].down) {
					sweet_hid_key(k, 0);
				}
			}
			printf("SWEET KEYS: stopped after %u consecutive read failures st=%#x\n",
			    errors, keys_last_status);
			(void)thread_terminate(current_thread());
			thread_block(THREAD_CONTINUE_NULL);
			return;
		}
		for (int k = 0; k < 3; ++k) {
			if (fuxi_key_sample(&keys[k], k == 1 ? gok : pok, now[k], t)) {
				printf("SWEET KEYS: key=%d down=%d pon=%#x gpio4:c110=%#x\n",
				    k, keys[k].down, pon, gpio);
				sweet_hid_key(k, keys[k].down);
			}
		}
		keys_wait();
	}
}

void
sweet_keys_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (keys_started) {
		return;
	}
	keys_started = true;
	(void)PE_parse_boot_argn("sweetkeys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("SWEET KEYS: disabled (sweetkeys=0)\n");
		return;
	}
	keys_core = ml_io_map(KEYS_CORE_PA, 0x2000);
	keys_cnfg = ml_io_map(KEYS_CNFG_PA, 0x1000);
	keys_obs = ml_io_map(KEYS_OBS_PA, 0x10000);
	if (!keys_core || !keys_cnfg || !keys_obs) {
		printf("SWEET KEYS: MMIO mapping failed\n");
		return;
	}
	keys_version = keys_rd(keys_core, 0);
	uint32_t count = keys_rd(keys_core, 4) & 0x7ffu;
	if (keys_version < 0x50000000u || keys_version >= 0x70000000u ||
	    count == 0 || count > 512) {
		printf("SWEET KEYS: unsupported arbiter version=%#x apids=%u\n",
		    keys_version, count);
		return;
	}
	keys_pon_apid = keys_find_apid(0x008, count);
	keys_gpio_apid = keys_find_apid(0x4c1, count);
	uint8_t ptype = 0, gtype = 0;
	uint8_t pon = 0, gpio = 0, mode = 0, en = 0, subtype = 0;
	bool pok = keys_read(keys_pon_apid, 0x0804, &ptype) && ptype == 0x01 &&
	    keys_read(keys_pon_apid, KEYS_PON_RT, &pon);
	bool gok = keys_read(keys_gpio_apid, 0xc104, &gtype) && gtype == 0x10 &&
	    keys_read(keys_gpio_apid, KEYS_GPIO_RT, &gpio);
	if (!pok) {
		keys_pon_apid = KEYS_NO_APID;
	}
	if (!gok) {
		keys_gpio_apid = KEYS_NO_APID;
	}
	printf("SWEET KEYS: peripheral types pon=%#x gpio=%#x\n", ptype, gtype);
	bool mok = keys_read(keys_gpio_apid, 0xc140, &mode);
	bool eok = keys_read(keys_gpio_apid, 0xc146, &en);
	bool sok = keys_read(keys_gpio_apid, 0xc105, &subtype);
	printf("SWEET KEYS: arb=%#x apids=%u pon-apid=%u gpio4-apid=%u pon=%#x/%d gpio=%#x/%d mode=%#x/%d en=%#x/%d subtype=%#x/%d\n",
	    keys_version, count, keys_pon_apid, keys_gpio_apid,
	    pon, pok, gpio, gok, mode, mok, en, eok, subtype, sok);
	/* GPIO configuration belongs to firmware. Diagnose, never repair it here. */
	unsigned dir = subtype >= 0x10u && subtype <= 0x13u ?
	    mode & 3u : (mode >> 4) & 7u;
	if (!eok || !mok || !sok || !(en & 0x80u) || (dir != 0 && dir != 2)) {
		printf("SWEET KEYS: GPIO2 not confirmed enabled as input; volume-up withheld, PMIC unchanged\n");
		keys_gpio_apid = KEYS_NO_APID;
	}
	if (keys_pon_apid == KEYS_NO_APID && keys_gpio_apid == KEYS_NO_APID) {
		printf("SWEET KEYS: no readable keys\n");
		return;
	}
	kern_return_t kr = kernel_thread_start(keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET KEYS: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
