# WLAN W14/W15: firmware readiness and WLAN start

## Evidence and boundary

The supplied try13 excerpt establishes **observed W13 success**: the delayed
RESPOND_MEM was accepted, firmware replaced its large request with 32 segments
no larger than 512 KiB (15 MiB total), real RESPOND_MEM transaction 4 succeeded,
FW_MEM_READY arrived, and CAP transaction 5 succeeded. Chip/family were 2/4,
board ID 0, SoC ID `0x40170200`, firmware `0x1103006c`, build
`WLAN.HMT.1.1.c7-00108-QCAHMTSWPL_V1.0_V2.0_SILICONZ`, two MACs and firmware
caps `0x3`. Step 28 passed with zero faults and keep selected. The excerpt is
read-only evidence; neither it nor firmware contents are committed here.

**Current device evidence (supplied for W15):** `fuxiwlanqrtr=4` on
WCN7850 hw2.0, firmware `WLAN.HMT.1.1.c7-00108`, completed IND_REGISTER,
HOST_CAP, memory negotiation, FW_MEM_READY, CAP (chip 2, family 4, board ID
absent), REGDB, BDF, M3_INFO and **FW_READY_IND `0x38`**. W14 stops before
WLAN config/mode. The legacy ath11k `0x21` indication alone is insufficient.
Earlier missing-M3 log templates below describe the historical host-only
milestone, not the current supplied device result.

**W15 device evidence supplied for this audit:** with upstream firmware and
board-2 data for chip 2 / board `0xff`, INI and CFG replies return result 0.
MODE normal has no reply within 10 seconds. EE remains AMSS, MHISTATUS
`0x201` (M0), SYSERR 0; BHI ERRCODE/ERRDBG1 are `0xcafecace`,
ERRDBG2/3 `0x14feeb3`/`0x496`. The same pair occurred during an earlier
vendor-BDF failure and is absent at clean FW_READY stops. This suggests a
firmware failure but does **not** decode an assert or establish its cause.
The excerpt was read outside the repository; no log or firmware was copied.

`fuxiwlanqrtr=5` inherits W14, then sends
WLAN_INI, WLAN_CFG and WLAN_MODE normal, observes indications for 2 seconds,
and stops before CE/HTC. Values 0 through 4 retain their previous behavior;
unknown selector values still stop at W10. No CE rings, HTC, WMI, network
interface, retry/recovery or background polling is added. No device, image
build, staging, push or CI execution was used for this audit.

## Protocol sources and licence

Original codecs/state machine/tests use field facts from Linux **v6.6**
[ath12k qmi.h](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.h)
and [qmi.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.c),
under **BSD-3-Clause-Clear**. Credit **The Linux Foundation (2018–2021)** and
**Qualcomm Innovation Center, Inc. (2021–2022)**. This is the permissive
licence/provenance used for ath12k protocol work; no Linux helper or EI table
is imported.

Upstream ath12k, including its current schema at inspection, **does not define
the four unknown vendor TLVs in the supplied result**. They cannot correctly
be named rd_card_chain_cap, phy_he_channel_width_cap or dev_mem_info from
ath12k alone. The vendor protocol facts and Android filename behavior were
cross-checked against Xiaomi's
[fuxi CNSS source](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/qmi.c),
[vendor protocol schema](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss_utils/wlan_firmware_service_v01.c),
and the later
[share-memory schema](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss_utils/wlan_firmware_service_v01.c).
These CNSS sources are GPL: **only protocol facts and a description of their
behavior are used**. No CNSS implementation, declarations or EI tables are
copied or translated. The independent decoders use explicit byte offsets and
length checks. Downloaded sources remain outside the repository.

## Named vendor TLVs

All integers below are little endian. Duplicate recognized TLVs, wrong scalar
lengths, excessive counts, truncation and trailing array bytes fail cleanly.
These are reports only: no reported device address is mapped/dereferenced,
no crash data is read and no AOP/voltage setting is applied.

| Message / TLV | Name and wire layout | Meaning of observed length |
| --- | --- | --- |
| CAP `0x1e` | `hang_data_addr_offset`, u32 | Exactly 4 bytes; dump metadata. |
| CAP `0x1f` | `hang_data_length`, u16 | Exactly 2 bytes; dump metadata. |
| CAP `0x22` | `ol_cpr_cfg`: u8 `pmu_param_len` (0–16), then 48 bytes per entry | 433 = 1 + 9 × 48; nine PMU parameters. |
| RESPOND_MEM `0x10` | `share_mem`: u8 count (0–8), then u32 type, u64 start, u64 size per entry | 61 = 1 + 3 × 20; three reported shared-memory regions. |

Each PMU entry has a fixed 32-byte pin name, then four u32 fields:
`wake_volt_valid`, `wake_volt`, `sleep_volt_valid`, `sleep_volt`. These nested
valid fields are actual wire integers, unlike top-level optional TLV presence
flags. Names are bounded, terminated and sanitized for logging. The driver
logs the count and every named field, never a raw payload dump. The excerpt
contains lengths only, so the actual pin/voltage/shared-region values remain
unknown until the next boot.

Existing ath12k CAP fields remain `rd_card_chain_cap` at `0x1b` (four-byte
enum) and `dev_mem_info` at `0x1c` (four static u64 start/size pairs). The later
vendor `he_channel_width_cap` is at `0x26` (four-byte enum); it is also decoded
if present. It is not the observed two-byte `0x1f`. No serial-ID TLV is decoded
or printed. Capability presence bits extend through the new field range.

## Board-data choice: observed facts versus inference

Xiaomi's published fuxi CNSS selection behaves as follows:

* ELF board ID below `0xff` selects `bdwlan.e%02x` (or `bdwlang.e%02x` for
  its GF chip mask). Board ID above `0xff` uses the high byte in the stem and
  low byte in the suffix. BIN selection uses `.bXX` similarly.
* Board ID `0xff` selects the generic ELF name, with Xiaomi overrides for
  hardware projects M2/M3/M11 and CountryGlobal, producing `bd_<hw>[gl].elf`.
  These overrides are inside the `0xff` branch, not the board-ID-0 branch.
* Xiaomi requests `regdb_xiaomi.bin` for the regulatory database.

**Observed:** W13 reports board ID **0**, chip ID **2**. That published rule
would request **`bdwlan.e00`**. The local vendor inventory contains
`bdwlan.e01`–`bdwlan.e36`, `bdwlan.elf` and Xiaomi variants, but **no e00**.
There is no captured Android filename request or successful BDF download in
the supplied evidence. We cannot claim Android loaded a particular BDF on
this firmware simply from the CAP board ID.

