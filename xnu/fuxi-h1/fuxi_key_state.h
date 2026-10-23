/*
 * H1 polled-key state, shared by both PMIC readers, the Fuxi touch driver
 * and native host tests.
 *
 * Ported from HackiOS hackios-xnu-userland userland/src/inject.c (MIT):
 * the tap-to-wake policy (TouchDetectionMode low byte 0xfe: a touch while
 * the display is off becomes a side-button press and the rest of that touch
 * is kept from iOS). Its licence:
 *
 * MIT License
 *
 * Copyright (c) 2026 OOSEX GANG
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Ported from HackiOS hackios-xnu-kernel xnu/files/bsd/dev/redwood_buttons.c
 * (APSL 2.0): the press start time and held-duration bookkeeping. Adapted
 * from a backboardd virtual service and a record queue to this kernel's
 * SweetHIDButtons path; the arbiter and injection word are this tree's.
 */
#ifndef FUXI_KEY_STATE_H
#define FUXI_KEY_STATE_H

#define FUXI_KEYS_FAILURE_LIMIT 8u
#define FUXI_KEYS 3u                    /* 0 power, 1 volume up, 2 volume down */
struct fuxi_key_state {
	bool armed;       /* two released samples seen since polling began */
	bool sampled;
	bool last;
	bool down;        /* state delivered to HID */
	uint64_t first;   /* time of the first sample of the current steady run */
	uint64_t at;      /* timestamp of the last delivered transition */
	uint64_t since;   /* timestamp of the delivered press that is (or was) down */
};

/*
 * Return true only when a debounced HID transition is ready. Its timestamp
 * (key->at) is when the new level was first sampled, not when the second,
 * confirming sample arrived: the HID event carries the press time.
 */
static inline bool
fuxi_key_sample(struct fuxi_key_state *key, bool valid, bool pressed, uint64_t now)
{
	if (!valid) {
		key->sampled = false; /* a failed read breaks consecutive samples */
		return false;
	}
	bool steady = key->sampled && pressed == key->last;
	if (!steady) {
		key->first = now;
	}
	key->sampled = true;
	key->last = pressed;
	if (!steady) {
		return false;
	}
	if (!key->armed) {
		key->armed = !pressed;
		return false;
	}
	if (pressed == key->down) {
		return false;
	}
	key->down = pressed;
	key->at = key->first;
	if (pressed) {
		key->since = key->at;
	}
	return true;
}

/* A failed active peripheral counts even when another one still works. */
static inline bool
fuxi_keys_failed(unsigned *consecutive, bool failed)
{
	*consecutive = failed ? *consecutive + 1 : 0;
	return *consecutive >= FUXI_KEYS_FAILURE_LIMIT;
}

/*
 * Every Fuxi key event goes through one arbiter: the PMIC poller, the
 * test injection sysctl and tap-to-wake are separate sources. A key is down
 * for HID while any source holds it, so each HID down gets exactly one HID
 * up whatever the sources do, and repeated downs or unmatched ups from one
 * source produce nothing. Timestamps given to HID never go backwards.
 */
#define FUXI_KEY_SRC_HW     0x1u
#define FUXI_KEY_SRC_INJECT 0x2u
#define FUXI_KEY_SRC_TAP    0x4u
struct fuxi_key_arbiter {
	uint8_t held[FUXI_KEYS];  /* FUXI_KEY_SRC_* bits per key */
	uint64_t last;            /* last timestamp handed to HID */
};

/* 1: deliver an HID down, -1: deliver an HID up, 0: no HID event. */
static inline int
fuxi_key_arbitrate(struct fuxi_key_arbiter *arb, unsigned key, unsigned source, bool down)
{
	if (key >= FUXI_KEYS) {
		return 0;
	}
	bool was = arb->held[key] != 0;
	if (down) {
		arb->held[key] |= (uint8_t)source;
	} else {
		arb->held[key] &= (uint8_t)~source;
	}
	bool now = arb->held[key] != 0;
	return now == was ? 0 : now ? 1 : -1;
}

static inline uint64_t
fuxi_key_stamp(struct fuxi_key_arbiter *arb, uint64_t at)
{
	if (at < arb->last) {
		at = arb->last;
	}
	arb->last = at;
	return at;
}

/* Bit k: key k held by a source in mask (any source: what HID last saw). */
static inline unsigned
fuxi_key_held_mask(const struct fuxi_key_arbiter *arb, unsigned sources)
{
	unsigned mask = 0;
	for (unsigned k = 0; k < FUXI_KEYS; ++k) {
		if (arb->held[k] & sources) {
			mask |= 1u << k;
		}
	}
	return mask;
}

