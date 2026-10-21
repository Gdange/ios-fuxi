# WLAN M6l: pre-BHI sequence and FBC experiments

Source only. Defaults, `kernel-wlan6g`, artifact names and image command lines
remain unchanged. CI builds that target from main after integration; this work
adds its host test to `tools/ci/build.sh` without dispatching a build. No firmware,
external implementation, device dump or generated image is included.

## Evidence and ranked hypotheses

The supplied M6k observations establish a useful distinction: an intentionally
unmapped advertised image causes a matching CB translation fault; a mapped
invalid marker avoids that fault but retains `0xef128100`; real images instead
reach `0xef0e0003`, including with explicit CIVAC. These are observations from
the supplied evidence, not new M6l device results. They support DMA reaching
the owned translation context and a content-dependent PBL path. They do not
prove every byte was read, decode either error, or exclude image authentication.

1. **Endpoint power/reset state omitted before BHI.** Fuxi trains the host
   link and checks PBL/RESET but omits ath12k's endpoint LTSSM/hot-reset setup,
   pending-interrupt clear, QFPROM power control, warm-state cleanup, SoC reset
   and MHI reset. This is the largest concrete sequence difference. The warm
   cookie/reset cause may survive conditions which already look like PBL/RESET.
   Test `reset`, with `warm` and `qfprom` to isolate components. No evidence yet
   ties any of them specifically to `0xef0e`.
2. **QFPROM power/shutdown state.** ath12k always clears bit 2 of
   `WLAON_QFPROM_PWR_CTRL_REG`; CNSS conditionally clears bits 2 and 0 before
   starting MHI. A later validation stage could depend on fuse access (inference).
   Test `qfprom` and inspect the logged before value: clearing already-zero bits
   cannot establish a causal fix. The CNSS DT gate is discussed below.
3. **Wake state.** Fuxi does not assert endpoint-local SOC_WAKE or implement
   runtime MHI wake references. Test `wake` as a narrow experiment. ath12k's
   local force-wake write is in shutdown, and CNSS explicitly does **not** take
   a force-wake reference for its normal pre-MHI QFPROM write. That weakens the
   claim that a missing force-wake is a normal PBL prerequisite.
4. **Missing prearmed RDDM RX table.** Newer ath12k and CNSS prepare an RX
   crash sink before BHI, unlike Fuxi's previous “no RX buffers.” This is a real
   difference from those paths, but v6.6 ath12k leaves `rddm_size` zero and does
   not arm such a sink. Thus it is not a universal ath12k PBL precondition. Test
   `rddm` to distinguish a ROM dependency from missing error-reporting capacity;
   the documented host purpose is crash capture, not AMSS download.
5. **BHI size/loader contract different from FBC.** Test `full`, but both
   mainline and vendor KIWI explicitly choose FBC with a 512-KiB first transfer.
   Both inspected images contain their entire program table and hash metadata
   in that prefix. A full-file BHI transfer is a falsification experiment, not
   Linux parity, and could itself be rejected by the ROM.
6. **Early AMSS TX vector metadata required by PBL.** Test `early` last.
   Linux/vendor FBC does not program TX address/size/doorbell until after BHI
   succeeds and SBL handling begins. Thus zero pre-BHI TX registers are expected.
   There is no source evidence that PBL uses this table to finish hashing the
   initial ELF. Early TX staging here deliberately leaves its doorbell zero.
7. **Address width, routing, cache, clocks, ICC or ASPM.** Lower priority given
   the supplied fault/content experiments and unchanged real-image result with
   CIVAC, acknowledged ICC, clocks on and ASPM off. Both Linux implementations
   allow 36-bit DMA; Fuxi's submitted IOVAs are below 4 GiB. The existing legacy
   IOVA switch remains available; M6l adds no speculative high-IOVA experiment.

Authentication, signing/provisioning mismatch and another undiscovered host
precondition remain possible. Different firmware builds producing the same
code does not distinguish those. No inspected public source gives names to
`0xef128100`, `0xef0e0003`, `0xef0e0200` or `0xef000008`.

## Ordered comparison: all differences found in the requested boot scope

References are pinned below. L66/L18 refer to Linux v6.6/v6.18, LM to the
inspected newer ath12k revision, C to CNSS, V to vendor MHI. “Endpoint” internal
addresses are accessed through BAR windows; they are not SM8550 host physical
registers. Fuxi step numbers refer to the ordinary default path.

