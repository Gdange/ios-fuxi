/*
 * FUXI keys: polled Power / Volume up / Volume down for SweetHIDButtons.
 * Hardware (live DT research/device/probe-09/live.dtb, docs-lanes/buttons.md):
 *   Power    PMK8550 SID 0, pon_hlos@1300 RT_STS 0x1310 bit 7 (KPDPWR_N_SET)
 *   Vol down PMK8550 SID 0, same register, bit 6 (RESIN_N_SET)
 *   Vol up   PM8550 SID 1, GPIO6 @0x8d00, RT_STS 0x8d10 bit 0, active low
 *            (gpio_keys/vol_up: gpios <pm8550_gpios 6 GPIO_ACTIVE_LOW>,
 *            key_vol_up_default: input, pull-up, power-source 1)
 * SPMI PMIC arbiter v7 (qcom,spmi@c42d000, EE 0): core 0x0c400000 (APID map
 * 0x2000 + 4n), cnfg 0x0c42d000 (owner 4n), obsrvr 0x0c440000 (0x20 per
 * APID), chnls 0x0c500000 (0x1000 per APID). Offsets as Linux
 * drivers/spmi/spmi-pmic-arb.c v7.
 * READ ONLY by default: the only stores issue EXT_READL on observer
 * channels. With boot-arg fuxikeys_gpiocfg=1, and only if GPIO6 is not an
 * enabled input, the Linux pinctrl defaults for that one pin are written
 * through its RW channel (volatile PMIC GPIO config, nothing else).
 *
 * All HID key events of this file pass one arbiter (fuxi_key_state.h):
 * the PMIC poller, the root-only test injection sysctl debug.fuxikeys.inject
 * (boot-arg fuxikeyinject=1) and tap-to-wake (boot-arg fuxitaptowake=1,
 * driven by bsd/dev/fuxi_touch.c), plus the boot-time key script
 * (boot-arg fuxikeyscript=S) that uses the injection path. Each HID event carries the time its new
 * level was first sampled. debug.fuxikeys.held reports what HID has seen.
 * Ported from HackiOS hackios-xnu-userland userland/src/inject.c (MIT):
 * tap-to-wake (fuxi_keys_touch_wake).
 * Ported from HackiOS hackios-xnu-kernel xnu/files/bsd/dev/redwood_buttons.c
 * (APSL 2.0): the held-duration log. Notices and details in fuxi_key_state.h.
 */
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi_bus.h"
#include "fuxi_key_state.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_key_at(int key, int down, uint64_t timestamp);
extern int sweet_hid_touch_mode(void);
extern void fuxi_perf_input(void);
void fuxi_keys_init(void);
bool fuxi_keys_touch_wake(void);

#define FK_CORE_PA     0x0c400000u
#define FK_CNFG_PA     0x0c42d000u
#define FK_OBS_PA      0x0c440000u   /* EE 0 */
#define FK_CHNLS_PA    0x0c500000u
#define FK_MAP_OFF     0x2000u
#define FK_MAX_APIDS   1024u
#define FK_PON_PPID    0x013u        /* SID 0, 0x13xx */
#define FK_GPIO_PPID   0x18du        /* SID 1, 0x8dxx = GPIO6 */
#define FK_PON_RT      0x10u
#define FK_PON_PWR     0x80u
#define FK_PON_RESIN   0x40u
#define FK_NO_APID     0xffffu

static vm_offset_t fk_core, fk_cnfg, fk_obs;
static uint16_t fk_pon_apid = FK_NO_APID, fk_gpio_apid = FK_NO_APID;
static uint32_t fk_last_status;
static bool fk_started;
/* A late completion after timeout must never be mistaken for a new sample. */
static bool fk_timed_out[1024];
static int fk_chan;
static const char *const fk_names[FUXI_KEYS] = {"power", "volume-up", "volume-down"};

