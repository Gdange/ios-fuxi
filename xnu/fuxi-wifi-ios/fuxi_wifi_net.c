/*
 * Ported from HackiOS xnu/files/bsd/dev/redwood_net.c (APSL-2.0)
 * Fuxi adaptations, 2026-10-23: opt-in iOS shim; see docs-lanes/wifi-ios.md.
 * Original file commentary is retained below. See LICENSE.HackiOS.
 */
/*
 * en0: a network interface that is Ethernet as far as the rest of the kernel and iOS can tell.
 *
 * iOS expects its interfaces from IONetworkingFamily, which this kernel does not have; the BSD
 * half of the kernel does not need it. The interface is made with the kernel's own interface
 * KPI, the way bsd/net/if_fake.c makes its "feth": family Ethernet, the stock Ethernet framing
 * and demultiplexing, one function that is given a frame to send and one call to hand a frame
 * that has arrived to the stack. What iOS's user space makes of an interface that has no
 * IOKit object behind it is another matter: see userland/src/net_test.m.
 *
 * What carries the frames is a link driver's business; sys/fuxi_wifi_net.h is all that the two
 * know of each other. The first link is QEMU's virtio network device (redwood_virtio_net.c).
 */
#if defined(FUXI)
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/fuxi_wifi_net.h>
#include <sys/sockio.h>
#include <sys/systm.h>

extern void FuxiWLANStart(const char *interface, const uint8_t *address);  /* iokit/Kernel/FuxiWLAN.cpp */

static LCK_GRP_DECLARE(net_lck_grp, "fuxi-wifi-net");
static LCK_MTX_DECLARE(net_tx_lock, &net_lck_grp);
static const struct fuxi_wifi_net_link *net_link;
static bool net_link_up;
static ifnet_t net_ifp;

/* ---- the interface -------------------------------------------------------------------------------- */

/* The stack has a frame to send (a chain of mbufs that starts with the Ethernet header). */
static errno_t
net_output(ifnet_t ifp, mbuf_t packet)
{
	size_t length = mbuf_pkthdr_len(packet);
	bool sent = false;

	if (length >= ETHER_HDR_LEN && length <= FUXI_WIFI_NET_FRAME_MAX && __atomic_load_n(&net_link_up, __ATOMIC_ACQUIRE)) {
		bpf_tap_out(ifp, DLT_EN10MB, packet, NULL, 0);
		lck_mtx_lock(&net_tx_lock);
		sent = net_link->transmit(packet, length);
		lck_mtx_unlock(&net_tx_lock);
	}
	if (sent) {
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

/* A frame has arrived. */
void
fuxi_wifi_net_input(const uint8_t *frame, size_t length)
{
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	if (net_ifp == NULL || !__atomic_load_n(&net_link_up, __ATOMIC_ACQUIRE) || length < ETHER_HDR_LEN || length > FUXI_WIFI_NET_FRAME_MAX ||
	    mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		return;
	}
	/* as every Ethernet driver hands a frame up: the header noted and stepped over */
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, net_ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_adj(packet, ETHER_HDR_LEN);
	bpf_tap_in(net_ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	ifnet_input(net_ifp, packet, &stats);
}

static unsigned net_ioctls_said;

static errno_t
net_ioctl(ifnet_t ifp, u_long command, void *data)
{
	/* (a radio says it is one here too: IO80211.framework lists the interfaces whose media is 802.11) */
	const int media[] = { (net_link->wireless ? IFM_IEEE80211 : IFM_ETHER) | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *media_request = data;
	user_addr_t list;
	errno_t error = 0;

	if (net_link->wireless && net_ioctls_said < 80) {
		char who[32] = "?";

		/* (while iOS's Wi-Fi side is being got to talk to this: who asks the interface what) */
		proc_selfname(who, sizeof(who));
		if (strcmp(who, "configd") != 0) {
			net_ioctls_said++;
			printf("FUXI WIFI IOS: en0 is asked %#lx by %s\n", command, who);
		}
	}
	switch (command) {
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		/* running whenever it is up: the link is there or it is not, nothing is switched on */
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		/* what configd asks to learn whether the cable is in */
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)media_request)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)media_request)->ifmu_ulist);
		media_request->ifm_active = media_request->ifm_current = media[0];
		media_request->ifm_mask = 0;
		media_request->ifm_status = IFM_AVALID | (__atomic_load_n(&net_link_up, __ATOMIC_ACQUIRE) ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && media_request->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		media_request->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = ETHERMTU;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;                          /* the link hands us everything anyway */
	default:
		/* what iOS's Wi-Fi side asks of a radio's interface: bsd/dev/fuxi_wifi_apple80211.c */
		error = net_link->wireless ? fuxi_wifi_apple80211_ioctl(command, data) : EOPNOTSUPP;
		break;
	}
	return error;
}

