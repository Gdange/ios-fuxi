/* Fuxi Wi-Fi iOS shim: boot-lifetime backend boundary. */
#ifndef FUXI_WIFI_IOS_H
#define FUXI_WIFI_IOS_H
#include <stddef.h>
#include <sys/fuxi_wifi_sta.h>

#define FUXI_WIFI_FRAME_MAX 1514

/* No mbuf/IOKit objects cross this boundary. RX is copied before return.
 * link_state is called after association/key installation or on link loss.
 * Callbacks run in thread context; no callback may retain a frame pointer.
 */
struct fuxi_wifi_input {
	void (*rx_frame)(const uint8_t *frame, size_t length);
	void (*link_state)(bool up);
};

/* One immutable backend for the boot. No detach/hot replacement. Station
 * callbacks are serialized by fuxi_wifi_sta; state() must synchronize its snapshot.
 * tx_frame can run concurrently with station callbacks and must copy before return.
 * start() only installs the input callbacks; it must not send events yet.
 * join() returns only after keys are installed; asynchronous loss also calls
 * fuxi_wifi_sta_lost(errno). A future WCN7850 backend owns its own EAPOL/key path,
 * using fuxi_wifi_wpa.h, and Ethernet decap/encap. No hardware is accessed here.
 */
struct fuxi_wifi_backend {
	struct fuxi_wifi_sta_ops station; /* power, scan request/results, connect/disconnect, state */
	bool (*tx_frame)(const uint8_t *frame, size_t length);
	void (*start)(const struct fuxi_wifi_input *input);
};

#ifdef KERNEL
bool fuxi_wifi_ios_attach(const struct fuxi_wifi_backend *backend);
void fuxi_wifi_ios_init(void);
#endif
#endif
