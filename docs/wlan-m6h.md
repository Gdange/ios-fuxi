# WLAN M6h: upstream DMA audit

Source-only milestone. This does not establish that either optional fix cures
the PBL read failure. No new firmware, hardware observations, or boot results
are included. The M6g IOVA/coherency profile and BHI sequence remain the baseline.

## Findings and sources

* [Linux v6.18 pcie-qcom.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/pci/controller/dwc/pcie-qcom.c?h=v6.18):
  SM8550 uses `cfg_1_9_0`, `qcom_pcie_init_2_7_0`, and
  `qcom_pcie_config_sid_1_9_0`. Our M3 PARF setup matches this init path:
  device type 4, PHY powerdown clear, DBI/ATU bases, slave address space,
  SYS_CTRL bit29 clear, MHI clock/reset BYPASS bit4 set, PM bit5 clear,
  and AXI_MSTR_WR_ADDR_HALT_V2 bit31 set. The SYS_CTRL wake/clock-gating
  mask and Q2A_FLUSH write in other ops revisions are not missing SM8550 writes.
  SM8550's configuration does not request the optional NO_SNOOP override.
* [Linux v6.18 sm8550.dtsi](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/boot/dts/qcom/sm8550.dtsi?h=v6.18):
  PCIe0 `iommu-map` maps RID 0 to SID 0x1400 and RID 0x100 to SID 0x1401.
  MSB CRC8 (polynomial 0x07), seed 0, over bytes `01 00` gives slot 0x15.
  Its table word must be 0x01000100: BDF 0x100, SID offset 1, NEXT 0.
  M6b already scrubs the table and verifies this word, then clears lookup bypass.
  M6h re-reads all 256 words immediately before BHI and after its result.
  The logged base is the DT SID base; the table encodes an offset, and these
  checks do not measure the SID actually arriving at the SMMU.
* [Linux v6.18 pcie-designware-host.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/pci/controller/dwc/pcie-designware-host.c?h=v6.18)
  and [pcie-designware.h](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/pci/controller/dwc/pcie-designware.h?h=v6.18):
  Linux clears implemented inbound/outbound iATUs before configuring ranges.
  Our M3/M4 programs outbound regions 0/1 but leaves inherited inbound state.
  M6h reads eight inbound slots, the entire 0x1000-byte DT ATU aperture,
  without Linux's destructive region-discovery probe. Unimplemented slots
  may read zero. Only enabled slots get a detailed line. The optional fix
  clears REGION_EN in enabled inbound slots and checks exact readback; it
  leaves outbound regions intact. No enabled inbound region has been observed
  by this source-only work. Bridge MEM/PREF windows govern downstream traffic;
  they are not prerequisites for the endpoint's upstream memory reads.
* [Linux v6.18 gcc-sm8550.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/clk/qcom/gcc-sm8550.c?h=v6.18):
  M3 already votes the master, slave, Q2A, aggregation, and DDRSS branches.
  XNU does not explicitly vote CNOC PCIe SF or CFG NOC PCIe ANOC AHB.
  The latter is a clock of the `pcie_noc` provider in sm8550.dtsi. The optional
  clock fix adds these two votes, preserving other bits, checking readback
  and bounded branch readiness. Shared GCC votes are retained for this boot;
  no failure cleanup clears them. Shared RMW follows the existing M3 scheme;
  other drivers do not share a GCC lock with this lane.
* [Xiaomi CNSS2 pci_qcom.c, pinned revision](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/pci_qcom.c):
  enumeration/link control delegates to the platform PCIe driver. The IOMMU
  setup obtains the PCI device's domain and fastmap configuration; it does
  not program a second BDF-to-SID table. Its address-pool selection agrees
  with M6g's CNSS window.

## Interconnect gap

In `qcom_pcie_icc_init`, Linux requests average/peak bandwidth in kB/s:
pcie-mem 0/250000 initially (Gen1 x1), cpu-pcie 0/1. After training,
`qcom_pcie_icc_opp_update` scales the memory peak by negotiated speed and width;
Gen2 x2 is 0/1000000. The DT paths are:

| Path | Source → destination | Tag |
| --- | --- | --- |
| pcie-mem | pcie_noc MASTER_PCIE_0 → mc_virt SLAVE_EBI1 | ALWAYS |
| cpu-pcie | gem_noc MASTER_APPSS_PROC → cnoc_main SLAVE_PCIE_0 | ACTIVE_ONLY |