| Order / operation | Reference behavior | Fuxi default and consequence |
| --- | --- | --- |
| Platform power and PCIe enumeration | Mainline's PCI function is already enumerated; platform/PCI core supplies power, reset, clocks, link and resource ownership. C powers the device and resumes PCIe before MHI. | Steps 1–15 implement board rails/GPIOs, QMP PHY, host LTSSM, link, BAR and routing directly. This does not reproduce endpoint-local reset. Existing M6i clocks/ICC and M6k ASPM remain. Board PMIC/ACPI sequencing is not inferred from a PCI-function driver. |
| PCI/DMA initialization | PCI core enables the function; ath12k/C enable bus mastering during PCI setup and negotiate a 36-bit DMA mask for WCN7850/KIWI. | EP BME stays off until immediately before BHI; RC BME is enabled at step 14. This intentionally postpones DMA until owned SMMU mappings are verified. M6l RX table can be armed while BME is off. |
| WCN7850 hw/PCI ops | WCN7850 has wake/release callbacks using `mhi_device_get_sync`/`mhi_device_put`; register accesses use them after PCI INIT_DONE, outside always-accessible regions. QCN9274 has null callbacks. L18 hw params select WCN7850 registers and `rddm_size=0x780000`. | No runtime wake/refcount state machine or INIT_DONE; dedicated, bounded bring-up thread only. No automatic wake-reference acquisition before PBL. Reset variant checks PCI ID and hw major 2 before writes. |
| Register window setup | `ath12k_pci_power_up` resets cached window selection/INIT_DONE. Later static windows are installed only when selected by hw params. | Fuxi saves/restores actual window register for each internal access, preserving static bits. WCN7850 has `static_window_map=false` in ath12k; its internal reset registers use the dynamic window, with local SoC reset/wake accessed directly. It has no cached window selection; no CE/DP access is needed here. |
| Endpoint LTSSM | `ath12k_pci_enable_ltssm` writes endpoint `0x01e081b0=0x111`, retrying up to five times, including waits on inaccessible reads. | Only **host** PARF LTSSM was configured. `reset` supplies endpoint setup but fails closed on inaccessible/failed readback instead of retrying an unstable link. |
| Endpoint hot-reset control | L66 uses `0x01e38338`; L18/LM select the WCN7850 corrected address **`0x01e40304`**, setting bit `0x10`, then waiting 5 ms. QCN9274 retains the old address. | Absent by default. `reset` uses only the corrected WCN7850 address. The old generic address is never written. |
| Endpoint interrupt latch clear | `ath12k_pci_clear_all_intrs` writes all ones to endpoint `0x01e08228`, preventing stale hot-reset interrupts from reaching SBL. | Disabling MSI/MSI-X and legacy INTx does not clear that endpoint latch. `reset` adds the W1C write; it does not require all ones to read back. |
| QFPROM power | ath12k clears `0x4` at endpoint `0x01f8031c`. C `cnss_pci_set_wlaon_pwr_ctrl(false,false,false)` clears `0x5` before MHI **only if** `qcom,set-wlaon-pwr-ctrl` is present. C also gates on link availability. | No default write. `reset` clears `0x4` for ath12k parity; `qfprom` clears `0x5` for the conditional CNSS behavior, preserving all other bits and verifying the result. The pinned KIWI `chip_cfg@1` does not contain that DT property; the helper's existence alone does not prove a stock KIWI boot executes it. |
| L1SS fix | Neither L66, L18 nor LM ath12k has `ath12k_pci_fix_l1ss`. The similarly named **ath11k** helper changes SERDES sysclk/PCS oscillator detection, under its hw `fix_l1ss` flag. | No ath11k PHY workaround transplanted. Fuxi disables ASPM on both EP and RC; L1SS controls are observed. ath12k disables endpoint ASPM during download and restores later; Fuxi ends partial/failed boots with PERST instead of runtime restore. |
| BHIe vector status clear | ath12k clears TX DB/status and RX DB/status before debug cleanup and SoC reset. Register POR values may be undefined. | Step 21 already clears those four words, but without the preceding reset sequence. `reset` additionally clears them at the corresponding earlier point. M6l does not interpret zero as a missing TX-table fault. |
| Warm/debug state | Read Q6 cookie `0x01f80500`, read warm entry `0x01f80504`, wait 10 ms, clear warm entry, wait 10 ms, read **read-clear** reset cause `0x01f8060c`. This avoids a stale Q6 warm path. | Previously absent. `warm` and `reset` implement these transactions, including delays. Cookie is observed, not overwritten. Reset cause is read once, not written. |
| SoC global reset | ath12k toggles local `0x3008` bit 0: assert 10 ms, deassert 10 ms. This is endpoint SoC reset, not PERST or host LTSSM. | Previously absent. `reset` toggles while BME is off, attempts deassert even after a latched exception, then requires accessible/deasserted readback. No such power-up toggle was found in C's normal `cnss_qca6290_powerup` chain, which relies on platform power/link sequencing. |
| MHI reset after SoC reset | ath12k writes `MHICTRL=2` and waits 10 ms because SoC reset can leave SYS_ERR. Generic MHI also resets/polls when async power-up encounters SYS_ERR. | Default only accepts PBL, no pending reset and RESET/no SYS_ERR at step 20; abort resets after failure. `reset` now resets *before* loading, polls up to 1 s more, requires RESET/no SYS_ERR and unchanged BHIOFF/BHIEOFF. It does not repair a failing transport and retry BHI. |
| Local force wake | ath12k's `0x3004=1`, 5-ms helper is called during power-down. C's KIWI sync force wake uses MHI device get, with atomic/non-atomic forms and a release; ordinary power-up QFPROM call passes `do_force_wake=false`. | No default call. `wake` writes/readbacks local `0x3004=1`, waits 5 ms, leaves it asserted until containment. This tests a local request, **not** equivalence to a completed MHI wake handshake. |
| QRTR node ID | L18/LM conditionally write local `0x3164` (alias of `0x01e03164`) for the firmware MULTI_QRTR_ID feature. L66 lacks this feature path. It disambiguates later QMI service instances. C also calls a QRTR helper between INIT/POWER_ON, but that helper accepts QCA6390/QCA6490 only, skips KIWI, and writes SCRATCH2 only for a nonzero configured node ID. | No QRTR ID override. One device, no QMI/IPCR startup. The feature is not an ELF hash/size requirement or an unconditional PBL register write. |
| SOL | C initializes optional HOST_SOL output low and DEV_SOL input/IRQ. Device SOL feeds the SYS_ERR/RDDM recovery path; host SOL assertion is a fallback to request recovery/crash capture. ath12k's inspected PCI/MHI power-up has no SOL GPIO step. | No SOL GPIO discovery/IRQ/recovery. PCIe WAKE#/CLKREQ# setup is a different signal. No evidence here supports asserting HOST_SOL during normal PBL download; no guessed GPIO write added. |
| `ath12k_mhi_start` | INIT calls `mhi_prepare_for_power_up`; POWER_ON calls sync power-up (which calls async power-up and waits for mission). L66 uses async in its older state wrapper. | Fuxi manually runs the relevant phases, polling rather than using IRQ/workqueue transitions. Its five-second per-phase bounds and experiment stops differ from a driver waiting for full mission initialization. |
| Serial / TCSR / debug-only registers | Generic MHI reads BHI serial metadata; C reads TCSR hardware version to select the hw-major firmware name, and reads PBL TCSR/BOOT_CFG/bootstrap/ERRDBG on failure. C’s WDOG discriminator write is QCA6390-only, spare debug marker is QCA6490 recovery-only. No additional unconditional KIWI TCSR/BOOT_CFG write was found in the inspected power-up chain. | Fuxi reads hw version and bounded failure logs, deliberately skips serial/key metadata and unrelated chip debug writes. These are not demonstrated missing PBL setup writes. |
| MHI contexts | Prepare allocates 128 channel contexts, two WCN7850 events and command resources before power-up. Context MMIO initialization happens in READY, not before the PBL image fetch. | Step 20 allocates disabled channels plus 32/256-element event rings and command ring; step 24 writes context bases. No IPCR rings/channels or runtime consumers. Their absence is intentional and downstream of this PBL failure. |
| RDDM before BHI | When `rddm_size` is nonzero (L18/LM/C; **not L66 ath12k**), prepare reads offsets, clears **all RX words** from RXADDR through RXSTATUS, allocates crash storage/vector table, programs RX addr/size and a random nonzero RX DB. Allocation failure can leave Linux without a sink. | Default has no RX storage, only clears RX DB/status. `rddm` provides a zeroed `0x780000` sink, fifteen 512-KiB vectors, 240-byte RX table and pre-BHI RX DB. It never prints the sink or treats RX completion as firmware success. |
| INTVEC / async power-up | Generic MHI writes BHI INTVEC=0, validates power-up EE, handles SYS_ERR, enables requested IRQs and queues PBL or READY work. | INTVEC=0 and PBL validation already match. Fuxi refuses other EEs; MSI/MSI-X stay off and it polls. No MHICTRL M0/READY write is introduced before BHI by any variant. |
| SBL transfer policy | L66/L18/C select `fbc_download=true`, `sbl_size=512 KiB`, `seg_len=512 KiB` for WCN7850/KIWI. Vendor's 256-KiB exception is QCN7605, not KIWI. | Default also transfers the raw first 512 KiB, no ELF segment extraction. It rejects files smaller than that rather than clamping. Both real files exceed it. `full` overrides only the advertised BHI length, not vector segment length. |
| BHI allocation and DMA tuple | Linux copies the prefix into a temporary coherent allocation; high address, low address, length, nonzero 30-bit DB. It observes BHI status/error; ERRDBG is printed raw. | Fuxi allocates/maps the whole file, exposes its prefix at the first IOVA, writes the same tuple, validates it and walks live mappings before BME. It also accepts SBL EE as BHI completion, diagnoses inaccessible/all-ones status separately, and observes sticky CB faults. `full` walks/hashes the entire advertised extent including a partial last page. |
| TX FBC table timing | Only **after successful BHI**, the host allocates/copies the whole image into 512-KiB buffers/vectors while transitioning to READY. AMSS handling then programs TXADDR/TXSIZE/TXDB on SBL transition. | Step 20 builds vectors earlier in host memory, but TX MMIO remains untouched until step 26 (SBL/M0 already observed). Pre-BHI zero TX MMIO is consistent with Linux. `early` writes only TXADDR/TXSIZE before BHI to test visibility, without starting a TX transfer. |
| READY / AMSS after BHI | MHI waits for reset-clear/READY, initializes context MMIO and event rings, requests M0; SBL work downloads AMSS via BHIe. | Steps 23–27 enforce READY, publish contexts, request M0/SBL, ring TX then poll completion/sequence. Fuxi serializes SBL/M0 before TX more strictly than queued Linux work. None of these later ordering differences explains an error while still in PBL by itself. |
| Vendor TME path | V can exclude the SBL prefix from the later TX image if `tme_supported_image`; C's selector returns true for PEACH, **false for KIWI**. | Full-file AMSS vectors are appropriate for this reference KIWI path. No prefix-skipping variant added; that would change a later transfer unrelated to the observed PBL rejection. |
| Newer `fw_load_type` | L18 chooses FBC first when `fbc_download` is true. With FBC false, nonzero `seg_len` selects full-image BHIe; zero selects BHI. Firmware-container `fw_data` still uses `sbl_size` for FBC. | No accidental full-file BHI policy follows from that enum for ath12k. `full` is intentionally outside the configured reference path. Firmware-container headers must not be substituted for the AMSS ELF payload. |
| DMA width / IOVA / physical pages | ath12k and C use 36-bit masks; BHI and vector entries carry 64-bit DMA addresses. DMA/IOMMU may assemble scattered pages into contiguous IOVA; no inspected KIWI rule requires physically contiguous or >4-GiB IOVA. | Fuxi uses 32-bit IOVA pools, 36-bit physical output, explicit scattered-page mappings and 64-bit tuple/vector fields with zero high IOVA words. These fit the reference masks. It does not truncate high physical addresses into image IOVA. |
| Cache / timing / error recovery | Linux uses coherent DMA allocation and IRQ/workqueue waits; ath12k adds the explicit reset delays above. ROM error/debug words have no decoder here. | Wired pages plus explicit PoC maintenance, optional CIVAC/NC, no IRQs. M6l changes no default cache policy or global post-link delay. It stops/contains instead of retrying firmware or collecting an unbounded crash dump. |

