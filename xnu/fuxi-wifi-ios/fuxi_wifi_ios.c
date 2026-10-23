/* Fuxi Wi-Fi iOS shim entry and synthetic Ethernet link. No radio access. */
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fuxi_wifi_ios.h>
#include <sys/fuxi_wifi_net.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>

extern void FuxiNetworkStackStart(void);
static const struct fuxi_wifi_backend *backend;
static bool enabled;

static bool
transmit(mbuf_t packet, size_t length)
{
	uint8_t frame[FUXI_WIFI_FRAME_MAX];
	return length >= 14 && length <= sizeof(frame) &&
	       mbuf_copydata(packet, 0, length, frame) == 0 && backend->tx_frame(frame, length);
}

static struct fuxi_wifi_net_link link = {
	.name = "synthetic-loopback", .wireless = true, .transmit = transmit,
};
static const struct fuxi_wifi_input input = {
	.rx_frame = fuxi_wifi_net_input, .link_state = fuxi_wifi_net_link_state,
};

bool
fuxi_wifi_ios_attach(const struct fuxi_wifi_backend *ops)
{
	/* Init-only, before publication. Backend/storage must live until shutdown. */
	if (!enabled || backend != NULL || ops == NULL || ops->tx_frame == NULL || ops->start == NULL ||
	    ops->station.power == NULL || ops->station.scan == NULL || ops->station.heard == NULL ||
	    ops->station.join == NULL || ops->station.leave == NULL || ops->station.state == NULL ||
	    ops->station.channels == NULL) {
		return false;
	}
	backend = ops;
	memcpy(link.address, ops->station.address, sizeof(link.address));
	ops->start(&input);
	fuxi_wifi_sta_attach(&ops->station);
	if (!fuxi_wifi_sta_present() || !fuxi_wifi_net_attach(&link)) {
		printf("FUXI WIFI IOS: attach failed; no retry this boot\n");
		return false;
	}
	FuxiNetworkStackStart();
	return true;
}

/* The synthetic station has one open BSS. Never accept real credentials or
 * claim WPA2 success: there is no access point or firmware key installation.
 */
static LCK_GRP_DECLARE(fake_group, "fuxi-wifi-fake");
static LCK_MTX_DECLARE(fake_lock, &fake_group);
static const struct fuxi_wifi_input *fake_input;
static struct fuxi_wifi_sta_state fake_state;
static bool fake_heard;
static const struct fuxi_wifi_sta_bss fake_bss = {
	.bssid = { 0x02, 0x46, 0x55, 0x58, 0x49, 0x02 },
	.ssid = "Fuxi Loopback", .ssid_length = 13, .frequency = 2412,
	.rssi = -42, .beacon_interval = 100, .capability = 1,
	.elements_length = 4, .elements = { 1, 2, 0x82, 0x84 },
};

static void
fake_start(const struct fuxi_wifi_input *in)
{
	fake_input = in;
	fake_state.phase = FUXI_WIFI_STA_IDLE;
}

static int
fake_power(bool on)
{
	lck_mtx_lock(&fake_lock);
	bzero(&fake_state, sizeof(fake_state));
	fake_state.phase = on ? FUXI_WIFI_STA_IDLE : FUXI_WIFI_STA_OFF;
	fake_heard = false;
	lck_mtx_unlock(&fake_lock);
	fake_input->link_state(false);
	return 0;
}

static int
fake_scan(const struct fuxi_wifi_sta_scan *scan)
{
	bool match = scan->ssid_length == 0 || (scan->ssid_length == fake_bss.ssid_length &&
	    memcmp(scan->ssid, fake_bss.ssid, scan->ssid_length) == 0);
	bool channel = scan->frequency_count == 0;
	for (unsigned i = 0; i < scan->frequency_count; i++) {
		channel |= scan->frequencies[i] == fake_bss.frequency;
	}
	lck_mtx_lock(&fake_lock);
	fake_heard = match && channel;
	lck_mtx_unlock(&fake_lock);
	printf("FUXI WIFI IOS: fake scan complete\n");
	return 0;
}