[Linux v6.18 interconnect sm8550.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/interconnect/qcom/sm8550.c?h=v6.18)
places the memory path through BCMs SN7, SH1, SH0, MC0/ACV and the CPU
path through SH1 and CN0.
[bcm-voter.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/interconnect/qcom/bcm-voter.c?h=v6.18)
and [icc-rpmh.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/interconnect/qcom/icc-rpmh.c?h=v6.18)
aggregate clients and encode votes using cmd-db unit/width/VCD metadata.
XNU currently has no ICC client for either PCIe path. Clock votes do not prove
that these BCM paths are voted, and an absence of XNU votes does not prove
firmware votes are zero. M6h explicitly logs both facts. No raw BCM-address
MMIO reads or guessed shared DDR/LLCC votes are introduced. Porting the shared
ICC/RPMh aggregation is still a gap; neither M6h toggle supplies bandwidth votes.

## Diagnostics and interpretation

`FUXI WLAN: M6h preflight ...` runs after EP BME readback, immediately before
the first BHI doorbell. Step 22 logs the same snapshot as `M6h failure ...`
if polling or either diagnostic sample reports failure, otherwise `completion`.
No PCI status/AER flags are cleared and no image retry is attempted.
An optional-fix failure logs `preflight-blocked` and returns step 21 reason 10.
RC BME loss or an MMIO fault during preflight also blocks the BHI doorbell.

| Line suffix | Fields / expected setup |
| --- | --- |
| RC | PCI_COMMAND/STATUS, BME=1, abort STA/RTA/RMA (signaled target / received target / received master; status bits 11/12/13), secondary status and aborts, MEM/PREF and PREF upper base/limit, AER uncorrectable/root status, AXI response controls |
| PARF | SYS/PM/PHY, MHI_CLK and its BYPASS bit, HALT v1/v2, Q2A, LTSSM, TYPE=4, NOSNOOP |
| PARF ADDR | Legacy DBI/SLV and active v2 DBI/SLV address space, ATU base, BDF_TRANSLATE |
| SID | cfg bit0=0, SID_OFFSET=0, hash=0x15, entry=0x01000100, BDF=0x100, SIDoff=1, NEXT=0, root=0, nonzero=1, mismatch=0; digest covers all 256 words |
| iATU | ctrl2 for slots 0..7, enabled mask; each enabled IB gets ctrl1/2, BAR-match mode, base/limit/target (high:low), plus the selected image IOVA |
| GCC | Vote registers and seven CBCRs in the order below; OFF is raw bit31, OFF_no_HWCG excludes bit1 hardware-gated branches |
| ICC | XNU_votes=none; Linux initial and negotiated avg/peak requests, speed/width and BCM names; firmware_votes=unknown. Link peak zero with Gen0/unknown speed means unclassified, not a Linux zero-bandwidth request |

Legacy PARF address registers are diagnostic only on this revision. All-ones
capability data is absent/unreadable, not proof of an AER event. OFF with HWCG
set may be normal at idle. Vote-register bits and OFF_no_HWCG together are
more useful than raw OFF alone.

| GCC order | CBCR offset | Vote register offset / bit |
| --- | --- | --- |
| aggre_noc_pcie_axi | 0x1003c | 0x52000 / 12 |
| cnoc_pcie_sf_axi | 0x10030 | 0x52008 / 6 |
| pcie_0_mstr_axi | 0x6b02c | 0x52008 / 1 |
| pcie_0_slv_axi | 0x6b020 | 0x52008 / 0 |
| pcie_0_slv_q2a_axi | 0x6b01c | 0x52008 / 5 |
| ddrss_pcie_sf_qtb | 0x1004c | 0x52000 / 19 |
| cfg_noc_pcie_anoc_ahb | 0x10028 | 0x52000 / 20 |

## Boot args, build and checks

Existing `fuxiwlan=1`, `fuxiwlan=22`, and `fuxiwlanoldiova=1` remain available.
New experiments default to observation only; exactly integer 1 enables them:

* `fuxiwlanclk=1`: add the two missing fabric clock votes before BHI.
* `fuxiwlaniboff=1`: disable inherited enabled inbound iATUs before BHI.

Use them separately to distinguish their effects. Fixes require the owned RC
route and EP bus mastering off; neither changes SMMU globals or SID lookup.

`tools/build-fuxi-wlan6h-images.sh` has the M6g packer shape: same base ADT,
header/firmware environment and full/s22 payload, loader and boot image pairs;
only the milestone/output names change. The new toggles are not appended by
the packer. The hosted target remains `tools/ci/build.sh kernel-wlan6g`, with
unchanged `kernel-inputs.py` cmdlines. That target now runs the host suite
before compilation and emits `wlan-source-SHA256SUMS.txt` to identify the
WLAN source and integration patch. All patches still enter the pinned builder.

Host validation: `python3 tools/test-fuxi-wlan-m6.py` (ASan/UBSan, source/patch
identity, read-only bounded snapshots, decoded status/route/clock values,
opt-in gating, preserved votes/outbound state, stopped/RAZ-WI/fault cases and
no-doorbell containment). `CC=clang` selects the compiler used by the Mac CI
host. The artifact verifier accepts `--milestone m6h` for later packed-image
verification; no artifacts were built or device behavior verified here.