/* Tell whoever listens for kernel events (configd does) that the link is up, or down. */
static void
net_link_event(ifnet_t ifp, uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(ifp);
	about->if_unit = ifnet_unit(ifp);
	strlcpy(about->if_name, ifnet_name(ifp), IFNAMSIZ);
	ifnet_event(ifp, header);
}

void
fuxi_wifi_net_link_state(bool up)
{
	if (net_ifp == NULL || up == __atomic_load_n(&net_link_up, __ATOMIC_ACQUIRE)) {
		return;
	}
	__atomic_store_n(&net_link_up, up, __ATOMIC_RELEASE);
	printf("FUXI WIFI IOS: the link (%s) is %s\n", net_link->name, up ? "up" : "down");
	net_link_event(net_ifp, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}


bool
fuxi_wifi_net_attach(const struct fuxi_wifi_net_link *link)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		/*
		 * The simple model: every frame straight to net_output(). And no Skywalk "nexus" made for
		 * the interface by itself: sockets are all that is asked of it so far.
		 */
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "fuxi-wifi-en0",
		.uniqueid_len = sizeof("fuxi-wifi-en0") - 1,
		.name = "en",
		.unit = 0,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = link->wireless ? IFNET_SUBFAMILY_WIFI : IFNET_SUBFAMILY_ANY,
		.type = IFT_ETHER,
		.output = net_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = net_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	const uint8_t *address = link->address;
	ifnet_t ifp = NULL;
	errno_t error;

	if (net_link != NULL) {
		printf("FUXI WIFI IOS: en0 is on %s already; %s is not attached\n", net_link->name, link->name);
		return false;
	}
	net_link = link;
	net_link_up = link->up;
	error = ifnet_allocate_extended(&init, &ifp);
	if (error != 0) {
		printf("FUXI WIFI IOS: the interface cannot be made: error %d\n", error);
		net_link = NULL;
		return false;
	}
	ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(ifp, 100 * 1000 * 1000);
	ifnet_set_mtu(ifp, ETHERMTU);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
	ifnet_set_offload(ifp, 0);
	error = ifnet_attach(ifp, NULL);
	if (error != 0) {
		printf("FUXI WIFI IOS: the interface cannot be attached: error %d\n", error);
		ifnet_release(ifp);
		net_link = NULL;
		return false;
	}
	ifnet_set_lladdr(ifp, address, ETHER_ADDR_LEN);
	bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
	net_ifp = ifp;
	if (net_link_up) {
		net_link_event(ifp, KEV_DL_LINK_ON);
	}
	if (link->wireless) {
		/* (a radio's address is the phone's own: not for the log; and iOS's Wi-Fi side finds the interface by a service) */
		printf("FUXI WIFI IOS: en0 attached on %s (a radio), link %s\n", link->name, net_link_up ? "up" : "down");
		FuxiWLANStart("en0", address);
	} else {
		printf("FUXI WIFI IOS: en0 attached on %s, address %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", link->name, address[0],
		    address[1], address[2], address[3], address[4], address[5], net_link_up ? "up" : "down");
	}
	return true;
}


#endif /* FUXI */