static LCK_GRP_DECLARE(fk_lck_grp, "fuxi-keys");
static LCK_MTX_DECLARE(fk_hid_lock, &fk_lck_grp);
static struct fuxi_key_arbiter fk_arb;        /* fk_hid_lock */
static uint64_t fk_tap_last;                  /* fk_hid_lock */
static thread_call_t fk_inject_call;
static int fk_inject_chan;

static uint64_t
fk_ms(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_MSEC;
}

/* With fk_hid_lock held: the one place that hands Fuxi keys to HID. */
static int
fk_deliver_locked(unsigned key, unsigned source, bool down, uint64_t at)
{
	int change = fuxi_key_arbitrate(&fk_arb, key, source, down);
	if (change) {
		sweet_hid_key_at((int)key, change > 0, fuxi_key_stamp(&fk_arb, at));
	}
	return change;
}

static int
fk_deliver(unsigned key, unsigned source, bool down, uint64_t at)
{
	lck_mtx_lock(&fk_hid_lock);
	int change = fk_deliver_locked(key, source, down, at);
	lck_mtx_unlock(&fk_hid_lock);
	return change;
}

static uint32_t
fk_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* v7 can map one PPID more than once; prefer the entry owned by EE 0. */
static uint16_t
fk_find_apid(uint16_t ppid, uint32_t count, unsigned *owner_out)
{
	uint16_t found = FK_NO_APID;
	unsigned found_owner = 7;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t map = fk_rd(fk_core, FK_MAP_OFF + 4u * i);
		if (map == 0 || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		unsigned owner = fk_rd(fk_cnfg, 4u * i) & 7u;
		if (found == FK_NO_APID || owner == 0) {
			found = (uint16_t)i;
			found_owner = owner;
		}
	}
	*owner_out = found_owner;
	return found;
}

static bool
fk_read_locked(uint16_t apid, uint8_t reg, uint8_t *out)
{
	if (apid == FK_NO_APID || fk_timed_out[apid]) {
		return false;
	}
	uint32_t off = 0x20u * apid;
	uint32_t cmd = (1u << 27) | ((uint32_t)reg << 4); /* EXT_READL, 1 byte */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(fk_obs + off) = cmd;
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		fk_last_status = fk_rd(fk_obs, off + 0x08u);
		if (fk_last_status & 1u) {
			if (fk_last_status & 0x0eu) {
				return false;
			}
			*out = (uint8_t)fk_rd(fk_obs, off + 0x18u);
			return true;
		}
		delay(1);
	}
	fk_timed_out[apid] = true;
	return false;
}

static bool
fk_read(uint16_t apid, uint8_t reg, uint8_t *out)
{
	sweet_spmi_bus_lock();
	bool ok = fk_read_locked(apid, reg, out);
	sweet_spmi_bus_unlock();
	return ok;
}

/* Opt-in only (fuxikeys_gpiocfg=1): one EXT_WRITEL on the pin's RW channel. */
static bool
fk_write_locked(vm_offset_t chan, uint8_t reg, uint8_t val)
{
	*(volatile uint32_t *)(chan + 0x10u) = val;           /* WDATA0 */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(chan + 0x00u) = (uint32_t)reg << 4; /* EXT_WRITEL */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		fk_last_status = fk_rd(chan, 0x08u);
		if (fk_last_status & 1u) {
			return (fk_last_status & 0x0eu) == 0;
		}
		delay(1);
	}
	return false;
}

static bool
fk_write(vm_offset_t chan, uint8_t reg, uint8_t val)
{
	sweet_spmi_bus_lock();
	bool ok = fk_write_locked(chan, reg, val);
	sweet_spmi_bus_unlock();
	return ok;
}

static bool
fk_gpio_input(uint8_t subtype, uint8_t mode, uint8_t en)
{
	unsigned dir = subtype >= 0x10u && subtype <= 0x13u ?
	    mode & 3u : (mode >> 4) & 7u;
	return (en & 0x80u) && (dir == 0 || dir == 2);
}

