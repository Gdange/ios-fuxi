# WLAN W13: RESPOND_MEM audit and small-chunk experiment

## Evidence and conclusion

Observed in the supplied try12 excerpt (read-only; not copied into this tree):
IND_REGISTER transaction 1 and HOST_CAP transaction 2 succeeded. REQUEST_MEM
was queued during HOST_CAP: DDR/type 1 requested 7,274,496 bytes and
CALDB/type 4 requested 8,454,144 bytes, total 15 MiB. Both allocations passed
live SMMU/window readback. RESPOND_MEM transaction 3 received no reply in
10 seconds; the attempted STOP completion also timed out. BHI_ERRCODE was
`0xcafecace`; BHI_ERRDBG1/2/3 were `0xcafecace/0x14feeb3/0x496`. Unlike try11,
the latter two debug words no longer contained the repeated sentinel.

The evidence is consistent with a firmware assert/stall after RESPOND_MEM,
but does **not prove its cause**, decode an assert location, or show RDDM:
BHI_EXECENV remained AMSS (`2`), MHISTATUS was `0x201` (M0, SYSERR bit clear).
CB6 FSR `0x400` is unchanged and contains no bits in the driver's fault mask;
it is not evidence of a new translation/permission fault. The stale FAR and
syndrome words cannot establish a fault without valid FSR fault bits.

The audit found **no RESPOND_MEM wire-layout or HOST_CAP memory-mode error**.
A concrete difference is that W12 supplied only 16-KiB-aligned IOVA bases;
Linux's documented coherent DMA alignment is stronger. The observed bases
are not aligned to the next powers of two for their requested sizes (8 MiB
and 16 MiB). Whether this firmware relies on that alignment is **unknown**.
Large chunks, a firmware-specific host profile, and other firmware assumptions
remain hypotheses. W13 provides a selectable smaller-chunk/alignment
experiment; **only host tests have run, not a hardware fix validation**.

## Sources, licence and audit

Protocol/allocation facts were checked against pinned Linux **v6.6**
[ath12k qmi.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.c),
[qmi.h](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/qmi.h),
and the WCN7850 profile in
[hw.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/hw.c).
The qmi.c EI definitions and allocation policy are **BSD-3-Clause-Clear**:
credit **The Linux Foundation (2018–2021)** and **Qualcomm Innovation Center,
Inc. (2021–2022)**. W13 uses that permissive provenance and an original
encoder/state machine/tests; no EI table or Linux implementation is copied.
No GPL code, downloaded source, firmware or device log is added to the repo.

### RESPOND_MEM (`0x36`)

`qmi_wlanfw_respond_mem_req_msg_v01_ei` defines:

| Item | Wire layout | W12 / W13 |
| --- | --- | --- |
| Mandatory array TLV | type `0x01`, LE u16 value length | Correct, unchanged. |
| `mem_seg_len` | `QMI_DATA_LEN`, `elem_size=sizeof(u8)` | One byte, **not u16**. The host C struct's u32 length is not transmitted. |
| Array | variable length, maximum 52 nested structs | No per-entry TLV or length prefix. |
| `addr` | u64, entry offset 0 | Explicit LE IOVA. |
| `size` | u32, offset 8 | Explicit LE requested size, not page-rounded allocation size. |
| `type` | signed four-byte enum, offset 12 | Echoed 32-bit representation, explicit LE. |
| `restore` | u8, offset 16 | Zero, matching ath12k's zero-initialized request. |

The wire stride is **17 bytes**, regardless of native C struct padding
(typically 24 bytes). Neither ath12k struct is declared packed; the EI
encoder visits fields individually. The XNU implementation writes bytes
explicitly, independent of alignment and host representation. A two-entry
value is 35 bytes; TLV plus QMI header is 45 bytes. Fifty-two entries use
895 QMI bytes. Zero segments still carry TLV bytes `01 01 00 00`, for an
11-byte QMI request; they do not produce an absent TLV. W13 factors this into
`wl12_encode_respond` without changing `=2` wire bytes.

### HOST_CAP (`0x34`)

