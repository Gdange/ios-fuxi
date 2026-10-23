# W16: WLAN_MODE silence, Linux ordering and opt-in IPCR experiments

The observed stop is before WLAN_MODE's QMI reply, not at HTC readiness.
The experiments preserve the existing default and all qrtr selector values.
They do not claim a device fix until a new run supplies the MODE reply.

## OBSERVED: device runs try15c and try15d

Both runs receive ath12k FW_READY (0x38), then successful WLAN_INI (0x2f)
and WLAN_CFG (0x23) replies at 12 and 36 ms since FW_READY. MODE normal
(0x22, hw_debug=0) is sent at 37 ms. The 10,000 ms polling budget expires
without its reply; logged elapsed time since FW_READY is about 10.9 seconds.

At timeout both show AMSS (BHI_EXECENV=2), MHI M0 (state=2), SYSERR=0,
ERRCODE/ERRDBG1=0xcafecace, ERRDBG2=0x14feeb3 and ERRDBG3=0x496, with no
access faults. These raw debug words have no established firmware-specific
decode here. AMSS/M0 does not prove the WLAN application made progress.
There is no pending command, either event ring reports zero pending events,
and the bounded snapshot finds no pending RX QMI header.

The host TX read and write indices match (6/6 in try15c, 5/5 in try15d).
Thus the transport saw completion of the MODE transfer; this does not prove
WLFW processed the request. Host RX read/write are 3/2 and 2/1 respectively:
with the spare-slot convention, 15 receive TREs remain posted in the host's
accounting. There is no demonstrated steady-state RX exhaustion.

Both channel contexts report state 2 (RUNNING). However ch20 RP is
0x100000001 and ch21 RP is 0x100000002. These cannot be aligned pointers
inside their IPCR rings. Numerically they resemble type=1/2 followed by
ER index=1. WP remains inside the expected ring. Existing logs do not show
base, length, full configuration words or when RP first took these values.
They therefore do not establish a stride error or a safe RP repair.
The driver retires transfers using completion events, not channel-context RP.

try15c's PHY_CAP reports num_phy=2 and single_chip_mlo_support=1, matching
HOST_CAP. try15d omits shadow-v3 from CFG. Both reach the same MODE timeout;
neither MLO negotiation nor shadow omission resolved it.

## OBSERVED: Linux v6.12 ordering

The relevant upstream files are [core.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/core.c),
[qmi.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c),
[pci.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c),
[ce.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/ce.c),
[hal.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hal.c),
[hw.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hw.c)
and [mhi.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/mhi.c).

1. PCI probe calls `ath12k_hal_srng_init()` and `ath12k_ce_alloc_pipes()`.
   These allocate host ring bookkeeping, coherent pointer storage and CE
   descriptors; allocation is distinct from programming live CE rings.
2. On FW_READY, `ath12k_core_qmi_firmware_ready()` first calls
   `ath12k_core_start_firmware()`. It computes the shadow address list in
   host memory, then calls `ath12k_qmi_firmware_start()`.
3. That function synchronously sends INI, waits for its reply, sends CFG,
   waits for its reply, sends MODE, and waits for its reply. There is no
   intervening CE initialization, CE buffer posting or HTC handshake.
4. Only after successful MODE does the caller run `ath12k_ce_init_pipes()`
   (which calls HAL SRNG setup to program the CE rings), then DP allocation,
   then `ath12k_core_start()`.
5. `core_start()` attaches WMI, initializes HTC and calls `ath12k_hif_start()`.
   PCI start enables CE interrupts and calls `ath12k_ce_rx_post_buf()`.
   HTC target-ready waiting and HTT/WMI service connections follow.

Consequently Linux does **not** require live host CE rings or posted CE RX
before replying to MODE. The earlier allocated CE descriptors are not
passed as DMA addresses in CFG. Adding live CE initialization before MODE
would change the reference ordering and is not justified by these sources.
The firmware-facing work at this point is QMI over functioning IPCR,
previously supplied target memory/downloads, and the target configuration.

