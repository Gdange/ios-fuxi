# WLAN W12: WLFW setup and target memory handshake

The supplied try11 excerpt establishes a fault-free W11 transport: START 20/21
completed, 15 RX buffers were posted, QRTR HELLO was answered by peer node 7,
and WLFW v1 instance 0x101 was discovered at node 7 port 1. CAP_REQ transaction
1 / message 0x24 received result 1, error **90**; channels were stopped and the
device was contained. The excerpt is read-only evidence and is not stored here.

Error 90 is **INCOMPATIBLE_STATE**, not an unsupported message or transport
failure. The [QMI common error list](https://code.googlesource.com/linux/torvalds/linux/+/94d957ae513fc420d0a5a9bac815eb49ffebb56f/include/linux/soc/qcom/qmi.h)
names it `QMI_ERR_INCOMPATIBLE_STATE_V01`. Only that numeric fact is used;
no GPL header, helper, decoder or driver implementation is imported.

The source ordering confirms W11 sent CAP_REQ before its setup prerequisites:
ath12k registers indications, sends host capabilities, services REQUEST_MEM,
and waits for FW_MEM_READY before requesting target capabilities. A correlated
state rejection at the W11 boundary is consistent with those missing steps.
The excerpt alone cannot prove firmware-internal causality or that there are
no other prerequisites; successful W12 hardware progress is still required.
**W12 has host validation only.**

## Selection and stop boundary

Use the proven hardware selectors with `fuxiwlanqrtr=2`:

```text
fuxiwlan=1 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix fuxiwlanqrtr=2
```

These are WLAN arguments only. Preserve the caller's other boot arguments.

| Argument | Behavior |
| --- | --- |
| `fuxiwlanqrtr=2` | W12 setup handshake inside step 28, after AMSS/M0 and transport preflight. |
| `fuxiwlanqrtr=1` | Retained W11 CAP_REQ-only diagnostic, including its expected rejection on try11 firmware. |
| QRTR absent/0/other integer | W10 boundary; no IPCR channels or WLFW handshake. |
| `fuxiwlankeep=1` | Retain only a complete, fault-free selected milestone, including target DMA, channels, RX buffers and asserted wake. |
| keep absent/0 | Contain after success, as well as on failure. |
| `fuxiwlan=N`, 2–27, or `fuxiwlanstop22=1` | Existing partial/diagnostic boundary, always contained; W12 does not run. |

The stop is immediately after capability discovery and a bounded 200-ms passive
indication observation period. There is **no BDF, regdb, calibration or M3
image download**, WLAN mode/config request, network interface, recovery or
second attempt. The log identifies regdb/BDF/M3 as the next **host-driven**
ath12k steps; firmware need not send another indication to ask for them.
Indications received during observation are logged. Keep ends polling; it does
not create a background service or promise indefinite firmware health.

## Protocol sources and attribution

The encoders, decoders, state machine, SMMU extension and simulated peer are
original code. Protocol field facts and the WCN7850 profile are taken from
Linux **v6.6**, whose relevant ath12k files are **BSD-3-Clause-Clear**:

* [qmi.h](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.h):
  message IDs, segment/config limits, enum values and field widths.
* [qmi.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.c):
  `qmi_wlanfw_*_ei` wire layouts; `ath12k_qmi_fw_ind_register_send`,
  `ath12k_qmi_host_cap_send`, `ath12k_host_cap_parse_mlo`, server-arrival,
  memory-request, memory-ready and load-BDF event ordering. No EI tables or
  routines are copied; QMI is encoded/decoded directly without qmi_helpers.
* [hw.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/hw.c):
  WCN7850 hw2.0 selects internal sleep clock and CNSS feature bits 3 and 4.

Credit: **The Linux Foundation (2018–2021)** and **Qualcomm Innovation Center,
Inc. (2021–2022)**. The production C also credits these sources. W11 supplies
the unchanged public MHI/OpenBSD QRTR register and transport provenance.
No firmware or downloaded source/blob is added to the repository.

### Message sequence and correlation

| Stage | Message | Transaction / payload |
| --- | --- | --- |
| IND_REGISTER | request/response `0x20` | txn 1; optional enable TLVs below; common result required in reply; optional `0x10` u64 firmware status logged. |
| HOST_CAP | request/response `0x34` | txn 2; WCN7850 profile below; common result required. |
| REQUEST_MEM | indication `0x35` | QMI flag 4; mandatory TLV `0x01` contains the requested segments. |
| RESPOND_MEM | request/response `0x36` | txn 3; TLV `0x01` supplies allocated IOVAs; common result required. |
| FW_MEM_READY | indication `0x37` | QMI flag 4; no required payload. |
| CAP_REQ | request/response `0x24` | txn 4; empty request TLVs; full response decoded below. |

QMI uses the existing seven-byte little-endian header, request flag 0 and
response flag 2. Responses must match the selected QRTR node/port and the
single outstanding transaction/message. Unknown/stale replies cannot advance
the handshake. Required result TLV `0x02` is exactly two u16 values, both zero;
missing, duplicate, malformed or rejecting results fail the milestone.

IND_REGISTER sends exactly these ath12k fields. The `_valid` host struct
members are represented by TLV presence, never extra on-wire bytes:

| TLV | Field | Wire value |
| --- | --- | --- |
| `0x10` | fw_ready_enable | u8 1 |
| `0x15` | client_id | u32 `0x4b4e454c` |
| `0x16` | request_mem_enable | u8 1 |
| `0x17` | fw_mem_ready_enable | u8 1 |
| `0x18` | fw_init_done_enable | u8 1 |
| `0x1b` | cal_done_enable | u8 1 |

As in ath12k, pin-connect, initiate-cal-download/update, MSA-ready,
rejuvenate and XO-cal enable TLVs are absent. Firmware-init-done is called
FW_READY (`0x38`) in ath12k's indication handler; older FW_READY (`0x21`),
calibration and unknown indications are logged/deferred without download work.

### HOST_CAP WCN7850 values

This milestone sends the ath12k WCN7850 bring-up profile, including fields
ath12k piggybacks on its internal-clock branch. It does not claim those bits
prove a working XNU data path. Their effect on this integrated KIWI firmware
still needs device evidence; there is no alternate-profile retry.

| TLV | Field / width | Value and reason |
| --- | --- | --- |
| `0x10` | num_clients, u32 | 1: one WLFW host client. |
| `0x13` | nm_modem, u8 | `0x0e`: internal sleep-clock bit `0x02`, host-C-state/non-Qualcomm-platform bit `0x04`, PCIe global-reset capability `0x08`, exactly ath12k's WCN7850 branch. This is a compatibility profile, despite XNU running on Qualcomm hardware. |
| `0x14` | bdf_support, u8 | 1: host BDF support advertised for the next milestone. |
| `0x16` | m3_support, u8 | 1: host M3 support advertised; W12 does not send M3_INFO. |
| `0x17` | m3_cache_support, u8 | 1: ath12k retains its M3 image on the host. W12 stops before that operation. |
| `0x1a` | cal_done, u8 | 0: fresh boot; no calibration has completed. |
| `0x1c` | mem_cfg_mode, u8 | 0: `ATH12K_QMI_TARGET_MEM_MODE_DEFAULT`. |
| `0x21` | mlo_capable, u8 | 1: ath12k internal-clock branch profile. |
| `0x22` | mlo_chip_id, u16 | 0: local chip. |
| `0x23` | mlo_group_id, u8 | 0: single group. |
| `0x24` | max_mlo_peer, u16 | 32: ath12k's fixed peer maximum. |
| `0x25` | mlo_num_chips, u8 | 1. |
| `0x26` | mlo_chip_info, static array | Three six-byte entries. First `[0,2,0,1,1,1]`: chip 0, two local links, hardware IDs 0/1, both valid. Other two entries zero. No array count prefix. |
| `0x27` | feature_list, u64 | `0x18`: CNSS_QDSS_CFG_MISS (bit 3), CNSS_PCIE_PERST_NO_PULL (bit 4), from WCN7850 hw.c. |

Wake MSI, GPIOs, BDF-cache, cal-filesystem/cache, mem-bucket, cal-duration,
platform name, DDR ranges and host-build-type TLVs remain absent, as they do
in this ath12k profile. HOST_CAP is 85 TLV bytes plus the QMI header.

## Target memory ownership and bounds

REQUEST_MEM TLV `0x01` contains **u8** segment count (1–52), followed by each
segment's u32 size, four-byte memory-type enum, **u8** config count (0–2), and
13 bytes per config: u64 offset, u32 size, u8 secure flag. All counts, TLV
lengths and trailing bytes are checked. Every segment and config is logged.
Type is echoed without translating it to an address; e.g. DDR 1, BDF 2,
M3 dump 3, CAL 4, pageable 9. Unknown types are logged and provisioned as plain
nonsecure target RAM, matching ath12k's type-agnostic allocation path.

Zero sizes and total allocations above **64 MiB** are refused. The cap counts
**XNU page rounding per segment**, so many small segments cannot evade it.
The parser checks the entire request and aggregate IOVA space before any
allocation. Offset/size config subranges must fit the containing segment;
secure configs are refused explicitly because W12 cannot provision secure RAM.
No fallback zero-address response, smaller-chunk negotiation or retry occurs.

Each segment gets zeroed wired storage with potentially noncontiguous backing,
a contiguous IOVA from the existing owned 256-MiB pool, and Normal-NC CPU
aliases with attribute readback. Firmware sees the **IOVA**, never a CPU VA or
backing PA. Requested byte sizes are returned; page-rounded padding remains
owned and counts toward the cap. No contents are read or written by the CPU
after RESPOND_MEM publication during this milestone.

The SMMU table hierarchy for the whole pool already exists before BHI. W12
extends the **mapped portion** of that window by adding only invalid leaves;
it does not rebuild live tables or change either pool's outer bounds. Every
new parent/leaf is checked through owned aliases rooted in the live TTBR.
Live TTBR, SCTLR, TCR, MAIR, SMR/S2CR and RC ownership readbacks are checked;
existing leaves are never overwritten. New table entries are cleaned (also
CIVAC in that mode), then ASID TLB invalidation/synchronization is bounded to
100 ms, and every new PTE is read back against its backing PA/attributes.
MHI's inherited 32-bit CTRL/DATA aperture already covers these sub-4-GiB IOVAs;
no live MHI aperture change is needed.

RESPOND_MEM TLV `0x01` contains u8 count followed by 17 bytes per segment:
u64 IOVA, u32 requested size, four-byte echoed type, u8 restore=0. At 52
segments it is 888 TLV bytes, 895 QMI bytes, fitting the enlarged 1024-byte
QRTR TX staging buffer and one MHI transfer. No fragmented packet is needed.

Partial target allocations participate in existing containment and quarantine.
Success without keep sends BYE/STOP then resets MHI, clears BME with readback,
restores MSI, asserts PERST/drains, undoes host/SMMU ownership and frees all
buffers. Failure takes the same containment path without a success BYE.
**Keep never overrides a failure or partial run.** Unproven containment/undo
quarantines all target allocations together with the other DMA allocations.

## Indication ordering, deadlines and capabilities

The one-conversation-per-boot latch is set before channel setup. No setup
request is retried. REQUEST_MEM may race the HOST_CAP response and is retained
until HOST_CAP succeeds; FW_MEM_READY may race RESPOND_MEM's response and is
retained until that response succeeds. Such races are logged as out of order.
Earlier REQUEST_MEM/FW_MEM_READY indications are logged and ignored and
cannot satisfy a later wait. A second accepted REQUEST_MEM fails without
reprovisioning memory the device may own. Duplicate FW_MEM_READY is logged
and ignored. Unknown/calibration/ready indications never complete a response
wait or initiate new operations; malformed TLV framing still fails.

Every WLFW response/indication wait has a 10-second deadline with 10-ms sleeps
and bounded event work, matching ath12k's timeout. START/STOP, QRTR discovery
and TX drain retain W11's two-second bounds. SMMU sync is bounded separately.
Access/SMMU faults, loss of AMSS/M0/discovery/ownership, rejected messages,
malformed selected packets, allocation/map failures or timeouts contain the
device even with keep selected. Stage numbers in indication logs are 0
(discovery), 1 (register), 2 (host), 3 (request-memory), 4 (respond-memory),
5 (memory-ready), 6 (capability), 7 (completed/observing).

CAP_RESP checks duplicate/length rules and logs every v6.6 capability field:

| TLV | Fields |
| --- | --- |
| `0x02` | mandatory common result/error |
| `0x10` | u32 chip ID, u32 chip family |
| `0x11`, `0x12` | u32 board ID, u32 SoC ID |
| `0x13` | u32 FW version, nested u8 length + up to 32 timestamp characters |
| `0x14` | up to 128 build-ID characters; top-level TLV length is string length |
| `0x15` | u8 number of MACs |
| `0x16`, `0x17`, `0x18`, `0x19` | u32 voltage mV, time frequency Hz, OTP version, EEPROM calibration read timeout |
| `0x1a` | u64 firmware capability flags |
| `0x1b` | u32 read-card chain capability |
| `0x1c` | static four u64 device-memory start/size pairs, reported only; never mapped or dereferenced |

The validity mask uses bit 0 for result and bits 1–13 for TLVs `0x10`–`0x1c`.
Absent numeric fields print zero with their bits clear; absent strings print
`absent`. Strings are bounded, terminated and sanitized. Unknown optional TLVs
are length-checked and logged by type/size. Reported capability data does not
trigger another request.

## Expected next-boot logs

These are templates, not captured W12 hardware results. Segment counts/sizes,
addresses, endpoint and capabilities depend on the supplied firmware:

```text
FUXI WLAN: W12 selected qrtr=2 target memory cap 67108864 bytes
FUXI WLAN: W11 chan 20/21 started
FUXI WLAN: W11 RX posted 15 buffers of 4096 bytes; polling RAM-sink MSI
FUXI WLAN: W11 QRTR hello peer node <node>
FUXI WLAN: W11 WLFW server node <node> port <port> instance 0x101
FUXI WLAN: W12 send txn 1 msg 0x20 stage 1
FUXI WLAN: W12 reply txn 1 msg 0x20 result 0 error 0
FUXI WLAN: W12 ind register ok
FUXI WLAN: W12 send txn 2 msg 0x34 stage 2
FUXI WLAN: W12 reply txn 2 msg 0x34 result 0 error 0
FUXI WLAN: W12 host cap ok
FUXI WLAN: W12 indication msg 0x35 stage <2-or-3> bytes <length>
FUXI WLAN: W12 request segment <i> type <type> size <bytes> cfg <count>
FUXI WLAN: W12 request mem n=<count> total=<bytes> rounded=<bytes> cap=67108864
FUXI WLAN: W12 DMA window extended IOVA <start>-<end> (pool end <end>), live SMMU/leaves readback verified
FUXI WLAN: W12 target segment <i> type <type> size <bytes> IOVA <address> allocated <bytes>
FUXI WLAN: W12 send txn 3 msg 0x36 stage 4
FUXI WLAN: W12 reply txn 3 msg 0x36 result 0 error 0
FUXI WLAN: W12 respond mem ok
FUXI WLAN: W12 indication msg 0x37 stage <4-or-5> bytes <length>
FUXI WLAN: W12 fw mem ready
FUXI WLAN: W12 send txn 4 msg 0x24 stage 6
FUXI WLAN: W12 WLFW reply txn 4 msg 0x24 result 0 error 0
FUXI WLAN: W12 cap: valid <mask> chip id <id> chip family <family> board id <board> soc id <soc> firmware <version> timestamp <text> build <text>
FUXI WLAN: W12 cap: num macs <n> voltage mv <mv> time freq hz <hz> otp <version> eeprom cal timeout <timeout> fw caps <flags> chain cap <value> (presence in valid mask)
FUXI WLAN: W12 capability complete; next ath12k host steps regdb/BDF/M3 deferred; observing indications 200 ms
FUXI WLAN: W12 handshake complete; stop/contain
FUXI WLAN: step 28 ok code 0x101c (... ms, faults 0)
```

Race logs may precede their stage's `ok` line. For keep=1, the final handshake
line says `keep requested, DMA/channels/wake retained (polling ended)`.
Without keep, BYE, STOP completion/unconfirmed, reset/BME/PERST and allocation
release follow. Rejection logs include result/error and label error 90 as
`(INCOMPATIBLE_STATE)`. Other failures use `W12 failure: <reason>; contain, no
retry` or `W12 timeout <stage> (10000 ms); contain, no retry`, followed by
step-28 failure and containment. Quarantine means containment/undo was unproven.

## Host verification

`python3 tools/test-fuxi-wlan-w12.py` compiles the production C with ASan/UBSan,
reusing W11's independent MHI event/credit/transfer peer and replacing its QMI
endpoint. Literal expectations check the complete IND_REGISTER/HOST_CAP and
CAP_REQ byte sequences; the peer checks RESPOND_MEM sizes/types/restore/IOVAs,
zeroed owned backing and PTEs before accepting it. Tests cover:

* Full optional capability decoding including OTP, 64-bit flags/device-memory,
  optional registration status, unknown TLVs and stale/duplicate/malformed replies.
* Indications before triggering requests, before triggering replies, unknown
  and premature-ready indications, duplicate memory requests/ready indications.
* Normal and maximum 52-segment provisioning, noncontiguous backing, crossing
  an L3 boundary, per-segment and aggregate/page-rounded caps, zero/oversized
  counts, truncation, config bounds/security and IOVA exhaustion.
* Partial allocation and NC conversion failures, occupied leaves, changed
  live TTBR/parents/stream route, bounded TLB synchronization failure.
* Timeouts at every WLFW stage, explicit error 90, no capability request before
  memory readiness, one-shot refusal, success keep/contain and failed-containment
  quarantine/free ordering for target DMA.

The W12 suite is registered beside W11 in `tools/ci/build.sh`; that script and
CI/workflows are not run. Patch 0099 is regenerated from canonical C, and the
existing tests check byte identity and hunk counts for both embedded sources.
All eleven source-only WLAN test entrypoints pass. The separate artifact
verifier requires prebuilt kernel/ADT/loader/boot images absent from this
worktree; no native XNU/image build or artifact validation is claimed.
No phone, adb/fastboot/SSH/serial/USB access, push or CI run is part of W12.
