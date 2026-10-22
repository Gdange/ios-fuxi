# WLAN M6n: Xiaomi CNSS2 / MHI audit before BHI

Source-only analysis and one default-off experiment. The device observations
in the request are the input evidence; this change has no new device result.
The strongest new omission found is **AOP WLAN PDC initialization before power-on**.
No public symbolic decoder was found for the three supplied PBL words. The
Xiaomi source supplies neither a pre-BHI image hyp-assignment nor an ELF
program-header-derived SBL length.

## Sources and reproducibility

All four public repositories were shallow-cloned into temporary storage,
outside this repository, and inspected locally. Nothing was vendored.

| ID | Tree / branch | Inspected commit / scope |
| --- | --- | --- |
| K | MiCode/Xiaomi_Kernel_OpenSource, `fuxi-t-oss` | `7fad87dc178d09489b74a714e4a525d56f546855`; downstream 5.15, **`drivers/bus/mhi/core`**, `drivers/pci/controller/pci-msm.c`, fastmap DMA, AOSS/IPCC |
| C | MiCode/vendor_qcom_opensource_wlan, `fuxi-t-oss` | `518147fd7e0199758efe9bd96a22c9c2ea30bcd9`; `platform/cnss2` |
| X | MiCode/kernel_devicetree, `fuxi-t-oss` | `5647c799a064dfec73332a4fe2ef7aa3fb2f4887`; Kalama PCIe/AOSS and Fuxi overlays |
| D | LineageOS/android_kernel_xiaomi_sm8550-devicetrees | `13913424c9d0af031a8b8f9981c003bbb9632f73`; supplementary **`qcom/wlan/kalama-cnss.dtsi`**, absent from X's published file inventory |