static bool
fk_gpio_dump(const char *when, uint8_t *subtype, uint8_t *mode, uint8_t *en)
{
	uint8_t type = 0, vin = 0, pull = 0, rt = 0;
	bool ok = fk_read(fk_gpio_apid, 0x04, &type) && type == 0x10 &&
	    fk_read(fk_gpio_apid, 0x05, subtype) &&
	    *subtype >= 0x10 && *subtype <= 0x13 &&
	    fk_read(fk_gpio_apid, 0x40, mode) &&
	    fk_read(fk_gpio_apid, 0x41, &vin) &&
	    fk_read(fk_gpio_apid, 0x42, &pull) &&
	    fk_read(fk_gpio_apid, 0x46, en) &&
	    fk_read(fk_gpio_apid, 0x10, &rt);
	printf("FUXI KEYS: %s gpio6 ok=%d type=%#x subtype=%#x mode=%#x vin=%#x pull=%#x en=%#x rt=%#x\n",
	    when, ok, type, *subtype, *mode, vin, pull, *en, rt);
	return ok;
}

static void
fk_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&fk_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
fk_setup(void)
{
	fk_core = ml_io_map(FK_CORE_PA, 0x3000);
	fk_cnfg = ml_io_map(FK_CNFG_PA, 0x1000);
	fk_obs = ml_io_map(FK_OBS_PA, 0x8000);
	if (!fk_core || !fk_cnfg || !fk_obs) {
		printf("FUXI KEYS: MMIO mapping failed\n");
		return false;
	}
	uint32_t version = fk_rd(fk_core, 0);
	uint32_t count = fk_rd(fk_core, 4) & 0x7ffu;
	if (version < 0x70000000u || version >= 0x80000000u ||
	    count == 0 || count > FK_MAX_APIDS) {
		printf("FUXI KEYS: unsupported arbiter version=%#x apids=%u\n",
		    version, count);
		return false;
	}
	unsigned pon_owner, gpio_owner;
	fk_pon_apid = fk_find_apid(FK_PON_PPID, count, &pon_owner);
	fk_gpio_apid = fk_find_apid(FK_GPIO_PPID, count, &gpio_owner);
	uint8_t ptype = 0, psub = 0, pon = 0;
	bool pok = fk_read(fk_pon_apid, 0x04, &ptype) && ptype == 0x01 &&
	    fk_read(fk_pon_apid, 0x05, &psub) && psub == 0x09 &&
	    fk_read(fk_pon_apid, FK_PON_RT, &pon);
	printf("FUXI KEYS: arb=%#x apids=%u pon-apid=%u/ee%u gpio6-apid=%u/ee%u pon ok=%d type=%#x subtype=%#x rt=%#x st=%#x\n",
	    version, count, fk_pon_apid, pon_owner, fk_gpio_apid, gpio_owner,
	    pok, ptype, psub, pon, fk_last_status);
	if (!pok) {
		fk_pon_apid = FK_NO_APID;
	}

	uint8_t subtype = 0, mode = 0, en = 0;
	bool gok = fk_gpio_dump("boot", &subtype, &mode, &en);
	if (gok && !fk_gpio_input(subtype, mode, en)) {
		int cfg = 0;
		(void)PE_parse_boot_argn("fuxikeys_gpiocfg", &cfg, sizeof(cfg));
		vm_offset_t chan = 0;
		if (cfg == 1 && gpio_owner == 0) {
			chan = ml_io_map(FK_CHNLS_PA + 0x1000u * fk_gpio_apid, 0x1000);
		}
		if (chan) {
			/* Linux pinctrl-spmi-gpio order for key_vol_up_default. */
			bool wok = fk_write(chan, 0x41, 0x01) &&  /* VIN1 (power-source 1) */
			    fk_write(chan, 0x42, 0x00) &&          /* pull-up 30 uA */
			    fk_write(chan, 0x40, 0x00) &&          /* digital input */
			    fk_write(chan, 0x46, 0x80);            /* enable */
			printf("FUXI KEYS: gpio6 configured as input: ok=%d st=%#x\n",
			    wok, fk_last_status);
			gok = fk_gpio_dump("after-cfg", &subtype, &mode, &en);
		} else {
			printf("FUXI KEYS: gpio6 cfg skipped (fuxikeys_gpiocfg=%d owner=ee%u)\n",
			    cfg, gpio_owner);
		}
	}
	if (!gok || !fk_gpio_input(subtype, mode, en)) {
		printf("FUXI KEYS: gpio6 not an enabled input; volume-up withheld\n");
		fk_gpio_apid = FK_NO_APID;
	}
	if (fk_pon_apid == FK_NO_APID && fk_gpio_apid == FK_NO_APID) {
		printf("FUXI KEYS: no readable keys\n");
		return false;
	}
	printf("FUXI KEYS: polling every 20 ms (power=%d volume-up=%d volume-down=%d)\n",
	    fk_pon_apid != FK_NO_APID, fk_gpio_apid != FK_NO_APID,
	    fk_pon_apid != FK_NO_APID);
	return true;
}

