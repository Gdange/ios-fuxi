# WLAN M6k: distinguish DMA routing from image visibility

Source-only experiments; no device result is claimed. Keep the existing
`kernel-wlan6g` CI target and artifact names. The kernel contains all modes;
select one with boot arguments when preparing a subsequent boot. No firmware,
device logs or host-specific inputs are part of this change.

Use `fuxiwlan=22 fuxiwlanclk=1` and exactly one `fuxiwlanprobe` value below.
ICC remains enabled by default. Keep the same firmware selection, IOVA pool
and other arguments across comparisons. These probes also stop at step 22
when `fuxiwlan=1` requests the full sequence. They never enter READY/M0 or
ring BHIe, even if BHI unexpectedly succeeds. Omitting the probe argument
(or using `fuxiwlanprobe=off`) retains the ordinary loader path.

| Argument | Controlled change | Expected identifying output | Interpretation |
| --- | --- | --- | --- |
| `fuxiwlanprobe=unmapped` | Omit **all** image leaves before enabling the CB; keep the advertised image IOVA and size. Vector/context mappings remain present. Walk the live CB tables and require all 128 advertised 4-KiB leaves to be zero. | `M6k hole IOVA … invalid leaves 128/128 VERIFIED`; `M6k result probe=unmapped …` | `target-translation-fault` requires FSR.TF, FAR within the advertised 512-KiB interval and CBFRSYNRA.SID = `0x1401`. This proves a WLAN transaction targeting that interval reached the owned context. `no-CB-translation-fault` means no translation fault was captured there during five seconds: investigate wrong SMMU/stream/context, bypass or no request. It does **not** distinguish those possibilities. |
| `fuxiwlanprobe=pattern` | After validating the normal file, replace the entire allocated image with byte `0xa5 XOR (offset AND 255)`. First 64 bytes are zero except ELF magic, invalid class/endian/version `ff/ff/00`, and `FUXI-M6K-PATTERN` at byte 16. Keep IOVA, length, SMMU attributes and normal cache handling. | `M6k pattern first64 ELF class ff endian ff version 00 marker FUXI-M6K-PATTERN FNV1a64 …`; the existing source/walk hash must `MATCH`. | A reproducible change in PBL error/stage/content fields supports content reaching the PBL through some path. Identical errors leave pre-fetch rejection and a shared parser/authentication error unresolved. Changes only in apparent timing/counter words are insufficient evidence. The host-side hash is not endpoint-read proof. |
| `fuxiwlanprobe=civac` | Normal firmware and mappings; explicitly issue `DC CIVAC` for every cache line of image, vector table, context/ring buffer and SMMU tables, each followed by `DSB SY`, immediately before BHI doorbell. Line size comes from CTR_EL0.DminLine. | `M6k DC CIVAC + DSB SY image/vectors/contexts/tables before BHI doorbell` | Success or a reproducible change from the normal firmware result implicates cache visibility/ordering. Unchanged error words weaken that explanation without establishing which DMA path the PBL used. Existing cache maintenance remains in all modes. |
| `fuxiwlanprobe=nc` | Normal firmware; use `pmap_set_cache_attributes` on every wired image page **before VFS fills it**, selecting Normal NC (`VM_WIMG_WCOMB`, MAIR `0x44`) for all CPU aliases, including the physical aperture. CB attributes stay unchanged. | `M6k image CPU mapping Normal-NC WCOMB all aliases bytes …` | A reproducible change implicates CPU mapping/cache visibility. No change does not identify routing. This uses Normal NC so ordinary VFS/memcpy accesses can be unaligned. No additional Device alias is created. |

All diagnostic modes observe for five seconds after the doorbell, including
after an early PBL error. FSR is sticky and never cleared during observation.
They print BHI error/debug registers twice, CB `FSR/FAR/FSYNR0/FSYNR1`,
`CBFRSYNRA` with SID, host transport diagnostics and bounded CNSS boot logs.
FSR `0x400` alone is FORMAT metadata, not FSR.TF. `other-translation-fault`
means TF was captured with a different FAR/SID; `transport-inconclusive`
means inaccessible/changed transport or an MMIO exception prevents drawing
the intended conclusion. Treat any other FSR error bits separately.

