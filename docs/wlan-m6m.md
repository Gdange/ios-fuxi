# WLAN M6m: PBL rejection, image provenance and L1SS controls

Source and host tests only. `kernel-wlan6g`, its artifacts, and its packaged
command lines retain their names and defaults. No device access, workflow
dispatch, push, firmware staging or firmware extraction was performed.

## What the error establishes

The fitted device is **WCN7850/KIWI, PCI `17cb:1107`, hardware major 2**.
QCA6490 (`17cb:1103`) is an alternate, disabled DT configuration, not the
observed endpoint. See the hardware evidence in [the lane document](../docs-lanes/wlan-bt.md).
CNSS's KIWI major-2 rule selects `kiwi/amss20.bin`; upstream's WCN7850/hw2.0
image happens to be named `amss.bin`. Those basenames do not establish image
equivalence. [CNSS selector, pinned revision](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/pci.c)

**There is still no defensible symbolic decode of `0xef0e0003`.** Mainline
MHI and the inspected Qualcomm-derived 5.15 MHI driver print ERRCODE and
ERRDBG1–3 as raw values. CNSS's KIWI bootloader diagnostic code reads stage,
bootstrap and bounded log registers, but contains no corresponding error
enumeration. Searches for the exact code, `ef0e`, and PBL error symbols found
no applicable public definition. Calling `0x0e` an authentication module or
`3` a certificate/hash failure would invent a ROM ABI. The low `3` in ERRCODE
is also not BHI_STATUS's error field, which is separately in bits [31:30].
[Linux error dump and BHI loader](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/boot.c),
[5.15 vendor loader](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/boot.c)

The supplied M6k/M6l results support a **content-dependent rejection while
still in PBL**, after the initial doorbell, with other transport registers
readable. Zero ERRCODE immediately after submission means no error reported
yet; it does not by itself prove header acceptance. The unmapped control
faults at the image IOVA; mapped pattern and CIVAC controls do not. This makes
a basic SID/translation mistake much less likely, without proving every byte
arrived or that authentication passed. All-ones BHI_STATUS alone is ambiguous.
FSR `0x400` is the format bit, outside the existing translation-fault mask;
unrelated residual FAR/SID values are not a new matching DMA fault.

M6l's qfprom/warm/wake controls retain PBL EE 0 and `0xef0e0003` after five
seconds. Its reset control instead fails at step 18: SYS_ERR, reset unacknowledged
after about 1.1 seconds, before firmware DMA. That is evidence against using
that reset as the next default. Clearing QFPROM power-control bits is not
changing fuse contents or disabling secure boot. No experiment here writes
security fuses, OEM IDs, key hashes, boot straps or undocumented ROM controls.

## Ranked hypotheses and the state that would have to differ

| Rank | Hypothesis and required difference | Evidence, limit and experiment |
| --- | --- | --- |
| 1 | **Image provenance / signing or build compatibility.** The bytes presented to PBL must match the fitted silicon and its provisioned trust policy. A suitable image, rather than a host fuse-register override, would differ. | Content distinguishes real ELF from the pattern; both tested major-2 images have no embedded v5 signature/certificate material, as detailed below. This makes provenance a useful first control, not a decode of the error. New `fuxiwlanfw=stock` selects a separate input with no fallback. If the baseline already equals the original device firmware byte for byte, restaging it supplies no new evidence and this hypothesis loses priority. |
| 2 | **PBL requires bytes beyond the 512-KiB FBC prefix for these particular images/state.** Only the advertised initial extent would differ. | Existing ELF inspection puts all headers, hash metadata and kind-3 entries within the prefix, and both mainline and vendor KIWI explicitly use FBC. Thus a whole-file requirement contradicts the reference contract, but is cheaply falsifiable. Retain M6l `fuxiwlanbhi=full`; M6m tests it with the new selector and both IOVA pools. Do not extract SBL segments or rewrite ELF offsets. |
| 3 | **An inherited link-power control survives disabling ordinary ASPM.** L1SS PCI-PM and ASPM enables on the EP/RC would differ. | M6k clears LNKCTL's ASPM bits, while observing L1SS. The L1SS capability has separate PCI-PM enables. New `fuxiwlanpre=l1ss` clears CTL1 bits [3:0] on both functions. This is lower confidence with both functions D0, no matching translation fault, and a readable link. A zero/absent initial capability makes it a no-op, not evidence of a fix. |
| 4 | **RDDM crash sink or early BHIe vector visibility.** RX resources or TX metadata would differ. | Real reference differences, already selectable with M6l `vec=rddm` / `early`; Linux arms RX for crash capture and programs TX after successful BHI. No source establishes either as a prerequisite to PBL ELF authentication. RDDM EE 3 would be crash evidence, not boot success. |
| 5 | **Endpoint reset/warm/QFPROM power/wake, cache or routing.** | Supplied M6k/M6l controls weaken these. Reset regresses earlier. Keep their explicit controls for isolation; do not combine them into a presumed fix. |