The audit rechecked every W12 field against the EI definitions and
`ath12k_qmi_host_cap_send` / `ath12k_host_cap_parse_mlo`. Optional `_valid`
flags become TLV presence, never payload bytes. All integers are LE.

| TLV | Field (wire width) | Sent value / audit |
| --- | --- | --- |
| `0x10` | num_clients (u32) | 1. |
| `0x13` | nm_modem (u8) | `0x0e`: internal sleep clock, host C-state, PCIe reset profile. |
| `0x14` | bdf_support (u8) | 1. |
| `0x16`, `0x17` | m3_support, m3_cache_support (u8 each) | 1, 1. |
| `0x1a` | cal_done (u8) | 0 for a fresh boot. |
| `0x1c` | mem_cfg_mode (u8) | **0**, `ATH12K_QMI_TARGET_MEM_MODE_DEFAULT`, initialized by ath12k service setup. |
| `0x21` | mlo_capable (u8) | 1. |
| `0x22` | mlo_chip_id (u16) | 0. |
| `0x23` | mlo_group_id (u8) | 0. |
| `0x24` | max_mlo_peer (u16) | 32. |
| `0x25` | mlo_num_chips (u8) | 1. |
| `0x26` | mlo_chip_info (static 3 × 6 bytes) | First `00 02 00 01 01 01`, remaining entries zero; no count. |
| `0x27` | feature_list (u64) | `0x18`, WCN7850 hw2.0 CNSS feature bits 3 and 4. |

Wake MSI/GPIO/BDF-cache/cal-filesystem/cal-cache/mem-bucket/cal-duration/
platform/DDR-range/build-type fields remain absent. Payload stays 85 TLV
bytes. There is no evidence to justify changing memory mode or MLO here.
This upstream compatibility profile is not proof of the integrated firmware's
expectations; BDF/M3 support continues to advertise later host operations,
which this milestone intentionally does not execute.

## ath12k allocation behavior and the selectable experiment

In v6.6, `ath12k_qmi_alloc_target_mem_chunk` calls `dma_alloc_coherent` once
per supported requested segment, with its exact requested size and
`GFP_KERNEL | __GFP_NOWARN`. Supported types are host DDR **1**, M3 dump **3**,
CALDB **4**, and pageable **9**. Type 4 gets ordinary coherent RAM: no CALDB
fixed address, contents, size override, or calibration download in this path.
The CALDB address/size constants in qmi.h are not used here. Type 3 is a dump
region, separate from the later host-loaded M3 image / `M3_INFO` (`0x3c`).
Unsupported types, including BDF/type 2, retain their size/type and get address
zero; ath12k does not allocate arbitrary RAM for every enum. W12's earlier
claim of a fully type-agnostic ath12k allocation path was incorrect.

**2 MiB is a fallback threshold, not a maximum legal allocation.** ath12k
first attempts even larger allocations. Only if one fails and its size is
strictly greater than 2,097,152 does it free earlier unadvertised allocations,
set `target_mem_delayed`, and send zero segments. Failure at/below 2 MiB is
fatal. A delayed response with non-success result and **error 0** is accepted;
nonzero error is rejected. Firmware can then send another REQUEST_MEM.
Upstream does not impose W13's one-round bound.