The read-only live DT dump identifies the model as fuxi, AP board ID
`<0x10008 0>` (separate from WLFW board ID), and names a **Global M3 display**
selector in chosen boot arguments. It has no explicit WLAN BDF filename.
That supports an **inferred, unverified default `bd_m3gl.elf`**, whose local
ELF32 header has one program header and whose size is 88,728 bytes. This is a
maintainer-overridable bring-up candidate, not an assertion that the vendor
board-ID-0 rule chooses it. The country/project mapping and BDF acceptance
still need runtime evidence. Non-Global candidate `bd_m3.elf` has the same
size; other platform variants should only be selected with suitable evidence.

`fuxiwlanbdf=<basename>` selects one file under
`/usr/local/share/fuxi/wlan/`. Maximum length is 63 bytes, first character
ASCII alphanumeric, remaining characters `[A-Za-z0-9_.-]`; consecutive dots,
empty values, slashes, backslashes, whitespace, punctuation outside that set,
and unterminated/truncated values are refused before firmware bring-up.
The string-only XNU parser prevents numeric coercion; a 65-byte parse buffer
and 63-byte accepted limit detect truncation even when XNU terminates the
truncated string ([XNU parser](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/pexpert/gen/bootargs.c)). There is no path search or alternate-file fallback. BDF wire type comes from
the file's ELF magic (ELF=1, otherwise BIN=0), so eXX and bXX names are usable.

## Maintainer staging list

Stage these ordinary files in **`/usr/local/share/fuxi/wlan/`**, root:wheel,
mode 0644. This task does not edit the iOS root or access any phone.

| Filename | Exact local size, bytes | Use |
| --- | ---: | --- |
| `amss.bin` | 7,458,880 | Existing AMSS loader for SoC major other than 2. |
| `amss20.bin` | 5,890,112 | Existing AMSS loader for SoC major 2; stage both AMSS variants to retain that selection. |
| `regdb_xiaomi.bin` | 24,278 | W14 regdb, BDF type 4, four segments (6144/6144/6144/5846). |
| `bd_m3gl.elf` | 88,728 | Default BDF, type 1, fifteen segments (14 × 6144 + 2712). |
| `m3.bin` | **Absent; size unknown** | Required before M3_INFO/FW_READY can be attempted. Accepted size 1–2,097,152 bytes. |

If overriding to `bd_m3.elf`, stage that file (88,728 bytes) instead of the
default BDF. If selecting another basename, stage exactly that file and record
its actual size; no automatic fallback occurs. `regdb.bin` (24,278 bytes) is
also locally present, but W14 uses Xiaomi's explicit `regdb_xiaomi.bin` name.
Data.msc/Data20.msc, phy_ucode files and the INI are not loaded by W14 and are
not staging requirements for this milestone. No firmware content is committed.

Regdb/BDF are bounded to 1–1,048,576 bytes each; M3 to 1–2,097,152 bytes.
All opens use the existing kernel VFS pattern with FREAD, O_NOFOLLOW and
O_NONBLOCK, require a regular file and supported size attribute, read once
from the opened vnode and reject errors/short reads/close errors. Every
missing/unreadable-file log names the complete device path. Invalid size/type
and read failures also name the file. Absence is fatal even with keep enabled.
These bounds are host policy, not statements of firmware maximum sizes.

## Requests, transport and DMA lifetime

After correlated CAP success:

1. BDF_DOWNLOAD `0x25`, type **4**, regdb; check every segment response.
2. BDF_DOWNLOAD `0x25`, type **1** ELF or **0** BIN, selected BDF; check every
   segment response. Segment IDs restart at zero for each file.
3. Open/read M3, clean its cacheable CPU aliases (also CIVAC in that mode),
   convert every alias to Normal-NC, map its owned IOVA through invalid live
   SMMU leaves, invalidate/synchronize TLB and verify every leaf/ownership.
   Send M3_INFO `0x3c`, require a successful response and TX drain.
4. Wait at most 10 seconds for ath12k FW_READY `0x38`. The legacy ath11k
   ready `0x21` is reported only and cannot satisfy this wait. Drain
   transport and pass W14, or continue into W15 when selected.

BDF request TLVs, from ath12k EI layout (no native struct packing):

| TLV | Value |
| --- | --- |
| `0x01` | u8 valid = 1 |
| `0x10` | four-byte file_id = CAP board ID, including for regdb |
| `0x11` | u32 total_size = **remaining bytes before this segment**, as in ath12k |
| `0x12` | u32 segment ID |
| `0x13` | u16 data count + at most 6144 data bytes; TLV value length is count + 2 |
| `0x14` | u8 end = 1 only on the final segment, including exact multiples of 6144 |
| `0x15` | u8 BDF type |

M3_INFO TLV `0x01` is u64 **IOVA**, `0x02` is u32 exact file size. The
page-rounded allocation and padding remain owned; padding starts zero. Its
IOVA has next-power-of-two alignment, minimum XNU page size. The backing may
be physically noncontiguous. No M3 data is accessed by the CPU after its
publication. Target memory and M3 share the existing bounded IOVA pool; the
separate 64-MiB REQUEST_MEM budget remains unchanged. Staging regdb/BDF data
is copied into the owned IPCR TX slot and is not advertised as target memory.

W14 and W15 use **8192-byte TX slots**; RX remains 4096 bytes. The enlarged
preallocated IPCR allocation has 16 TX and 16 RX slots; all TX slots and RX
buffers are mapped before AMSS. A full BDF QMI request is 6189 bytes, QRTR v1
header/padding makes a 6224-byte MHI transfer. Static bounded packet/encoder
storage avoids large nested kernel-stack frames. Each segment must also
drain its TX completion before sending the next. Other selectors retain
4096-byte slots and their original packet bytes and ordering.

The existing single outstanding txn/node/port/message correlation and result
checks apply: missing/duplicate/malformed common results and either nonzero
result or error fail. W13's empty-memory negotiation retains its special
result-nonzero/error-zero acceptance; downloads never use that exception.
The opt-in optional PHY_CAP has ath12k's result-only check and fallback
semantics described below.
A 512-request guard bounds W14 (two maximum-size files need at most 342
segments, plus setup and M3). WLFW waits are ten seconds with 10-ms polls;
TX drains retain two-second deadlines. No request/segment is retried.

