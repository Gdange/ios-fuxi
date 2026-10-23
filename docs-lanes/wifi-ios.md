# P-WIFI: iOS Wi-Fi interface shim

Source-only, opt-in bring-up. Patch **0104-fuxi-wifi-ios.patch** publishes a
synthetic Wi-Fi `en0` to iOS 16.7 on Fuxi. The WCN7850 driver in patch 0099 is
unchanged: QMI setup does not yet provide Ethernet TX/RX, association, scanning
or key installation. This lane performs **no radio, PCIe, firmware or DMA access**.
Host tests and local arm64 syntax checks pass; no device result is claimed.

## Enablement and backend boundary

`fuxiwifiios=1` enables the shim. Missing, zero or other values do nothing:
no interface, station thread or discovery services are created. Preserve all
other caller boot arguments. No `fuxiwlan` argument is required for the fake
backend. The existing USB Ethernet interface is `ue0`; it is not renamed.

The entry runs after `net_init_run()` in BSD initialization, before launchd.
`fuxi_wifi_ios.h` defines one immutable, boot-lifetime backend:

- `station`: power, scan request, cached results, connect (`join`), disconnect
  (`leave`), state snapshot, allowed channels and a locally administered MAC.
- `tx_frame`: borrowed Ethernet bytes, 14–1514 bytes without FCS; copy before
  returning. TX is serialized, but may overlap station/state calls.
- `start`: receives the `rx_frame` and `link_state` input callbacks. RX is
  copied before return; invoke both in thread context, never an interrupt.

Station requests are queued to one worker and answered asynchronously by
Apple80211 events. State snapshots and cached scans have separate locks.
Only init may attach the backend; replacement and detach are unsupported.
`start` must not emit events before attachment finishes. `join` returns only
after association and keys are ready; the worker then sends association/link
events. The backend changes the BSD carrier with `link_state`; unrequested
loss also calls `fuxi_wifi_sta_lost(errno)` after updating its state snapshot.
The shim bounds cached SSIDs, information elements and channel counts.

For a future `fuxi_wlan` adapter, select its backend instead of `fake_backend`
at the init call, after firmware/data-path readiness. Supply Ethernet
encapsulation/decapsulation and a bounded worker for real RX. The ported
`fuxi_wifi_wpa.h` offers PMK derivation, four-way/group EAPOL processing and
key outputs. The adapter must generate fresh nonces, own EAPOL routing and
retransmission/timeouts, program pairwise/group firmware keys, and assert
carrier only after successful installation. Nothing binds QMI success to a
usable link today. The crypto library is compiled and tested but is not used
by the open synthetic BSS.

The fake backend reports **Fuxi Loopback**, an open BSS on channel 1, RSSI
−42 dBm. It honors SSID/channel scan filters, accepts only that BSS, rejects
WPA2 requests with `EOPNOTSUPP`, and supports disconnect/power changes.
Only Ethernet type `0x88b5` is echoed to RX after joining. IP, ARP and EAPOL
are dropped: no DHCP server, gateway, DNS, Internet or real authentication
is simulated. A Settings association checkmark/routability is consequently
**not** a success criterion. No passphrase/network file is read or retained
as a standing/fallback connection; wifid owns rejoin policy.

## iOS discovery and private ioctl ABI

The BSD interface uses the Ethernet ifnet KPI, Wi-Fi subfamily and 802.11
media reporting. Its discovery tree is a property-only `IO80211Controller`
with an `IONetworkInterface` subclass, `FuxiWLAN`. It supplies
`IO80211InterfaceRole=Infrastructure`, `IOInterfaceName=en0`, MAC, type/unit
and built-in properties. `newUserClient` returns unsupported so the framework
can fall back to `SIOCGA80211`/`SIOCSA80211`. This does not implement Skywalk,
IO80211Family's private vtables or a user client ABI.