D supplies the converged CNSS KIWI child, matching the already documented live
DT's ID `1107`, RC0, GPIO80/81/82/83, six supplies, 32-MiB reusable DMA pool,
fastmap IOVA pool and 0x780000 crash size. It is a public matching-platform
reference, not proof that every AOP setting equals the device's running DT.
The entire D initialization table is therefore an explicit experimental input.
Its exact data are listed below. X's PCIe0 node is independently available at
[X `qcom/kalama-pcie.dtsi:4`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama-pcie.dtsi#L4); D's KIWI child starts at
[D `qcom/wlan/kalama-cnss.dtsi:148`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/wlan/kalama-cnss.dtsi#L148). No inference that an omitted file is an
omitted hardware feature is made.

For reproduction, clone the four named repositories into a disposable directory,
check out the listed commits, then restrict checkout to those source directories.
C is out of tree; searching K only under `drivers/net/wireless/cnss2` or MHI
only under `host/` would miss the actual implementation. Source tokens below
link to immutable file:line locations. Ours means
[xnu/fuxi-wlan/fuxi_wlan.c](../xnu/fuxi-wlan/fuxi_wlan.c), its included
[shim/fuxi-probe/wlan.c](../shim/fuxi-probe/wlan.c), and matching patch 0099.

The audit follows successful ordinary KIWI cold power-up, initial enumeration,
CNSS's probe-time power-off, then its actual MHI power-up through BHI completion.
Conditional/recovery writes are identified separately; they are not silently
included in the cold path. Generic Linux PMIC/clock/IOMMU frameworks and AOP/TZ
firmware have further internal behavior: the tables identify their API calls
and board parameters, not an unavailable firmware instruction trace.

## Ranked remaining differences

The eliminated IOVA/attributes, ICC, fabric clocks, cache, ASPM/L1SS, full-file
length, ath12k reset/QFPROM/wake/warm controls and image-choice experiments are
**not proposed again**.

| Rank | Candidate | Source evidence / relevance to a post-header rejection |
| --- | --- | --- |
| 1 | **AOP autonomous WLAN PDC policy missing.** | C probe initializes AOP **before** obtaining/enabling supplies, then sends the DT table: [C `main.c:4126`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/main.c#L4126), [C `power.c:1204`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L1204), [C `power.c:1265`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L1265), [D `qcom/wlan/kalama-cnss.dtsi:243`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/wlan/kalama-cnss.dtsi#L243). Our AP RPMh ACTIVE rail votes do not configure these sequences. A later PBL internal-power transition could expose a different BB/RF policy despite working PCIe and initial DMA. That mechanism is an inference; neither ROM stage nor crypto-rail dependency is decoded. Relative best new candidate, with uncertain absolute likelihood. Implemented as `fuxiwlancnss=pdc`. |
| 2 | **Other downstream root-controller initialization differs.** | Complete register differences below, especially PARF SYS_CTRL/address-space configuration and DBI transaction controls: [K `drivers/pci/controller/pci-msm.c:4695`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4695), [K `drivers/pci/controller/pci-msm.c:4559`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4559), [K `drivers/pci/controller/pci-msm.c:3744`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3744). A controller issue could affect later/larger reads, but no source connects these fields to `ef0e`, and the known DMA/content results reduce the likelihood. No new clock/routing or broad register-recipe variant added. |
| 3 | **Interrupt/completion setup and BME timing.** | CNSS enables PCI MSI (32 vectors, 3 MHI), restores D0/configuration and sets MASTER before MHI prepare: [C `pci.c:4387`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L4387), [C `pci_qcom.c:7`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci_qcom.c#L7), [C `pci.c:1216`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1216). Ours polls, disables MSI/MSI-X/INTx and defers EP BME until all mappings and BHI setup are verified. Missing IRQ delivery normally explains a host timeout, not a PBL content-dependent raw rejection. Enabling unhandled MSI or exposing early DMA would require a real interrupt/sink implementation; no speculative enable variant added. |
| 4 | **Staging-buffer lifetime and pre-BHI metadata differ.** | Separate coherent prefix buffer is freed after BHI, full-image FBC allocation happens only afterward: [K `drivers/bus/mhi/core/boot.c:476`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L476). Ours holds the whole image and prepares host contexts/TX vectors first; default does not prearm RDDM. These differences are real but do not change the advertised prefix bytes/address/size. RX exists for diagnostics; old M6l already exposes it. Fastmap proves physical contiguity is not a BHI prerequisite in this source. No repeat experiment added. |

No source-backed host secure-boot unlock, anti-rollback initialization or
signing-policy write was found to rank above these. Image authentication or
provisioned ROM policy remains **unresolved**, without a public decode or a
new image-choice recommendation. This audit cannot identify a required fix
from the three raw words alone.

## Ordered power / PCIe / MHI delta

Rows enumerate software-visible writes/calls that differ, including omissions
in either direction. Equal operations are included where needed to establish
ordering. Addresses marked EP are endpoint-internal addresses, not host PAs.

| Order | Downstream call / writes / delay | Ours / delta |
| --- | --- | --- |
| 1. Select child | Read WLAN_SW_CTRL GPIO83; converged DT chooses KIWI for low, QCA6490 for high: [C `main.c:3980`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/main.c#L3980). | Fixed Fuxi KIWI recipe; reads SW_CTRL82/WLAN_SW_CTRL83 in step 3, subsequently verifies PCI ID. No bootstrap override written. |
| 2. Platform setup | `cnss_power_misc_params_init`, AOP mailbox setup and DT `pdc_init_table` sends before `cnss_get_resources`: [C `main.c:4126`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/main.c#L4126). | **Default has no QMP/AOP packet.** New PDC variant does this before step 1/rail votes. Does not substitute for AP RPMh rail votes. |
| 3. Secure policy query | If `CONFIG_CNSS_HW_SECURE_DISABLE`, open smcinvoke object `0x108`, invoke GET_STATE op 1 with Wi-Fi UID `0x508`, release objects; disabled state prevents WLAN power/enumeration: [C `main.c:36`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/main.c#L36), [C `main.c:3731`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/main.c#L3731), [C `main.c:4188`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/main.c#L4188). Query is also called on power-on/off. | No smcinvoke object service. This is a **query**, not a SET_STATE/unlock, PAS auth or image-memory grant. Config presence in source does not establish the shipped binary enables it. No speculative SMC implemented. |
| 4. WLAN supplies | `cnss_power_on_device`: hardware-disable check → primary vregs → RF clock → pinctrl: [C `power.c:1023`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L1023). Supported supplies follow the list in [C `power.c:26`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L26), filtered by DT. Actual KIWI enable order is **L15B IO, S5G WLAN, S2G AON, S4E DIG, S6G RFA1, S4G RFA2**. For each, set DT min/max voltage, optional load, optional pre-enable delay, then enable: [C `power.c:185`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L185), [C `power.c:313`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L313). KIWI requests 1800/1000/980/950/1900/1350 mV, zero load and zero delay: [D `qcom/wlan/kalama-cnss.dtsi:164`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/wlan/kalama-cnss.dtsi#L164). | Step 2 sends voltage then enable via RPMh in pwrseq order **S5G, L15B, S2G, S4E, S4G, S6G**, 1 ms after each; DIG vote is 952 mV. Different order and nominal request, not newly proposed as the blocker after established power success. Linux regulator quantization is separate from requested microvolts. |
| 5. RF clock/GPIO | `rf_clk` if supplied, optional bootstrap active +1 ms, optional SOL default, optional XO hold, WLAN_EN high +1 ms, release XO: [C `power.c:884`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L884). KIWI DT only supplies WLAN_EN active/sleep; GPIO80 gpio/pull-up/16 mA/output-high: [D `qcom/wlan/kalama-cnss.dtsi:3`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/wlan/kalama-cnss.dtsi#L3). No KIWI 100-ms BT_EN-off delay: that branch is **QCA6490 only**, [C `power.c:974`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L974). | Same GPIO80 electrical settings; step 3 waits **50 ms**. No bootstrap/SOL/XO supplied in this KIWI child. GPIO81 BT_EN untouched on ordinary success in both. BT low is link-retry fallback only: [C `pci.c:2640`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L2640). |
| 6. Host power | Assert PERST94, wait 1000–1005 us, enable PCI supplies in list order (1p2, 0p9, CX, MX if present), DT voltage/load then enable: [K `drivers/pci/controller/pci-msm.c:4672`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4672), [K `drivers/pci/controller/pci-msm.c:3244`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3244). X supplies PHY 1200 mV/18.2 mA, 880 mV/80.9 mA and CX/MX corner bounds: [X `qcom/kalama-pcie.dtsi:45`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama-pcie.dtsi#L45). | Steps 4–5 enable 880 then 1200 mV, 1 ms wait, assert PERST94 and configure CLKREQ95/WAKE96 then 1 ms. No explicit PHY load or CX/MX corner API. Existing clocks/ICC controls stay unchanged; not suggested again. |
| 7. Domains/clocks/reset | Enable core and PHY GDSCs, set pipe parent, ICC path, every nonzero clock rate, enable clocks; assert/deassert each core reset with 1–1.005 ms assertion: [K `drivers/pci/controller/pci-msm.c:3362`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3362). Pipe setup separately sets rate/enables and handles pipe resets: [K `drivers/pci/controller/pci-msm.c:3527`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3527). X clock/reset lists: [X `qcom/kalama-pcie.dtsi:79`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama-pcie.dtsi#L79). | Steps 6–9 directly enable domains/branches, pulse core BCR 1 ms, PHY BCR 250 us +10 us, select pipe after PHY table. No clock-framework/refcount/regulator framework. Existing optional M6h fabric and default M6i ICC cover the previously investigated votes. Raw GCC offsets/table are already enumerated in shim source and lane §7. |
| 8. PARF/TCSR | Controller writes before PHY are individually listed in the next table: [K `drivers/pci/controller/pci-msm.c:4695`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4695). Optional `pcie_tcsr_init` iterates `qcom,tcsr` offset/value pairs: [K `drivers/pci/controller/pci-msm.c:1428`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L1428), [K `drivers/pci/controller/pci-msm.c:4743`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4743). | Step 8 uses the mainline-derived PARF recipe instead. X PCIe0 has **no TCSR resource or `qcom,tcsr` table**, so no hidden platform TCSR recipe follows this branch. CLKREF enable is a separate TCSR clock-provider write already done at step 7. |
| 9. PHY | Iterate every DT `{offset,value,delay}` tuple, 995–1005 us stabilization, enable pipe clock, `wmb`, poll status up to 10 retries at 1000–1005 us: [K `drivers/pci/controller/pci-msm.c:3634`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3634). X table starts [X `qcom/kalama-pcie.dtsi:124`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama-pcie.dtsi#L124). | Same substantive SM8550 Gen3x2 PHY values encoded in shim; ours adds PCS_MISC RXEQEVAL_TIME offset `0x6f4 = 0x27`. PHY software-reset/start and timing are applied explicitly by our step 9; downstream supplies those as DT tuples. No new PHY experiment. |
| 10. PERST/link | Optional aux-parent switch; EP latency **10 ms** from X, optional EP GPIO, optional target width; PARF/DBI dynamic-width bits; PERST deassert then default 1000–1005 us; configure training controls and start LTSSM; poll at 5–5.1 ms intervals: [K `drivers/pci/controller/pci-msm.c:4752`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4752), [K `drivers/pci/controller/pci-msm.c:4553`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4553). | Step 10 waits **100 ms** before PERST deassert, then 1 ms; step 11 trains with 1-ms polls/1-s bound. No EP GPIO in X. Gen2 x2 already succeeds. Gen3 preset/equalization fields below are another source difference, not a new reset recommendation. |
| 11. Host routing/config | Install RID/SID table, CFG0 iATU, N_FTS/AUX, enumerate PCI and set RC MEMORY|MASTER: [K `drivers/pci/controller/pci-msm.c:4814`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4814), [K `drivers/pci/controller/pci-msm.c:4966`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4966), [K `drivers/pci/controller/pci-msm.c:5093`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L5093). | Steps 12–17 install only owned SID entries, CFG0, MEM outbound iATU, RC bridge window and BAR0; verify inherited table entries rather than zeroing 256 entries. Ours sets bypass clear **last**. SID/CB route already established; no repeat routing change. |
| 12. PCI probe/config | Attach reusable reserved memory, initialize SMMU, register PCI events, enable bus, allocate MSI, register MHI: [C `pci.c:5886`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5886). 36-bit streaming/coherent masks and BAR resource: [C `pci.c:4547`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L4547). | Ours allocates wired pages and owns one CB with 36-bit physical-address checks. BAR0 assigned manually. No MSI allocator/ITS device/table; bus master delayed until step 21. Reserved-memory attachment is not a hyp assignment. |
| 13. Version/name | MHI reads BAR `SOC_HW_VERSION_OFFS=0x224`: [K `drivers/bus/mhi/core/misc.c:1909`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/misc.c#L1909); C masks major to four bits and selects `kiwi/amss20.bin` for major 2 mission mode: [C `pci.c:5553`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5553), [C `pci.c:5236`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5236). FTM can select `amss20_ftm.bin`, with ordinary image fallback. | Step 18 reads EP TCSR hardware version through BAR window (`0x310c=0x40000036`, then restores old selector). Same major-2 ordinary basename. Stock selector remains an existing explicit control, not recommended anew. This TCSR access is a **read** reached via a window-control write. |
| 14. Probe-time off | CNSS probe does gated QFPROM clear, then gated shutdown-enable, 1–2 ms; suspend saves config, clears BME, disables PCI function, D3hot, link-down and WLAN power-off: [C `pci.c:5952`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5952), [C `pci.c:1177`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1177), [C `pci.c:5844`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5844). | Ours proceeds directly from initial enumeration to BHI; no probe-only off/on cycle or saved PCI state. QFPROM gate absent in D KIWI child, so those writes are no-ops for that DT. This is distinct chronology, not evidence to rerun eliminated reset controls. |
| 15. Actual power-up | `cnss_qca6290_powerup` also handles KIWI: supplies/pins on → resume link → D0 → enable PCI device → restore config → MASTER → gated WLAON clear → MHI start: [C `pci.c:2600`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L2600), [C `pci.c:1216`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1216), [C `pci.c:2815`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L2815). | No second power-up, no explicit PMCSR D0 write. Our diagnostics record D0 versus non-D0; a non-D0 endpoint is transport-inconclusive, not evidence of authentication rejection. |
| 16. WLAON | `qcom,set-wlaon-pwr-ctrl` gates RMW of EP `WLAON_QFPROM_PWR_CTRL_REG`, clearing VDD4BLOW enable and shutdown; pre-MHI call passes **no force-wake**, [C `pci.c:1931`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1931), [C `pci.c:2668`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L2668). | Default omits it. Existing M6l `pre=qfprom` already performs the corresponding clear and was eliminated. No fuse data is programmed; DT gate is absent in D's KIWI child. |
| 17. MHI configuration | CNSS FBC=true, `sbl_size=seg_len=0x80000`, `rddm_size=0x780000` from DT, IOVA boundaries, callbacks, satellite config for KIWIv2: [C `pci.c:5580`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5580). INIT=`mhi_prepare_for_power_up`, POWER_ON=`mhi_sync_power_up`: [C `pci.c:1757`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1757). | Same prefix and later vector segment length. No satellite/DRV runtime support; those are not invoked to authenticate the first prefix. |
| 18. Prepare/RDDM | Allocate host contexts; read BHIOFF/BHIEOFF; zero **all RX words +0x60 through +0x78**, allocate RX, program RX address high→low, vector-table byte size and random nonzero 30-bit RX DB: [K `drivers/bus/mhi/core/init.c:1099`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/init.c#L1099), [K `drivers/bus/mhi/core/boot.c:22`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L22). | Step 20 prepares contexts/TX vectors before BHI. Step 21 default clears TX DB/status (+0x3c/+0x44) and RX DB/status (+0x70/+0x78); no default RX address/length zero or RX allocation. M6l RDDM mode implements full RX preparation. Purpose is crash capture; no dependency on PBL validation is stated. |
| 19. MHI POR | Write BHI INTVEC=0, read EE/state; only SYS_ERR triggers MHICTRL.RESET and 25-ms reset polling, then INTVEC=0 again; install MHI IRQ handlers, queue PBL worker: [K `drivers/bus/mhi/core/pm.c:1122`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/pm.c#L1122). | Require PBL/RESET/no reset pending and keep interrupts disabled. Write INTVEC=0 during step 21 instead of early POR. No unconditional RESET in successful downstream PBL cold path. |
| 20. Image/BHI | BHI metadata read, file request, coherent prefix allocation/copy and transfer, detailed below: [K `drivers/bus/mhi/core/boot.c:414`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L414). | Same bytes and tuple; extra CPU ELF/bounds checks, translation walk, cache maintenance and readbacks before BME/DB. |
| 21. Completion | `wait_event_timeout` tests BHI_STATUS [31:30], prints raw error registers on error; C temporarily expands 10-s default MHI timeout by 3 (perf) or 6 (non-perf), boot-debug timer while waiting: [K `drivers/bus/mhi/core/boot.c:281`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L281), [C `pci.c:1841`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1841). | Millisecond polling; diagnostic modes observe five seconds including after a raw PBL error. No IRQ requirement for polling STATUS itself. Timing is not a new candidate once ROM has already supplied the stable rejection. |
| 22. After BHI | Free prefix buffer, allocate/copy full FBC vectors while READY develops, poll reset-clear/READY every 25 ms, initialize MHI MMIO, wait for SBL/mission, then BHIe AMSS TX: [K `drivers/bus/mhi/core/boot.c:491`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L491), [K `drivers/bus/mhi/core/pm.c:194`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/pm.c#L194), [K `drivers/bus/mhi/core/boot.c:551`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L551). | Whole image retained; vectors/context already prepared. Steps 23+ do READY/M0/TX. Every M6n variant deliberately stops/contains at step 22 even if BHI succeeds. No later TX DB can repair the earlier rejection. |

### Individual host-controller register writes

These are the direct writes in K's selected `msm_pcie_enable` / `link_train` /
`config_controller` chain, excluding equal rail/clock-provider facts already
listed above. `set/clear` denotes preserving the other bits. Constants and
source locations are facts; private bit meanings are not inferred.

| Downstream write and source | Our difference |
| --- | --- |
| PARF DEVICE_TYPE `+0x1000=4`; PHY_CTRL `+0x40` clear bit0; PM_CTRL `+0x20` clear bit5 when L1 supported; MHI_CLOCK_RESET_CTRL `+0x174` set bit4; AXI_MSTR_WR_ADDR_HALT `+0x1a8` set bit31, retain default size unless DT overrides; [K `drivers/pci/controller/pci-msm.c:4695`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4695) | Same bit operations in step 8; ours unconditionally clears PM_CTRL bit5. X supplies no wr-halt override. |
| PARF DBI_BASE_ADDR `+0x350=0`; SYS_CTRL `+0=0x365e`; SLV_ADDR_SPACE_SIZE `+0x358=0x04000000`; [K `drivers/pci/controller/pci-msm.c:4705`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4705) | Ours sets `+350=0x60000000`, `+354=0`, `+634=0x60001000`, `+638=0`; `+358=0`, `+35c=0x80000000`; SYS_CTRL only clears bit29. This is the mainline-derived address-decode recipe, not byte-identical downstream programming. It nevertheless yields working BAR/config and PBL DMA. |
| PARF INT_ALL_MASK `+0x22c=0`, then OR link-down/L1SUB-timeout/AER/bridge-flush/MSI0..7 bits; [K `drivers/pci/controller/pci-msm.c:4713`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4713) | Not programmed/enabled by ours. No corresponding RC IRQ handler. |
| PARF CFG_BITS_3 `+0x2c4` clear bit0 (Qactive veto override), set bit8; DBI LANE_SKEW `+0x714` set bit5; [K `drivers/pci/controller/pci-msm.c:4775`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4775) | Omitted; dynamic width support, no width-scaling runtime path in ours. |
| DBI GEN3_GEN2_CTRL `+0x80c` bits[12:8]=1; for each supported Gen≥3, GEN3_RELATED `+0x890` rate-shadow select[25:24], EQ_CONTROL `+0x8a8` preset request vector[23:8], clear RELATED bit0, EQ_FB_MODE_DIR_CHANGE `+0x8ac` [4:0]; [K `drivers/pci/controller/pci-msm.c:4559`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4559), [K `drivers/pci/controller/pci-msm.c:3715`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3715) | Omitted. Generation/support dependent even when negotiated link later falls to Gen2. Defaults for DT-absent vector/FMDC are zero. |
| DBI `+0x8bc` RO-write enable bit0; Gen3 `+0x154=core_preset`, Gen4 `+0x188=core_preset`; disable RO write bit0; [K `drivers/pci/controller/pci-msm.c:3689`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3689) | Omitted. Core preset default `0x55555555`. Gen4 offset from `PCIE_PL_16GT_CAP` + LE_CTRL, not an EP security register. |
| DBI LNKCTL2 TLS if target speed nonzero; DEVCTL `+0x78` MRRS bits[14:12]=5 (4096 B); PARF LTSSM `+0x1b0` set bit8; [K `drivers/pci/controller/pci-msm.c:4574`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4574) | Ours sets RC TLS=2 at step 11 and LTSSM bit8; no MRRS setting. The downstream write here is the **root port's** requester setting, not automatically the endpoint's read request size. Subsequent negotiated-speed TLS/resource adjustment is at [K `drivers/pci/controller/pci-msm.c:4628`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4628). |
| RID table: clear PARF `+0x2c00` bypass bit0, zero 256 dwords `+0x2000`, install CRC8 RID/SID/next entries (and collision links); [K `drivers/pci/controller/pci-msm.c:4966`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4966) | Ours verifies and claims minimal slots and clears bypass last, retains unrelated entries. No new route experiment. |
| CFG0 iATU region0: CTRL2=0 → CTRL1=type → lower/upper base → limit → lower BDF target → upper=0 → CTRL2 bit31; 4-KiB config aperture; [K `drivers/pci/controller/pci-msm.c:2978`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L2978) | Ours also establishes CFG0, with a larger aperture, then a MEM outbound region1 and RC memory bridge window. Same endpoint BDF. iATU is CPU outbound; no BHI-image inbound iATU window is needed for PCI DMA. |
| DBI ACK_F_ASPM_CTRL `+0x70c` bit15 when no N_FTS override, else replace[15:8]; AUX_CLK_FREQ `+0xb40=20`; DEVCTL2 `+0x98` completion timeout[3:0] only if configured **and bridge_found**; [K `drivers/pci/controller/pci-msm.c:3744`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3744) | Ours omits N_FTS/AUX register programming and optional timeout. X PCIe0 has AUX=20, no N_FTS/cpl-timeout override. No switch is present in this link. |
| Optional AER: bridge-control bits16/17; RC DEVCTL bits[3:0]; per-device AER reporting DEVCTL; [K `drivers/pci/controller/pci-msm.c:3784`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L3784), [K `drivers/pci/controller/pci-msm.c:4953`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L4953) | Ours reads AER diagnostics, does not enable AER IRQs/reporting. |
| RC COMMAND MEMORY|MASTER after enumeration: [K `drivers/pci/controller/pci-msm.c:5101`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L5101) | Ours step14 uses IO|MEMORY|MASTER|SERR and explicit bus/window registers. |
| Link PM: DT-gated common-clock LNKCTL CCC and CLKREQ enable if capable, L1SS L1.2 threshold scale/value=2/150; DEVCTL2 LTR_EN, capability-supported L1SS_CTL1 low enables; LNKCTL L1 then L0s if supported; PARF SYS_CTRL bit3 cleared for aux sync if needed: [K `drivers/pci/controller/pci-msm.c:6214`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L6214), [K `drivers/pci/controller/pci-msm.c:6382`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L6382), [K `drivers/pci/controller/pci-msm.c:6470`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L6470), [K `drivers/pci/controller/pci-msm.c:6522`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/pci/controller/pci-msm.c#L6522) | Ours retains CCC/CLKREQ/threshold/LTR fields, clears ordinary ASPM for download; M6m can clear L1SS enables. These already investigated power states are inventory, not new recommendations. X disables L0s support and lacks the common-clock/clock-power-management enable flags, so those two optional writes are not selected by X alone. |
| PCI core BAR0/upper BAR assignment, COMMAND MEMORY, PMCSR D0; saved-state restoration of BARs/command/PCIe/MSI control; PCI MSI addr/data/MME/enable for 32 vectors; [C `pci.c:4547`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L4547), [C `pci.c:1216`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1216), [C `pci.c:4387`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L4387) | Ours sets BAR0/upper manually, disables MSI/MSI-X and legacy INTx, EP BME last. MSI address/data values are allocated by IRQ domains; no universal fixed value exists in CNSS source. X uses ITS SID mapping `0x1400/0x1401`, not a fixed RAM sink. |

K's host `pcie_tcsr_init` is not an endpoint PBL-logging writer. The TCSR clock provider sets CLKREF bit0 at
TCSRCC +0x15100 ([K `drivers/clk/qcom/tcsrcc-kalama.c:28`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/clk/qcom/tcsrcc-kalama.c#L28)); ours also sets it. The
endpoint window selector is at BAR `+0x310c`; C writes `0x40000000 |
((internal_address >> 19) & 63)` and reads it back for each windowed access,
[C `pci.c:804`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L804). Ours additionally restores its previous value. No C cold-path
write to `TCSR_PBL_LOGGING`, `PBL_WLAN_BOOT_CFG`, fuse anti-rollback counters,
OEM key hashes or bootstrap straps was found.

The lower SMMU implementation also contains secure helpers, which must not be
confused with WLAN-image authorization. Kalama APPS SMMU is `qcom,qsmmu-v500`
([X `qcom/msm-arm-smmu-kalama.dtsi:69`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/msm-arm-smmu-kalama.dtsi#L69)).
Its reset calls the wait-for-safe SCM toggle only on the SDM845-specific branch
([K `drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c:384`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c#L384));
SCM IO reads in its TLB-timeout diagnostics read TBU status, not image policy
([K `drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c:1243`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c#L1243)).
The separate legacy `qcom_iommu.c` secure-context restore/page-table init calls
are another implementation. D's CNSS fastmap group supplies no secure VMID.
This audit finds no corresponding secure-image call to add to our owned CB.

## AOP packet and ordering detail

`cnss_aop_mbox_init` sends each DT entry synchronously, in order, using a
64-byte packet / 1000-ms mailbox timeout; [C `power.c:82`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L82), [C `power.c:1204`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L1204),
[C `power.c:1246`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L1246). All entries use `class: wlan_pdc`; table cells supply the
`ss`, `res`, and single parameter/value. This is the complete KIWI table,
[D `qcom/wlan/kalama-cnss.dtsi:243`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/wlan/kalama-cnss.dtsi#L243):

| Packet | ss | res | parameter | value |
| --- | --- | --- | --- | --- |
| 1 | rf | s4e.v | upval | 916 |
| 2 | rf | s4e.v | dwnval | 612 |
| 3 | rf | s4g.v | upval | 1316 |
| 4 | rf | s4g.v | dwnval | 944 |
| 5 | rf | s6g.v | upval | 1864 |
| 6 | rf | s6g.v | dwnval | 1820 |
| 7 | rf | s2g.m | enable | 0 |
| 8 | rf | s2g.v | enable | 0 |
| 9 | bb | pdc | enable | 1 |
| 10 | bb | s2g.v | upval | 976 |
| 11 | bb | s2g.v | dwnval | 800 |
| 12 | bb | s5g.v | upval | 940 |
| 13 | bb | s5g.v | dwnval | 420 |

This is not the QCA6490 child table (different voltages, RF MX enabled,
BB PDC disabled). Copying the adjacent child would invert the intended policy.
Later QMI capability messages can revise open-loop CPR values; those are
**after firmware is running**, [C `qmi.c:595`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/qmi.c#L595), [C `power.c:1334`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/power.c#L1334).

The selected mailbox is AOSS at host `0x0c300000`, 0x400 bytes; IPCC MPROC
at `0x00408000`; AOP client=0, GLINK_QMP signal=0, SEND_ID offset `0xc`:
[X `qcom/kalama.dtsi:2366`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama.dtsi#L2366), [X `qcom/kalama.dtsi:2680`](https://github.com/MiCode/kernel_devicetree/blob/5647c799a064dfec73332a4fe2ef7aa3fb2f4887/qcom/kalama.dtsi#L2680),
[K `drivers/soc/qcom/qcom_ipcc.c:180`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/soc/qcom/qcom_ipcc.c#L180). The QMP open/send protocol is
[K `drivers/soc/qcom/qcom_aoss.c:136`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/soc/qcom/qcom_aoss.c#L136), [K `drivers/soc/qcom/qcom_aoss.c:241`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/soc/qcom/qcom_aoss.c#L241):

1. Validate magic `0x4d41494c` and version 1. Read descriptor MCORE mailbox
   size `+0x34`, offset `+0x38`. These are read-only protocol inputs.
2. Ack UCORE link state (`read +0xc`, write `+0x10`), write MCORE link UP
   `+0x24=0xffff`, kick IPCC SEND_ID=0, wait `+0x28=0xffff` up to 1 s.
3. Write MCORE channel UP `+0x2c=0xffff`, kick, wait UCORE channel `+0x14=0xffff`.
4. Write UCORE channel ACK `+0x18=0xffff`, kick, wait MCORE channel ACK
   `+0x30=0xffff`. Each stage has its own 1-s bound.
5. Write 16 little-endian payload dwords at mailbox+4, write length=64 at
   mailbox, read length to drain posted writes, kick, wait length=0 up to 1 s.
   No extra per-packet fixed delay is present; completion orders the next packet.

The variant independently implements that protocol with 1-ms yielding polls,
reuses an already UP link/channel instead of renegotiating it, checks bounds
and an empty slot before writing, and never clears an outstanding timed-out
packet. K's generic send clears a timed-out length; our experiment deliberately
leaves it pending and refuses any second attempt. Consumption is the protocol
acknowledgement, **not** an AOP response describing effective voltage or a
PBL fix. No IRQ or secure object is required for our polling implementation.
No other XNU driver currently sends QMP; the shim audio helper runs in a
separate boot environment. A future kernel QMP user must share serialization.

## BHI buffer contract, secure calls and error decoding

| Question | Exact source finding |
| --- | --- |
| Prefix vs ELF SBL segment | C selects FBC, SBL maximum 512 KiB. K uses `min(sbl_size, firmware_size)`, allocates a **separate** coherent buffer and copies from file offset **zero**. There is no ELF parser, PT_LOAD selection, segment concatenation, hash stripping or program-header-derived BHI size: [C `pci.c:5632`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5632), [K `drivers/bus/mhi/core/boot.c:476`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L476). Ours keeps that 512-KiB contract; files smaller than it are rejected locally instead of clamped. Both supplied real files exceed it. |
| Physical contiguity | Initial BHI uses `dma_alloc_coherent(..., GFP_KERNEL)` with **no FORCE_CONTIGUOUS**: [K `drivers/bus/mhi/core/boot.c:482`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L482). D selects `fastmap`; its blocking ordinary allocator calls `__fast_smmu_alloc_pages`, which allocates **individual pages**, then maps them to contiguous IOVA: [K `drivers/iommu/dma-mapping-fast.c:473`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/iommu/dma-mapping-fast.c#L473), [K `drivers/iommu/dma-mapping-fast.c:554`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/iommu/dma-mapping-fast.c#L554). Thus this exact downstream path disproves a blanket requirement for physical contiguity or below-4-GiB physical memory. Reusable pool attachment does not force every ordinary fastmap allocation through CMA. |
| CMA/RDDM/FBC confusion | `mhi_alloc_bhie_table` uses `DMA_ATTR_FORCE_CONTIGUOUS` **per vector allocation**, [K `drivers/bus/mhi/core/boot.c:350`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L350); fastmap has a separate CMA branch for that flag, [K `drivers/iommu/dma-mapping-fast.c:500`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/iommu/dma-mapping-fast.c#L500). That applies to RX crash vectors and later FBC chunks, not the initial BHI allocation. The 4-MiB alignment in D is the **reserved pool's** alignment, not IMGADDR/SBL alignment. |
| Alignment | Ordinary fastmap allocation size rounds to 4 KiB; BHI writes the original byte count unrounded. Ours starts at a 512-KiB-aligned IOVA and maps 16-KiB CPU pages as 4-KiB leaves. No downstream 512-KiB physical-base requirement was found. There is no allocation from an image VMID-specific carveout. |
| Register order | K `mhi_fw_load_bhi`: BHI_STATUS `+0x2c=0` → IMGADDR_HIGH `+0x0c` → LOW `+8` → IMGSIZE `+0x10` → random nonzero 30-bit IMGTXDB `+0x18`; [K `drivers/bus/mhi/core/boot.c:269`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L269). INTVEC `+0x20=0` was written in POR. Ours matches high→low/size/DB, adds readback, then EP BME just before DB. No missing size rounding or mandatory fixed sequence ID. |
| Firmware entry points | PBL chooses `fw_image`; **EDL EE only** chooses `edl_image`: [K `drivers/bus/mhi/core/boot.c:444`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L444). C does not populate an EDL image here. No `mhi_dev_fw_load` symbol/call occurs in these CNSS2/MHI directories; the actual path is `mhi_fw_load_handler` → `mhi_fw_load_bhi`. RDDM is a crash EE, not another secure SBL selector. |
| SCM/hyp/PAS | The successful path is ordinary firmware request → DMA allocation/copy → register writes. No `qcom_scm_assign_mem`, `hyp_assign_phys`, PAS init/auth/reset, VMID list or `wlan` image-buffer handoff surrounds the BHI allocation or transfer. `cnss_pci_of_reserved_mem_device_init`, [C `pci_qcom.c:539`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci_qcom.c#L539), calls Linux reserved-memory/CMA attachment, **not** TrustZone. CNSS's conditional smcinvoke GET_STATE is the separate read-only hardware-disable query described above. This is not the integrated-WCNSS remoteproc/PAS boot path. |
| Secure boot / anti-rollback | K reads BHI serial and sixteen OEM key-hash words as metadata before loading, [K `drivers/bus/mhi/core/boot.c:429`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L429). The header also names BHI_SBLANTIROLLVER at +0x44, NUMSEG at +0x48 and MSMHWID from +0x4c; the boot path does not read/write them ([K `drivers/bus/mhi/core/internal.h:155`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/internal.h#L155)). Serial/key metadata are not compared to the firmware or written to authorize it. Ours omits these identity/key reads. Source contains no pre-BHI security-policy initialization or error-code decoder, so adding an invented SCM grant or changing boot straps has no parity basis. |
| PCIE_SOC_GLOBAL_RESET | Not an unconditional KIWI pre-BHI write in C. Generic `mhi_soc_reset` writes MHI `+0xb0=1` only through reset/recovery users: [K `drivers/bus/mhi/core/main.c:168`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/main.c#L168); RDDM panic fallback at [K `drivers/bus/mhi/core/boot.c:147`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L147). ath12k endpoint hot/warm reset and old M6l controls are a different recipe already eliminated by the request. |
| Debug-only WLAON/TCSR | C's WCSS/WLAON/TCSR PMM status arrays are diagnostics; write entries are filtered by device mask and activated on dumps: [C `pci.c:2387`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L2387). `cnss_pci_config_regs` configures a WDOG discriminator for **QCA6390**, no KIWI write: [C `pci.c:5687`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L5687). This older C revision contains no QRTR-instance scratch-write helper; none is introduced here. |

After a successful power-up C sets `img_pre_alloc`, retaining FBC/RDDM
allocations across later power cycles; it does not skip or hyp-assign the
initial coherent BHI prefix on those cycles ([C `pci.c:1769`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1769),
[K `drivers/bus/mhi/core/boot.c:319`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/boot.c#L319)).
That later-allocation optimization cannot be a first-BHI prerequisite.

The named CNSS diagnostic addresses are fixed register facts:
[C `reg.h:92`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/reg.h#L92), [C `reg.h:102`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/reg.h#L102), [C `pci.c:1475`](https://github.com/MiCode/vendor_qcom_opensource_wlan/blob/518147fd7e0199758efe9bd96a22c9c2ea30bcd9/platform/cnss2/pci.c#L1475).

| Value/location | Decoded from public source | What remains inference/unknown |
| --- | --- | --- |
| `ERRCODE=0xef0e0003` | BHI ERRCODE at BHI+0x30; K logs it as raw `ERROR_CODE`. | No public enum maps `ef`, `0e`, or `0003` to a module/reason. **Not decoded as authentication, anti-rollback, hash, certificate or DMA failure.** |
| `TCSR_PBL_LOGGING=0xef0e0200` | EP `0x01b000f8`; C calls the value `pbl_stage` and prints it. | No field mask or stage enumeration. Similarity of the `ef0e` prefix is not a symbolic decode. |
| `0xef128100` | Another raw BHI ERRCODE observed for the unmapped and invalid-image controls. | The shared code cannot uniquely mean SMMU fault: the non-ELF control also produces it. Matching SID/FAR/FSR is the independent DMA-routing evidence. |
| BHI_STATUS `0xffffffff` | Field [31:30]=3 means transfer error in MHI, field=2 means success: [K `drivers/bus/mhi/core/internal.h:160`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/7fad87dc178d09489b74a714e4a525d56f546855/drivers/bus/mhi/core/internal.h#L160). | Whole-word all-ones can also be an inaccessible register. Other link/MMIO observations are required; decoding its field does not decode ERRCODE's low `3`. |
| KIWI PBL log | 40 bytes / ten dwords at EP `0x01403d98`, read/printed raw. Bootstrap status at EP `0x01a10008`; WLAN boot config at `0x01e22b34`. | No word semantics provided. Do not rename unknown words as hashes, stack traces or secure policy. |
| SBL log | C reads EP `0x01e0e238/23c` as start/size, caps at 48 bytes and checks SRAM `0x01400000..0x01800000`. | Not every PBL ERRDBG tuple is an SRAM log pointer. Ours bounds-checks before reading and can correctly skip an invalid SBL range. |

Exact-word searches for `0xef0e0003`, `0xef0e0200` and `0xef128100` found no
applicable public decoder; recursive searches of the cloned CNSS2 and MHI
sources likewise find only raw logging definitions. This is a search result,
not proof that private ROM documentation does not exist. The supplied real-vs-
pattern experiment supports a later content-dependent PBL path; “post-header
rejection” is the working interpretation of the observation, not a complete
ROM execution trace. No signing-policy bypass is inferred or implemented.

## Runner note and local verification

Keep existing image, pool, clock/ICC/cache and power-state controls constant.
Use a fresh boot for each comparison; these are WLAN argument sets for a future
runner, not commands executed by this task:

| Mode | WLAN arguments |
| --- | --- |
| Matched default-off control | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanfw=off fuxiwlanbhi=off fuxiwlanvec=off fuxiwlancnss=off` |
| Candidate 1: initialize KIWI PDC | `fuxiwlan=22 fuxiwlanclk=1 fuxiwlanprobe=civac fuxiwlanpre=off fuxiwlanfw=off fuxiwlanbhi=off fuxiwlanvec=off fuxiwlancnss=pdc` |

Absent/`off` means no added AOSS/IPCC mappings, reads or writes. Unknown values
are invalid at step 17; combinations with `probe=unmapped`/`pattern` are invalid.
A PDC attempt with any invalid selector stops before WLAN power writes. The
new mode is one-shot; it must consume all 13 messages before step 17 can claim
a CB. Errors block before step 1's normal rail/ICC operations; pending packets
and partial table state are retained for diagnosis, with no guessed rollback.
The two extra 16-KiB kernel mappings are used only in valid `pdc` mode; all
QMP accesses remain within the 0x400-byte descriptor/mailbox resource and IPCC
writes use only SEND_ID. No phone access or firmware staging was performed.

Expect `M6n PDC 1/13 ...` through `13/13`, then `M6n PDC consumed 13/13 before
WLAN rails`. Failure instead prints `M6n PDC blocked before WLAN power,
consumed N/13 reason ...` and stops. One-second protocol waits yield the WLAN
thread and never retry. Successful mailbox consumption alone does not establish
an effective voltage or firmware success.

After BHI, expect `M6n result cnss=pdc PDC consumed 13/13 EE ... ERRCODE ...`,
five seconds of existing diagnostics, then deliberate step-22 `0x166c` containment
(the matched CIVAC/off control uses `0x166b`). Even unexpected success stops;
no AMSS doorbell follows. Useful positive evidence is BHI completion **and SBL
EE 1**, readable transport and no matching translation fault. A changed raw
error is a discriminator, not a security decode. Existing BME-off/PERST
containment frees DMA allocations only when proven safe; failure quarantines
them. AOP policy persists until reconfigured; WLAN containment does not undo
PDC. A plain warm reboot need not reset AOP state: return through the normal
stock CNSS initialization before a genuinely matched off control if needed.

Local checks run: M6, M6i, M6k, M6l, M6m and M6n Python suites. They compile
production loader C with synthetic MMIO/VFS/DMA under ASan/UBSan. M6n covers
both QMP-open paths, exact packet order/padding, off/invalid selection, malformed
and busy descriptors, read/write faults, handshake/partial-packet timeouts, no retry,
BHI one-shot/step-22 stop, and containment/quarantine. M6i exercises the actual
`fw_run` early-policy failure gate. Patch 0099 is checked byte-for-byte against
the standalone source. The existing `kernel-wlan6g` prebuild list includes the
new suite; **no workflow or CI was run**.

A native kernel build requires the macOS/Xcode environment described in
[xnu/README.md](../xnu/README.md). The available Linux host checks compile the
actual changed C with mocked kernel primitives/privileged instructions; they
are not a full kernel link or hardware validation. Manual review checked
mapping gates, 32-bit MMIO bounds, write ordering, failure-before-power and
reuse of the existing DMA containment path. The optional packed-artifact audit
could not run because its ignored M6 outputs are absent; no new boot artifact was generated. No downloaded binary
or downstream source was added to the repository.