The expected stop is `step 22 FAILED code 0x166b`, preceded by
`M6k result probe=… … observation 5000 ms; stop/contain (not firmware success)`.
This is a deliberate experiment-completion code, not a firmware-success code.
Then expect `abort step 22 … bus master off yes, PERST asserted yes` and the
owned SMMU undo/free lines. Failed containment quarantines buffers until reboot;
NC attributes are restored only when freeing after proven containment. Invalid
or combined mode strings stop at step 17. A second step-21 invocation is blocked
after the first doorbell. Selecting a limit below 22 cannot produce the full
experiment observation and should not be used for these comparisons.

Recommended first three boots:

1. `fuxiwlanprobe=unmapped`: establish whether the intended transaction can
   reach this CB at all. A matching translation fault is the strongest result.
2. `fuxiwlanprobe=pattern`: compare error/stage words and the ten bounded PBL
   words against the normal-image evidence. Even without a CB fault, a content
   dependency could indicate a different/bypass DMA path.
3. `fuxiwlanprobe=civac` if either earlier boot indicates a DMA/content path.
   `nc` is the alternative CPU-mapping experiment, or a follow-up if CIVAC
   leaves the same result. If both earlier boots show no discriminant, spend
   the next investigation on routing/request issuance; another cache variant
   cannot by itself prove DMA reaches the expected SMMU.

Keep `fuxiwlanclk=1` for those boots to retain the previously exercised fabric
clock condition. Leave the ASPM safeguard enabled and inspect its before/after
values. If it changes inherited bits, obtain a same-source normal-image control
(`fuxiwlanprobe=off`) before attributing a difference to pattern/cache contents.

## BHI programming comparison

The independent sources inspected were Linux v6.6 MHI/ath12k and the public
SM8550 vendor implementation. These establish host behavior, not a WCN7850 ROM
error-code specification.

| Item | Source behavior and Fuxi finding |
| --- | --- |
| Registers/doorbell | Linux `mhi_fw_load_bhi`: clear STATUS, IMGADDR **high then low**, IMGSIZE, one random nonzero 30-bit IMGTXDB. Fuxi already does this, with readback before enabling endpoint BME. No address-order fix needed. |
| INTVEC/state | Linux power-up writes INTVEC = 0; it resets MHI on SYS_ERR, then dispatches PBL firmware loading. READY/reset-clear polling happens **after** BHI. Fuxi already writes INTVEC = 0 and requires PBL, MHI RESET and no pending reset before BHI. No speculative READY/reset write added. |
| Length/allocation | Linux copies the first `sbl_size` bytes into a separate `dma_alloc_coherent` allocation; ath12k uses 512 KiB. Fuxi already advertises the same first 512 KiB at a page-aligned, contiguous IOVA, with no ELF segment extraction. Allocation of the whole file rather than just the prefix changes host allocation extent, not the advertised transfer. |
| Physical contiguity/alignment | Generic Linux IOMMU DMA allocation can map noncontiguous pages into a contiguous IOVA; MHI passes no forced-contiguous attribute. No special WCN7850 physical-contiguity/alignment requirement was found in these sources. Vendor CMA can produce contiguous storage, but that is not proof the device requires it. Fuxi maps each physical page explicitly. The image is the first IOVA allocation at the selected pool base, so its advertised address is already 512-KiB aligned; CPU pages are 16-KiB aligned. |
| Delay | Fuxi waits 100 ms before PERST deassert, then 1 ms before link training; link/configuration/firmware I/O precede BHI. The inspected MHI load functions add no fixed post-link sleep. New `M6k BHI timing` reports time since PERST-step completion (sampled 1 ms after deassert) and since link-step completion, immediately before the doorbell. No timing experiment is silently combined with the probes. |
| ASPM/L1SS | **Fixed safeguard omission:** ath12k disables ASPM during download due to AMSS-transition problems. Fuxi previously inherited it. Now clear Link Control ASPM bits on EP and RC and require readback; write only the low half so Link Status W1C bits are untouched. Log L1SS_CTL1 without changing it. With ASPM L1 disabled and functions in D0, ASPM L1 substates cannot initiate. This does not claim ASPM caused the observed PBL failure. |