`FuxiNetworkStack.cpp` publishes `IOResources/IONetworkStack` and its minimal
`IONetworkStackUserClient`. HackiOS found that configd's InterfaceNamer waits
about five minutes without it, blocking interface enumeration. The user
client opens and refuses naming commands because the ifnet is already named.
This must be validated against this root's configd; other drivers keeping the
registry busy can still delay quiet-state detection.

The existing [iOS 16.7 lane investigation](wlan-bt.md#3-what-ios-16-expects-iphone-x-userland)
records wifid's DT gate, interface discovery and ioctl usage. HackiOS's source
comments report private layouts reverse-read from **16.5** IO80211.framework.
No matching 16.7 private header or independently decoded 20H392 framework
layout was available in the inspected notes/headers. **All private payload
layouts below remain unverified for 16.7**; same-generation XNU alone proves
no compatibility. Static assertions and tests verify our encoding, not Apple's
16.7 consumer. No Apple binaries or extracted private headers are committed.

| Request / data | HackiOS 16.5 bytes / offsets retained | 16.7 evidence |
| --- | --- | --- |
| Outer request | 40; type 16, value 20, length 24, arm64 user pointer 32 | arm64 C layout/ioctl size checked; consumer unverified |
| GET / SET ioctl | `0xc02869c9` / `0x802869c8` | use recorded in lane notes; layout unverified |
| ASSOCIATE (20) | 592; SSID length `0x0c`, key `0x48` | unverified |
| SCAN_REQ (10) | 5456; channels `0x48`, flags `0x1308`, names `0x1318` | unverified |
| SCAN_RESULT (11), CURRENT_NETWORK (103) | 1164; SSID length `0x60`, age `0x84`, IEs `0x8c` | unverified |
| SUPPORTED_CHANNELS (27/254) | 4824 | unverified |
| CHANNELS_INFO (207) | 10016; number row `0x32e`, flags `0x12d0` | unverified |
| Link event 4 | 32 after the 16-byte interface name; reason `0x14` | unverified |
| POWER (19) | 24 | unverified |
| CHANNEL (4), AUTH_TYPE (2) | 16 each | unverified |
| RSSI (16) | 52 | unverified |
| CARD_CAPABILITIES (12) | 13 bare bytes (no version prefix) | unverified |
| DRIVER/HARDWARE_VERSION (43/44) | 256 text bytes | unverified |
| SSID / BSSID (1/9) | raw SSID bytes / 6 bytes | unverified |
| STATE, OP_MODE, ASSOCIATE_RESULT (13/15/21) | outer `req_val` | unverified |

Known SET sizes are checked (scan/association require the complete known
layout). Unsupported types return `EOPNOTSUPP`; malformed lengths return
`EINVAL`; bad user buffers propagate `EFAULT`. SET requires a superuser
caller. GET copies at most the caller's buffer size. Logs contain request
types, lengths and status, never SSIDs, BSSIDs, passphrases or keys. The
shared `fuxi_wifi_apple80211.h` is the single ABI definition for kernel,
host fixtures and the optional probe. Record actual 16.7 request sizes and
fallback logs before broadening any layout; do not silently treat new bytes
as a known structure.

## Explicit mounted-root edit

The root tool is independent of the boot argument and is **off by default**:

```sh
python3 tools/enable-wifi-ios.py "$IOS_ROOT"                  # inspect only
python3 tools/enable-wifi-ios.py "$IOS_ROOT" --enable-wifid   # explicit edit
python3 tools/enable-wifi-ios.py "$IOS_ROOT" --restore-wifid  # undo job keys
```

It finds `/usr/sbin/wifid` in `System/Library/xpc/launchd.plist` and, if
present, `System/Library/LaunchDaemons/com.apple.wifid.plist`. It removes only
`_LimitLoadToDeviceTree` and sets `Disabled=false`. Use the existing
`launchd_unsecure_cache=1` boot option for the edited cache. This does not
create a `wlan` DT node, enable other jobs, change sandbox/entitlements or
modify the root staging defaults. It refuses symlinked paths, validates all
inputs before writing, preserves the edited files' owner/mode/inode, and
saves the original two keys beside each plist in a mode-0600
`.fuxi-wifi-ios-backup.plist`. Repeat enable preserves the first backup;
restore reinstates only those keys and preserves unrelated later edits.
Keep backups with the edited offline root until undo is no longer needed.
No root image was mounted or edited in this task.

## Maintainer device test plan (not executed)

1. First boot the newly built patch stack with `fuxiwifiios` omitted and the
   unedited root. Require no `FUXI WIFI IOS:` lines, no new `en0` or
   FuxiWLAN/IONetworkStack service, and the existing UI/`ue0` behavior.
2. Prepare a separate offline root with the explicit tool above. Build/sign
   `tools/fuxi-wifi-ios-test.c` with the normal iOS arm64 SDK and existing
   local signing/staging policy; stage it as `/usr/local/bin/fuxi-wifi-ios-test`.
   The source header gives the compile command. Do not replace any system
   daemon. Boot that root with the previous working command line plus
   `fuxiwifiios=1` and `launchd_unsecure_cache=1`, without enabling WCN7850.
3. Expected kernel startup lines (ordering of asynchronous events may vary):

   ```text
   FUXI WIFI IOS: enabled, synthetic backend; Apple80211 16.7 ABI unverified
   FUXI WIFI IOS: en0 attached on synthetic-loopback (a radio), link down
   ```

   Require an exec/start of wifid. Inspect the registry for the controller,
   FuxiWLAN and `IOResources/IONetworkStack`, and the service's `en0` and
   Infrastructure properties. Expected client-attempt line:

   ```text
   FuxiWLAN: a user client of type <type> is asked for: there is none
   ```

   Look for framework `Falling back to IOCTL path` if its logging is enabled.
   Require requests logged as `FUXI WIFI IOS: apple80211: wifid ...` with the
   table's sizes, and `MiniIONetworkStack: opened by configd; ...`. Observe
   InterfaceNamer reaching `*QUIET*` without the roughly five-minute
   `*TIMEOUT*` stall. A fallback failure or changed payload size is an ABI
   investigation result, not a passed shim test.
4. Run `/usr/local/bin/fuxi-wifi-ios-test --loopback` as root using an existing
   local test launch mechanism. It verifies the synthetic driver identity
   before sending SETs, powers on, scans, joins, sends a 60-byte experimental
   frame through BPF with outgoing capture disabled, checks RX, then queues
   disconnect. Expected probe lines:

   ```text
   P-WIFI TEST: scan PASS (synthetic BSS)
   P-WIFI TEST: connect PASS (no radio/DHCP)
   P-WIFI TEST: RX loopback PASS (outgoing BPF capture disabled)
   P-WIFI TEST: disconnect queued; PASS
   ```

   Corresponding kernel lines include:

   ```text
   FUXI WIFI IOS: fake scan complete
   FUXI WIFI IOS: fake connect result 0 (no radio)
   FUXI WIFI IOS: the link (synthetic-loopback) is up
   FUXI WIFI IOS: fake loopback frame 60 bytes
   FUXI WIFI IOS: the link (synthetic-loopback) is down
   ```

   `--loopback` needs the root's BPF device support; without it, run the probe
   without that option and report the RX test as untested. The queued
   disconnect is complete only when the kernel logs link down. No DHCP or
   routability is expected. Retain errors and ioctl lengths if a step fails.
5. Open Settings → Wi-Fi, toggle off/on and request a scan. Expect POWER SET
   (19), power events 0/1 and SCAN_REQ/SCAN_RESULT plus scan-done event 10.
   The synthetic BSS should appear if 16.7 discovery/ABI agrees. Attempting
   to join it must give association result 1, link-on and assoc event 9;
   IP configuration may remain pending. Confirm no panic, wifid respawn loop
   or configd five-minute enumeration delay. Do not enter a real password.
6. Restore the root job keys, omit `fuxiwifiios`, and repeat the baseline.
   Hardware driver validation remains a separate WLAN lane test.

## Provenance and exclusions

HackiOS kernel checkout `8e61187098f2c26c5daee8f9f72b6416aa64b9ab`,
**APSL-2.0**:

| Upstream source | Canonical Fuxi source |
| --- | --- |
| `xnu/files/bsd/dev/redwood_net.c`, `bsd/sys/redwood_net.h` (same `xnu/files/` prefix) | `fuxi_wifi_net.c/.h` |
| `xnu/files/bsd/dev/redwood_sta.c`, `bsd/sys/redwood_sta.h` (same prefix) | `fuxi_wifi_sta.c/.h` |
| `xnu/files/bsd/dev/redwood_apple80211.c` | `fuxi_wifi_apple80211.c/.h` |
| `xnu/files/iokit/Kernel/RedwoodWLAN.cpp` | `FuxiWLAN.cpp` |
| `xnu/files/iokit/Kernel/MiniIONetworkStack.cpp` | `FuxiNetworkStack.cpp` |
| `wifi/lx/src/lx_wpa.c/.h` | `fuxi_wifi_wpa.c/.h` |

Ported files retain their original commentary plus provenance/adaptation
headers. `LICENSE.HackiOS` retains the upstream licence and is also carried
inside 0104. Fuxi changes include names/gating, backend lifetime, removal of
QEMU/poll/packet-name tracing and credential-file rejoin policy, request bounds,
root SET checks, safe mbuf header adjustment, open-auth reporting and WPA reply
capacity/completed-handshake replay checks.

HackiOS userland checkout `906425b75e02b4f99eb5d7bb1ef7c9583472d772`,
**MIT**, was read for `userland/build.sh` wifid job notes and
`userland/src/net_test.m` diagnostic/16.5 association layout. No implementation
was copied from it. The root tool and diagnostic probe are original Fuxi code.
No UniXNU code was needed. Both reference projects were read only; none of
their tools, scripts, imports or builds was executed.

Excluded: Linux/ath11k compatibility layer and GPL sources; WCN6750/SM7325
hardware, virtio, firmware, root credential files, Apple binaries, private
framework patches, Skywalk, real radio scanning/association/key installation,
DHCP emulation, Mesa, metal2vulkan and NativeMetal. They are unnecessary for
this iOS shim or belong to later hardware/userland work. WPA arithmetic alone
does not supply retransmissions or a working radio supplicant.

## Host validation

```sh
python3 tools/sync-fuxi-wifi-ios.py --check
python3 tools/test-fuxi-wifi-ios.py
python3 tools/check-fuxi-wifi-ios.py "$XNU_BUILDTREE"
```

The tests compile production C with ASan/UBSan and a pthread station adapter;
cover missing/invalid/enabled gates, one-time attachment, malformed requests,
privilege/user-copy failures, synthetic scan/connect/disconnect/power, event
class/name/payload, truncation limits, Ethernet TX/RX ownership and mbuf header
adjustment, allocation/copy failures and fake frame filtering. An independent
Python hashlib/HMAC/AES-wrap peer checks PMK, PTK, message 1/3, group rekey,
MIC failure, replay and truncated EAPOL frames. Tests require `cryptography`.
Mounted-root fixtures cover dry-run, enable/idempotence, restore preserving
unrelated edits, malformed input and symlink refusal. The probe's portable
ioctl portion is also host syntax-checked; its Apple BPF portion needs an iOS
SDK build/device validation.

All seven kernel C/C++ units passed a local `arm64-apple-macos13` syntax check
using existing xnu-8796 build headers (older SWEET artifact selected with
`--config DEVELOPMENT_ARM64_SWEET --header-machine SWEET`). Apple type-signature
builtins are substituted as in `xnu-cc.py`; header/macro initializer warnings
remain. This is not a linked kernel or an iOS SDK build. Integration deltas
through 0104 were applied on the available local base; already present
0083/0084 deltas were reverse-checked. The sync check covers byte equality,
hunk counts and application of every generated source/integration hunk.
Existing `tools/test-fuxi-h1.py` and `tools/test-fuxi-wlan-w14.py` also pass.
No phone tools, CI/workflow, push or firmware/blob copy was used.