Ready indications before M3 publication are logged and ignored. A ready
indication racing the M3 reply is queued, but cannot bypass a missing or
rejected reply. Duplicate ready indications are harmless. Legacy ath11k `0x21`
is logged if received while polling; no additional wait is imposed for it.

Regdb/BDF/M3 allocations, including partial reads/conversions/maps, remain
owned through the common failure path. Reset/BME-off/PERST/SMMU undo must
prove containment before freeing. Unproven containment quarantines all
allocations, including M3. Successful keep retains DMA, channels and wake;
keep cannot preserve a failed milestone. Success without keep contains as
before. Stage values 8/9/10/11 denote regdb/BDF/M3/firmware-ready respectively.

## Boot arguments and expected logs

Preserve the caller's other boot arguments. WLAN selection:

```text
fuxiwlan=1 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix fuxiwlanqrtr=4 fuxiwlanbdf=bd_m3gl.elf fuxiwlankeep=1
```

Keep is optional; it retains only complete fault-free success. With the
currently available files, missing M3 causes containment regardless.

These are **templates, not W14 device observations**. With observed W13
negotiation CAP remains txn 5; four regdb segments use txns 6–9, fifteen BDF
segments txns 10–24, M3_INFO txn 25. Without negotiation subtract one.

```text
FUXI WLAN: W14 selected BDF bd_m3gl.elf (override fuxiwlanbdf; default unverified)
FUXI WLAN: W14 RESPOND_MEM share_mem_len 3 (reported only)
FUXI WLAN: W14 cap hang_data_addr_offset <value> (reported only)
FUXI WLAN: W14 cap hang_data_length <value> (reported only)
FUXI WLAN: W14 cap ol_cpr_cfg pmu_param_len 9 (reported only; no power writes)
FUXI WLAN: W14 file regdb_xiaomi.bin bytes 24278
FUXI WLAN: W14 regdb ok
FUXI WLAN: W14 file bd_m3gl.elf bytes 88728
FUXI WLAN: W14 bdf bd_m3gl.elf ok
```

Current inventory's expected stop (assuming both downloads are accepted):

```text
FUXI WLAN: W14 missing/unreadable file /usr/local/share/fuxi/wlan/m3.bin errno 2; contain
FUXI WLAN: W12 failure: W14 firmware open; contain, no retry
```

After staging a suitable M3 image, the intended remaining success lines are:

```text
FUXI WLAN: W14 file m3.bin bytes <size>
FUXI WLAN: W14 M3 DMA IOVA <address> bytes <size> Normal-NC mapped
FUXI WLAN: W14 m3 ok
FUXI WLAN: W14 fw ready (ath11k 0x21 also seen <0-or-1>); stop before WLAN config/mode
FUXI WLAN: W14 complete; keep requested, DMA/channels/wake retained (polling ended)
FUXI WLAN: step 28 ok code 0x101c (... ms, faults 0)
```

Rejected segment/M3 replies report transaction/message/result/error and
`WLFW setup rejected`. Missing ready logs `timeout W14 FW_READY indication
(10000 ms); contain, no retry`. A BDF rejection disproves acceptance of that
candidate; there is no silent alternate selection.

## Host validation

`python3 tools/test-fuxi-wlan-w14.py` compiles production C with ASan/UBSan and
extends the independent MHI/QRTR peer. All file data is synthetic. It checks
literal EI-derived golden bytes, a full 6144-byte transfer, exact-multiple
end flags, a seven-byte last segment, remaining-size semantics, board/file
IDs, type and segment restart/order, all responses before progress, stale
replies, duplicate/missing/error-only/rejecting results and response timeouts.
It exercises named 433/61-byte vendor reports, scalar/array bounds and
repetition, absent board ID, safe/unsafe/overlong/truncated boot arguments,
missing each file, zero/oversized files, VFS attribute/read/short-read errors,
M3 allocation/NC/map failures, IOVA versus high noncontiguous backing, every
M3 PTE and zero padding, early/queued/optional ready indications, strict
FW_READY timeout, successful keep/contain and M3 quarantine on failed undo.

Historical W14 validation passed **13 source-only** WLAN entrypoints: M5, M6,
M6i, M6k, M6l, M6m, M6n, M6o/W9, W10, W11, W12, W13 and W14. W11's unsupported-selector fixture
now uses 6, because 4 and 5 are assigned to W14 and W15. The patch's embedded
C matches the canonical source byte-for-byte with checked hunk counts. W14 is registered in
the host-check list; the build script, CI/workflows and image builds are not
run. The separate M6 artifact verifier requires prebuilt kernel/ADT/loader/
boot artifacts and is not part of these source-only tests. No phone tools,
root staging, push, firmware copies or external messages were used.

## W15 firmware start (`fuxiwlanqrtr=5`)

The W15 tables and encodings are adapted from Linux **v6.12 ath12k**, under
**BSD-3-Clause-Clear**, with attribution beside the ported tables. Credit The
Linux Foundation (2018–2021) and Qualcomm Innovation Center, Inc. (2021–2024).
Sources: [qmi.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c),
[qmi.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.h),
[hw.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hw.c),
[ce.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/ce.c),
[hal.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hal.c),
and [hal.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hal.h).
No GPL code or downloaded source/firmware blobs are committed.

After the accepted M3_INFO reply and FW_READY `0x38`, W15 follows
`ath12k_qmi_firmware_start()` in order:

| Request | Message | Exact TLVs (value lengths) |
| --- | --- | --- |
| WLAN_INI | `0x2f` | `0x10` (1): `enable_fwlog=0`. |
| WLAN_CFG | `0x23` | `0x10` (3): raw `WIN`, without NUL or count; `0x11` (181): u8 count 9 + nine 20-byte target CE entries; `0x12` (169): u8 count 14 + fourteen 12-byte service entries; `0x17` (145): u8 count 36 + thirty-six LE u32 shadow addresses. |
| WLAN_MODE | `0x22` | `0x01` (4): normal mode 0; `0x10` (1): hw_debug 0. |

**INI policy difference:** the supplied upstream v6.12 implementation sets
`enable_fwlog=1`. W15 sends **0** to honor this lane's explicit request for
fw log off; it does not claim that value matches upstream. Optional-valid
flags are local EI metadata and are not serialized. The CFG body is 510
bytes (517 including the QMI header); INI/MODE bodies are 4/11 bytes.