static void
fake_results(void (*each)(const struct fuxi_wifi_sta_bss *, void *), void *context)
{
	bool have;
	lck_mtx_lock(&fake_lock);
	have = fake_heard;
	lck_mtx_unlock(&fake_lock);
	if (have) {
		each(&fake_bss, context);
	}
}

static int
fake_leave(void)
{
	lck_mtx_lock(&fake_lock);
	bzero(&fake_state, sizeof(fake_state));
	fake_state.phase = FUXI_WIFI_STA_IDLE;
	lck_mtx_unlock(&fake_lock);
	fake_input->link_state(false);
	return 0;
}

static int
fake_join(const struct fuxi_wifi_sta_join *join)
{
	static const uint8_t any[6];
	int error = 0;
	if (join->security != FUXI_WIFI_STA_OPEN) {
		error = EOPNOTSUPP;
	} else if (join->ssid_length != fake_bss.ssid_length ||
	    memcmp(join->ssid, fake_bss.ssid, fake_bss.ssid_length) != 0 ||
	    (memcmp(join->bssid, any, 6) != 0 && memcmp(join->bssid, fake_bss.bssid, 6) != 0)) {
		error = ENOENT;
	}
	lck_mtx_lock(&fake_lock);
	bzero(&fake_state, sizeof(fake_state));
	fake_state.phase = error == 0 ? FUXI_WIFI_STA_JOINED : FUXI_WIFI_STA_IDLE;
	fake_state.last_error = error;
	if (error == 0) {
		fake_state.bss = fake_bss;
	}
	lck_mtx_unlock(&fake_lock);
	fake_input->link_state(error == 0);
	printf("FUXI WIFI IOS: fake connect result %d (no radio)\n", error);
	return error;
}

static void
fake_snapshot(struct fuxi_wifi_sta_state *state)
{
	lck_mtx_lock(&fake_lock);
	*state = fake_state;
	lck_mtx_unlock(&fake_lock);
}

static unsigned
fake_channels(struct fuxi_wifi_sta_channel *out, unsigned most)
{
	if (most == 0) {
		return 0;
	}
	out[0] = (struct fuxi_wifi_sta_channel) { .frequency = 2412 };
	return 1;
}

static bool
fake_tx(const uint8_t *frame, size_t length)
{
	bool up;
	lck_mtx_lock(&fake_lock);
	up = fake_state.phase == FUXI_WIFI_STA_JOINED;
	lck_mtx_unlock(&fake_lock);
	/* Echo only local experimental frames. IP/ARP/EAPOL have no synthetic peer. */
	if (!up || length < 14 || length > FUXI_WIFI_FRAME_MAX || frame[12] != 0x88 || frame[13] != 0xb5) {
		return false;
	}
	fake_input->rx_frame(frame, length);
	printf("FUXI WIFI IOS: fake loopback frame %zu bytes\n", length);
	return true;
}

static const struct fuxi_wifi_backend fake_backend = {
	.station = { .power = fake_power, .scan = fake_scan, .heard = fake_results,
		.join = fake_join, .leave = fake_leave, .state = fake_snapshot, .channels = fake_channels,
		.address = { 0x02, 0x46, 0x55, 0x58, 0x49, 0x01 } },
	.tx_frame = fake_tx, .start = fake_start,
};

void
fuxi_wifi_ios_init(void)
{
	int on = 0;
	PE_parse_boot_argn("fuxiwifiios", &on, sizeof(on));
	if (on != 1 || enabled) {
		return;
	}
	enabled = true;
	printf("FUXI WIFI IOS: enabled, synthetic backend; Apple80211 16.7 ABI unverified\n");
	(void)fuxi_wifi_ios_attach(&fake_backend);
}
#endif /* FUXI */