## ELF layout inspection

`tools/wlan-elf-layout.py` reads ELF32/64 little-endian headers and every program
entry with file bounds checks. It reports only names and metadata, never extracts
segments or retains firmware. Invoke it with one or more caller-supplied firmware
filenames; `--prefix 0x80000` is the default. JSON contains all entry extents,
Qualcomm kind bits, and whether each extent fits the advertised prefix.

The two **uncommitted local inputs** were inspected with that script:

| Image basename | File bytes | PH offset / count / end | Hash offset / bytes / end | Kind-3 entry maximum end |
| --- | --- | --- | --- | --- |
| `amss20.bin` (vendor) | `0x59e040` | `0x34` / 23 / `0x314` | `0x1000` / `0x308` / `0x1308` | `0x294f8` |
| `amss.bin` (upstream WCN7850 hw2.0) | `0x5cf040` | `0x34` / 26 / `0x374` | `0x1000` / `0x368` / `0x1368` | `0x2a4f8` |

Both are ELF32, little-endian, version 1, machine 40, with one hash segment.
The program table, hash segment, and all kind-3 segments fit in the first
`0x80000` bytes. Other load segments extend beyond that prefix; one crosses its
boundary (entry 12 in vendor, entry 14 in upstream). That is compatible with
later full-image FBC, not proof PBL must load/hash every PT_LOAD immediately.
The script does not assign undocumented ROM meanings to kind 3. Linux does not
parse these program headers to choose BHI size; it passes the configured raw
prefix. Fuxi's existing metadata-fit check already covers the table/hash extents.