The [Linux DMA guide](https://docs.kernel.org/core-api/dma-api-howto.html#using-coherent-dma-mappings)
describes one contiguous DMA address range and alignment to the smallest
page order covering the allocation. An IOMMU can provide that range over
noncontiguous physical pages. ath12k adds no special alignment argument;
its DMA handle, not the first backing PA, is sent to firmware. Firmware
requires each advertised segment to be contiguous in **device address space**;
physical contiguity is not necessary with our verified IOVA translation.
XNU CPU aliases remain Normal-NC; their virtual alignment is not visible to
firmware, so W13 strengthens the **IOVA** alignment only.

| Selector | Behavior |
| --- | --- |
| `fuxiwlanqrtr=2` | Retains direct W12 allocation, type handling and 16-KiB IOVA alignment; receives the shared encoder audit logs and crash snapshots. |
| `fuxiwlanqrtr=3` | W13 policy deliberately declines supported chunks >2 MiB before allocating/mapping any target memory, exercising ath12k's zero-segment fallback. Allows one smaller re-request, then provisions types 1/3/4/9 with next-power-of-two IOVA alignment (minimum XNU page size); unsupported types return address 0. |
| `=1` | W11 CAP_REQ-only diagnostic. |
| absent/0/other values | W10 boundary. |

The `=3` threshold is a **host policy to force the experiment**, not a claim
that XNU allocation failed or that upstream always splits large requests.
After the empty transaction-3 response, the replacement indication has a
10-second deadline. It may race the empty reply and is queued. A duplicate,
a second oversized supported request, malformed/secure config, premature
FW_MEM_READY during negotiation, allocation failure, or exhausted IOVA space
contains the device. No second negotiation/retry/recovery is attempted.
All requests retain the 52-segment and 64-MiB page-rounded budget; alignment
gaps also enter the IOVA-space preflight. Unsupported sizes still count
conservatively against those budgets.

With negotiation, the real response uses transaction **4**, CAP_REQ uses **5**.
Without negotiation they remain 3/4. Both the real response and FW_MEM_READY
are required before CAP_REQ; rejected/missing real replies cannot advance.
Once real memory is advertised, no accepted re-request can overwrite its
ownership. Partial allocations remain in the existing containment/quarantine
path. The `=3` implementation never needs to free/reuse live target mappings
to negotiate, because the decline happens before the first allocation.

## DMA-write audit

`wl12_map_target` checks the live TTBR/ASID, SCTLR, TCR, MAIR, RC route and
SMR/S2CR on each of its three passes. The route is RID `0x100` -> SID `0x1401`
-> owned CB (CB6 in try12, dynamically selected). Parent descriptors are
exact owned table addresses with `|3`, without inherited write prohibition.
Leaf `WL_DMA_PTE=0x60000000000e63` has AP[7:6]=**01 (RW)**, AF=1, NS=1,
AttrIdx=0, outer-shareable and XN. MAIR slot 0 is `0xff` (Normal WB); this is
the existing coherent-device SMMU profile. The allocation's CPU aliases are
Normal-NC with readback before publication. No CPU accesses to contents occur
after advertisement in this milestone. WB device mapping plus NC CPU access
is an intentional inherited profile, not a new cacheability experiment.

The extension writes only previously invalid leaves, cleans the owned tables,
invalidates the ASID, waits boundedly for TLB synchronization, and walks every
new 4-KiB leaf against its backing PA and full attributes. Every segment fits
the existing 32-bit MHI CTRL/DATA apertures. Alignment gaps remain unmapped.
The audit finds no missing device-write permission; host tests verify the
permission bits independently and use noncontiguous physical backing.
This is configuration/readback evidence, **not a device-write success test**.

## Crash snapshot and expected logs

On a RESPOND_MEM deadline **or earlier transport failure during that response**,
`wl13_respond_diagnostics` reads only these six registers before STOP/reset:

| Name | Offset |
| --- | --- |
| BHI_EXECENV | discovered BHIOFF + `0x28` |
| MHISTATUS | BAR0 + `0x48`: SYSERR bit 2 and MHI state bits 15:8 (`0xff` = SYS_ERR) |
| BHI_ERRCODE | BHIOFF + `0x30` |
| BHI_ERRDBG1/2/3 | BHIOFF + `0x34/0x38/0x3c` |

Access faults invalidate the snapshot visibly. RDDM is EE **3**; observing it
adds a `dump not collected` line. There is no dump allocation, RX arming,
dump collection, firmware address dereference, or new large transfer. The
existing guard can detect SYSERR/RDDM before ten seconds, so diagnostics must
also cover early failure. Raw debug values are not assigned an undocumented
assert meaning. STOP/reset/BME-off/PERST containment still follows.

Next boot WLAN arguments (preserve the caller's other arguments):

```text
fuxiwlan=1 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix fuxiwlanqrtr=3
```

Leave keep absent for the first experiment; `fuxiwlankeep=1` retains resources
only after complete fault-free success, as before. `=2` remains available for
the original allocation behavior with W13 diagnostics. No BDF/regdb/calibration/
M3 download, network interface, background service or second boot attempt is
performed by this code.

Expected templates, **not W13 hardware observations**:

```text
FUXI WLAN: W13 small-chunk policy limit 2097152 bytes; one zero-segment negotiation, aligned IOVAs
FUXI WLAN: W12 host cap ok
FUXI WLAN: W13 decline large chunks n=2 limit=2097152; RESPOND_MEM zero segments before any target allocation
FUXI WLAN: W13 RESPOND_MEM TLV 0x01 count-u8 0 entry-bytes 17 restore 0 LE delayed
FUXI WLAN: W12 send txn 3 msg 0x36 stage 4
FUXI WLAN: W12 reply txn 3 msg 0x36 result <0-or-1> error 0
FUXI WLAN: W13 delayed response complete; waiting smaller REQUEST_MEM (10000 ms)
FUXI WLAN: W12 request mem n=<count> total=<bytes> rounded=<bytes> cap=67108864
FUXI WLAN: W13 target DMA SID 0x1401 CB <cb> AP[7:6]=1 RW AttrIdx=0 MAIR0=0xff OSH; CPU Normal-NC
FUXI WLAN: W13 RESPOND_MEM TLV 0x01 count-u8 <count> entry-bytes 17 restore 0 LE
FUXI WLAN: W12 send txn 4 msg 0x36 stage 4
FUXI WLAN: W12 reply txn 4 msg 0x36 result 0 error 0
FUXI WLAN: W12 fw mem ready
FUXI WLAN: W12 send txn 5 msg 0x24 stage 6
FUXI WLAN: W12 handshake complete; stop/contain
```

The request may precede the empty reply, logging `W13 smaller REQUEST_MEM
queued before delayed RESPOND_MEM reply`. Firmware chooses the count/sizes;
the nine-segment 15-MiB split in the host peer is only a synthetic fixture.
The special delayed result/error acceptance line appears only for nonzero
result with error zero. Success still stops at capability discovery.

If the observed stall recurs after either empty or real RESPOND_MEM:

```text
FUXI WLAN: W12 timeout RESPOND_MEM reply (10000 ms); contain, no retry
FUXI WLAN: W13 RESPOND_MEM diagnostic BHI_EXECENV 0x2 (AMSS) MHISTATUS 0x201 SYSERR(bit2) 0 MHI_STATE 0x2 BHI_ERRCODE 0xcafecace BHI_ERRDBG1 0xcafecace BHI_ERRDBG2 0x14feeb3 BHI_ERRDBG3 0x496 access-faults 0 valid 1
```

For EE 3, the separate line is `FUXI WLAN: W13 RDDM observed; dump not
collected; contain`. Missing replacement requests log `timeout smaller
REQUEST_MEM`; a still-oversized replacement logs `negotiation exhausted`.

## Host validation

`python3 tools/test-fuxi-wlan-w13.py` compiles production C with ASan/UBSan and
the existing independent MHI/QRTR peer. Coverage: EI-derived literal golden
bytes with high address words/signed enum bits, zero and maximum count,
complete unchanged HOST_CAP bytes, the synthetic observed-size request,
zero allocations before decline, result=1/error=0 acceptance and nonzero-error
rejection, response/indication races, missing/oversized/duplicate re-requests,
partial allocation/quarantine, supported CALDB/M3/pageable/DDR versus BDF
address zero, alignment including IOVA exhaustion, zeroed backing and explicit
RW PTE decoding, real/delayed timeouts, AMSS stall versus SYSERR/RDDM, exactly
six read-only diagnostics without serial/dump access, and snapshot read faults.

All twelve source-only `tools/test-fuxi-wlan-*.py` suites pass (M5, M6, M6i,
M6k, M6l, M6m, M6n, M6o/W9, W10, W11, W12, W13). The separate M6 artifact
verifier requires prebuilt images and is not a source-only suite; no image
build or artifact validation is claimed. W13 is registered in the host-check
list, but the build script, workflows and CI are not run. Patch 0099's C hunk
is regenerated and host tests check exact source identity and line counts.
No phone/device tools, push or external messages are used.