static void
fk_thread(void *arg, wait_result_t result)
{
	struct fuxi_key_state keys[FUXI_KEYS] = {0};
	unsigned errors = 0;
	(void)arg;
	(void)result;
	if (!fk_setup()) {
		goto out;
	}
	for (;;) {
		uint8_t pon = 0, gpio = 1;
		uint64_t t = mach_absolute_time();
		bool pok = fk_read(fk_pon_apid, FK_PON_RT, &pon);
		bool gok = fk_read(fk_gpio_apid, 0x10, &gpio);
		bool ok[3] = {pok, gok, pok};
		int now[3] = {!!(pon & FK_PON_PWR), !(gpio & 1u), !!(pon & FK_PON_RESIN)};
		bool failed = (fk_pon_apid != FK_NO_APID && !pok) ||
		    (fk_gpio_apid != FK_NO_APID && !gok);
		if (fuxi_keys_failed(&errors, failed)) {
			/* Balance delivered downs, so HID cannot retain a stuck key. */
			for (unsigned k = 0; k < FUXI_KEYS; ++k) {
				if (keys[k].down) {
					(void)fk_deliver(k, FUXI_KEY_SRC_HW, false, mach_absolute_time());
				}
			}
			printf("FUXI KEYS: stopped after %u consecutive read failures st=%#x\n",
			    errors, fk_last_status);
			goto terminate;
		}
		for (unsigned k = 0; k < FUXI_KEYS; ++k) {
			if (fuxi_key_sample(&keys[k], ok[k], now[k], t)) {
				if (keys[k].down) {
					printf("FUXI KEYS: %s down (pon=%#x gpio6=%#x)\n", fk_names[k], pon, gpio);
				} else {
					printf("FUXI KEYS: %s up (pon=%#x gpio6=%#x) held %llu ms\n", fk_names[k],
					    pon, gpio, fk_ms(keys[k].at - keys[k].since));
				}
				fuxi_perf_input();
				(void)fk_deliver(k, FUXI_KEY_SRC_HW, keys[k].down, keys[k].at);
			}
		}
		fk_sleep(20);
	}
out:
	printf("FUXI KEYS: stopped\n");
terminate:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * fuxikeyscript=S: S seconds after the key thread starts, press Power (lock),
 * Power (wake), Volume Up twice and Volume Down once through the injection
 * path, so a boot without anyone at the phone shows how iOS maps each key.
 * Only for device tests; it does not need fuxikeyinject=1.
 */
static int fk_inject(const struct fuxi_key_inject *cmd);
static int fk_script_chan;
static const struct { unsigned key, gap_ms; const char *why; } fk_script_steps[] = {
	{0, 8000, "power (expect lock)"},
	{0, 8000, "power (expect wake)"},
	{1, 4000, "volume-up"},
	{1, 4000, "volume-up"},
	{2, 4000, "volume-down"},
};

static void
fk_script_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&fk_script_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
fk_script(void *arg, wait_result_t wr)
{
	(void)wr;
	unsigned delay = (unsigned)(uintptr_t)arg;
	printf("FUXI KEYS: script starts in %u s\n", delay);
	fk_script_sleep(delay * 1000u);
	for (unsigned i = 0; i < sizeof(fk_script_steps) / sizeof(fk_script_steps[0]); ++i) {
		struct fuxi_key_inject cmd = {fk_script_steps[i].key, FUXI_KEY_INJECT_CLICK, 120};
		printf("FUXI KEYS: script step %u %s\n", i + 1, fk_script_steps[i].why);
		(void)fk_inject(&cmd);
		fk_script_sleep(fk_script_steps[i].gap_ms);
	}
	printf("FUXI KEYS: script done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
fuxi_keys_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (fk_started) {
		return;
	}
	fk_started = true;
	(void)PE_parse_boot_argn("fuxikeys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("FUXI KEYS: disabled (fuxikeys=0)\n");
		return;
	}
	if (kernel_thread_start(fk_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("FUXI KEYS: kernel_thread_start failed\n");
	}
	unsigned script = 0;
	if (PE_parse_boot_argn("fuxikeyscript", &script, sizeof(script)) && script > 0 && script < 3600) {
		if (kernel_thread_start(fk_script, (void *)(uintptr_t)script, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			printf("FUXI KEYS: script thread_start failed\n");
		}
	}
}
/* Injected downs are released at the latest FUXI_KEY_INJECT_HOLD_MAX_MS later. */
static void
fk_inject_expire(thread_call_param_t p0, thread_call_param_t p1)
{
	(void)p0;
	(void)p1;
	uint64_t now = mach_absolute_time();
	lck_mtx_lock(&fk_hid_lock);
	unsigned mask = fuxi_key_held_mask(&fk_arb, FUXI_KEY_SRC_INJECT);
	for (unsigned k = 0; k < FUXI_KEYS; ++k) {
		if (mask & (1u << k)) {
			(void)fk_deliver_locked(k, FUXI_KEY_SRC_INJECT, false, now);
		}
	}
	lck_mtx_unlock(&fk_hid_lock);
	if (mask) {
		printf("FUXI KEYS: inject auto-release mask=%#x after %u ms\n", mask,
		    FUXI_KEY_INJECT_HOLD_MAX_MS);
	}
}

static int
fk_inject_one(unsigned key, bool down)
{
	int change = fk_deliver(key, FUXI_KEY_SRC_INJECT, down, mach_absolute_time());
	printf("FUXI KEYS: inject %s %s -> hid %s\n", fk_names[key], down ? "down" : "up",
	    change > 0 ? "down" : change < 0 ? "up" : "unchanged");
	if (change) {
		fuxi_perf_input();
	}
	return change;
}

static int
fk_inject(const struct fuxi_key_inject *cmd)
{
	if (!fk_inject_call) {
		thread_call_t call = thread_call_allocate(fk_inject_expire, NULL);
		if (!call) {
			return ENOMEM;
		}
		lck_mtx_lock(&fk_hid_lock);
		if (!fk_inject_call) {
			fk_inject_call = call;
			call = NULL;
		}
		lck_mtx_unlock(&fk_hid_lock);
		if (call) {
			(void)thread_call_free(call);
		}
	}
	if (cmd->op != FUXI_KEY_INJECT_UP) {
		uint64_t deadline;
		clock_interval_to_deadline(FUXI_KEY_INJECT_HOLD_MAX_MS, NSEC_PER_MSEC, &deadline);
		(void)thread_call_enter_delayed(fk_inject_call, deadline);
		(void)fk_inject_one(cmd->key, true);
	}
	if (cmd->op == FUXI_KEY_INJECT_CLICK) {
		uint64_t deadline;
		clock_interval_to_deadline(cmd->hold_ms, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&fk_inject_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
	if (cmd->op != FUXI_KEY_INJECT_DOWN) {
		(void)fk_inject_one(cmd->key, false);
	}
	return 0;
}

/*
 * debug.fuxikeys.inject: write a command word (fuxi_key_state.h) to press or
 * release a key through the same arbiter and HID path as the PMIC keys.
 * Root only (no CTLFLAG_ANYBODY, checked again here) and only with boot-arg
 * fuxikeyinject=1; otherwise writes fail with ENOTSUP. Reads return the
 * keys currently held by injection.
 */
static int
fk_sysctl_inject SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	lck_mtx_lock(&fk_hid_lock);
	int value = (int)fuxi_key_held_mask(&fk_arb, FUXI_KEY_SRC_INJECT), changed = 0;
	lck_mtx_unlock(&fk_hid_lock);
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	int enabled = 0;
	if (!PE_parse_boot_argn("fuxikeyinject", &enabled, sizeof(enabled)) || enabled != 1) {
		return ENOTSUP;
	}
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	struct fuxi_key_inject cmd;
	if ((error = fuxi_key_inject_decode(value, &cmd))) {
		return error;
	}
	return fk_inject(&cmd);
}

/*
 * debug.fuxikeys.held: bits 0-2 the keys HID has down (power, volume up,
 * volume down); bits 8-10 held by the PMIC, 16-18 by injection, 24-26 by
 * tap-to-wake. A SpringBoard-side long-press guard can read bit 0.
 */
static int
fk_sysctl_held SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	lck_mtx_lock(&fk_hid_lock);
	int value = (int)(fuxi_key_held_mask(&fk_arb, ~0u) |
	    fuxi_key_held_mask(&fk_arb, FUXI_KEY_SRC_HW) << 8 |
	    fuxi_key_held_mask(&fk_arb, FUXI_KEY_SRC_INJECT) << 16 |
	    fuxi_key_held_mask(&fk_arb, FUXI_KEY_SRC_TAP) << 24);
	lck_mtx_unlock(&fk_hid_lock);
	return SYSCTL_OUT(req, &value, sizeof(value));
}

SYSCTL_NODE(_debug, OID_AUTO, fuxikeys, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "Fuxi buttons");
SYSCTL_PROC(_debug_fuxikeys, OID_AUTO, inject, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, fk_sysctl_inject, "I", "inject a key event (root, boot-arg fuxikeyinject=1)");
SYSCTL_PROC(_debug_fuxikeys, OID_AUTO, held, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, fk_sysctl_held, "I", "keys held as HID sees them, and by which source");

/*
 * Called by bsd/dev/fuxi_touch.c for a new contact while the display is off.
 * With boot-arg fuxitaptowake=1 and TouchDetectionMode 0xfe, press and
 * release power (as HackiOS does) and return true: the touch driver then
 * keeps the rest of that contact from iOS. Not while power is held.
 */
bool
fuxi_keys_touch_wake(void)
{
	static int enabled = -1;
	static uint64_t interval;
	if (enabled < 0) {
		int on = 0;
		(void)PE_parse_boot_argn("fuxitaptowake", &on, sizeof(on));
		nanoseconds_to_absolutetime(1500 * NSEC_PER_MSEC, &interval);
		enabled = on == 1;
	}
	if (!enabled) {
		return false;
	}
	int mode = sweet_hid_touch_mode();
	uint64_t now = mach_absolute_time();
	lck_mtx_lock(&fk_hid_lock);
	bool go = fk_arb.held[0] == 0 && fuxi_tap_wake_allowed(mode, now, &fk_tap_last, interval);
	if (go) {
		(void)fk_deliver_locked(0, FUXI_KEY_SRC_TAP, true, now);
		(void)fk_deliver_locked(0, FUXI_KEY_SRC_TAP, false, now);
	}
	lck_mtx_unlock(&fk_hid_lock);
	if (go) {
		fuxi_perf_input();
		printf("FUXI KEYS: tap-to-wake power click (touch mode %#x)\n", mode);
	}
	return go;
}
#endif /* FUXI */