Consequently, the specific claim “the hash segment or program table lies beyond
512 KiB and requires preprogrammed BHIe” is contradicted by these local layouts.
This does not prove signature validity, trust roots, rollback state, or the
firmware's internal loader contract. ERRDBG values resembling submitted
address/high-word/size are not a DMA completion report; CNSS's bounded SBL-log
interpretation of DBG2/3 must not be applied to a PBL tuple outside SRAM bounds.

## Exact boot arguments and expected lines

Use the **same firmware and other conditions** for each comparison. These are
complete argument sets for the proposed tests; ICC is enabled by default, ASPM
safeguard remains enabled. CIVAC holds the previously exercised cache condition
constant. Explicit `off` values make each trial independent. First capture the
control, then run one changed condition per boot in this order:

| Trial | Boot arguments | Identifying lines before BHI |
| --- | --- | --- |
| Control (M6k behavior) | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanbhi=off fuxiwlanvec=off` | `M6l pre=off bhi=off vec=off valid=1`; existing `DC CIVAC + DSB SY` and BHI `bytes 0x80000` |
| ath12k reset | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=reset fuxiwlanbhi=off fuxiwlanvec=off` | `M6l pre-BHI reset begin BME off EE PBL`; `SOC write [0x1e40304]`; `QFPROM before … clear mask 0x4`; `warm entry cleared reset cause … (read-clear)`; `pre-BHI reset complete BME off EE PBL` |
| Warm state only | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=warm fuxiwlanbhi=off fuxiwlanvec=off` | `M6l warm cookie … entry …`; `SOC write [0x1f80504] 0 ok window restore checked`; `pre-BHI warm complete BME off EE PBL` |
| CNSS power bits only | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=qfprom fuxiwlanbhi=off fuxiwlanvec=off` | `M6l QFPROM before … clear mask 0x5`; `SOC write [0x1f8031c] … ok window restore checked`; `pre-BHI qfprom complete BME off EE PBL` |
| RDDM RX | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanbhi=off fuxiwlanvec=rddm` | `BHIe TX/RX doorbells/status cleared; RX buffers prepared`; `M6l BHIe RDDM RX staged bytes 240 before BHI; RX doorbell armed; sink only` |
| Local wake | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=wake fuxiwlanbhi=off fuxiwlanvec=off` | `M6l pre-BHI wake begin BME off EE PBL`; `M6l pre-BHI wake complete BME off EE PBL` |
| Whole-file BHI | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanbhi=full fuxiwlanvec=off` | BHI preflight `size 0x59e040` (vendor) or `size 0x5cf040` (upstream); walk `pages 1439/1439 MATCH` or `1488/1488 MATCH`; BHI doorbell bytes equal that size |
| Early TX metadata | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanbhi=off fuxiwlanvec=early` | `M6l BHIe AMSS TX staged bytes 192 before BHI; TX doorbell remains zero` for either inspected image |

