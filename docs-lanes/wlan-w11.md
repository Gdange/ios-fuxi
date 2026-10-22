# WLAN W11: first IPCR / QRTR / WLFW conversation

The supplied try10 excerpt establishes the W10 hardware boundary: steps 17–28
passed with zero access faults, SBL READY, two event rings and all 128 channels
disabled, a matching BHIe TXSTATUS after 325 ms (12 vectors, 6,090,816 bytes),
exact AMSS EE 2, and MHISTATUS 0x201 (M0). That run contained the device with
keep disabled. These are observations from the supplied excerpt; the excerpt
and firmware are not stored here. **W11 has only host validation so far.**

## Selection and scope

Keep the successful W9/W10 hardware selectors and add `fuxiwlanqrtr=1`.
For a full run that retains successful W11 state:

```text
fuxiwlan=1 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix fuxiwlanqrtr=1 fuxiwlankeep=1
```

This is the WLAN argument subset; preserve the caller's other boot arguments.

| Argument | Behavior |
| --- | --- |
| `fuxiwlanqrtr` absent, 0, or any integer except 1 | Existing W10 behavior; no IPCR allocation or channel commands. |
| `fuxiwlanqrtr=1` | Allocate W11 DMA before BHI; run one conversation inside step 28 after verified BHIe completion and AMSS/M0. |
| `fuxiwlankeep=1` | Retain only after the entire selected milestone succeeds with zero access faults. With W11 selected, receiving a successful capability response and draining TX completions are also required. |
| `fuxiwlankeep` absent/0 | W11 still runs when selected, then sends BYE, attempts STOP_CHAN, and contains the device. |
| `fuxiwlan=N`, 2–27 | Inclusive partial stop; no W11 conversation, always contain. |
| `fuxiwlanstop22=1` | Existing diagnostic stop, never overridden by QRTR or keep. |

No retry, alternate firmware, registration of memory-request indications,
firmware memory provisioning, calibration, board data, networking interface,
or data traffic is implemented. The first QMI request is **CAP_REQ only**;
IND_REGISTER is deliberately deferred. A firmware that requires more setup
before answering CAP_REQ will fail or time out and be contained. Success proves
only the first request/reply, not WLAN readiness.

Keep retains the channels, outstanding posted RX buffers, owned SMMU mapping,
firmware image and MSI sink. Polling ends after the conversation. Device wake
remains asserted to inhibit idle transitions; there is no continuing service
thread or claim that retained firmware will remain healthy indefinitely.

## Sources and independent implementation

The transport, bounds checks, encoders, decoders and simulated peer are original
code written for this lane. No Linux GPL MHI core, QRTR core or qmi_helpers
implementation is imported or used. The sources of protocol/layout facts are:

* [Public MHI protocol documentation](https://docs.kernel.org/mhi/mhi.html),
  especially Transfer rings, Event rings, Ring Element and M0: host-owned
  transfer WP; device-owned event RP; circular queue spare slot; advance and
  notify using doorbells; start channels in M0. This is a public protocol
  description, not a claim to have obtained a separate Qualcomm MHI
  specification document.
* [OpenBSD if_qwz_pci.c](https://cvsweb.openbsd.org/src/sys/dev/pci/if_qwz_pci.c?rev=1.21&content-type=text/plain),
  revision 1.21: MHI register/context/TRE definitions, command and event codes,
  and WCN7850 IPCR channel assignments 20 outbound, 21 inbound, event ring 1.
  These are register and wire-layout facts; its MHI routines are not copied.
* [OpenBSD qwzreg.h](https://cvsweb.openbsd.org/src/sys/dev/ic/qwzreg.h?rev=1.19&content-type=text/plain),
  revision 1.19: QRTR v1 and compact v3 headers, control-packet layout and
  command numbers. The public OpenBSD definitions carry permissive notices.
* [ath12k qmi.h](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.h)
  and [qmi.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.c),
  v6.6, **BSD-3-Clause-Clear**: credit The Linux Foundation (2018–2021) and
  Qualcomm Innovation Center, Inc. (2021–2022). Only WLFW service/version,
  message IDs, field types and TLV layouts are used. No table or routine is
  copied. A short source credit is also present in the C implementation.

W9/W10 continue to supply the validated BAR discovery, SMMU register layout,
MSI-to-RAM sink and containment design. W11 introduces no ITS/GIC routing.

| Register or wire fact | W11 use |
| --- | --- |
| Discovered CHDBOFF / ERDBOFF, eight-byte stride | Channel 20/21 transfer WP, channel 127 device wake, event 0/1 credits. Recheck discovery during every polling pass. |
| BAR0 +0x70/+0x74 | Command ring doorbell; high word first, low word commits. |
| 44-byte channel context | `cfg`: ENABLED 1, burst mode DISABLE 2 in bits 9:8; `type`: 1 outbound / 2 inbound; `index`: event 1; base/length/RP/WP. Other 126 channel contexts stay zero. |
| 16-byte command TRE | Pointer/data zero; dword 1 contains channel in bits 31:24 and START 0x12 or STOP 0x11 in bits 23:16. |
| Command event 0x21 | Pointer must match the single outstanding command TRE; completion code 1 in dword 0 bits 31:24. Channel/opcode are checked against the submitted TRE, not an assumed event channel field. |
| Transfer TRE type 2 | Owned buffer IOVA; byte length in dword 0 bits 15:0; IEOT bit 9, no chaining or burst suppression. |
| Transfer event 0x22 | Exact next submitted TRE pointer and channel 20/21; EOT completion 2; bounded returned byte length. Overflow/EOB/error fail. OOB 5 / DB_MODE 6 return event credit without retiring a buffer. |
| State event 0x20 / EE event 0x40 | Require M0 state events; permit historical boot EEs 0/1/2 while checking live EE=2. Crash/unsupported EEs fail. Unknown events fail visibly. |

## DMA and ring ownership

All allocations exist and enter the owned stage-1 tables before the BHI image
doorbell. The existing 256-MiB selectable IOVA pool has sufficient room;
its bounds are unchanged. Allocation checks, live CB register readbacks and
SMMU TLB synchronization remain in place. W11 additionally walks the live
TTBR through owned page-table aliases, checking every 4-KiB context/IPCR page
against the expected backing PA and PTE attributes before any channel starts.
No PA, CPU pointer or unowned IOVA is published as a DMA buffer address.

The existing context page retains its 128 channel contexts, two event
contexts, command context and event/command rings. A separate IPCR allocation
uses the following offsets (relative to that allocation's IOVA):

| Offset | Contents |
| --- | --- |
| 0x0000 | TX ring: 16 elements, 256 bytes. |
| 0x0100 | RX ring: 16 elements, 256 bytes. |
| 0x1000 | Sixteen 4096-byte TX slots. |
| 0x11000 | Sixteen 4096-byte RX slots. |

The requested size is 0x21000 bytes, rounded to 0x24000 on XNU's 16-KiB
pages. Transfer ring bases are aligned to their lengths. TX leaves one slot
empty and never reuses a slot until its correlated completion. RX posts 15
buffers after both START completions. Each completed RX buffer is parsed
within its owned slot, then moved to the current free producer slot; private
consumer/producer indices wrap independently.

The context and IPCR allocations are converted to **Normal-NC on every CPU
alias before publication**, using the existing pmap WCOMB mechanism and
attribute readback. Conversion failures contain the device. The packed
contexts place device RP and host WP in shared cache lines; whole-page cache
writeback while they are active could overwrite a newer device RP. W11 uses
NC accesses and `dsb sy` publication/consumption barriers, with no cache
maintenance on these active allocations. Table/image cache maintenance
continues as before. After proven containment, cleanup restores default CPU
attributes before freeing; quarantined allocations retain their attributes.

For each event ring, the private consumer starts at its base, including any
queued W10 boot events. The device context RP points to its next producer
slot; the host context WP/doorbell identifies the last free slot. Each event
consumed returns exactly one credit. RP alignment/range is checked before
indexing. A pass processes at most 32 control and 256 data events, even if
the producer keeps moving. Shared command/event layout and empty-command
preconditions are checked before START; W11 does not reset/reinitialize live
event rings after AMSS.

Doorbells are notifications, not echo registers. Publication ordering and
context/base readbacks are verified; there is no equality-to-write test on a
command or transfer doorbell. One outstanding command is allowed. START 20,
START 21 and their pointer-correlated completions precede any QRTR packet.

## QRTR and QMI

TX uses QRTR v1: eight little-endian u32 fields (version, type, source node,
source port, confirm, unpadded payload size, destination node, destination
port). RX also supports the compact header whose on-wire version is **3**,
with 16-bit node/port fields and control port 0xffff normalized to 0xfffffffe.
Compact optional headers are explicitly unsupported and cause containment.
All packets require exact four-byte payload padding and fit one 4096-byte
MHI buffer; fragmented/chained MHI packets are not supported.

The host uses node 1, client port 0x4000, control port 0xfffffffe. It sends
HELLO and NEW_LOOKUP (service 0x45, version 1, wildcard instance), answers the
first peer HELLO once, and accepts a NEW_SERVER announcement of WLFW v1.
The announced node/port are used, not assumed. Other services/destinations
are ignored. Discovery requires HELLO and a matching announcement, in either
order. BYE/EXIT from the selected peer or server, and DEL_SERVER for the
selected endpoint, fail immediately. Confirm-RX requests receive RESUME_TX
with the local receiving node/port. On healthy success without keep, BYE is
sent before STOP; failures proceed directly to containment.

CAP_REQ is message **0x0024**, transaction **1**, request flag 0, a seven-byte
QMI header and an empty TLV list. It is encoded by hand without QMUX. Only
response flag 2 with matching transaction/message and the discovered source
node/port can complete the request. Unsolicited indications/unrelated
transactions cannot satisfy it, nor can a response arriving before CAP_REQ.

TLVs use one-byte type plus little-endian u16 length:

| TLV | Decoded fields |
| --- | --- |
| 0x02, required, 4 bytes | u16 result and u16 error; both must be zero. |
| 0x10, optional, 8 bytes | u32 chip id and u32 chip family. |
| 0x11 / 0x12, optional, 4 bytes each | u32 board id / u32 SoC id. |
| 0x13, optional | u32 firmware version, nested u8 timestamp length, up to 32 timestamp bytes. |
| 0x14, optional | Up to 128 build-id bytes; top-level string length is the TLV length. |

Lengths, truncation and duplicate known TLVs are rejected. Unknown optional
TLVs are bounds-checked and skipped. Strings are bounded, terminated and
sanitized for logging. The capability log's validity bitmap has bit 0 for
result and bits 1–5 for TLVs 0x10–0x14. An absent numeric field prints zero
with its validity bit clear; absent strings print `absent`.

## Bounds and containment

The W11 latch is set before its first transport attempt and rejects a second
conversation. Every command, discovery, capability and TX-drain wait has at
most 200 ten-millisecond sleeps, plus bounded event-processing work. No
wait, event producer, TLV parser or ring traversal can loop indefinitely.

Access faults, lost AMSS/M0, changed discovery, lost bus master/ownership,
owned SMMU faults, invalid completions and malformed selected packets return
failure from step 28. Keep never overrides failure. Cleanup attempts STOP for
each channel whose START was attempted. A still-pending command cannot be
overwritten with STOP; the log says `unconfirmed` and cleanup continues.
STOP itself uses the same bounded completion wait. QRTR handling/RX reposting
is disabled while stopping so shutdown cannot create new protocol work.

Regardless of STOP completion, containment proceeds through MHI reset,
BME-off readback, MSI disable/saved-register restoration, PERST assertion and
drain, reverse host undo, SMMU/RC undo and allocation release. Failure to prove
BME/PERST/undo quarantines **all** DMA allocations, including IPCR. No recovery
START or second firmware transfer is attempted.

## Expected next-boot evidence

These are templates, not W11 hardware observations. Node/port and capability
values depend on the firmware:

```text
FUXI WLAN: W11 qrtr=1 keep=1 (one conversation after AMSS/M0)
FUXI WLAN: W10 contexts published; event rings only, all 128 channels disabled
FUXI WLAN: W10 poll mode 4 success EE 0x2 MHISTATUS 0x201 ...
FUXI WLAN: W11 DMA live walk verified, contexts/IPCR Normal-NC, all buffers in owned IOVA window
FUXI WLAN: W11 command 0x12 chan 20 completed
FUXI WLAN: W11 command 0x12 chan 21 completed
FUXI WLAN: W11 chan 20/21 started
FUXI WLAN: W11 RX posted 15 buffers of 4096 bytes; polling RAM-sink MSI
FUXI WLAN: W11 QRTR hello sent; lookup service 0x45 version 1
FUXI WLAN: W11 QRTR hello peer node <node>
FUXI WLAN: W11 WLFW server node <node> port <port> instance <instance>
FUXI WLAN: W11 WLFW capability request txn 1 msg 0x24
FUXI WLAN: W11 WLFW reply txn 1 msg 0x24 result 0 error 0
FUXI WLAN: W11 WLFW cap: valid <mask> chip id <id> chip family <family> board id <board> soc id <soc> firmware <version> timestamp <text> build <text>
FUXI WLAN: W11 conversation complete; keep requested, channels/DMA/wake retained (polling ended)
FUXI WLAN: step 28 ok code 0x101c (... ms, faults 0)
FUXI WLAN: 28 steps done
```

HELLO and server lines can arrive in either order. With keep=0 the final W11
line says `stop/contain`, followed by BYE, STOP completion/unconfirmed lines,
MHI reset/BME/PERST and `abort buffers freed`. Failure is a named
`W11 failure: ...; contain, no retry` or
`W11 timeout <stage> (2000 ms); contain, no retry`, followed by step-28 failure
and the same containment path. A quarantine line means resources remain
allocated because containment/undo could not be proven.

## Host validation

`python3 tools/test-fuxi-wlan-w11.py` compiles the production C using the
existing M6o/W10 PCI/VFS/DMA harness with ASan/UBSan. Its independent device
peer has private command/transfer/event cursors and event credits, checks
context publication before low-word doorbells, and supplies literal wire
fixtures. Coverage includes:

* START/STOP command correlation, bad pointers/codes, partial START, missing
  completions, bounded STOP failure and reset fallback.
* Owned noncontiguous DMA backing, live table walking, NC conversion/failure,
  allocation failure, ring-full refusal, transfer/event wrap, RX reposting,
  invalid pointers, oversized payloads and unsupported event types.
* QRTR v1/compact framing, padding/length/version/flags, HELLO/lookup and
  NEW_SERVER, BYE/DEL_SERVER, confirm-RX acknowledgments, wrong destinations,
  pre-request replies and one-shot refusal.
* Exact CAP_REQ bytes with empty TLVs, literal CAP_RESP decoding, optional and
  unknown TLVs, stale transactions, missing/duplicate/malformed result,
  string bounds and successful capability logging.
* Discovery/capability/TX-drain timeouts; command/TX/RX/event/wake doorbell
  access faults; live EE/SMMU faults; keep success versus failed/partial runs;
  containment ordering and failed-containment quarantine.

The W11 suite is registered with the existing host checks in the build
script; no CI/workflow is run for this milestone. Patch 0099's new-file hunk
is regenerated from canonical `fuxi_wlan.c`; existing tests check reconstructed
bytes and hunk counts for both embedded C files.

All ten source-only WLAN entrypoints (M5, M6, M6i, M6k, M6l, M6m, M6n,
M6o/W9, W10, W11) pass. The separate M6 artifact verifier needs prebuilt
kernel/ADT/loader/images; those inputs are absent here. No phone access,
firmware download, native kernel image build, push, workflow or CI run is
part of W11. Hardware validation of the new transport remains for the caller's
next boot.