No public source or supplied result establishes which host state **must**
change to fix this device. The table states discriminating changes and their
limits; it does not promote private ROM codes into known error names.

## Image and secure-boot audit

The existing ignored inputs were inspected offline, without retaining bytes
in source or extracting segments:

| Input | File bytes | Hash segment offset / bytes | v5 hash-code bytes | QTI sig / cert bytes | OEM sig / cert bytes |
| --- | --- | --- | --- | --- | --- |
| Existing vendor major-2 `amss20.bin` | `0x59e040` | `0x1000 / 0x308` | `0x2e0` (23 SHA-256 slots) | `0 / 0` | `0 / 0` |
| Existing upstream hw2.0 `amss.bin` | `0x5cf040` | `0x1000 / 0x368` | `0x340` (26 slots) | `0 / 0` | `0 / 0` |

Each segment consists of the 40-byte v5 header plus its hash table; there is
no embedded signature or certificate chain in that segment. This refines M6l's
"hash metadata fits" observation. Internal digest matches reported earlier
check consistency, not authenticity. The v5 length-field interpretation was
cross-checked against Qualcomm's `MbnHdr40BV5` in this
[public source mirror](https://github.com/David112x/android-firmware-qti-sdm660/blob/b5e5bb9ed6c92442fd502b3e726d7c8024dc0763/LINUX/android/vendor/qcom/proprietary/common/scripts/SecImage/sectools/common/parsegen/mbn/header.py).
No implementation body was copied.

Qualcomm describes signed ELF authentication as validation of a signed hash
segment, a certificate chain anchored to a provisioned root hash, and policy
fields including hardware/OEM/software identity. A signature valid for another
OEM need not be accepted. That description supplies a plausible mechanism;
it does **not** prove KIWI's enabled policy or decode this failure. Zero v5
lengths could be normal for a device/configuration that accepts these images.
Presence of signature material would likewise not prove acceptance.
[Qualcomm Secure Boot and Image Authentication, sections 1–5](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/secure-boot-image-authentication_11.30.16.pdf)

Vendor MHI reads BHI OEM public-key-hash metadata for its controller info but
does not compare it to the file or choose a signing override in this boot
path. M6m does not read/print serial or key material. A useful provenance
control is the exact major-2 KIWI image from the device's matching stock
firmware partition/package, including vendor signing if present. The earlier
lane doc already describes its vendor copies as coming from `firmware_mnt`;
their provenance cannot simply be assumed wrong because upstream also fails.
Compare the original and staged file privately first. An identical file
under a different name cannot test a signing mismatch.

`tools/wlan-elf-layout.py` now reports v5 code/signature/certificate lengths,
bounds and `authentication=unverified` for each hash segment. Other header
versions remain explicitly unparsed. The kernel adds the same length-only
diagnostic at step 19 and rejects a known-v5 extent whose summed lengths
overflow the segment, using a 64-bit sum. No signature pointers are followed.
The tool reports metadata, never exports firmware or certificates.

## Linux / CAF-CLO comparison

The Qualcomm-derived SM8550 5.15 host drivers were read from public OEM
mirrors alongside current mainline and directly from CLO's
`KERNEL.PLATFORM.2.0.r1-17800-kernel.0` tag. CLO's BHI handler and firmware
selection/transfer body match the inspected OEM MHI implementation; the boot
file differs in its RDDM retry-time source. These are the msm-kernel source
contents used by kernel_platform, not a claim that a full kernel_platform tree
was built. Some direct fetches were rate-limited; the pinned mirror supplies
the corresponding prepare and CNSS evidence.

| Question | Source finding and implication |
| --- | --- |
| SBL versus AMSS ELF | ath11k and ath12k configure FBC, `sbl_size=seg_len=0x80000`. CNSS does the same for KIWI; its 256-KiB exception is another device. MHI copies from **file offset zero**. Neither PCI loader selects PT_LOAD, concatenates kind-3 segments nor strips the ELF/hash headers. "SBL size" names the first-transfer maximum, not a separate SBL file contract. |
| Whole ELF versus prefix | BHI's first transfer is `min(sbl_size, file_size)`. Later BHIe vectors contain the **whole file**, again starting at offset zero. The vendor TME prefix-skipping exception is enabled by CNSS for PEACH, not KIWI. Both inspected images exceed 512 KiB. M6m preserves those bytes/offsets. |
| Alignment | Both loaders write the byte count directly to BHI_IMGSIZE without rounding it to 4 KiB or 512 KiB. The coherent allocation's alignment is distinct from the advertised length. The ordinary size is already 512-KiB aligned. `full` advertises the exact file size, including its unaligned tail, with live translation verification over that extent. |
| TX vector | A BHI buffer address is not a vector-table address. BHIe uses 16-byte `{u64 address, u64 size}` entries, TXVECSIZE is the table's byte count, and the final entry may be partial. FBC TX address/size/doorbell follow BHI/SBL, not initial PBL submission. |
| IMGTXDB | Both reference drivers choose a random nonzero 30-bit session ID. It is neither a firmware segment index nor an image-size multiple; no rule requiring `1` or an increment from inherited PBL state was found. Existing code masks/falls back to nonzero and issues exactly one BHI doorbell. |
| RDDM / BHIe | RX setup is crash capture in prepare; TX is AMSS download later. Prearming RDDM can improve diagnostics without satisfying PBL authentication. A five-second delay cannot turn a sticky explicit PBL rejection into successful FBC. |
| QMI / M3 / board files | Target memory, BDF/regdb/calibration and M3 information are exchanged after MHI firmware boot/service discovery. They cannot be presumed missing inputs to the earlier BHI prefix. The inspected ath11k, ath12k and CNSS QMI paths supply no PBL `ef0e` decoder or pre-BHI OEM-signature fix. |
| PCIe power | ath12k clears LNKCTL ASPM bits for download. ath11k's hw-gated `fix_l1ss` changes endpoint SERDES/PCS registers; it is not a generic WCN7850 recipe. M6m instead uses standard PCI L1SS CTL1 enable definitions, preserving timing/threshold fields and CTL2. |

Primary references, independently read rather than vendored:

- Mainline v6.18 [ath12k MHI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/mhi.c), [PCI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/pci.c), [QMI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/qmi.c); [ath11k MHI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath11k/mhi.c), [PCI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath11k/pci.c), [QMI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath11k/qmi.c).
- Mainline v6.18 [MHI boot](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/boot.c), [prepare/RDDM](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/init.c), [power/READY](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/pm.c).
- Qualcomm-derived 5.15 MHI at `c462ef8ffab7a58e035ee04705b16cdfced494b1`: [boot](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/boot.c), [prepare](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/init.c), [power](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/pm.c).
- Direct CLO msm-5.15, `KERNEL.PLATFORM.2.0.r1-17800-kernel.0`: [boot](https://git.codelinaro.org/clo/la/kernel/msm-5.15/-/blob/KERNEL.PLATFORM.2.0.r1-17800-kernel.0/drivers/bus/mhi/host/boot.c), [power](https://git.codelinaro.org/clo/la/kernel/msm-5.15/-/blob/KERNEL.PLATFORM.2.0.r1-17800-kernel.0/drivers/bus/mhi/host/pm.c).
- Xiaomi SM8550 CNSS2 at `92a9fde479d3262da080cfd28bab0775b0da500a`: [PCI / image selection / TME / bootloader logging](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/pci.c), [QMI](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/qmi.c), [platform](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/main.c).
- Standard [L1SS control masks](https://github.com/torvalds/linux/blob/v6.18/include/uapi/linux/pci_regs.h), [PCI core L1SS handling](https://github.com/torvalds/linux/blob/v6.18/drivers/pci/pcie/aspm.c).

## What Topaz and UniXNU actually contribute

The inspected `ios-topaz` revision explicitly lists Wi-Fi as future work and
points to `topaz-woa`. The working Windows reference uses integrated
WCN3990-class WLAN on the modem DSP: stock modem/WLAN firmware, TrustZone
PAS authentication, SMEM/SMP2P, GLINK/QRTR and firmware service traffic precede
WLFW ready and copy-engine/WMI operation. It is not a PCIe KIWI BHI example.
Segment selection for its modem PAS image does not justify extracting KIWI
SBL segments or adding a modem PAS dependency to ath12k's PCI boot path.
[ios-topaz README at 33852e3c45566951a76f7d5e2ba7f0d026f8432f](https://github.com/MakrSas/ios-topaz/blob/33852e3c45566951a76f7d5e2ba7f0d026f8432f/README.md),
[Topaz boot research at bfe5c15fd63e0038826d84dcbd2a1ff814d52654](https://github.com/MakrSas/topaz-woa/blob/bfe5c15fd63e0038826d84dcbd2a1ff814d52654/docs/RESEARCH_wifi.md)

UniXNU's inspected tree (`b6ae99c17194e6a18fd819d3148e69fe405e91fc`) contains
platform/driver infrastructure but no matching WLAN/MHI firmware loader in
its drivers/kits. It supplies no additional KIWI PBL sequence or error decoder.
[Inspected tree](https://github.com/UniXNU/UniXNU/tree/b6ae99c17194e6a18fd819d3148e69fe405e91fc)

## Selectable experiments and log decisions

These are exact **WLAN argument sets** to replace the WLAN arguments in an
otherwise unchanged boot command line. Keep the common CIVAC/clock control
constant across comparisons. None of these commands has been booted here.

| Experiment | WLAN arguments |
| --- | --- |
| Matched prefix control | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanfw=off fuxiwlanpre=off fuxiwlanbhi=off fuxiwlanvec=off` |
| 1: supplied stock major-2 image | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanfw=stock fuxiwlanpre=off fuxiwlanbhi=off fuxiwlanvec=off` |
| 2: whole-file BHI, existing M6l control | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanfw=off fuxiwlanpre=off fuxiwlanbhi=full fuxiwlanvec=off` |
| 3: explicit L1SS disable | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanfw=off fuxiwlanpre=l1ss fuxiwlanbhi=off fuxiwlanvec=off` |

For experiment 1 the caller must separately stage its chosen stock image as
`stock-amss20.bin` beside the ordinary runtime `amss20.bin` in the established
WLAN firmware directory. This task adds no image or partition reader. The
selector accepts only WCN7850 major 2, opens that single fixed runtime filename,
and fails at step 19 if absent. It neither selects the major-1 `amss.bin` nor
falls back to the baseline. Log labels describe a supplied input, not proven
stock provenance. The usual step-19 image size and step-21 prefix FNV/walk
evidence allow comparisons with the control. Unknown firmware selector strings
fail at step 17 before allocation/writes.

For experiment 3, expect `M6m EP L1SS_CTL1 <before> -> <after> mask 0xf BME off`
and the corresponding RC line at step 21. Upper CTL1 bits and CTL2 remain
unchanged. Absent capabilities are logged. Inaccessible/malformed capabilities,
faults and failed readback stop step 21 (`0x150b`) before BME/doorbell.
`pre=l1ss` conflicts with `fuxiwlanaspm=1` and is rejected at step 17.
Containment uses the existing BME-off/PERST path; these bounded experiments
have no runtime PM restore/resume path, and RC L1SS remains disabled for that
boot. Use a fresh boot for each comparison.

All three variants emit `M6m result fw=... pre=... bhi=... vec=...` and
deliberately stop at step 22 (`0x166c`) even on unexpected BHI success, with no
AMSS TX doorbell. The matched all-off CIVAC control retains `0x166b`.
Expect confirmed BME off, PERST asserted and four buffers freed; containment
failure retains/quarantines buffers. Repeated step 21 cannot ring a second DB.

The useful positive result is loss of `0xef0e0003` **plus BHI completion / SBL
EE 1**, with matching translation evidence and readable link state. A zero
error immediately after DB is already the control behavior. A changed raw
error is a useful discriminator but needs the surrounding EE/transport data.
Same error on a privately verified distinct stock input weakens image
provenance as the cause; same bytes merely repeat the control. L1SS already
zero or absent cannot discriminate that hypothesis. RDDM completion/EE 3 is
a crash outcome. Inaccessible transport makes a content/authentication result
inconclusive. Five-second observation does not itself establish successful boot.

## Host verification and integration

`tools/test-fuxi-wlan-m6m.py` compiles the actual loader C with Linux
ASan/UBSan and synthetic MMIO, VFS, ELF32/64 and scattered physical pages.
It covers default/off mission behavior, strict selectors and conflicts,
wrong chip/major, missing stock with one open/no fallback, exact image bytes,
prefix/full lengths and vectors in both IOVA pools, single DB, L1SS masks,
ordering before BME/DB, no-op capabilities, malformed/inaccessible/readback
failures, v5 unsigned/signed/overflow/unknown-version metadata, and containment
quarantine/retry. The tests verify source/patch byte identity and CI wiring.

The M6, M6i, M6k, M6l and new M6m host suites pass. The new suite is wired into
the existing `kernel-wlan6g` prebuild checks. Patch 0099 carries the exact
standalone source. No kernel CI/native kernel build or new hardware outcome
is claimed; those remain outside this source-only task.