## OBSERVED: transport and provisioning comparison

| Item | Linux WCN7850 / existing driver comparison |
| --- | --- |
| IPCR 20/21 | Linux: 64 TREs per channel, event ring 1, directions outbound/inbound, AMSS EE mask, burst disabled. Driver: 16 TREs, matching directions/ER/burst mode. W14/W15 already use 8192-byte **TX** slots, but RX slots are 4096 bytes. Linux controller buffer length is 8192 and inbound auto-queue is enabled. |
| Event rings | Both use 32 control and 256 data events. Linux config assigns control/data MSI vectors (with shared-vector fallback) and 1 ms data interrupt moderation; driver polls with RAM-sink MSI. W16 retains polling and existing event geometry. |
| Publication/doorbells | Driver writes TRE, barrier, shared WP, barrier, doorbell high then low; it posts N-1 RX slots and replenishes each completion. Wake remains asserted. There is no special CE doorbell between Linux CFG and MODE. |
| OOB / DB_MODE | Linux MHI host processing re-rings queued transfers on these notifications. Existing driver returns event credit but ignores notification recovery, even though queued TREs can remain. W16 redrive uses the committed WP and private indices. The supplied logs have no notification counters, so they cannot prove this happened. |
| Context geometry | The public MHI layout is three u32 fields followed by four packed u64 fields: 44 bytes, base/len/RP/WP at 12/20/28/36. The driver matches it. W16 adds compile-time offset checks and raw-word snapshots; it does not change the stride or overwrite device RP. |
| CFG | The independent W15 audit matches v6.12 EI encoding: WIN, nine target CE rows of five u32 fields, fourteen service-map rows of three u32 fields, and 36 computed shadow-v3 addresses. Array counts are u8; target `reserved` and service sentinel are not sent. The shadow list is an address mapping, not live CE ring allocation. |
| INI / MODE | MODE matches Linux: normal=0, hw_debug valid and zero. INI explicitly disables firmware logging; Linux enables it. This known policy difference is retained. |
| regdb / BDF | Linux loads regdb then board data, choosing ELF versus BIN from the magic. Driver does this too, with 6144-byte segments and an acknowledged final segment. Both runs acknowledge 24,278 regdb bytes and 88,760 ELF BDF bytes. A successful download does not verify board calibration suitability. OTP board ID is absent; named BDF selection remains unverified. |
| Calibration | WCN7850 `hw_params.download_calib=false`; Linux does not perform an additional calibration-file download on this path. |
| M3 | Linux loads an M3 firmware image into coherent host memory and sends its DMA address and size with M3_INFO. Driver keeps a mapped Normal-NC image (299,660 bytes in both runs), gets M3_INFO success and FW_READY. This is an input firmware image, distinct from crash dump collection. |
| Crash dump | BHIe RX vectors/RDDM are a separate facility. No RDDM transition or SYSERR appears in these two runs; no M3 dump collection occurs between Linux CFG and MODE. W16 does not invent another download or request a dump. |