`qmi_wlanfw_wlan_{ini,cfg,mode}_req_msg_v01` and their `_ei` tables define
the encodings. Target entries follow
`qmi_wlanfw_ce_tgt_pipe_cfg_s_v01_ei`: pipe number, enum32 direction,
entries, maximum bytes, flags. The native `ce_pipe_config.reserved` field
is omitted. Service entries follow `qmi_wlanfw_ce_svc_pipe_cfg_s_v01_ei`:
service ID, enum32 direction, pipe number. Counts are exactly hw.c's WCN7850
`target_ce_count=9` and `svc_to_ce_map_len=14`, excluding the sentinel. The
RSVD_CTRL downlink maps to CE2. Shadow v1 TLV `0x13` is absent.

The shadow-v3 array is the result of `ath12k_ce_get_shadow_config()` for
WCN7850 hw2.0: `ath12k_hal_srng_create_config_wcn7850()` supplies ring bases,
counts and strides; `ath12k_hal_srng_shadow_config()` traverses HAL enum order,
skipping CE and DMAC/PMAC rings. Each entry is `reg_start[1] + ring *
reg_size[1]`, plus 4 for destination TP. This produces 29 addresses: 8 REO
destination, exception, reinject, command, status; 5 TCL data, command,
status; WBM idle, SW release, 8 WBM completion. Then
`ath12k_ce_shadow_config()` adds 7 addresses in CE order: CE0 source, CE1
destination/status, CE2 destination/status, CE3 source, CE4 source.
`ath12k_host_ce_config_wcn7850` has no host entries for CE5–8, so they add no
shadows even though target CE5 has 32 entries. The total 36 is below both
HAL's 40-register limit and QMI's 60-entry limit. These WCSS-relative values
are sent in `qmi_wlanfw_shadow_reg_v3_cfg_s_v01_ei`; W15 never accesses those
registers or creates rings.

Each request has a unique transaction and one 10-second reply wait. Common
response TLV `0x02` must occur exactly once with a four-byte result/error;
both must be zero. Rejected, missing, duplicate or truncated results contain
without retry or advancing. Indications cannot complete a transaction.
Stale/wrong-message replies are ignored within the same deadline. Existing
MHI TX drain checks apply between requests. After MODE, 200 samples at 10 ms
plus a final event pass report indications without provisioning or sending
additional WLFW requests. IPCR RX reposts and event credits continue. Six
read-only BHI/MHI/ERRDBG reads capture state after observation (also on W15
failure). MODE timeouts additionally take the bounded snapshot below before
the existing containment path can change it. Keep retains DMA/channels/wake only on complete success; failed containment quarantines
all published allocations.

Expected success lines, following the inherited W12–W14 setup logs:

```text
FUXI WLAN: W15 fw ready; starting WLAN INI/CFG/MODE
FUXI WLAN: W15 wlan ini enable_fwlog 0
FUXI WLAN: W15 send txn <n> msg 0x2f stage 12
FUXI WLAN: W15 reply txn <n> msg 0x2f result 0 error 0
FUXI WLAN: W15 wlan ini ok
FUXI WLAN: W15 wlan cfg host WIN target CE 9 service map 14 shadow v3 36
FUXI WLAN: W15 send txn <n+1> msg 0x23 stage 13
FUXI WLAN: W15 reply txn <n+1> msg 0x23 result 0 error 0
FUXI WLAN: W15 wlan cfg ok
FUXI WLAN: W15 wlan mode normal hw_debug 0
FUXI WLAN: W15 send txn <n+2> msg 0x22 stage 14
FUXI WLAN: W15 reply txn <n+2> msg 0x22 result 0 error 0
FUXI WLAN: W15 wlan mode normal ok; stop before CE/HTC
FUXI WLAN: W15 observing indications 2000 ms (reported only)
FUXI WLAN: W15 indication msg <id> stage 15 bytes <n> (reported only)
FUXI WLAN: W15 WLAN start diagnostic BHI_EXECENV <ee> (<name>) MHISTATUS <status> SYSERR(bit2) <bit> MHI_STATE <state> BHI_ERRCODE <code> BHI_ERRDBG1 <d1> BHI_ERRDBG2 <d2> BHI_ERRDBG3 <d3> access-faults <n> valid <bit>
FUXI WLAN: W15 complete; keep requested, DMA/channels/wake retained (polling ended)
```

The indication line is conditional, and indications may also race any W15
reply (stages 12–14). Transactions depend on segment counts; the synthetic
host peer uses 13/14/15. Without keep, completion ends `stop/contain`.
A timeout logs `FUXI WLAN: W15 timeout WLAN_<INI|CFG|MODE> reply (10000 ms);
contain, no retry`, then `FUXI WLAN: W15 failure: <reason>; contain, no retry`.
Rejection includes the decoded result/error; RDDM additionally logs
`FUXI WLAN: W15 RDDM observed; dump not collected; contain`.

`python3 tools/test-fuxi-wlan-w15.py` compiles the production C using the
existing ASan/UBSan host peer. Independently written Python expectations
construct all three complete QMI requests, derive shadow addresses by ring
arithmetic, and compare every byte; the peer separately decodes CFG TLVs,
lengths, counts and strides. Twenty-one failures cover rejection, timeout,
missing result, truncated result, nonzero error with success result,
duplicate result and stale/wrong-message-only replies at each step. Tests
also cover the FW_READY-only and absent-board device shapes, M3/ready
failure gates, indication races, late memory/ready indications at 10 ms/1 s/2 s,
exact 2-second observation, no CE writes or further allocations, precisely
six diagnostic reads without writes, SYSERR/RDDM evidence, keep/non-keep and
failed-containment quarantine. W10–W14 suites retain legacy behavior checks;
W11's unsupported selector moves from 5 to 6. W15 is registered alongside
them in the host check list. Patch 0099's C hunk is regenerated byte-for-byte.
All six `tools/test-fuxi-wlan-w1*.py` entrypoints (W10–W15) pass with
ASan/UBSan, including the source/patch equality and hunk-count checks.

**Still unverified:** successful MODE reply, normal-mode stability, the meaning
of the error tuple, and the effect of the new experiment/diagnostics on hardware.
The supplied device result establishes INI/CFG acceptance only. No device was
accessed and no kernel cross-build was performed for this audit. CE/HTC/WMI
and networking remain future lanes.


### Independent W15 audit (v6.12)

The new `tools/test-fuxi-wlan-w15-audit.py` has its own EI metadata encoder
and target/service/address fixtures. It replaces the earlier peer's golden
requests and compares all three complete requests against production C. It
also accepts `--ath12k-src <reference-directory>` to parse the upstream EI
tables directly, checking **every field**, TLV, type, element size, element
limit, nested struct order and u8 count; it checks all target/service words,
WCN7850 hardware counts and the shadow-address derivation against those
reference files. No upstream source is required by its offline CI run.

