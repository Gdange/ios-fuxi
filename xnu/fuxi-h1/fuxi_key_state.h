/* H1 polled-key state, shared by both PMIC readers and native host tests. */
#ifndef FUXI_KEY_STATE_H
#define FUXI_KEY_STATE_H

#define FUXI_KEYS_FAILURE_LIMIT 8u
struct fuxi_key_state {
	bool armed;       /* two released samples seen since polling began */
	bool sampled;
	bool last;
	bool down;        /* state delivered to HID */
};

/* Return true only when a debounced HID transition is ready. */
static bool
fuxi_key_sample(struct fuxi_key_state *key, bool valid, bool pressed)
{
	if (!valid) {
		key->sampled = false; /* a failed read breaks consecutive samples */
		return false;
	}
	bool steady = key->sampled && pressed == key->last;
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
	return true;
}

/* A failed active peripheral counts even when another one still works. */
static bool
fuxi_keys_failed(unsigned *consecutive, bool failed)
{
	*consecutive = failed ? *consecutive + 1 : 0;
	return *consecutive >= FUXI_KEYS_FAILURE_LIMIT;
}
#endif /* FUXI_KEY_STATE_H */
