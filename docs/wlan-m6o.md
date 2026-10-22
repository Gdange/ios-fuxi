# WLAN M6o: independent downstream sequence experiments

The supplied device result eliminates M6n's rank-1 AOP PDC candidate: AOP
consumed all 13 `wlan_pdc` messages before rails; steps 1–21 passed; step 22
still reported PBL ERRCODE `0xef0e0003`, BHI STATUS `0xffffffff`, ERRDBG
`0xa0000000/0/0x80000`, without an SMMU fault. This is the caller's observed
result, not a new measurement in this change. The eliminations and raw-code
interpretation limits in [M6n](wlan-m6n.md) still apply.

M6o implements the remaining software-visible deltas as **default-off**
experiments in [the XNU driver](../xnu/fuxi-wlan/fuxi_wlan.c), with early host
hooks in [the shared shim sequence](../shim/fuxi-probe/wlan.c). Patch 0099
contains identical copies. No device, firmware staging, CI, or kernel build
was used for this change.

## Selection and stop

`fuxiwlanseq` accepts a comma-separated list of the following tokens, in any
order. Absent or `off` preserves the existing sequence and performs no extra
experiment MMIO. A single token boots independently; all eight tokens can be
combined. List order does not change hardware order. Duplicates, empty tokens,
unknown values and truncated lists are rejected before power writes. Combining
any experiment with `probe=unmapped` or `pattern` is invalid. `prefix` also
rejects `bhi=full` and `vec=early`: those require the full image/TX allocation
that this experiment intentionally defers. Other existing selectors remain
available; `por` owns RX setup when combined with `vec=rddm`.

Use this fixed control argument set, appending one selector from the table:

```
fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanfw=off fuxiwlanbhi=off fuxiwlanvec=off fuxiwlancnss=off
```

| Variant | Append to control |
| --- | --- |
| Downstream probe off/on chronology | `fuxiwlanseq=cycle` |
| Downstream PARF decode | `fuxiwlanseq=parf` |
| Dynamic width / transaction controls | `fuxiwlanseq=dbi` |
| DBI equalization and core presets | `fuxiwlanseq=eq` |
| PARF controller event mask | `fuxiwlanseq=hostirq` |
| 32-message MSI capability / early MASTER | `fuxiwlanseq=msi` |
| MHI prepare/RDDM/POR | `fuxiwlanseq=por` |
| Separate 512-KiB BHI allocation | `fuxiwlanseq=prefix` |
| All deltas | `fuxiwlanseq=cycle,parf,dbi,eq,hostirq,msi,por,prefix` |
| Existing behavior | `fuxiwlanseq=off` |

All active variants observe for five seconds, print `FUXI WLAN: M6o result seq
mask ... EE ... STATUS ... ERRCODE ...`, and deliberately fail/contain at step
22 (`0x166c`), including unexpected BHI success. They never issue an AMSS TX
doorbell. Step 21 remains one-shot. The mask bits are cycle=1, parf=2, dbi=4,
eq=8, hostirq=16, msi=32, por=64, prefix=128. Each write prints its name,
address, written value, readback and writable mask; bounded waits print their
readback or timeout. Rail logs report command completion, not analog voltage.
Use fresh boots for independent comparisons. PDC need not be rerun; its policy
persists independently as described in M6n.

## Pinned sources

Only register names/offsets/constants and protocol facts were used from these
public sources. The implementation is independent; no downstream driver code,
firmware, or downloaded source is included in the repository.

* K: MiCode/Xiaomi_Kernel_OpenSource commit
  `7fad87dc178d09489b74a714e4a525d56f546855`.
* C: MiCode/vendor_qcom_opensource_wlan commit
  `518147fd7e0199758efe9bd96a22c9c2ea30bcd9`.
* X: MiCode/kernel_devicetree commit
  `5647c799a064dfec73332a4fe2ef7aa3fb2f4887`.
* D: LineageOS/android_kernel_xiaomi_sm8550-devicetrees commit
  `13913424c9d0af031a8b8f9981c003bbb9632f73` (matching public KIWI child;
  the same qualification as M6n).

## `cycle`: probe-time power off, then actual power-up

