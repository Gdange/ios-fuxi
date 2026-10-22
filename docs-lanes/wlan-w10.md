# WLAN W10: SBL READY to full AMSS over BHIe

The supplied try9 result establishes that the first BHI image was accepted:

```text
FUXI WLAN: M6 step 22 EE 0x1 (SBL) MHISTATUS 0x101 (READY ready 1) MHICTRL 0 BHI 0x8c170601 ERRCODE 0 ERRDBG 0x42c97/0x17af58c/0x2c BHIE 0 EPcmd 0x100406
FUXI WLAN: M6o result seq mask 0xff EE 0x1 STATUS 0x8c170601 ERRCODE 0; stop/contain at 22
```

Status bits [31:30] are 2; bits [29:0] match the host's `0x0c170601`.
Together with exact SBL EE and READY, this is positive BHI completion evidence.
The old `0x166c` result was the intentional diagnostic stop, not a rejection.
This observation does not establish full AMSS execution. The complete log is
not included here; W10 has had no device test.

## Selection

Keep the successful W9 hardware selectors, including `prefix` and `civac`,
and use `fuxiwlan=1` (all 28 steps). Example WLAN arguments:

```text
fuxiwlan=1 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix
```

Other inherited boot arguments remain the caller's responsibility. W10 does
not enable `cyclerails`; its historical freeze remains documented in M6o.

| Argument | Behavior |
| --- | --- |
| `fuxiwlan` absent/0 | Driver remains off. |
| `fuxiwlan=1` or `28` | Run through BHIe, AMSS and M0; contain even on success by default. |
| `fuxiwlan=N`, 2–27 | Inclusive step limit; contain at that limit. In particular, **22 still ends the run before BHIe**. |
| `fuxiwlanstop22=1` | Preserve five-second BHI diagnostics and the deliberate step-22 failure. Prefix boots retain the old prefix-only allocation and zero vectors. |
| `fuxiwlanstop22` absent/0 | Default W10 continuation, including valid sequence/firmware/precondition selectors and CIVAC/NC modes. |
| `fuxiwlankeep=1` | Retain the device, MSI sink, SMMU ownership and all DMA allocations only after a fault-free step 28, exact AMSS EE and M0. |
| `fuxiwlankeep` absent/0 | Default containment at every completed run. |

Only exactly 1 enables either integer option. `unmapped` and `pattern` probes
always stop at 22, including an unexpected BHI success; their deliberately
unusable image mappings/data cannot continue. Existing invalid selector
combinations still fail before power writes. Keep cannot override a partial
limit, diagnostic stop, failed step, MMIO fault or containment failure.

## Implementation and register provenance