| Field | ath12k v6.12 | Default W15 | Verdict |
| --- | --- | --- | --- |
| INI message / enable_fwlog | `0x2f`; optional TLV `0x10`, u8 `1` | Same layout, u8 `0` | Deliberate policy difference; retained, not an encoding bug. |
| CFG message / host_version | `0x23`; optional `0x10`; QMI_STRING max 17 native bytes, raw `57 49 4e` | Same three bytes, no count or NUL | Match. |
| CFG tgt_cfg | Optional `0x11`; **u8** count 9 (limit 12), 20-byte structs | Same, TLV value 181 bytes | Match. |
| Target struct | u32 pipe, enum32 direction, u32 entries, u32 max bytes, u32 flags | Same order/LE widths; native reserved omitted | Match. |
| CFG svc_cfg | Optional `0x12`; **u8** count 14 (limit 24), 12-byte structs | Same, TLV value 169 bytes; sentinel omitted | Match. |
| Service struct | u32 service ID, enum32 direction, u32 pipe | Same order and all 14 values | Match. |
| CFG shadow v3 | Optional `0x17`; **u8** count 36 (limit 60), u32 address structs | Same, TLV value 145 bytes | Match. |
| CFG other optional fields | v1 shadow `0x13` unset; no MSI, v2, memory-address or extra optional TLVs in this EI table | Absent | Match. |
| MODE message / mode | `0x22`; mandatory `0x01`, u32 NORMAL=0 | Same four zero bytes | Match. |
| MODE hw_debug | Optional `0x10`, u8 0 | Same byte | Match. |
| Optional-valid / native length fields | Valid flags omitted; native u32 array lengths serialize as EI **u8** | Same | Match; no u16 counts in W15. |
| Ordering | INI reply → CFG reply → MODE | Same, with IPCR TX drain between requests | Extra host drain only; no extra QMI message. |