The prefix `FUXI WLAN:` precedes these lines. Ellipses denote observed register
values, not literal output. All variant boots identify `valid=1`, print the
usual BHI diagnostics immediately and after observation, sticky CB fault status,
host transport diagnostics and bounded boot logs when transport permits.

Expected experiment end, **even if BHI succeeds**:

- `M6l result pre=<mode> bhi=<off|full> vec=<mode> EE … ERRCODE … observation 5000 ms; stop/contain`
- `step 22 FAILED code 0x166c`
- `abort step 22 … bus master off yes, PERST asserted yes`
- owned SMMU undo and `abort buffers freed` (six allocations for RDDM, four otherwise).

The all-off CIVAC control retains M6k's `step 22 FAILED code 0x166b` and M6k
result; it does not emit an M6l result. An unselected ordinary loader still has
its original 28-step behavior. Selecting `fuxiwlan=1` with a variant also stops
at 22; variants never ring the AMSS TX doorbell or enter the READY/M0 steps.
The RDDM RX doorbell is the deliberate exception: it arms a distinct mapped,
zeroed sink, which is retained until proven containment and never dumped.

An unchanged result is PBL EE 0 / ERRCODE `0xef0e0003`. A useful improvement is
BHI success plus SBL EE 1 and loss of that error, rather than just a zero error
immediately after DB (already observed in M6k). A reproducible changed error is
also evidence, but must be reported raw. RDDM EE 3/RX completion would improve
crash visibility, not establish a boot fix. MMIO faults may shorten observation;
transport-inconclusive outcomes cannot establish a PBL content result.