W10 is original C polling/validation glue. No Linux MHI/QRTR implementation
or other third-party driver code is copied. The factual source for the MHI
layout is the BSD/ISC-licensed ath12k-derived OpenBSD
[if_qwz_pci.c, revision 1.13, MHI definition block](https://cvsweb.openbsd.org/src/sys/dev/pci/if_qwz_pci.c?rev=1.13&content-type=text/plain)
([public mirror](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_qwz_pci.c)).
This uses its register definitions, EE/state numbers, vector and context
formats; it does not import its loader routines. The earlier lane's
[READY/context/M0/FBC plan](wlan-bt.md#steps-and-bounds) supplies the retained
step ordering. This is an implementation based on register-layout facts,
not a claim to have validated a new ordering on KIWI or obtained a separate
MHI specification document.

| Layout fact used by W10 | Source / meaning |
| --- | --- |
| BAR0 `+0x28/+0x2c` | `MHI_BHI_OFFSET` / `MHI_BHIE_OFFSET`; dynamically discovered, not hard-coded bases. M4/W9 observed `0x100/0x224`. |
| BHI `+0x28/+0x2c` | `MHI_BHI_EXECENV` / `MHI_BHI_STATUS`. The status field is [31:30], sequence [29:0]; success 2, error 3. |
| BHIe `+0x2c/+0x30` | `MHI_BHIE_TXVECADDR_LOW/HIGH_OFFS`: address of the vector table, **IOVA**, not image PA/CPU pointer. |
| BHIe `+0x34` | `MHI_BHIE_TXVECSIZE_OFFS`: table length in bytes, not number of vectors or AMSS length. |
| BHIe `+0x3c` | `MHI_BHIE_TXVECDB_OFFS`: sequence [29:0], upper bits preserved. |
| BHIe `+0x44` | `MHI_BHIE_TXVECSTATUS_OFFS`: sequence [29:0], status [31:30], completion 2, error 3. Pre-doorbell status is not an owned result. |
| BAR0 `+0x38/+0x48` | `MHI_CTRL` / `MHI_STATUS`: state [15:8], CTRL RESET bit 1, STATUS READY bit 0 and SYSERR bit 2. READY state 1, M0 state 2, SYS_ERR state 255. |
| EE values | PBL 0, SBL 1, AMSS 2, RDDM 3, WFW 4, EDL 6, FP 7. W10 requires exact AMSS 2; WFW/FP are reported as unsupported, never accepted through an ordinal comparison. |
| BAR0 `+0x10/+0x18/+0x20` | `MHI_CFG`, `MHI_CHDBOFF`, `MHI_ERDBOFF`. NCH [7:0], NER [23:16]; event doorbells have eight-byte stride. M4/W9 observed channel/event bases `0x300/0x700`. |
| BAR0 `+0x58..0x6c` | `MHI_CCABAP`, `MHI_ECABAP`, `MHI_CRCBAP` low/high context-base pairs. |
| BAR0 `+0x80/+0x88/+0x98/+0xa0` (each with high word +4) | `MHI_CTRLBASE`, `MHI_CTRLLIMIT`, `MHI_DATABASE`, `MHI_DATALIMIT`; existing sub-4-GiB aperture setup retained. |
| Vector/context memory | Two little-endian u64 fields, address/size (16 bytes); three u32 plus four u64 context fields (44 packed bytes). Public BSD definitions and existing compile-time stride checks. |

At the observed BHIEOFF, TX low/high/size/DB/status are BAR0
`+0x250/+0x254/+0x258/+0x260/+0x268`. These are derived addresses for the
observed discovery, not universal WCN7850 constants. The older ath12k fixed
`PCIE_TXVECDB=0x360` alias is deliberately not used on this KIWI path.
No new host-controller/SMMU register offsets are introduced. Existing SMMU
fault masks, own-bank fault checks and the SID-qualified global fault check
retain their M6/W9 provenance and policy.

## Steps and containment

Step 19 in prefix mode now allocates both the separate 512-KiB BHI buffer and
a full AMSS buffer. One read from the already-open vnode fills the full image;
the BHI prefix is copied from those same bytes. There is no file reopen after
SBL, no extraction of ELF segments, no payload download or alternate image
retry. With the reported 6,090,816-byte file, step 20 prepares 12 vectors,
192 table bytes. Each vector spans a 512-KiB slice except the final remainder.
Non-prefix mode uses the existing full `wl_image` allocation directly.
All buffers are allocated and added to the owned SMMU tables before BHI.

| Step | W10 behavior |
| --- | --- |
| 22 | Require BHI success with the owned nonzero sequence, SBL EE, READY state and READY bit. Retain the two transport snapshots and reject a late loss/error/sequence change. Mark success and continue. |
| 23 | Reconfirm reset-clear and SBL READY with a bounded poll. |
| 24 | Publish existing disabled channel contexts, two event contexts/rings and an empty command ring. Verify context bases, aperture and event count before event WP doorbells. Reject event/channel doorbell aperture overlap. |
| 25 | Request M0 once from confirmed READY; wait for exact SBL/M0. This retains the existing pre-FBC ordering. |
| 26 | Validate full-image vector coverage, SMMU ownership, bus master and discovery; clean AMSS/vectors/tables to PoC, with CIVAC when selected. Clear TXSTATUS; write high then low table IOVA and byte size; verify the tuple and cleared status. Choose a separate nonzero sequence, recheck SBL/M0 and faults, then ring exactly one TX doorbell. Latch the attempt before writing, including on write fault. |
| 27 | Require TXSTATUS completion and matching sequence. A stale/mismatched completion waits to the deadline; error aborts. EE=AMSS alone cannot substitute for TX completion. |
| 28 | Require the same owned TX completion, exact AMSS EE and M0; report success and end W10. |

Every poll has a five-second deadline, sleeping 25 ms between samples. SBL
requesting RDDM, EDL, SYS_ERR/reset, access loss and owned SMMU faults produce
named reasons and containment. Existing RDDM buffers, if selected, remain a
sink only; W10 neither dumps them nor attempts recovery. BHIe error status is
ignored before the TX doorbell, then treated conservatively as a failure even
if its sequence is unexpected. An AMSS transition that stays READY is logged
as AMSS reached but fails the bounded M0 wait; W10 does not reinitialize
contexts or send another M0 request after that transition. That result needs
fresh evidence for the next state-machine change.

There are **no channel doorbells, wake doorbells, START_CHANNEL commands,
transfer rings or QMI operations**. Only the existing event-ring WP doorbells
are used to make context/event DMA valid before M0. W10 does not consume or
recycle events during this bounded boot experiment.

On success without keep, or on any failure/partial stop, use the existing
MHI reset, BME-off readback, MSI disable/saved-address-data restore, PERST
assert/drain, reverse host journal, SMMU/RC undo and buffer release. The new
full-image allocation participates in both mapping and cleanup. Failed
containment/undo quarantines **all** buffers. Keep-success retains everything
needed for firmware paging; it does not start the next milestone.

## Expected next-boot evidence

Templates below are expectations, not captured W10 device logs. Sequences,
IOVAs, file length and times vary. With the same firmware length and prefix:

```text
FUXI WLAN: W10 continue=1 keep=0 (stop22=1 retains BHI diagnostics; no data channels)
FUXI WLAN: W10 prefix 0x80000 bytes; full AMSS 6090816 bytes retained from same file read
FUXI WLAN: prepared 12 vectors (192 bytes), 44-byte contexts, channels disabled; BME follows selected sequence
FUXI WLAN: W10 SBL READY success BHI STATUS <success-with-BHI-seq> seq <BHI-seq>; continue to AMSS
FUXI WLAN: step 22 ok code 0x1016 (... ms, faults 0)
FUXI WLAN: W10 contexts published; event rings only, all 128 channels disabled
FUXI WLAN: W10 poll mode 2 success EE 0x1 MHISTATUS <M0> ...
FUXI WLAN: W10 BHIe TX preflight table <IOVA> bytes 192 vectors 12 full AMSS 6090816; cache-clean + dsb sy
FUXI WLAN: W10 BHIe TX doorbell seq <TX-seq> (one shot)
FUXI WLAN: W10 poll mode 3 success EE <SBL-or-AMSS> MHISTATUS <raw> TXSTATUS <success-with-TX-seq> seq <TX-seq> after ... ms
FUXI WLAN: W10 AMSS EE reached; M0 confirmed; no channels/QMI; stop/contain
FUXI WLAN: step 28 ok code 0x101c (... ms, faults 0)
FUXI WLAN: 28 steps done
FUXI WLAN: M6o MSI disabled, saved address/data restored before PERST
FUXI WLAN: abort step 28 MHI reset done, bus master off yes, PERST asserted yes
FUXI WLAN: M6o undo host entries pending 0 WLAN power contained result done
FUXI WLAN: abort buffers freed
```

| Outcome | Interpretation |
| --- | --- |
| `BHI STATUS sequence mismatch`, mode 0 timeout | No correlated BHI/SBL READY completion; no contexts or TX doorbell. |
| `BHIe TX refused: ...` | Guard, vector bounds, discovery, ownership or tuple/readback failed before TX, or a second attempt was refused. |
| `BHIe TXSTATUS sequence mismatch`, mode 3 timeout | A completion for another sequence is not success; no retry. |
| `BHIe TXSTATUS error` | Explicit TX status error; snapshot and contain. |
| `RDDM requested ...` / `EDL error EE` / `SYS_ERR/reset` | Boot/crash-state failure; no recovery transfer or channels. |
| Mode 3 success, mode 4 timeout with EE SBL | Image transfer completed; AMSS execution was not observed. |
| `AMSS EE reached state 0x1; waiting M0`, then timeout | AMSS was observed in READY; no final M0 confirmation and no keep. |
| `AMSS EE reached; M0 confirmed ... keep-running requested` | Explicit keep success only; DMA resources retained. |
| `abort buffers quarantined ...` | Containment or undo could not be confirmed; allocations deliberately retained. |

## Host validation

`python3 tools/test-fuxi-wlan-w10.py` compiles the production C under ASan/UBSan
using the M6o synthetic PCI/VFS/DMA peer. It covers the supplied BHI sequence,
32-bit MSI, full prefix allocation/vector mapping with noncontiguous backing,
one file read, SBL READY classification, separate BHIe sequence, matching/
mismatched/delayed/error/pending TXSTATUS, AMSS arrival/timeouts, AMSS READY
without M0, RDDM/EDL/WFW/FP/SYS_ERR, failed allocations/reads/tuple readbacks,
changed discovery, faults, one-shot attempts, disabled channels, partial stops,
keep success, normal reverse undo and failed-containment quarantine.

All existing source-only entrypoints pass: M5, M6, M6i, M6k, M6l, M6m, M6n,
and M6o/W9. Legacy diagnostic fixtures now explicitly select the retained
stop behavior; the shared peer reports a correlated BHI sequence and SBL EE.
Patch 0099's driver new-file hunk is regenerated from the canonical C, and
existing byte-for-byte and hunk-count checks cover both embedded C sources.
The W10 suite is registered alongside the existing host tests in the build
script; no CI was run. `git diff --check` passes.

The separate artifact verifier requires prebuilt lane kernels, ADT, loader
and boot images, which are absent here; it cannot validate packing in this
worktree. No native XNU build, phone access, push, workflow/CI run, firmware
access or downloaded binary is part of W10. Hardware SBL/M0 and BHIe/AMSS
progress remain for the caller's next boot to establish.