Upstream EI evidence: [qmi.c:1624–1999](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L1624),
request population [qmi.c:2843–3059](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L2843),
limits/native definitions [qmi.h:541–575](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.h#L541).
INI body `10 01 00 00`; MODE body `01 04 00 00 00 00 00 10 01 00 00`.
CFG starts `10 03 00 57 49 4e 11 b5 00 09`; service TLV starts
`12 a9 00 0e`; shadow TLV starts `17 91 00 24`. Full bodies are
4/510/11 bytes; complete QMI requests are 11/517/18 bytes.

All nine target rows (`pipe, direction, entries, max bytes, flags`) match
[hw.c:249–337](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hw.c#L249):

```text
0 2  32  2048 0
1 1  32  2048 0
2 1  32  2048 0
3 2  32  2048 0
4 2 256   256 8
5 1  32  2048 0
6 3  32 16384 0
7 4   0     0 8
8 3  32 16384 0
```

The service sequence in [hw.c:457–543](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hw.c#L457)
is VO `0x104`, BK `0x102`, BE `0x101`, VI `0x103`, WMI control `0x100`,
RSVD `1`, HTT `0x300`, each OUT=2 followed by IN=1. The first five use
UL/DL pipes 3/2, RSVD 0/2, HTT 4/1. Flags are CE_ATTR_FLAGS=0,
CE_ATTR_DIS_INTR=8; direction 4 is INOUT_H2H, not a QMI enum truncation.

Shadow derivation was independently rechecked from
[hal.c:1338](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hal.c#L1338),
[hal.c:2428](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hal.c#L2428),
[hal.h:45](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hal.h#L45),
and [ce.c:787](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/ce.c#L787).
These are **target HP/TP addresses**, not `HAL_SHADOW_REG(index)` host aliases.
The REO/TCL/WBM bases are `0xa38000`/`0xa44000`/`0xa34000`; offsets and
strides are the WCN7850 HAL config's `reg_start[1]` and `reg_size[1]`.
Destination rings add `HAL_OFFSET_FROM_HP_TO_TP=4`; CE_DST is a HAL **source**
ring so its HP does not add 4. Non-CE enum order yields:

| Ring | Count | Address range / value (stride 8 where multiple) |
| --- | --- | --- |
| REO_DST | 8 | `0xa3b04c` … `0xa3b084` |
| REO_EXCEPTION / REINJECT / CMD / STATUS | 4 | `0xa3b08c`, `0xa3b028`, `0xa3b020`, `0xa3b0ac` |
| TCL_DATA | 5 | `0xa46000` … `0xa46020` |
| TCL_CMD / STATUS | 2 | `0xa46028`, `0xa4604c` |
| WBM_IDLE / SW2WBM | 2 | `0xa370b8`, `0xa37010` |
| WBM2SW | 8 | `0xa370cc` … `0xa37104` |
| CE0 SRC | 1 | `0x1b80400` |
| CE1 DST / STATUS | 2 | `0x1b83400`, `0x1b8340c` |
| CE2 DST / STATUS | 2 | `0x1b85400`, `0x1b8540c` |
| CE3 / CE4 SRC | 2 | `0x1b86400`, `0x1b88400` |

CE SRC/DST bases are `0x1b80000`/`0x1b81000`, stride `0x2000`, HP offset
`0x400`, status HP offset `0x408` plus TP 4. Host entries select SRC 0/3/4
and DST/status 1/2; CE5–8 have no host rings. DMAC/PMAC and zero-count rings
contribute nothing. WCN7850 overrides REINJECT/SW2WBM to one and TCL_DATA
to five, giving 29+7=36, below HAL's 40 and QMI's 60 limits. **No W15
encoding bug was found; no default request bytes changed.**

### Host state before mode-on

Line numbers below refer to the linked v6.12 files and the audited local
`xnu/fuxi-wlan/fuxi_wlan.c`. Differences are facts; causal relevance remains
unproven. Do not add CE programming before MODE based on this failure alone.

| State | ath12k evidence | Our difference / evidence |
| --- | --- | --- |
| PCI/MSI | [pci.c:55](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c#L55) assigns 16 vectors: MHI 0–2, CE 3–7, DP 8–15; one-vector fallback shares vector 0. `power_up` enables MSI at pci.c:1276. | `fuxi_wlan.c:2549–2555` enables 32 MSI vectors into an owned RAM sink when `seq=msi`; polling, no IRQ handler. Default disables MSI/INTx (`:2142–2154`); no CE/DP vectors serviced. |
| QMI MSI config | qmi.c:2069–2114 leaves `wake_msi_valid` zero; qmi.c:2893–2943 sets host_version, target/service arrays and shadow v3 only. | Same omission. There is no missing MSI TLV in either HOST_CAP or CFG for this path (`fuxi_wlan.c:4117`, `:3571`). CE MSI ring programming occurs after MODE. |
| PHY/MLO capability | [qmi.c:3082](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L3082) sends PHY_CAP before IND_REGISTER/HOST_CAP. qmi.c:2147–2205 uses num_phy and single_chip_mlo_support; fallback def_num_link=2. qmi.c:2019–2066 conditionally populates MLO. | Default W12 setup has no PHY_CAP; HOST_CAP advertises two links, chip/group 0, max peers 32. `fuxiwlanmlo=1` now follows PHY_CAP negotiation; `=2` omits all MLO TLVs. See the detailed comparison below. Default matches fallback, but may differ from this firmware's PHY_CAP. CAP's two MACs are not proof of PHY_CAP response. |
| HOST_CAP other fields | qmi.c:2069–2114: one client, memory mode 0, BDF/M3/cache support 1, cal_done 0 on fresh boot; WCN internal-clock nm_modem bits 1/2/3 and features 3/4 (hw.c:945,989). | Same values in `fuxi_wlan.c:4117–4130`. This audit found no omitted upstream-required wake MSI field. |
| ASPM / L1 | [pci.c:916](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c#L916) clears endpoint L0s/L1 before MHI boot; restore only in HIF start (pci.c:1152), after MODE. | Default clears L0s/L1 at **both** RC/EP; `aspm=1` retains inherited state; `pre=l1ss` also clears four L1SS enable bits (`fuxi_wlan.c:1437–1470`). No normal restore before MODE. |
| Boot register writes | [pci.c:328](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c#L328): LTSSM/hot-reset workaround, interrupt clear, QFPROM VDD4BLOW clear, BHI vector clear, warm entry clear/reset-cause read, SoC global reset, MHI reset. | Comparable sequence is optional `pre=reset`, not default (`fuxi_wlan.c:1230–1288`). `pre=qfprom` uses a different mask (5 versus reset's 4). Existing sequence/POR flags add independent deltas; this audit does not assume the supplied run selected them. |
| QRTR node register / static window | pci.c:1278 writes node ID only with MULTI_QRTR_ID firmware feature; pci.c:1287 selects static window if configured; WCN7850 explicitly sets static_window_map=false at pci.c:1415. | No feature-negotiated node-ID write in W15; default dynamic windows are saved/restored by `wl_soc_read/write` (`fuxi_wlan.c:1195`). WLFW discovery already works. |
| Wake / runtime power | [pci.c:137](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c#L137) uses MHI get_sync/put for high-register access only after INIT_DONE (`pci.c:1178`); INIT_DONE set at HIF start, after MODE. Runtime callbacks in [mhi.c:233](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/mhi.c#L233) are no-ops. | Holds device wake channel-127 doorbell at 1 throughout QMI and keep (`fuxi_wlan.c:3381`); requires live AMSS/M0 in guard (`:2993`). No runtime suspend or wake-release cycle. Existing regulators/ICC remain voted; W15 adds no power vote. |
| MHI/IPCR | [mhi.c:86](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/mhi.c#L86): channels 20/21 AMSS, burst disabled, 64 elements each, RX autoqueue, 8192-byte buffers; event rings 32/256, moderation 0/1 ms. mhi.c:548–566 prepares and synchronously powers up. | Same channels/EE/burst/event sizes; 16-element transfer rings, 15 RX buffers of **4096** bytes, W14+ TX slots 8192, polling (`fuxi_wlan.c:2890–2900`, `:3383–3404`). Smaller RX/ring capacity is another possible transport difference, though no oversized reply was observed. |
| CE/HAL/HTC/DP readiness | [pci.c:1462–1466](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c#L1462) allocates HAL read/write-pointer DMA and CE descriptor rings before power-up (hal.c:2529–2546, ce.c:580–621); addresses are not sent in W15 CFG. core.c:865 derives shadow config before INI. **core.c:880–900 sends MODE before ce_init_pipes and dp_alloc**; core.c:760 invokes HIF start later. [ce.c:825](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/ce.c#L825) initializes hardware rings only there. | Computes equivalent shadow addresses statically; no HAL pointer DMA or CE/DP allocation or registers (`fuxi_wlan.c:3545–3582`, `:4025`). Missing CE register initialization/RX posting is **not** a skipped upstream prerequisite at MODE-on. |
| Time synchronization | No time-sync request or register setup in the audited pci_power_up → mhi_start → core_start_firmware → qmi_firmware_start path. INI/CFG/MODE have no time TLV. | No time sync added. MHI synchronous power-up means waiting for mission mode, not clock synchronization. |

The strongest narrow CFG isolation available without adding host ring writes
is to remove shadow v3. This tests whether this firmware/build/board combination
rejects applying the otherwise matching address set during MODE. It is a
hypothesis, not a known ath12k fix. The PHY_CAP/MLO variants below and RX capacity are separate candidates;
no causal link to the observed MODE failure has been established.

### Opt-in experiment and read-only timeout evidence

`fuxiwlanqrtr=5 fuxiwlanw15=1` omits the **entire** CFG TLV `0x17`;
host_version, target/service arrays, INI and MODE are unchanged. CFG becomes
362 bytes (369 with header). `fuxiwlanw15=0` or absent retains all 36 shadows;
unknown/negative values refuse the run. The selector is ignored outside W15.
No CE writes or log-enable change accompany this experiment.

A timestamp is captured on the **first accepted FW_READY `0x38`**, including
when it races the M3_INFO reply. Elapsed values use mach absolute time
converted to milliseconds, so they include scheduling/printing/TX drain time,
not just the sleep-loop budget. Each send and accepted reply logs elapsed time;
MODE timeout logs its elapsed time too. Exact new line formats are:

```text
FUXI WLAN: W15 variant <0|1> (0 default, 1 omit shadow v3) valid <0|1>
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: INI send
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: INI reply
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: CFG send
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: CFG reply
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: MODE send
FUXI WLAN: W15 wlan cfg host WIN target CE 9 service map 14 shadow v3 0
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: MODE reply
FUXI WLAN: W15 elapsed <ms> ms since FW_READY: MODE timeout
FUXI WLAN: W15 MODE timeout diagnostic BHI_EXECENV <ee> (<name>) MHISTATUS <status> SYSERR(bit2) <bit> MHI_STATE <state> BHI_ERRCODE <code> BHI_ERRDBG1 <d1> BHI_ERRDBG2 <d2> BHI_ERRDBG3 <d3> access-faults <n> valid <bit>
FUXI WLAN: W15 timeout MHICTRL <ctrl> BHIE_TXVECSTATUS <tx> BHIE_RXVECSTATUS <rx> access-faults <n> valid <bit>
FUXI WLAN: W15 pending QMI txn <txn> msg <id> reply <bit> cmd_pending <bit>
FUXI WLAN: W15 IPCR ch <20|21> state <state> host read <i> write <i> RP <rp> WP <wp>
FUXI WLAN: W15 ER <0|1> host read <i> RP <rp> WP <wp> pending <n> valid <bit> truncated <bit>
FUXI WLAN: W15 pending RX QMI type <type> txn <txn> msg <id> bytes <n>
FUXI WLAN: W15 pending RX QMI headers <n> (snapshot only)
FUXI WLAN: W15 timeout ring snapshot unavailable
```

The variant CFG line appears only for `=1`; default still logs shadow v3 36.
MODE reply and timeout are alternatives. Pending RX lines require unconsumed,
validated IPCR RX completion events; no line claims to see messages still inside
firmware. Ring-unavailable is a fallback, not expected in a normal MODE timeout.
Each ring scans at most 16 events, total decoded headers at most 16; a truncated
scan is explicitly marked. Only seven-byte QMI headers and QRTR framing are
inspected, never dump/payload contents. The snapshot takes exactly nine MMIO
reads: EE/status/ERRCODE/ERRDBG1–3, MHICTRL, BHIe TX/RX vector status (relative
BHIe offsets `0x44`/`0x78`). **ERRCODE/ERRDBG belong to BHI; the referenced
BHIe register layout has no separate ERRDBG bank.** No guessed BHIe register
addresses are read. No event is consumed, credited or reposted; no QRTR resume,
QMI request, doorbell, wake vote, dump or device write is issued by the snapshot.
Normal containment still follows the diagnostic; it is not part of the snapshot.
The existing post-run six-read diagnostic is retained.

Host tests cover all previous W15 cases plus exact 10-second elapsed time,
queued reply/indication header inspection, nine-read/no-write bounds and bytewise
preservation of context/IPCR memory. The independent audit test also exercises
the full no-shadow conversation and invalid/non-W15 selector behavior. W10–W15
and the independent audit entrypoint pass with the existing ASan/UBSan harness;
the new test is registered in the host check list (no CI was invoked).


## PHY_CAP / HOST_CAP experiment (`fuxiwlanmlo`)

This is a host-only implementation/validation milestone, with **no new device
observations**. The MODE timeout and ERRDBG pair above remain unexplained.
Append one selector to the existing boot arguments:

| Boot argument | Behavior for `fuxiwlanqrtr=2..5` |
| --- | --- |
| Absent or `fuxiwlanmlo=0` | Exact previous behavior: no PHY_CAP, fixed two-link MLO HOST_CAP, same bytes/order/transaction IDs and logs. |
| `fuxiwlanmlo=1` | PHY_CAP → IND_REGISTER → derived HOST_CAP → existing selected lane. |
| `fuxiwlanmlo=2` | Skip PHY_CAP; omit the entire HOST_CAP TLV set `0x21`–`0x26`. |

For the MODE experiment use `fuxiwlanqrtr=5 fuxiwlanmlo=1` or
`fuxiwlanqrtr=5 fuxiwlanmlo=2`, preserving the caller's other arguments.
`fuxiwlanw15=1` remains an independent shadow-v3 omission selector. Negative
and unknown MLO values refuse W12+ before bring-up; the selector is ignored
outside W12+. No other HOST_CAP opt-in was needed: the audit found no other
field/value/presence mismatch for a WCN7850 fresh boot.

### Exact ath12k behavior and wire layout

Adapted from Linux v6.12 BSD-3-Clause-Clear ath12k, credited in the source:
The Linux Foundation (2018–2021), Qualcomm Innovation Center, Inc.
(2021–2024). Sources/functions:

* [ath12k_qmi_event_server_arrive](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L3082)
  calls PHY_CAP before IND_REGISTER and HOST_CAP, after WLFW discovery.
* [ath12k_qmi_phy_cap_send](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L2146)
  sends message `0x57`; its request EI contains only EOTI, **no TLVs**.
  Full first QMI request: `00 01 00 57 00 00 00`.
* [PHY response EI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L539):
  mandatory result `0x02` = u16 result + u16 error; optional `0x10` = u8
  num_phy; `0x11` = u32 board_id; `0x13` = u8 single_chip_mlo_support.
  There is no `0x12` in this EI. Top-level valid flags are presence only,
  never payload bytes. The complete all-fields body is 22 bytes despite
  qmi.h's stale response MAX_LEN=18; the decoder accepts all four fields.
* [ath12k_core_alloc](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/core.c#L1286)
  starts with intra-device MLO enabled and device_id=0.
  [WCN7850 hw_params](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/hw.c#L933)
  gives def_num_link=2 and max_mlo_peer=32.
* [ath12k_host_cap_parse_mlo](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L2019)
  omits all six MLO TLVs if support is disabled or num_radios is 0/255.
  Otherwise it sets capable=1, chip/group=0, max peers=32, num chips=1.
  `0x26` is a **static** three-chip array of six bytes per chip: u8 chip_id,
  u8 num_local_links, hw_link_id[2], valid_mlo_link_id[2]. Only chip zero
  is populated. For one link it is `00 01 00 00 01 00` plus twelve zeros;
  for two links `00 02 00 01 01 01` plus twelve zeros. No array count.

On successful PHY result, an explicit support field sets/clears MLO; absent
support preserves enabled. This occurs **before** checking num_phy presence.
Absent num_phy selects the fallback two links while retaining any explicit
support-disable. A rejected result ignores all optional fields; unavailable
or undecodable PHY_CAP and a ten-second timeout select fallback two links.
PHY result follows ath12k's result-only success check (the error value is
still decoded/logged); the existing strict result/error checks for all other
requests remain. PHY board_id is logged only: ath12k does not use it here to
select BDF or replace the later CAP board ID.

Port bounds: duplicate recognized fields, wrong scalar lengths, missing common
result, truncated headers/values and trailing partial TLVs invalidate the
whole correlated PHY reply, without applying partial fields. Unknown optional
TLVs are skipped/reported. This port completes malformed optional replies
immediately with fallback, rather than waiting for Linux QMI's decode failure
to time out. Transport/MMIO/SMMU errors still contain; no fault is cleared to
continue. For enabled MLO and num_phy 3–254, the port contains before HOST_CAP:
upstream's unbounded loop would overrun its two-element link arrays. Counts
0/255 follow upstream omission; a disabled-MLO reply with any count also
omits MLO. This is an explicit memory-safety boundary, not a guessed firmware
capability. No request is retried.

With `=1`, PHY_CAP is transaction 1 and every later QMI transaction increases
by one (IND_REGISTER 2, HOST_CAP 3). The existing request guard gains exactly
one slot for this selector; memory/DMA/download/ready sequencing is unchanged.
With `=2`, transaction IDs are unchanged. HOST_CAP body lengths: 85 bytes
with one/two links, 42 with MLO omitted (92/49 including QMI header).

### HOST_CAP comparison (all fields, WCN7850 fresh boot)

[ath12k_qmi_host_cap_send](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L2069)
zero-initializes its request and sets only the fields below. The full
[HOST_CAP EI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/qmi.c#L64)
is checked by the independent audit test. Scalars are little endian; widths
below exclude the three-byte TLV header.

| Field / TLV | ath12k v6.12 WCN7850 | Fuxi / difference |
| --- | --- | --- |
| num_clients `0x10` u32 | 1 | 1; match |
| wake_msi `0x11` u32 | Absent | Absent; match |
| gpios `0x12` u8 count + u32 array | Absent | Absent; match |
| nm_modem `0x13` u8 | `0x0e`: internal sleep clock bit 1, HOST_CSTATE bit 2, PCIe global reset bit 3 | Same, from internal_sleep_clock=true; match. This is upstream's non-Qualcomm-platform notification even on fuxi. |
| bdf_support `0x14` u8 | 1 | 1; match |
| bdf_cache_support `0x15` u8 | Absent | Absent; match |
| m3_support `0x16` u8 | 1 | 1; match |
| m3_cache_support `0x17` u8 | 1 | 1; match |
| cal_filesys_support `0x18`, cal_cache_support `0x19` u8 | Absent | Absent; match |
| cal_done `0x1a` u8 | 0 on fresh boot; may become 1 during upstream calibration lifecycle | 0; fresh-boot match; lane has no calibration lifecycle |
| mem_bucket `0x1b` u32 | Absent | Absent; match |
| mem_cfg_mode `0x1c` u8 | target_mem_mode = default 0, set in ath12k_qmi_init_service | 0; match |
| cal_duration `0x1d` u16 | Absent (native field spelling cal_duraiton) | Absent; match |
| platform_name `0x1e` string | Absent | Absent; match |
| ddr_range `0x1f` static three start/size u64 pairs | Absent | Absent; match |
| host_build_type `0x20` four-byte enum | Absent, not a transmitted zero | Absent; match |
| mlo_capable/chip_id/group_id/max_mlo_peer/num_chips `0x21`–`0x25`, widths 1/2/1/2/1 | Conditional 1/0/0/32/1 | Default fixed; `=1` follows PHY reply; `=2` all absent |
| mlo_chip_info `0x26`, 18 bytes | Conditional, links from PHY or fallback 2 | Default two links; `=1` derived as above; `=2` absent |
| feature_list `0x27` u64 | `0x18`: CNSS_QDSS_CFG_MISS bit 3 + CNSS_PCIE_PERST_NO_PULL bit 4 | Same hw_params bitmap; match |
| fw_mem_mode | **No separate HOST_CAP field/TLV** in ath12k v6.12; host memory mode is mem_cfg_mode `0x1c` | No extra TLV; match |

### Expected log lines (templates, not device observations)

Default/`=0` adds no log line. Nonzero selector configuration:

```text
FUXI WLAN: W12 mlo variant <N> (1 PHY_CAP, 2 omit MLO) valid <0|1>
```

For `=1`, the exact send/decode/selection formats are:

```text
FUXI WLAN: W12 send txn 1 msg 0x57 stage 0
FUXI WLAN: W12 phy cap result <result> error <error> num_phy valid <0|1> value <n> board_id valid <0|1> value <hex> single_chip_mlo_support valid <0|1> value <n>
FUXI WLAN: W12 phy cap unknown TLV <hex> bytes <n>
FUXI WLAN: W12 phy cap fallback num_phy 2 (rejected)
FUXI WLAN: W12 phy cap fallback num_phy 2 (absent num_phy)
FUXI WLAN: W12 phy cap fallback num_phy 2 (malformed reply)
FUXI WLAN: W12 phy cap fallback num_phy 2 (timeout 10000 ms)
FUXI WLAN: W12 host cap MLO TLVs <present|omitted> num_phy <n>
FUXI WLAN: W12 host cap ok
FUXI WLAN: W12 failure: PHY_CAP num_phy exceeds two-link EI capacity; contain, no retry
```

Fallback/unknown/failure lines appear only for their respective condition.
Zero formatted with `%#x` prints `0`; nonzero hex includes `0x`.
For `=2` the selection line is exactly:

```text
FUXI WLAN: W12 host cap MLO TLVs omitted num_phy 2
```

### Host validation and remaining uncertainty

W12 and W15's independent audit run production C with ASan/UBSan against
synthetic peers. A shared independent EI metadata encoder checks every
HOST_CAP field's TLV/type/presence, PHY's empty request, static chip/link
arrays, and complete request bytes before the old peer normalizes transaction
IDs to reuse its existing memory/download/INI/CFG/MODE checks. Cases cover
absent/explicit-zero selectors, omission, one/two links, disabled/absent
support, absent num_phy/board, 0/255 radio counts, nonboolean support, unknown
TLVs, rejected/error responses, exactly ten-second optional timeout, stale
transaction/message, out-of-capacity counts and malformed/duplicate/scalar/
truncated replies with atomic fallback. The optional `--ath12k-src` audit
checks the metadata against the local v6.12 EI declarations and hw_params.
The original literal default fixtures remain unchanged. All seven
`tools/test-fuxi-wlan-w1*.py` entrypoints passed, including the optional
upstream-source audit. The patch C hunk matches the source byte-for-byte,
with its new-file line count checked.

No phone, CI, firmware staging, image build or push was used. Firmware's
actual PHY_CAP reply, acceptance of the variants, MODE success/failure, and
any causal interpretation of BHI ERRDBG2/3 remain unverified.
