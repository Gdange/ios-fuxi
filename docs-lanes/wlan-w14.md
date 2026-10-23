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

**W15 has host validation only.** `fuxiwlanqrtr=5` inherits W14, then sends
WLAN_INI, WLAN_CFG and WLAN_MODE normal, observes indications for 2 seconds,
and stops before CE/HTC. Values 0 through 4 retain their previous behavior;
unknown selector values still stop at W10. No CE rings, HTC, WMI, network
interface, retry/recovery or background polling is added. No device, image
build, staging, push or CI execution was used for W15.

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
result or error fail. Only W13's empty-memory negotiation retains its special
result-nonzero/error-zero acceptance; downloads never use that exception.
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
failure) before the existing containment path can change it. Keep retains
DMA/channels/wake only on complete success; failed containment quarantines
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

**Still unverified:** target acceptance of W15 requests, WLAN normal-mode
stability and subsequent indications/ERRDBG state. No W15 device boot or
kernel cross-build has been performed. CE/HTC/WMI and networking remain
future lanes.