MHI cross-checks use the public [context definitions](https://android.googlesource.com/kernel/msm/+/03b459497177169664e5c12717ea09d3899dba6c/drivers/bus/mhi/core/mhi_internal.h)
and [host event processing](https://cos.googlesource.com/third_party/kernel/+/3f6732357e508c888d8aa50a28eb76253a57546b/drivers/bus/mhi/host/main.c).
Only protocol facts were used from these MHI sources; no GPL implementation
was copied. Capacity and ordering facts above come from the supplied v6.12
ath12k reference (BSD-3-Clause-Clear; The Linux Foundation and Qualcomm
Innovation Center). The added transport logic is an original bounded implementation.

## GUESS: what could explain the silence

A burst exceeding 15 posted receive buffers, or a packet exceeding 4096
bytes, could expose the transport capacity difference when WLAN starts.
Queued work may also stall if the endpoint asks for another doorbell and
the host ignores it. These are testable transport hypotheses, not evidence
that either happened: RX is still posted at timeout and MODE TX completed.

The invalid-looking RP values warrant comparison immediately after START,
after CFG and after MODE submission. Their resemblance to metadata could
indicate a firmware context-cache writeback peculiarity, memory corruption,
or an ABI disagreement. Successful earlier QMI traffic and the matching
44-byte layout argue against blindly changing stride. An RP anomaly alone
must not trigger reclamation or an in-place rewrite of a running channel.

A mismatched board/regdb/M3/AMSS set remains plausible: QMI success verifies
protocol acceptance, not the board identity or application readiness. There
is not enough evidence here to select different artifacts. If all transport
variants still stall with identical counters and contexts, compare a
known-compatible firmware/calibration set and investigate firmware-side
progress before attempting CE startup reordering.

## Variants and test order

Retain the previously working bring-up arguments and BDF selection. All
entries below require `fuxiwlanqrtr=5`; `fuxiwlanw16` defaults to zero and
is ignored for other qrtr levels. Negative values or values above four are
rejected for qrtr=5 before allocations. Existing `fuxiwlanmlo` and
`fuxiwlanw15` selectors remain independent; keep them constant when comparing.

| Priority | Additional boot arguments | Experiment |
| --- | --- | --- |
| 1 | `fuxiwlanqrtr=5 fuxiwlanw16=3` | 64 IPCR entries, 8192-byte RX buffers, notification redrive and one posted-RX re-ring after CFG before MODE. |
| 2 | `fuxiwlanqrtr=5 fuxiwlanw16=1` | Capacity change alone, to isolate the Linux WCN7850 geometry. |
| 3 | `fuxiwlanqrtr=5 fuxiwlanw16=2` | Original capacity; notification redrive and pre-MODE RX re-ring alone. |
| 4 | `fuxiwlanqrtr=5 fuxiwlanw16=4` | Original transport with added diagnostics only, for a baseline comparison. |

Each active variant logs its selector, capacities, full channel fields,
eleven raw context words, RP range validity, host indices, transfer counts,
OOB/DB_MODE counts and last transfer-event words. Snapshots occur after
channel START/RX posting, after CFG, after MODE queueing, after 1000 ms of
MODE waiting, and on MODE reply or timeout. They read shared RAM only and
do not credit, consume, acknowledge or repair anything. Existing timeout
register/event/QMI diagnostics remain. Redrive republishes the existing WP
only when private indices show pending TREs; it never uses channel RP.

The success discriminator is a matching successful MODE reply, followed by
the existing two-second indication observation. All variants stop before
CE/HTC. They retain the single transaction attempt, timeout, guard,
keep/containment and failed-containment allocation quarantine policies.

## Host validation

`tools/test-fuxi-wlan-w16.py` compiles the production source with ASan/UBSan
and an independent synthetic peer. It checks all selectors and legacy
geometry, literal context offsets and exact W15 requests, 64-entry ring
wraps, an 8192-byte packet, a 40-packet receive burst, empty versus queued
TX notification handling, lost-doorbell recovery, no-write snapshots with
the observed RP shapes, redrive access failure, MODE rejection/silence,
provisioning failure and allocation quarantine. It checks that no CE work
is introduced. The test is included beside W15 in `tools/ci/build.sh`.
The existing patch reconstruction checks verify the mirrored source hunk.

Validation completed: all 15 WLAN host suites invoked by that CI entry
point passed, including W14, W15, the independent W15 audit and W16. The
independent audit also passed with EI/table derivations checked against
the supplied ath12k source tree. Both 0099 source mirrors match and the
CI kernel-wlan6g dry run passed. A full kernel build requires macOS/Xcode
and was not run; these host tests do not validate real endpoint timing
or prove a hardware fix.