Invalid/combined mode strings stop at step 17, code `0x116c`, before allocation
or experimental writes. M6l variants cannot combine with `unmapped` or `pattern`;
`civac` and `nc` remain available. Precondition failure stops at step 18,
`0x126c`. RX/TX setup readback failure stops at step 21, `0x150c`. No failure
permits a BHI retry; unproven containment quarantines every buffer until reboot.

## Host verification

Run all four suites selected by `kernel-wlan6g`:

```sh
python3 tools/test-fuxi-wlan-m6.py
python3 tools/test-fuxi-wlan-m6i.py
python3 tools/test-fuxi-wlan-m6k.py
python3 tools/test-fuxi-wlan-m6l.py
```

M6l executes the actual loader C under ASan/UBSan with synthetic MMIO/ELF/VFS
and scattered physical pages. It checks absent/off/malformed selections,
chip/transport rejection, exact reset order and corrected WCN7850 address,
window restoration on failed writes and faults, bounded reset polling/release,
both IOVA pools, whole-file length and partial-tail translation, TX staging
without a doorbell, complete RDDM sink mappings/zeroing, RX programming before
BHI, allocation/setup failures and buffer quarantine/free behavior. Its ELF
inspector tests cover both classes, truncation, file bounds and prefix overflow.
The source/patch new-file hunk must remain byte-identical. These tests establish
host sequence and containment, not real ROM behavior. No kernel build or device
validation is claimed for M6l.

## Primary references

Behavior was read and independently implemented; no driver body is vendored.

- L66: [ath12k PCI](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/pci.c), [PCI definitions](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/pci.h), [MHI configuration](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/mhi.c), [MHI boot](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/boot.c), [MHI power](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/pm.c).
- L18: [ath12k PCI](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/pci.c), [PCI definitions](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/pci.h), [WCN7850 hw registers/params](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/hw.c), [MHI configuration](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath12k/mhi.c).
- L18 MHI: [prepare / RDDM](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/init.c), [BHI / BHIe / fw_load_type](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/boot.c), [power / READY / SBL](https://github.com/torvalds/linux/blob/v6.18/drivers/bus/mhi/host/pm.c).
- LM: [newer ath12k PCI at a804c5742bff224d4cb4d25dd383554d598be32b](https://github.com/torvalds/linux/blob/a804c5742bff224d4cb4d25dd383554d598be32b/drivers/net/wireless/ath/ath12k/pci.c), [MHI wrapper at that revision](https://github.com/torvalds/linux/blob/a804c5742bff224d4cb4d25dd383554d598be32b/drivers/net/wireless/ath/ath12k/mhi.c).
- C: [Xiaomi SM8550 CNSS PCI at 92a9fde479d3262da080cfd28bab0775b0da500a](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/pci.c), [register definitions](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/reg.h), [DT gate and SOL setup](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/main.c), [KIWI DT at 43b5ada5654ed84c25f74789bc8f6326b7bacd20](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/43b5ada5654ed84c25f74789bc8f6326b7bacd20/qcom/wlan/kalama-cnss.dtsi).
- V: [SM8550 vendor MHI boot at c462ef8ffab7a58e035ee04705b16cdfced494b1](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/boot.c), [prepare](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/init.c), [power](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/c462ef8ffab7a58e035ee04705b16cdfced494b1/drivers/bus/mhi/host/pm.c).
- Related, not a WCN7850 recipe: [ath11k L1SS helper](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/ath/ath11k/pci.c), [Qualcomm ELF hash-kind convention](https://github.com/torvalds/linux/blob/v6.18/drivers/soc/qcom/mdt_loader.c).