/*
 * debug.fuxikeys.inject command word: bits 0-7 key (0 power, 1 volume up,
 * 2 volume down), bits 8-15 operation, bits 16-27 click hold in ms (0 means
 * 100 ms, at most FUXI_KEY_INJECT_CLICK_MAX_MS), bits 28-31 zero.
 */
#define FUXI_KEY_INJECT_DOWN  1u
#define FUXI_KEY_INJECT_UP    2u
#define FUXI_KEY_INJECT_CLICK 3u
#define FUXI_KEY_INJECT_CLICK_DEFAULT_MS 100u
#define FUXI_KEY_INJECT_CLICK_MAX_MS 2000u
#define FUXI_KEY_INJECT_HOLD_MAX_MS 10000u /* an injected down is released after this */
struct fuxi_key_inject {
	unsigned key, op, hold_ms;
};

static inline int
fuxi_key_inject_decode(int value, struct fuxi_key_inject *cmd)
{
	uint32_t word = (uint32_t)value;
	cmd->key = word & 0xffu;
	cmd->op = (word >> 8) & 0xffu;
	cmd->hold_ms = (word >> 16) & 0xfffu;
	if ((word >> 28) != 0 || cmd->key >= FUXI_KEYS ||
	    cmd->op < FUXI_KEY_INJECT_DOWN || cmd->op > FUXI_KEY_INJECT_CLICK ||
	    (cmd->op != FUXI_KEY_INJECT_CLICK && cmd->hold_ms != 0) ||
	    cmd->hold_ms > FUXI_KEY_INJECT_CLICK_MAX_MS) {
		return EINVAL;
	}
	if (cmd->op == FUXI_KEY_INJECT_CLICK && cmd->hold_ms == 0) {
		cmd->hold_ms = FUXI_KEY_INJECT_CLICK_DEFAULT_MS;
	}
	return 0;
}

/*
 * Tap-to-wake: backboardd sets TouchDetectionMode on the touch service; its
 * low byte is 0xfe while the display is off and a tap is to wake it (0x1
 * with the display on, 0xff for no touch at all; -1 here: never set). At
 * most one wake press per interval, so a double tap cannot wake and then
 * lock again before the display has come on.
 */
#define FUXI_TOUCH_MODE_WAKE 0xfe
static inline bool
fuxi_tap_wake_allowed(int mode, uint64_t now, uint64_t *last, uint64_t interval)
{
	if (mode < 0 || (mode & 0xff) != FUXI_TOUCH_MODE_WAKE) {
		return false;
	}
	if (*last != 0 && now - *last < interval) {
		return false;
	}
	*last = now ? now : 1;
	return true;
}

/*
 * Touch side of tap-to-wake. While the display is off every contact is
 * tracked (and dropped, as before). Once a wake press has been sent in this
 * off period, the contacts present then and later stay hidden from iOS
 * until each lifts, so the waking finger does not start a touch mid-contact
 * when the display comes on. Without a wake press nothing is hidden.
 */
struct fuxi_wake_touch {
	uint32_t off;     /* contacts down while the display is off */
	uint32_t swallow; /* contacts kept from iOS until they lift */
	bool woke;        /* a wake press was sent in this off period */
};

static inline void
fuxi_wake_touch_display(struct fuxi_wake_touch *w, bool off)
{
	w->off = 0;
	if (off) {
		w->woke = false;
	}
}

static inline void
fuxi_wake_touch_woke(struct fuxi_wake_touch *w)
{
	w->woke = true;
	w->swallow |= w->off;
}

static inline void
fuxi_wake_touch_reset(struct fuxi_wake_touch *w)
{
	w->off = 0;
	w->swallow = 0;
}

/* True when the event must not reach iOS; *fresh: a new contact while off. */
static inline bool
fuxi_wake_touch_event(struct fuxi_wake_touch *w, bool off, unsigned id, bool touching,
    bool *fresh)
{
	uint32_t bit = 1u << (id & 31u);
	*fresh = false;
	if (off) {
		if (touching && !(w->off & bit)) {
			w->off |= bit;
			*fresh = true;
			if (w->woke) {
				w->swallow |= bit;
			}
		} else if (!touching) {
			w->off &= ~bit;
			w->swallow &= ~bit;
		}
		return true;
	}
	if (w->swallow & bit) {
		if (!touching) {
			w->swallow &= ~bit;
		}
		return true;
	}
	return false;
}
#endif /* FUXI_KEY_STATE_H */