The safeguard is default-on; `fuxiwlanaspm=1` retains inherited ASPM for a
revert/control boot. Expected lines: `M6k EP ASPM LNKCTL … -> … L1SS_CTL1 …
retained 0` and the corresponding `RC` line. Only exactly `1` reverts; malformed
capability chains or failed readback prevent the BHI doorbell. Reset/containment
ends the link; this bring-up loader does not attempt a runtime ASPM restore.

References for this comparison:

- [Linux MHI boot.c](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/boot.c), [power-up/READY in pm.c](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/pm.c).
- [ath12k MHI configuration](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/mhi.c), [ASPM download safeguard](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/pci.c), [generic IOMMU DMA allocator](https://github.com/torvalds/linux/blob/v6.6/drivers/iommu/dma-iommu.c).
- [SM8550 CNSS2 pci.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8550/blob/0241347436d48df011e2a6f6fe2d8740c26a379c/vendor/qcom/opensource/wlan/platform/cnss2/pci.c), [vendor MHI boot.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/boot.c), [vendor MHI pm.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/pm.c).
- [Linux DMA allocation/alignment contract](https://docs.kernel.org/core-api/dma-api-howto.html), [MHI protocol overview](https://docs.kernel.org/mhi/mhi.html).
- [XNU 8796 pmap cache attribute implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/osfmk/arm/pmap/pmap.c), [Normal NC MAIR definition](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/osfmk/arm64/proc_reg.h).

## PBL codes: known versus inferred

Exact-value public searches for `0xef0e0003` and `0xef0e0200`, including
GitHub/CodeAurora/QCA-related searches, did not locate an authoritative decoder.
Linux and the inspected vendor MHI implementation log ERRCODE/ERRDBG1..3 as raw
values without assigning these codes names. Do not label `0003` as a DMA,
authentication or ELF error based on those sources.

CNSS2 names the word at TCSR `0x01b000f8` as the PBL stage/logging value. Its
[KIWI register definitions](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8550/blob/0241347436d48df011e2a6f6fe2d8740c26a379c/vendor/qcom/opensource/wlan/platform/cnss2/reg.h)
locate **40 bytes / ten words** starting at `0x01403d98`. The dump routine prints
them without field definitions. Thus the supplied tuple is the correct bounded
KIWI log region, but the meanings of its individual words remain unknown. A
repeated first word matching TCSR is consistent with a stage/error record;
calling other words counters, addresses, bitfields, source locations or a final
sentinel is a hypothesis, not a decode.

Likewise, runtime ERRDBG resembling `IOVA / 0 / size` is consistent with an
echo of the submitted transfer and is **not proof of a successful read**.
CNSS2 sometimes interprets DBG2/DBG3 as an SBL-log SRAM start/length, with range
checks. That interpretation is invalid for this PBL tuple; the existing Fuxi
SRAM bounds reject it. STATUS all-ones is not a valid success result even when
the remaining BAR/configuration registers stay readable.

## Host verification

Run `python3 tools/test-fuxi-wlan-m6.py`, `python3 tools/test-fuxi-wlan-m6i.py`
and `python3 tools/test-fuxi-wlan-m6k.py`. The existing CI `kernel-wlan6g` target
now runs all three against the current source before its normal kernel build.
The M6/M6k tests independently reconstruct the new-file patch hunk and verify
its line count and byte identity with the standalone C file.

M6k uses the actual loader C with synthetic MMIO/VFS/scattered pages under
ASan/UBSan. It covers both IOVA pools, absent/malformed mode selection, live
hole validation and rejection of an accidentally mapped leaf, TF/FAR/SID and
FORMAT/transport outcomes, one-doorbell enforcement, expected diagnostic stop,
pattern bytes, all-buffer CIVAC line coverage/barriers, NC installation and
restoration/quarantine/failure, and ASPM readback/revert/W1C/capability bounds.
Host mocks cannot establish real DMA routing, cache visibility or ROM semantics.