The chronology comes from [C probe/off](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5952),
[C suspend/resume](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1177),
[C actual power-up](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L2600),
[K PME_TURNOFF/L23 suspend](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L7927), and
[D KIWI supplies/pins](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/wlan/kalama-cnss.dtsi#L164).

After initial enumeration/BAR setup and validation of BHI offsets, step 17:

1. Disable message IRQs using the existing helper; clear endpoint COMMAND
   MASTER (bit 2), then save config. The pinned suspend clears MASTER **before**
   its save call. Save COMMAND, cacheline/latency, six BAR dwords, interrupt
   line, and PCIe v2 Device/Link/Device2/Link2 controls. RO/status fields are
   never replayed. COMMAND/status and PCIe controls use only the low halfword.
2. Disable function decode (COMMAND=`0x400`, INTx disabled), PMCSR state=3
   (D3hot), wait 10 ms. PMCSR preserves other controls and writes zero to its
   PME-status W1C bit.
3. Write DBI ELBI SYS_CTRL `+0xf24=0x10` (PME_TURNOFF); require PARF PM_STTS
   `+0x24` bit5 within 100 ms. Assert PERST GPIO94 OUT=0; clear PARF LTSSM
   `+0x1b0` bit8; require root Link Status DLLLA=0 within 100 ms.
4. Drive WLAN_EN GPIO80 OUT=0; send enable=0 for the six existing WLAN RPMh
   votes in reverse downstream order: S4G, S6G, S4E, S2G, S5G, L15B; wait
   10 ms. Then voltage-before-enable in downstream order: L15B=1800,
   S5G=1000, S2G=980, S4E=950, S6G=1900, S4G=1350 mV. Each command must
   complete through the existing bounded RSC transport. Resource addresses
   are checked against the six existing cmd-db results before use.
5. GPIO80 OUT=1, wait 10 ms (X PCIe0 EP latency); clear ELBI SYS_CTRL, deassert PERST, wait 1 ms,
   enable LTSSM, require DLLLA=1 within 1 s and PCI ID `17cb:1107`. PMCSR D0,
   wait 10 ms, enable MEMORY/disable INTx, restore saved BARs/controls and
   COMMAND with MASTER clear. Revalidate BHIOFF/BHIEOFF and PBL EE.
6. At the end of step 18, after existing optional pre-BHI work, install an
   owned, initially fault-only SMMU mapping, perform the existing fabric/ASPM
   preflight, explicitly restore D0/enable/config, and set COMMAND=`0x406`
   (MEMORY|MASTER|INTx-disable) before MHI contexts/RX preparation. New owned
   DMA leaves are published and TLB faults invalidated at step 20.

This is the WLAN supply/PERST/L23 chronology experiment. It **retains the
controller clocks, PHY and outbound apertures** during suspension; it does
not reproduce Linux clock/regulator framework teardown and reinitialization.
No PHY table is changed or replayed. A negative result therefore tests this
specific chronology, not every internal effect of Linux runtime PM.

The downstream WLAON clear is gated by `qcom,set-wlaon-pwr-ctrl`, absent from
the matching KIWI child; it is explicitly logged as skipped. See [C gate and
clear](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1931).
No QFPROM/fuse programming, secure-world call, or eliminated reset recipe is
introduced. Existing `pre=qfprom` remains a separate, already eliminated control.

## Host-controller groups

Offsets are relative to PCIe0 PARF or DBI. “Mask” means replace only those
bits and preserve the remainder; full writes use `0xffffffff`. Shared
DEVICE_TYPE, PHY_CTRL, PM_CTRL, MHI clock-bypass and AXI halt operations already
match and stay in the existing sequence. PHY tables, RID/SID routing, ASPM/L1SS
policy, link target Gen2 and ordinary root/endpoint BAR assignments are not
new host-write experiments.

| Selector / point | Exact writes | Pinned source |
| --- | --- | --- |
| `parf`, step 8 before PHY | PARF `+0x350=0`, `+0=0x365e`, `+0x358=0x04000000`. Suppress the default mainline address-decode block (`+350/+354/+634/+638/+358/+35c` and SYS_CTRL bit29 clear), so it cannot overwrite downstream values. No guessed writes to the other decode registers. | [K initialization](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4705), [X PCIe0 size](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama-pcie.dtsi#L4) |
| `dbi`, step 10 before PERST release | PARF `+0x2c4`: clear bit0, set bit8 (mask `0x101`); DBI `+0x714`: set bit5. | [K dynamic width](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4775) |
| `dbi`, step 11 before LTSSM enable | DBI DEVCTL `+0x78`: MRRS=5, mask `0x7000`, value `0x5000`; write zero to upper W1C status. This is the root request size. | [K link train](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4574) |
| `dbi`, step 12 after CFG0 enumeration | DBI ACK_F_ASPM_CTRL `+0x70c` set bit15; AUX_CLK_FREQ `+0xb40=20`. No N_FTS/bridge completion-timeout override in X. | [K configuration](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3744) |
| `eq`, step 11 before MRRS/LTSSM | DBI `+0x80c`: mask `0x1f00`, value `0x100`. For each supported Gen3..Gen4 from root LNKCAP: `+0x890[25:24]=generation-3`; `+0x8a8[23:8]=0`; `+0x890` clear bit0; `+0x8ac[4:0]=0`. Then `+0x8bc` bit0=1, `+0x154=0x55555555`, also `+0x188=0x55555555` if Gen4 supported, and `+0x8bc` bit0=0. Unknown/non-Gen3/4 support aborts. These are DBI equalization shadows, not PHY-table writes. | [K field programming](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3715), [K presets](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3689), [K defaults](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L6770) |
| `hostirq`, step 8 | PARF INT_ALL_MASK `+0x22c=0`, then `0x3fc0d202`: link-down bit1, L1SUB timeout bit9, bridge flush bit12, AER bits14/15, MSI0..7 bits22..29. No GIC/ITS enable, interrupt handler or AER reporting setup is added. This tests the controller mask separately from endpoint MSI. | [K mask writes](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4713), [K event indices](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L330) |

Optional AER enable/reporting and bridge completion-timeout branches do not
supply an unconditional KIWI write recipe, and remain out of these groups.
Already audited routing/window/PM controls remain the established controls.

## `msi`: capability state and MASTER ordering

[C MSI allocation](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L4387)
requests exactly 32 vectors; [C assignment](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci_qcom.c#L7)
reserves vectors 0..2 for MHI. [C resume](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1216)
does link/D0/enable/restore/MASTER before MHI start. MSI address/data are
IRQ-domain allocations in Linux, **not pinned numeric constants**.

At step 18, save config, locate a valid 64-bit MSI capability with MMC>=5,
disable inherited MSI/MSI-X, allocate and clean a separate owned DMA page,
and install its SMMU mapping while BME remains clear. Run fabric preflight,
D0, function enable/config restore and download ASPM setup, then program:

* MSI address HIGH/LOW to that page's verified IOVA, data base=0 (32-message
  aligned); if per-vector masking is supported, clear its 32 mask bits.
* MSI Message Control MME=5 and Enable=1 (dword mask `0x00710000`, value
  `0x00510000`); preserve MMC, 64-bit support and all other capability fields.
* COMMAND=`0x406` before contexts/RX preparation. With `cycle`, these steps
  occur after the probe off/on sequence. With MSI alone there is no rail cycle.

Every message can only overwrite the sink dword, irrespective of vector data;
no interrupt handler, GIC/ITS programming, MSI-X table or arbitrary destination
is used. This reproduces the endpoint capability **enable/count** and early
MASTER ordering, not Linux ITS interrupt delivery or allocator-chosen data.
An unexpected requester/SID or access outside allocated leaves faults instead
of accessing unrelated memory. Real hardware delivery/routing is unverified.
The existing M6 SMMU/RC ownership checks still precede MASTER; early BME never
exposes bypass DMA. The image/contexts/RX leaves are added, cleaned and TLB
synchronized at step 20. Step 21 verifies early MASTER and the MSI tuple
survived and refuses to silently repair a changed state.

## `por`: MHI prepare, RX vector order and POR INTVEC

[K prepare](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/init.c#L1099),
[K RX programming](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L22), and
[K POR](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/pm.c#L1122)
provide these constants/order:

1. Step 20 creates disabled host contexts, then zeros **every** BHIE RX dword
   `+0x60,+64,+68,+6c,+70,+74,+78`, verifying zero, before RX allocation.
2. Allocate/zero/clean the existing `0x780000` RDDM sink and its 15-entry
   16-byte vector table; map them before advertising an RX doorbell.
3. Write BHIE RX address **HIGH `+0x64`, LOW `+0x60`**, byte size `+0x68=240`,
   then random nonzero 30-bit sequence at DB `+0x70` (preserve other bits).
   Every tuple component and sequence is read back. RDDM is a sink only;
   contents are never dumped.
4. Write BHI INTVEC `+0x20=0` in POR, then read EE/state. Require PBL. Only
   SYS_ERR state triggers MHICTRL.RESET, bounded 25-ms reset-clear polling
   (1 s), then a second INTVEC=0 write and PBL/RESET-state validation. No unconditional cold reset.
5. Step 21 clears only TX DB/status; it retains the already prepared RX
   tuple/doorbell and POR INTVEC. Existing BHI STATUS/address/size/random
   image-doorbell ordering stays intact.

This intentionally separates MHI's HIGH→LOW and early POR order from M6l's
older late RX staging. It does not attempt to turn RDDM into an image loader.

## `prefix`: allocation and VFS read lifetime

[K firmware/BHI staging](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L476)
allocates a separate coherent `min(sbl_size,file_size)` buffer from file offset
zero, frees it after BHI, then allocates FBC. [C sizes](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5632)
set the maximum/segment size to `0x80000`.

Step 19 checks the existing file-size/ELF/hash bounds, allocates **only 512 KiB**
using the existing coherent DMA contract, and reads exactly those prefix bytes
from the vnode. It retains the full file length as metadata; it does not
allocate or read the rest of the image. The existing `wl_image` ownership slot
holds this dedicated prefix allocation, separate from any full-file/FBC buffer.
Step 20 allocates contexts but **no TX/FBC vector table**. BHI advertises file
offset zero and `0x80000` bytes, with the existing translation walk, content
hash, cache publication and readbacks. No ELF segment extraction is introduced.

All experiments stop at 22, so there is no post-BHI full-file allocation.
The prefix is freed only after normal containment/SMMU undo; on an unproven
cleanup it is quarantined. That deliberate diagnostic lifetime differs from
Linux's immediate BHI-prefix free. Small files remain rejected by the existing
local contract; real inputs in the prior experiment exceed 512 KiB.

## Failure handling and verification

Unexpected writable-bit readback, invalid capability/PCI ID, MMIO error,
regulator completion failure, bad mapping or bounded wait failure aborts the
run without retry or panic. Host mutations are journaled before each write;
reverse undo includes the selected EQ shadow and DBI RO-write gate. W1C status
is never restored. On abort after early host changes, PERST is contained even
before MHI becomes available. The original global fault latch is retained.
MSI is disabled and its saved address/data/mask restored after BME clear and
before PERST assertion; its sink stays allocated until containment is proven.
Host undo occurs under asserted PERST. A cycle also drives WLAN_EN low and
attempts six disable votes. A latched access/RSC fault can prevent rail undo;
that uncertainty is logged and buffers remain quarantined. No guessed voltage
rollback or repeated power-up is attempted. The normal BME-off/PERST/drain and
owned SMMU undo remain prerequisites for freeing DMA allocations.

[The M6o host suite](../tools/test-fuxi-wlan-m6o.py) compiles production C with
ASan/UBSan. It checks all 255 nonempty combinations, exact host constants,
regulator order/data, actual config loss/restoration during a synthetic cycle,
MSI mapping/32 message values, complete RX clears/HIGH→LOW/random DB, POR
SYS_ERR-only reset, stuck/reset-state failures, prefix bytes/maximum-size file without full allocation,
invalid selectors/capabilities, failures at new register writes/regulator
commands/allocations, one-shot BHI/step-22 stop, undo and quarantine. M6i also
checks the actual runner's invalid-M6o-before-power gate. Both standalone C
files are checked byte-for-byte against patch 0099. M6o is registered beside
M6n in the existing `kernel-wlan6g` prebuild list; that list was not invoked.

Host results: M5 compatibility, M6, M6i/M6j, M6k, M6l, M6m, M6n and M6o pass.
The optional M6 packed-artifact audit needs ignored build outputs that are
absent here and cannot validate a newly linked kernel. Linux host mocks do
not establish physical MSI delivery, L23 transitions, effective rail voltage,
or ROM behavior. A native XNU link requires the macOS/Xcode environment in
[xnu/README.md](../xnu/README.md). No new hardware result is claimed.
