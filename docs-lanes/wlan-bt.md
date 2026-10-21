# Lane: Wi-Fi and Bluetooth (WCN7850 "kiwi")

Status:
- Milestone 1 (read-only snapshot): built.
- Milestone 3 (gated power-up, PCIe0 link, endpoint ID and BAR sizes): built, not yet run on the device.
- Milestone 4 (XNU PCIe0/BAR0/MHI read): passed on the device.
- Milestone 5 (polled BHI/BHIe): implemented and built; device SMMU state unmeasured, low-DDR prerequisite documented in section 11.
- Wi-Fi is the lane priority; Bluetooth (M2) waits.

## 1. Topology

### Facts (with source)

| Item | Value | Source |
|---|---|---|
| Connectivity chip | Qualcomm WCN7850 ("kiwi", FastConnect 7800), PCI ID `17cb:1107` | live DT `/soc/qcom,cnss-qca-converged/chip_cfg@1` (`supported-ids 0x1107`, `wlan-rc-num 0`) |
| Alternate chip_cfg | QCA6490 (`0x1103`) on PCIe RC1 | same node, `chip_cfg@0`; RC1 (`qcom,pcie@1c08000`) is `status = disabled` in the live DT |
| Wi-Fi bus | PCIe0 RC (`qcom,pcie@1c00000`, `qcom,pci-msm`): parf 0x1c00000, phy 0x1c06000, dm_core 0x60000000, elbi 0x60000f20, iatu 0x60001000, conf 0x60100000 | live DT |
| PCIe0 GPIOs | PERST# gpio94 (out), CLKREQ# gpio95 (func 1, pull-up), WAKE# gpio96 (pull-up) | live DT pinctrl `pcie0_*_default` |
| PCIe0 PHY rails | vreg-0p9 = PM_V6E L1 (ldoe1, 880 mV), vreg-1p2 = PM_V6E L3 (ldoe3, 1200 mV) | live DT |
| PCIe0 power domains | `gcc_pcie_0_gdsc` 0x16b004 and `gcc_pcie_0_phy_gdsc` 0x16c000, collapse vote 0x152020 bits 0 / 3; parents CX / MX level | live DT `qcom,gdsc@16b004/@16c000` |
| PCIe0 PHY init | `qcom,phy-sequence` (offset, value, delay) triples, starts `0x240=3`, ends `0x200=0, 0x244=3`; ready when `0x214` bit 6 clear | live DT |
| Bluetooth bus | QUP2 SE6 GENI HS-UART at 0x898000 (`qcom,msm-geni-serial-hs`, alias `hsuart0`, ttyHS0 in FIFO mode), wrapper 0x8c0000, clock `GCC_QUPV3_WRAP2_S6_CLK` | live DT `qcom,qup_uart@898000`; OrangeFox dmesg |
| BT UART pins | gpio76 CTS, 77 RFR, 78 TX, 79 RX, function 1 (`qup2_se6`), 2 mA | live DT + mainline `pinctrl-sm8550.c` |
| Chip control GPIOs | WLAN_EN gpio80, BT_EN gpio81, SW_CTRL gpio82, WLAN_SW_CTRL gpio83 | live DT cnss and `bt-qca-converged/bt_kiwi` |
| WCN rails (cnss) | vdd s5g 1.0 V, vdd-wlan-io l15b 1.8 V, aon s2g 0.98 V, dig s4e 0.95 V, rfa1 s6g 1.9 V, rfa2 s4g 1.35 V | live DT `chip_cfg@1` supplies |
| cmd-db VRM addresses | ldob15 0x40a00, smpe4 0x40200, smpg2 0x40400, smpg4 0x40500, smpg5 0x44000, smpg6 0x40600, ldoe1 0x43900, ldoe3 0x43a00 | OrangeFox cmd-db dump (verified again at runtime by the probe) |
| WLAN memory | `cnss_wlan_region` 32 MiB reusable, iommu IOVA pool 0xa0000000+0x10000000, SIDs 0x1400/0x1401 | live DT |
| Same topology upstream | sm8550-qrd: WCN7850 on PCIe0, BT on uart14 (= QUP2 SE6) | mainline `sm8550-qrd.dts` |

### Guesses

- The fitted chip is WCN7850, not QCA6490 (PCIe1 is disabled and the QRD board is the same). The milestone-2 config read is meant to confirm the ID `0x110717cb`.
- The bootloader most likely leaves PCIe0 and SE6 unclocked. The stage-1 snapshot answers this.

## 2. Linux power-up sequence (reference)

1. Power sequencer (`pwrseq-qcom-wcn.c`, wcn7850): enable rails in this order: vdd, vddio, vddaon, vdddig, vddrfa1p2, vddrfa1p8. Then drive the enable GPIO (BT_EN / WLAN_EN) and wait about 50 ms.
2. Bluetooth (`hci_qca.c` + `btqca.c`):
   - open the UART at 115200 8N1 with RTS/CTS;
   - `EDL_PATCH_VER_REQ` (vendor opcode 0xfc00, sub 0x19); WCN3991+ return it in a Command Complete event;
   - download the rampatch `qca/hmtbtfw%02x.tlv` and the NVM `qca/hmtnv%02x(.bNN).bin`;
   - switch to the operating speed of 3.2 Mbaud;
   - `HCI_Reset`.
3. Wi-Fi (`pcie-qcom.c` cfg 1.9.0 + `phy-qcom-qmp-pcie.c` + ath12k/MHI):
   - PERST# low;
   - clocks on, then pulse the core BCR;
   - PARF: DEVICE_TYPE=RC, clear PHY_TEST_PWR_DOWN, set the DBI/ATU base, set MHI bypass;
   - PHY: pulse the BCR, apply the table, switch pipe clock to PHY, poll PHYSTATUS;
   - wait 100 ms, then PERST# high and set LTSSM_EN;
   - wait for DLLLA; config-space access then goes through iATU CFG0;
   - ath12k loads `amss.bin`, `m3.bin` and the board data over MHI/QMI.

## 3. What iOS 16 expects (iPhone X userland)

Source: the rootfs, the iPhone X kernelcache and the `d22ap` device tree, read with `ipsw` and `strings`.

- **Bluetooth**
  - `/usr/sbin/bluetoothd` (MobileBluetooth) talks H4 over the UART tty `/dev/cu.bluetooth` or `/dev/uart.bluetooth`, or over the `com.apple.uart.sk.bluetooth` control socket. Those come from `AppleOnboardSerial`.
  - It reads `transport-encoding` and `transport-speed` from DT `/arm-io/uart1/bluetooth` (`bluetooth,n88`, 3000000).
  - Power is handled by `/dev/btreset`, `/dev/btwake` and `/dev/btpoweroff` (`com.apple.driver.AppleBluetooth`, class BTReset).
  - `BlueTool` runs the chip init scripts and carries the Broadcom `.hcd` files built in. It checks `/etc/bluetool/<model>.boot.script` first.
  - bluetoothd only knows BCM and Apple chipsets and sends Broadcom vendor commands. A QCA controller is likely to be rejected or to fail init. This is a guess and needs disassembly to confirm.
  - The kernelcache has no IOBluetoothFamily kext.
- **Wi-Fi**
  - `wifid` only loads when the DT has a `wlan` (or `marconi-wifi`) node (`_LimitLoadToDeviceTree`).
  - It waits for `IO80211SkywalkInterface` with `IO80211InterfaceRole = Infrastructure` and uses the `IO80211APIUserClient` and `SIOCGA80211`/`SIOCSA80211` ioctls. The IO80211 framework names about 428 `APPLE80211_IOC_*` ioctls.
  - The kernelcache has `com.apple.iokit.IO80211Family` 1200.13.0, `AppleBCMWLANCore` and `AppleBCMWLANBusInterfacePCIe`.
- **Plain Ethernet**: configd's InterfaceNamer and IPConfiguration bring up any IONetworkInterface `en`. The USB CDC-ECM interface that already works is the interim IP path; it needs no IO80211.

## 4. Shortest credible path

1. **Interim networking**: keep using USB CDC-ECM.
2. **Bluetooth first**. It needs no PCIe, no MHI and no SMMU.
   - Bring the UART up and probe the controller with HCI_Reset and the QCA version request (milestone 2).
   - Do the rampatch/NVM download in the kernel or in a small userland tool.
   - Expose an H4 tty as `uart.bluetooth`, plus the BTReset nodes and the DT node.
   - Make bluetoothd accept the controller: a BlueTool script override, then fake or patch around the chipset check.
3. **Wi-Fi later**.
   - Bring the PCIe0 link up and read the WCN config-space ID (milestone 3).
   - The full path needs MHI + QMI + ath12k firmware handling and a minimal IOEthernet netif. Real wifid integration needs an IO80211Controller/SkywalkInterface implementation and is a large project.

## 5. Probe: request 0x5c, value 0x574c ('WL')

| Index | Action |
|---|---|
| 1 | Read-only snapshot. No register writes. |
| 0x100 | Return the last report again. |

The request is refused after XNU launch. Implementation: `shim/fuxi-probe/wlan.c`; decoder: `tools/fuxi-wlan-probe.py`.

Report words (`u32[128]`):

| Words | Contents |
|---|---|
| 0 | magic `0x574c0001` |
| 3 | complete flag |
| 4-7 | GCC votes 0x52000/0x52008/0x52010 and the collapse vote 0x52020 |
| 8-17 | PCIe0 CBCRs |
| 19-22 | GDSCR/CFG for core and PHY |
| 23-27 | BCRs |
| 28-33 | pipe mux, aux/rchng RCGs, TCSR clkref |
| 34-38 | wrap2 clocks |
| 39-65 | SE6 RCG and its 8 DFS levels (cfg, M, N) |
| 66-87 | TLMM cfg/io for gpio76-83 and 94-96 |
| 88-89 | RPMh DRV id/config |
| 90-100 | cmd-db magic and the rail addresses |
| 101 | gated-read mask: 1 PARF, 2 PHY, 4 DBI, 8 SE6 |
| 102-112 | PARF/PHY/DBI values |
| 113-120 | QUP2 version and SE6 registers |
| 124-125 | sync-abort count and ESR |

Gating: an unclocked peripheral read hangs the NoC and resets the SoC; the touch lane hit this. So PARF is read only with the GDSC on and cfg_ahb running, PHY also needs the PHY GDSC and aux, DBI also needs slv_axi, and SE6 needs wrap2 m_ahb.

### Device test (coordinator)

1. `fastboot boot fuxi-wlan-m1-boot.img`, then wait for the 1209:0001 "Fuxi hardware probe".
2. Run `python3 tools/fuxi-wlan-probe.py --save wl-m1.bin`.
3. Expected: `magic=0x574c0001 complete=1 faults=0` and cmd-db magic 0xc0330db with the addresses from section 1.
   - Likely: PCIe0 CBCRs `OFF`, GDSCs off, gated mask 0, BT_EN/WLAN_EN outputs low.
   - If the SoC resets during the request, report it. That means a gated read is still unsafe.
   - Send back `wl-m1.bin`.

### Next milestones (planned, not implemented; each needs coordinator sign-off)

- **M2 (BT)**
  - Vote the cnss rails via RPMh, using the cmd-db-verified addresses and the DT voltages, in pwrseq order.
  - BT_EN high, then wait 50 ms.
  - Wrap2/SE6 clock votes, pinmux 76-79, GENI FIFO UART at 115200.
  - Send HCI_Reset `01 03 0c 00`; expect `04 0e 04 01 03 0c 00`. Then the QCA version request `01 00 fc 01 19`.
  - Bounded timeouts throughout.
- **M3 (PCIe)**: implemented; see section 7. Wi-Fi now has priority over M2.

## 6. Firmware (pulled by the coordinator; gitignored, never committed)

Observed in the phone's `firmware_mnt/image/kiwi` listing:
- `amss.bin` and `amss20.bin` (WLAN firmware; the `20` names are presumably for chip v2)
- `Data.msc`, `Data20.msc`
- `phy_ucode.elf`, `phy_ucode20.elf`
- `regdb.bin`, `regdb_xiaomi.bin`
- board files `bdwlan.e*` and `bd_*.elf`
- qdss configs

There is **no `m3.bin`** on this phone. Linux ath12k's WCN7850 hw2.0 set does include a separate `m3.bin`. My guess is that the kiwi build here carries M3 inside `amss.bin`, or that CNSS does not need it; the QMI capability exchange will tell.

Bluetooth: `hmtbtfw10/20.tlv` and `hmtnv*.b*`. Wi-Fi ini files: `etc-wifi/kiwi*/WCNSS_qcom_cfg.ini`.

Which board file to use is picked from the board ID the firmware reports over QMI. That needs M4+; it cannot be read from config space.

## 7. Milestone 3: gated power-up, PCIe0 link and endpoint ID

- **Request**: 0x5c, value 0x574c. Index `0x300 + N` runs steps 1..N; index `0x3ff` re-reads the report. The request is refused after XNU launch.
- **Re-runs**: steps that completed earlier in the same boot are skipped, so increasing limits can be run on one boot.
- **Failure**: the first failure latches. Every later request just returns the report; reboot to retry.
- **Attribution**: before each step touches hardware, the step number goes into report word 2. When the probe-03 scanout geometry is present, a band reading "WLAN M3 STEP nn" is also painted at y=1700. It may not show if the panel needs a START.
- **Fault handling**: all PCIe register accesses go through the sync-abort-safe helpers. An asynchronous SError or a NoC hang still resets the SoC.

### Writes, with their sources

| Step | Writes | Source |
|---|---|---|
| 1 | none: snapshot; check cmd-db magic and the 8 rail addresses against the OrangeFox dump; check RSC DRV2 id/TCS count; check GPLL0 is locked | — |
| 2 | RPMh VRM (voltage mV, then enable=1), in this order: s5g 1000, l15b 1800, s2g 980, s4e 952, s4g 1350, s6g 1900 | order: `pwrseq-qcom-wcn.c` `pwrseq_wcn7850_vregs`; values: live DT `chip_cfg@1` `qcom,vdd-wlan*-config`, each within that regulator's live-DT min/max (s4e uses the 952 mV init voltage, which covers the 950 mV request) |
| 3 | gpio80 (WLAN_EN): function gpio, pull-up, 16 mA, output high; then 50 ms | live DT `cnss_wlan_en_active`; pwrseq wcn7850 `pwup_delay_ms` |
| 4 | RPMh ldoe1 880 mV, then ldoe3 1200 mV | live DT `vreg-0p9` / `vreg-1p2`; order from `qmp_phy_vreg_l` |
| 5 | gpio94 output low (PERST# asserted, pull-down 2 mA); gpio95 function 1 (CLKREQ#) pull-up; gpio96 input pull-up | live DT `pcie0_*_default`; `qcom_ep_reset_assert` |
| 6 | clear GCC 0x52020 bit 0, poll `CFG_GDSCR` bit 16, set RETAIN_FF (GDSCR bit 11); same for the PHY GDSC with bit 3; 1.5 ms bound | `gcc-sm8550.c` `pcie_0_gdsc` / `pcie_0_phy_gdsc`; `gdsc.c` `gdsc_enable`; live DT `qcom,gds-timeout` |
| 7 | aux RCG = XO; rchng RCG = GPLL0_EVEN/3 (100 MHz); votes 0x52008 bits 0,1,2,3,5; 0x52000 bits 12,19,22; TCSR 0x15100 bit 0 | `sm8550.dtsi` pcie0 + pcie0_phy clocks; `assigned-clock-rates`; gcc-kalama offsets |
| 8 | PCIE_0_BCR pulse (1 ms / 1 ms); PARF DEVICE_TYPE=4, PHY_CTRL&=~1, DBI/ATU base, SLV_ADDR_SPACE, SYS_CTRL&=~bit29, MHI BYPASS, PM_CTRL&=~bit5, AXI halt EN | `pcie-qcom.c` `qcom_pcie_init_2_7_0` + `qcom_pcie_configure_dbi_atu_base` |
| 9 | PHY BCR pulse; POWER_DOWN_CONTROL\|=3; 109-entry table; pipe mux=PHY; pipe vote; SW_RESET release; START; poll PHYSTATUS for up to 10 ms | `phy-qcom-qmp-pcie.c` `qmp_pcie_init` / `qmp_pcie_power_on`, `sm8550_qmp_gen3x2_*_tbl` |
| 10 | 100 ms, then gpio94 high, then 1 ms | `qcom_ep_reset_deassert` |
| 11 | PARF_LTSSM\|=LTSSM_EN; poll DLLLA up to 1 s; records LTSSM states seen | `qcom_pcie_2_3_2_ltssm_enable`, `dw_pcie_wait_for_link` |
| 12 | 100 ms; RC bus numbers 0/1/0xff; iATU region 0 = CFG0 at 0x60100000 → bus 1; read the ID, retrying CRS / all-ones for up to 1 s | `dw_pcie_setup_rc`, `dw_pcie_prog_outbound_atu`, `dw_pcie_other_conf_map_bus` |
| 13 | for each BAR: write all-ones, read back, restore (64-bit pairs aware); the command register is not touched | standard PCI sizing |

The PHY table is the mainline `sm8550_qmp_gen3x2` table. It was compared programmatically with the live-DT `qcom,phy-sequence`:
- All 108 shared register writes are identical.
- The DT sequence also includes the power-down, SW_RESET and START writes; mainline issues those separately, and the probe does the same.
- Mainline additionally writes `RXEQEVAL_TIME` (0x6f4 = 0x27).

### Gaps (not done, with reasons)

- **32 kHz sleep clock**: mainline QRD routes it through PMK8550 GPIO3 (`pmk8550_sleep_clk`). That is a PMIC/SPMI write, and this round does none. Most likely the bootloader or Android left it configured. If the link fails at step 11, check this next with a read-only check.
- **Interconnect votes** (`pcie-mem`, `cpu-pcie`): not cast. CPU→PCIe config/DBI traffic uses the always-active config NoC, and the device does no DMA in M3.
- **ASPM/L1SS, BDF→SID table, MSI**: not needed for config reads; left alone.
- **BT_EN**: stays untouched.

### Device test order (coordinator)

1. Fresh `fastboot boot fuxi-wlan-m3-boot.img`. First run `python3 tools/fuxi-wlan-probe.py --save wl-m1.bin`; this is the M1 snapshot and is read-only.
2. On the same boot, run `python3 tools/fuxi-wlan-probe.py --m3 N --save wl-m3-N.bin` for N = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, in that order, one call per N.
   - Stop at the first FAIL or reset, and send the outputs.
   - If the SoC resets, note the last N and any "WLAN M3 STEP nn" band on screen. Then reboot and send `--m3-report` output if the device comes back to the probe; after a reset it will not have the state.
3. Expected on success:
   - steps 1–13 `OK`
   - step 9: `pcs_status` bit 6 clear
   - step 11: link up within about 100 ms, speed 1–3, width x1–x2
   - step 12: `endpoint id=0x110717cb`, class 0x0280xx (network controller)
   - step 13: BAR0 mem64, probably 2 MiB (Linux ath12k maps BAR0)
4. Known risk points:
   - step 2: RPMh vote errors are reported, not fatal to the SoC;
   - step 7: a branch not running means a missing parent;
   - step 9: PHYSTATUS stuck means a refclk/rail problem;
   - step 11: link timeout points at the chip not booting, i.e. sleep clock, rails or WLAN_EN.

Nothing here touches UFS, modem, persist or fuses.

## 8. What XNU needs for Wi-Fi (sketch, estimates for one experienced person)

| Piece | Content | Estimate |
|---|---|---|
| A. PCIe host (arm, qcom DWC) | Port the M3 sequence into a Fuxi XNU driver. bsd/dev style, like the buttons/keys drivers, or an IOService nub. It needs: config accessors through iATU, BAR assignment from the 0x60300000+0x3d00000 window, INTx (SPI 149) or the qcom SPI-MSI block (DT `qcom,pcie0_msi@0x17110040`), and apps-SMMU stream setup for SIDs 0x1400/0x1401 (reuse the GPU lane's SMMU code) | 1–2 weeks |
| B. MHI host | Minimal MHI host: BHI/BHIe image download of `amss.bin`, the MHI state machine (RESET→READY→M0), event/command rings, the IPCR channels for QRTR. Linux `drivers/bus/mhi/host` is about 6k lines; a needed subset is about 1.5–2k | 2–4 weeks |
| C. QRTR + QMI WLFW | Host-capability exchange, firmware memory request (host provides the `cnss_wlan_region`-style memory segments), BDF download (board file chosen by the reported board ID), regdb, cal data, wlan mode on. Linux `ath12k/qmi.c` is the reference | 2–3 weeks |
| D. ath12k core + data path | CE pipes, HTT, WMI init; vdev create, scan, connect; TX/RX rings (TCL/REO/WBM). The host must build 802.11 auth/assoc (mac80211 work) and run EAPOL/WPA2 (wpa_supplicant-class code) | 2–4 months for a minimal open/WPA2 station |
| E1. iOS interface, plain | An IOEthernetController subclass (`en1`) that bridges 802.3↔802.11 frames over D. configd/IPConfiguration should DHCP it like the USB `en` interface. Association driven by a small private tool, with no wifid | 1 week once D works |
| E2. iOS interface, native | IO80211Controller + IO80211SkywalkInterface subclass answering the APPLE80211 ioctls wifid uses, plus a `wlan` DT node so launchd loads wifid. This gives the Settings UI | 1–3 months on top of D |

Recommendation:
- Use the USB CDC-ECM `en` interface for IP now.
- For Wi-Fi, do A→B→C as the next milestones. Each is testable on its own: firmware boots, QMI ready, then a firmware version reply.
- Then build D with E1 for a first wireless IP connection. Consider E2 only after E1 works.

Section 9 revises these estimates after the prior-art study.

## 9. Prior art: OpenBSD qwz(4) and itlwm (study, 2026-10-20)

The sources were fetched read-only into `research/external/`, which is gitignored and never committed. Nothing was built or run.

| Source | Revision |
|---|---|
| openbsd/src (GitHub mirror) | 49330ea2bbd4, 2026-10-20 |
| OpenIntelWireless/itlwm | 53c51c2cdd6e, 2024-06-09 (v2.4.0) |

### Licenses (facts, from the file headers)

| File | Lines | License | Reusable here? |
|---|---|---|---|
| `sys/dev/ic/qwz.c` 1.92 | 25197 | ISC (S. Sperling 2023) **and** BSD-3-Clause-Clear (Linux Foundation, QuIC) | yes; keep both notices |
| `sys/dev/pci/if_qwz_pci.c` 1.21 | 4251 | ISC **and** BSD-3-Clause-Clear | yes; keep both notices |
| `sys/dev/ic/qwzreg.h` 1.19 | 13947 | BSD-3-Clause-Clear only | yes |
| `sys/dev/ic/qwzvar.h` 1.30 | 2242 | BSD-3-Clause-Clear only | yes |
| OpenBSD `sys/net80211` | about 25k | BSD/ISC | yes (not fetched yet) |
| itlwm, every file | | GPL-2.0, including the OpenBSD-derived files, which carry a GPL header above the BSD one | **no**, pattern reference only |

BSD-3-Clause-Clear grants no patent licence. It is permissive, but it is not plain ISC.

### qwz scope (facts)

From qwz.4 and the attach code:
- Station (BSS) mode only.
- WEP, WPA1 and WPA2, with CCMP in hardware.
- PMF and background scan.
- HT rates only (MCS 0–7); VHT, HE and EHT are not enabled.

It loads `qwz-wcn7850-hw2.0-{amss,board-2,m3}`. From qwz.c:
- **M3 is mandatory.** `qwz_loadfirmware(sc, QWZ_FW_M3, "m3")` runs in the QMI bring-up, and a failure aborts it (qwz.c:7002).
- Board data comes only from a `board-2` TLV container (magic "QCA-ATH12K-BOARD"). The board IE and the regdb IE are looked up by a name built from the PCI IDs and the QMI chip and board IDs.
- All coherent DMA memory comes from `qwz_dmamem_alloc`: one physically contiguous segment, `bus_dmamem_alloc_range(..., 0, 0xffffffff)`, so **below 4 GiB** (qwz.c:25124). That memory holds:
  - MHI contexts and rings;
  - HAL SRNGs and CE pipes;
  - the AMSS image and its BHIe vector table;
  - the M3 image;
  - one `fwmem` block that holds all the QMI target-memory segments (several MiB, size known only at runtime).
- Interrupts: it tries 32 MSI vectors, then **falls back to a single MSI** and polls every source from one handler (if_qwz_pci.c:747). Single MSI is our first target.

itlwm's structure (facts):
- `itlwm/`: an IOEthernetController with Intel HALs (`hal_iwm/iwn/iwx`).
- `AirportItlwm/`: macOS IO80211. Not applicable to iOS 16.
- `itl80211/`: the compat layer plus a 2021 snapshot of OpenBSD net80211. That snapshot is too old for qwz (no `IEEE80211_C_ADDBA_OFFLOAD`).

### What qwz needs from the OS, and where it comes from on our XNU

| qwz needs (OpenBSD API) | itlwm on macOS | Fuxi XNU (in-kernel, no kexts) |
|---|---|---|
| `bus_space_read_4/write_4` on BAR0 (the driver does its own windowing) | IOMemoryMap | **done in M4**: BAR0 assigned and mapped by our own code |
| `pci_conf_*`, `pci_get_capability`, `pci_mapreg_*`, `pci_set_powerstate` | IOPCIDevice (IOPCIFamily) | our iATU CFG0 accessors (M3/M4). A 50-line `pci_*` shim over them |
| `pci_intr_map_msi`, `pci_intr_establish`; the MSI address and data are read back from the endpoint capability | IOPCIDevice MSI + IOInterruptEventSource | **missing**: DWC iMSI (PCIE_MSI_ADDR/INTR0 at DBI 0x820+) or the qcom SPI-MSI block from the live DT. GIC SPI routing through XNU's interrupt controller (no Fuxi GIC handler is registered yet). Polling first |
| `bus_dmamem_alloc_range` (contiguous, <4 GiB), `bus_dmamap_load_mbuf`, `bus_dmamap_sync` | IOBufferMemoryDescriptor with a physical mask (signatures changed by hand) | **missing**: a contiguous carve-out below 4 GiB (DRAM starts at 0x80000000, so this is reachable) via `kmem_alloc_contig` with a max-address mask, or a loader-reserved ADT region. Cache maintenance on sync (`flush_dcache`/`invalidate_dcache`). Apps-SMMU state for PCIe0 SIDs 0x1400/0x1401 must be checked first (bypass or identity); this is a guess risk |
| mbufs (`m_gethdr`, `MCLGET`, `m_adj`, `m_copydata`, `mtod`, ...) | wrapped over the KPI `mbuf_*` because a kext cannot see `struct mbuf` | **better than itlwm**: compiled into the kernel we have the real BSD `struct mbuf` and `m_*` functions (XNU's mbuf is BSD-derived); a thin macro layer |
| `tsleep_nsec`/`wakeup`, `splnet`, rwlocks, refcnt | IOCommandGate, IOLock | `msleep`/`wakeup` exist in XNU BSD; `lck_mtx`/`lck_rw`; spl is a no-op under one driver mutex |
| `task_*`, `taskq`, `timeout_*` | IOCommandGate, IOTimerEventSource | `thread_call` (and a dedicated kernel thread, as in M4) |
| `loadfirmware()` | firmware compiled in as zlib blobs | loader-staged blobs (environment variables at build time, like the GPU firmware), or a `vn_open`/`vn_rdwr` read from the root fs after mount |
| net80211, ifnet, bpf | itl80211 + IOEthernetInterface | vendor current OpenBSD net80211 with prefixed symbols (to avoid clashes with XNU's BSD net headers), then `ifnet_allocate`/`ether` KPIs for `en1` |

### IOPCIFamily (facts)

- The iPhone X 16.7 kernelcache **does contain** `com.apple.iokit.IOPCIFamily` 2.9, with IOPCIDevice, IOPCIBridge and IOPCIHostBridge classes. Users: AppleEmbeddedPCIE, AppleT8015PCIe, AppleBCMWLANBusInterfacePCIe, AppleBasebandPCI.
- Our Fuxi XNU runs **no kexts**: drivers are compiled into the kernel.
- `xnu-8796` source has no IOPCIFamily: only forward declarations in IOService.h, IOKit headers and DriverKit `.iig`.
- IOPCIFamily is Apple OSS (APSL), so it could be built in-kernel. It would still need our own host-bridge driver, and it brings nothing that qwz needs which the 50-line config shim does not already give.
- **Decision:** no IOPCIFamily. qwz gets a direct `pci_*` shim over our own host code.

### Port plan and revised estimates (one experienced person)

| Milestone | Content | Done when | Estimate (old in brackets) |
|---|---|---|---|
| M4 (this round) | XNU driver: M3 power/PCIe sequence, MEM iATU, BAR0 assignment, MHI/BHI register read | log shows MHIVER and EE = PBL (expected) | done, pending a device test |
| M5 | DMA carve-out below 4 GiB, SMMU check, polled BHI download of AMSS (qwz `qwz_mhi_fw_load_bhi`/BHIe, rewritten minimally), MHI RESET→READY→M0 | EE goes PBL→SBL/AMSS and MHI reaches M0 | 1–2 weeks (part of A+B: 3–6 weeks) |
| M6 | Compile qwz.c + if_qwz_pci.c in-kernel over a compat header (bus_dma/bus_space/pci/mbuf/sleep/task/timeout/loadfirmware), net80211 stubbed; MSI via DWC iMSI or polling; firmware: AMSS, a board-2 we build from the phone's bdwlan/regdb, M3 | qwz prints the firmware build ID and reaches `fw_ready` over QMI | 3–5 weeks (B+C: 4–7 weeks) |
| M7 | Vendor current OpenBSD net80211; scan | scan results printed in the log | 2–4 weeks |
| M8 | WPA2 station + `en1` ifnet + DHCP via configd, driven by a small private tool (no wifid) | ping over Wi-Fi | 3–6 weeks (D+E1: 2–4 months + 1 week) |
| M9 (optional) | IO80211/wifid Settings integration | Settings shows networks | 1–3 months (unchanged) |

Total to first IP over Wi-Fi: **about 2.5–4 months**, against 3.5–6 months for a from-scratch ath12k port. The data path (CE/HAL/DP), QMI and WMI are already in qwz. What is left for us is the platform layer, which is what M3/M4 started.

Risks:
- *Guess:* PCIe0 sits behind apps_smmu. If unmatched streams fault, device DMA fails silently.
- No XNU GIC handler for Fuxi peripherals yet; polling works for bring-up.
- Shared GCC vote registers: touch and the GPU also do read-modify-write on GCC 0x52008/0x52000 without a common lock. Adding a lock is a later cleanup; M4 logs the final values.
- Firmware match (below).

### Firmware notes (facts, then guesses)

- `amss.bin` has `QC_IMAGE_VERSION_STRING=CI_WLAN.HMT.1.0-02352.5.1-QCAHMTSWPL_V1.0_V2.0_SILICON-2` (a 2022 CI build).
- `amss20.bin` has `QC_IMAGE_VERSION_STRING=WLAN.HMT.1.0.c2-00058.3-QCAHMTSWPL_V1.0_V2.0_SILICONZ-1...`.
- The endpoint reports PCI revision 1.
- `phy_ucode.elf` and `phy_ucode20.elf` are 32-bit ELFs with e_machine 3.
- *Guesses:*
  - hw2.0 silicon uses the `*20` files (Android cnss2 picks them by chip version).
  - Downstream cnss2 hands `phy_ucode*.elf` to the firmware through the same QMI M3 request that ath12k uses for `m3.bin`. If so, `phy_ucode20.elf` is our M3 image.
  - The board file is `bdwlan.e<board-id>` for the board ID that QMI reports.
  - All three are verified only at M6.

## 10. Milestone 4: XNU driver (patch 0099, boot-arg `fuxiwlan`)

Files:
- `xnu/fuxi-wlan/fuxi_wlan.c`, which becomes `bsd/dev/fuxi_wlan.c`.
- `bsd/dev/wlan.c`: an exact copy of `shim/fuxi-probe/wlan.c`, built with `FUXI_WLAN_XNU`, so steps 1–13 are the code that passed M3.
- A hook after `fuxi_keys_init()` in `sweet_ufs.c`.
- `xnu/patches/0099-fuxi-wlan.patch` holds all of it. Regenerate the patch's `wlan.c` copy whenever the loader's file changes.

Gate: `fuxiwlan=0` or absent does nothing; `1` runs steps 1–16; `2`..`16` stops after that step, for bisection. The work runs in its own kernel thread.

Mapping and access:
- 11 windows, 16 KiB aligned, mapped with `kmem_alloc(KMA_VAONLY)` + `pmap_enter_options_addr(VM_WIMG_IO)`, the same scheme as fuxi_adreno.
- Every access goes through `fuxi_gpu_access`, so synchronous aborts are recovered.
- After the first fault, all writes are refused and the run stops.

The XNU helpers equal the loader's `usb_rd/ts_wr/ts_gpio/rh_send` (apps RSC DRV2, ACTIVE TCS0; no other XNU code drives that RSC).

New steps:

| Step | Writes | Source |
|---|---|---|
| 14 | iATU outbound region 1 (unrolled, DBI+0x1200): type MEM, 0x60300000–0x63ffffff → PCI 0x60300000. RC memory window 0x60400000–0x605fffff (DBI 0x20 = 0x60506040), prefetchable window empty (0x24 = 0xfff0, 0x28/0x2c = 0), RC command IO\|MEM\|MASTER\|SERR | live-DT `ranges` (mem entry), `dw_pcie_prog_outbound_atu`, `dw_pcie_setup_rc`, PCI bridge window format |
| 15 | endpoint command decode off, BAR0 = 0x60400000, BAR1 = 0, command = MEMORY only (bus master stays off) | standard PCI BAR assignment; the size comes from the M3 step-13 sizing (must be a 64-bit non-prefetchable BAR ≤ 2 MiB) |
| 16 | none: reads BAR0 +0x00/08/10/18/20/28/2c/30/38/48 and BHI +0x00/04/28/2c/30/34/38/3c. BHI_SERIALNU (+0x40) is never read | `drivers/bus/mhi/common.h`; ath12k sets `mhi_ctrl->regs` = BAR0 |

### Device test (coordinator)

1. Boot `shim/out/fuxi-wlan-m4-boot.img`. The ADT is 0098i's and the cmdline is 0098i's plus `fuxiwlan=1`, 427 bytes.
2. Capture the console from boot until `FUXI WLAN: thread exit`.
3. Decode it with `python3 tools/fuxi-wlan-probe.py --xnu-log LOG`, which rebuilds the M3 and M4 reports from the `m3 r[..]`/`m4 r[..]` dump lines.
4. On a hang or reset, the last `FUXI WLAN: step N` line is the step that was running. Re-test with `fuxiwlan=N-1`.

Expected lines, in order. Values are those M3 measured; MHI values are guesses.
```
FUXI WLAN: enabled (fuxiwlan=1), WCN7850 power-up + PCIe0 + MHI read, limit 16
FUXI WLAN: mapped 901120 bytes in 11 windows; running steps 1..16
FUXI WLAN: step 1 ... FUXI WLAN: step 1 ok code 0x1001 (0 ms, faults 0)
FUXI WLAN: cmd-db 0x40a00 0x40200 0x40400 0x40500 0x44000 0x40600 0x43900 0x43a00 GPLL0 0xe0000005
  ... steps 2-8 ok ...
FUXI WLAN: PHY ready after ~620 us pcs_status 0x86
FUXI WLAN: link LTSSM 0x111 LNKCTL/STA 0xb0220008 (gen2 x2 DLLLA 1) 0 ms
FUXI WLAN: endpoint id 0x110717cb class/rev 0x2800001 cmd/status 0x100000 tries 1
FUXI WLAN: BAR0 orig 0x4/0x0 sizing readback 0xffe00004/0xffffffff
FUXI WLAN: iATU1 MEM 0x60300000-0x63ffffff -> 0x60300000 ctrl2 0x80000000; RC window 0x60506040 pref 0xfff0 cmd/status 0x...107
FUXI WLAN: BAR0 size 0x200000 assigned 0x60400000 (readback 0x60400004 0) cmd/status 0x100000 -> 0x100002
FUXI WLAN: MHI REGLEN 0x... MHIVER 0x... MHICFG 0x... (nhwer .. ner .. nhwch .. nch ..)
FUXI WLAN: MHI CHDBOFF 0x... ERDBOFF 0x... BHIOFF 0x... BHIEOFF 0x... DEBUGOFF 0x...
FUXI WLAN: MHICTRL 0x0 (state 0, reset 0) MHISTATUS 0x... (state RESET, ready 0, syserr 0)
FUXI WLAN: BHI version x.y EXECENV 0x0 (PBL) STATUS 0x0 (0) ERRCODE 0x0 ERRDBG ...
FUXI WLAN: RC cmd/status 0x...107 EP cmd/status 0x...0002
FUXI WLAN: 16 steps done; no firmware loaded, bus master off
FUXI WLAN: GCC votes 0x52000 ... 0x52008 ... gdsc-collapse 0x52020 ...
FUXI WLAN: m3 r[  0] 574c0003 ...
FUXI WLAN: m4 r[  0] 574c0004 ...
FUXI WLAN: thread exit
```
Guesses for step 16:
- MHIVER around 0x01000000.
- MHI state RESET (or READY), EE = PBL (0), since no image has been sent.
- BHIOFF and BHIEOFF are small offsets inside the first 16 KiB. Anything else stops the step with reason 3 and a log line.
- `MHIREGLEN = MHIVER = 0xffffffff` means BAR0 is not decoded (reason 2).

How to read a failure:
- **Steps 1–13:** same reasons as section 7.
- **Step 14, reason 2:** iATU region 1 not enabled or not programmed.
- **Step 14, reason 3:** RC command readback is missing MEM or MASTER.
- **Step 15, reason 1:** the step-13 sizing was unexpected.
- **Step 15, reason 3 or 4:** BAR or command readback mismatch.
- **Step 16, reason 1 or 4:** an access fault (synchronous abort): check the ESR in the "stopped" line.
- **Any step, reason 0xfd:** a fault in a plain access.

## 11. Milestone 5: polled BHI/BHIe firmware download

Implemented in the existing `fuxi_wlan.c` and regenerated patch 0099. M4 steps
1–16 are unchanged. `fuxiwlan=0`/absent is off, `1` runs through 28, and `N`
(2–28) stops after N. Values above 28 retain the old "run all" convention.
The new work runs in the same dedicated kernel thread; long waits yield it.
No phone was accessed during M5 development. **Actual apps-SMMU state and
firmware/READY/M0 success remain unmeasured**, pending the coordinator's run.
M4's measured MHIVER is `0xdeadbeef`, REGLEN `0x100`, MHICFG `0x800080`,
BHIOFF `0x100`, BHIEOFF `0x224`, EE PBL, MHISTATUS RESET, with no BHI errors.

### DMA safety and the SMMU decision

PCIe0 uses apps_smmu at `0x15000000`; DT assigns RC SID `0x1400`, endpoint
01:00.0 SID `0x1401`. Step 17 reads sCR0/ID0/ID1 and all ID0.NUMSMRG SMR/S2CR
pairs, including wildcard masks and EXIDENABLE's S2CR.EXIDVALID. It logs the
matching slots, context bank and BYPASS/FAULT/TRANSLATED classification for
both SIDs. An unmatched SID uses sCR0.USFCFG (bypass or fault). Multiple
matches, unsupported formats, CLIENTPD, reserved types, inaccessible registers,
or either SID not bypassing stop the run before firmware allocation or BME.
A translated stream is rejected even if it might happen to be identity mapped.

The DT route also needs the RC hardware table to survive reset: step 17 reads
PARF BDF_TO_SID_CFG/SID_OFFSET and traverses the CRC8 lookup for RID `0x0100`.
It requires the enabled table to yield relative SID 1 (DT's `0x1401`), or
requires every valid SMMU entry and the unmatched-stream policy to bypass so
that an unknown emitted SID cannot change DMA interpretation. An unproven
route with any translated/faulting stream also stops at 17. No RC SID table
or SMMU register is written by this probe.

The GPU lane targets a separate GPU SMMU and owns its context/stream slots.
That code does **not** establish apps-SMMU ownership, secure-firmware policy,
or a safe identity/bypass stream for PCIe0. M5 therefore performs **no SMMU
writes**. If 17 stops, the next platform work must establish the actual RC
RID-to-SID route and reserve apps-SMMU stream slots/context banks for these
SIDs without replacing another client's wildcard match. For translation,
map the allocated image, vectors, all context/ring pages at the device IOVAs,
set context TTBR/TCR/MAIR/SCTLR and stream S2CR consistently, clean page tables,
and complete the relevant context/global TLB invalidation and sync. Confirm
secure ownership and Qualcomm bypass quirks first. Never clear global USFCFG
or CLIENTPD to make a single Wi-Fi client work. This is a separate milestone.

### Known low-memory prerequisite in the requested base packing

The inherited loader passes `physBase=0x8c0000000`, `memSize=0x13e000000`
to XNU (both above 4 GiB), rather than advertising an allocator-owned low-DDR
pool. Thus **step 19's physical allocation below 4 GiB is expected to fail
with this exact base packing**, even if step 17 proves bypass. The allocation
failure explicitly reports the need for allocator-owned low DDR. No high-PA
truncation or unverified RAM bus alias is used. To execute the later DMA
steps, a platform follow-up must reserve and expose a suitable low-DDR pool
to XNU (with bootloader/secure/reserved-memory exclusions proved), or design
an apps-SMMU IOVA mapping for high XNU RAM; the latter changes the requested
physical-bypass policy and requires separate validation. M5 preserves the
specified base ADT and packing rather than inventing either arrangement.

### Steps and bounds

Each M5 step logs EE, MHISTATUS/state/READY, MHICTRL, BHI STATUS/ERRCODE/
ERRDBG1–3, BHIe TXVECSTATUS and endpoint command. SERIALNU is never read.
Every device access is fault-contained. Polls are 25 ms apart, up to 5 s each;
root availability is polled every 100 ms for at most 120 s.

| Step | Action / expected result |
|---|---|
| 17 | Read-only apps-SMMU and RC route probe; continue only with proven physical-address DMA bypass. |
| 18 | Save BAR0 window selector +0x310c; select 0x40000036; read endpoint-internal TCSR_SOC_HW_VERSION at BAR0+0x80000; restore selector. Major [11:8] == 2 selects `amss20.bin`, otherwise `amss.bin`; log minor [7:0]. Reject zero/all-ones/faulted reads. This is **not** the host TCSR address. |
| 19 | Wait for `vfs_rootvnode()`, release its iocount, open selected root file read-only/no-follow/nonblocking. Require regular file, 512 KiB–32 MiB, exact read, little-endian ELF32/64 ident/version/header/program-table bounds. Allocate page-rounded physically contiguous memory below 4 GiB with `kmem_alloc_contig` max PPN; verify every PPN and cache-clean the allocation. |
| 20 | Require PBL, RESET state, RESET bit clear, no SYSERR and BME off; mask PCI MSI/MSI-X. Allocate low contiguous vector and context pages. Build full-image 512 KiB chunks plus final remainder. All 128 channels disabled; two valid event contexts and an empty command context/ring. Cache-clean vectors/contexts. |
| 21 | Zero BHIe RX registers, INTVEC=0, BHI STATUS=0; image address high then low, size=512 KiB. Cache-clean image, then enable BME (and mask INTx) immediately before nonzero BHI sequence doorbell. |
| 22 | Poll BHI SUCCESS or exact SBL EE; errors, RDDM/EDL/SYSERR abort. |
| 23 | Poll MHICTRL.RESET clear and MHISTATUS.READY=1. This records READY before M0. |
| 24 | Set channel/event/command context bases high then low; control/data base=0, limits=0xffffffff; MHICFG NER=2/NHWER=0; ring both event WP doorbells. No command or channel doorbells. |
| 25 | Request MHICTRL.MHISTATE=M0, then poll exact SBL + M0. |
| 26 | Re-clean image/vectors; BHIe TX vector address high then low, size=table **bytes**; preserve upper doorbell bits and ring a new nonzero sequence. |
| 27 | Poll BHIe XFER_COMPL with the matching sequence number. |
| 28 | Poll mission EE (AMSS/WFW/FP) and M0; retain all DMA buffers for potential firmware paging. No channels/QMI. |

MHI contexts have three u32 plus four u64 fields: **44-byte packed stride**,
not the 40 bytes in the earlier private research notes. The single 16 KiB
context allocation contains channel array at +0, event contexts at +0x1800,
command context +0x1900, event ring 0 +0x1a00 (512 bytes), event ring 1 +0x2000
(4096 bytes), command ring +0x3000 (2048 bytes). Ring bases meet ring-length
alignment. Events can land in valid memory while the host polls registers;
the host does not consume events or reuse these buffers while running.

### Failure, timeout and deliberate partial stops

A failed M5 step snapshots status, requests MHI RESET and polls reset clear
for up to 1 s, clears endpoint BME with readback, asserts PERST# with readback,
waits 10 ms for reset/transaction drain, then frees the buffers. A synchronous
fault normally latches off all writes; only these three containment addresses
are allowed fault-contained cleanup writes after the latch. The original fault
state is preserved. If DMA started and BME-off/PERST cannot both be confirmed,
buffers are quarantined until reboot instead of risking DMA into freed pages.
No panic path is introduced. Successful partial stops 19–27 run the same
shutdown before freeing; 17/18 leave the M4 endpoint quiescent with BME off.
Reboot for every change of limit; there is no resume in one XNU boot.

### Firmware staging on the root filesystem

Use the coordinator's existing `firmware_mnt/image/kiwi` files, never kernel
embedding or files tracked by git. Stage both alternatives before testing:

| Source basename | Root destination | Owner | Mode |
|---|---|---|---|
| `amss20.bin` | `/usr/local/share/fuxi/wlan/amss20.bin` | root:wheel (0:0) | 0644 |
| `amss.bin` | `/usr/local/share/fuxi/wlan/amss.bin` | root:wheel (0:0) | 0644 |

The existing vendor copies pass the step-19 ELF/header/size rules:
- `amss20.bin`: 5,890,112 bytes, SHA256 `cc25a432aa19e71e8503b424c9a52be952f6dd1d2f93f38f1ff754f748d97667`.
- `amss.bin`: 7,458,880 bytes, SHA256 `c42e06f534c07ffdaec3b265ff1e38b312741e35eb5fb21b5ca77995fab8d688`.

Directories `/usr/local/share/fuxi` and `wlan` should be root:wheel, 0755.
No board data, regdb, Data*.msc, phy_ucode*.elf or m3.bin is loaded in M5;
those belong to the later QMI milestone. Upstream qwz/ath12k call the hw2.0
image `amss.bin`; the phone vendor's `amss20.bin` association with major 2 is
still an **explicit packaging inference**, not proved by CNSS source. M5 logs
this choice and does not automatically retry another file after DMA failure.
Non-major-2's `amss.bin` choice follows the requested vendor naming policy,
not a claim that other WCN7850 silicon versions are supported by upstream.

### Expected console and safe bisection

```
FUXI WLAN: apps_smmu sCR0 ... ID0 ... ID1 ... (read-only)
FUXI WLAN: SMMU SID 0x1400 BYPASS (...)
FUXI WLAN: SMMU SID 0x1401 BYPASS (...)
FUXI WLAN: RC RID 0x100 SID cfg ... offset ... entry ... route ...
FUXI WLAN: TCSR_SOC_HW_VERSION ... major 2 minor ... firmware /usr/local/share/fuxi/wlan/amss20.bin (vendor filename inference)
FUXI WLAN: firmware ... bytes ... ELF32 DMA ... cache-clean
FUXI WLAN: BHI doorbell seq ... bytes 0x80000 bus master on
FUXI WLAN: M5 step 23 EE ... MHISTATUS ... (READY ready 1) ...
FUXI WLAN: M5 step 25 EE 0x1 (SBL) MHISTATUS 0x200 (M0 ready 0) ...
FUXI WLAN: BHIe doorbell seq ... full image ... vector bytes ...
FUXI WLAN: M5 mission mode M0 confirmed (READY observed at step 23); no channels/QMI; DMA buffers retained
FUXI WLAN: M5 step 28 EE 0x2 (AMSS) MHISTATUS ... (M0 ready ...) ...
FUXI WLAN: 28 steps done
FUXI WLAN: m5 r[...]
FUXI WLAN: thread exit
```

Values after DMA are expectations, not device observations. MHISTATUS.READY
may clear in M0; a step-23 success is the recorded READY proof. On blocked DMA:
`FUXI WLAN: SMMU DMA blocked: ...`, `step 17 FAILED`, then the abort line and
`stopped at step 17 ...`. No firmware is opened and BME is never enabled.

Coordinator order, one fresh boot per limit: **16 → 17 → 18 → 19 → 20 → 21 →
22 → 23 → 24 → 25 → 26 → 27 → 28 → 1**. Step 21 is the first DMA; 25 first
requests M0; 26 first rings BHIe. Stop on the first failure/reset. If a step
hangs, the last `FUXI WLAN: step N` is the active one; reboot with N-1. A
partial run that starts DMA shuts down on reaching its requested limit.
Decode captured logs offline with `tools/fuxi-wlan-probe.py --xnu-log LOG`;
it accepts m3/m4/m5 report dumps and labels all new steps. Raw text retains
per-step status and containment results.

### Build and source references

Build only the dedicated native WLAN tree using its configured IIG cache and
`bash build-fuxi-native.sh xnu`. Copy stripped/unstripped kernel outputs into
`xnu/out-fuxi-wlan5/`. `tools/build-fuxi-wlan5-images.sh BASE_METAL2A_ARTIFACT_DIR`
copies the base ADT unchanged and appends exactly ` fuxiwlan=1` to the base
cmdline (asserting fewer than 608 bytes), then uses mkpayload and the existing
loader/header-v4 packing with the same private GPU firmware/toolchain inputs.
Outputs are `shim/out/fuxi-wlan-m5-{payload.bin,boot.img,loader.elf}` and
`xnu/out-fuxi-wlan5/SHA256SUMS`; none are committed.

Register and sequence references (independent implementation, no imported
third-party driver code): [Linux arm-smmu register definitions](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h),
[Qualcomm SMMU quirks](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c),
[Qualcomm RC RID lookup](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-qcom.c),
[Linux MHI context ABI](https://github.com/torvalds/linux/blob/master/drivers/bus/mhi/common.h),
[MHI firmware loading](https://github.com/torvalds/linux/blob/master/drivers/bus/mhi/host/boot.c),
[MHI state transitions](https://github.com/torvalds/linux/blob/master/drivers/bus/mhi/host/pm.c),
[OpenBSD qwz PCI MHI/FBC sequence](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_qwz_pci.c).

M5 validation: `python3 tools/test-fuxi-wlan-m5.py` compiles the actual M5 C
block against fake MMIO/VFS/DMA with ASan/UBSan. It covers the full download
sequence, 44-byte context stride, vector remainder, cache/BME/doorbell order,
SMMU wildcard/EXID/default/ambiguous/translated/fault paths, RC route gating,
all poll deadlines, RDDM/EDL/SYSERR rejection, wrong BHIe sequence, root/file/
allocation failures, MSI/MSI-X masking, ordered cleanup and buffer quarantine.
It also round-trips a synthetic M5 console report through the decoder. Native
XNU build passed; the existing M4 device log still decodes correctly. These
checks do not establish hardware DMA or firmware success.

## Milestone 6 — owned apps-SMMU translation for WLAN

M6 supersedes M5's physical-bypass requirement and low-DDR prerequisite.
The coordinator's M5 observation is: steps 1–16 passed; `sCR0=0x2d0406`,
`IDR0=0x4c017e69`, `IDR1=0x60000046`; SIDs `0x1400` and `0x1401`
were unmatched and faulted by USFCFG. The abort contained the endpoint and
freed buffers; iOS continued. Those observations establish **stage-1 support,
105 stream matching groups, 70 context banks, 4-KiB register pages, and context
base `0x15080000`**. They do not establish a vacant bank, IDR2 capabilities,
secure write permission, a working RID route, or successful endpoint DMA.
M6 reads these again at runtime and stops on uncertainty. No M6 phone run
was performed during development.

### Addressing and ownership

The saved live DT was inspected offline. `/soc/apps-smmu@15000000` is
`qcom,qsmmu-v500`, register size `0x100000`, with two IOMMU cells. PCIe0 has
`qcom,smmu-sid-base=<0x1400>` and vendor four-cell `iommu-map` entries
`<0 phandle 0x1400 1>` and `<0x100 phandle 0x1401 1>`. The final cell is
range length, **not a wildcard mask**. There is no `iommu-map-mask` override
and no endpoint `iommus` wildcard: match the complete endpoint SID, SMR mask
zero. The cnss IOMMU group requests coherent fastmap, a separate vendor IOVA
pool, and nonfatal faults with stall disabled. M6 owns its own small window
instead of using that unimplemented vendor allocator. The RC SID `0x1400`
remains unchanged; endpoint requests use `0x1401`.

Wired XNU allocations use `kmem_alloc` with ZERO/DATA/NOPAGEWAIT, with no
low-physical-address or contiguity requirement. Every physical page must be
nonzero and below the explicitly configured 36-bit output limit. XNU's RAM
starting at `0x8c0000000` meets that limit. Each 16-KiB XNU page contributes
four independently addressed 4-KiB SMMU pages. Device addresses in BHI, BHIe
vectors, MHI contexts and doorbells are IOVAs in `[0x10000000,0x11000000)`.
Only image, vector and context/ring allocations are mapped; zeroed allocation
padding is included, page tables are excluded. The image bound is now 15 MiB,
leaving room for metadata and tables inside the 16-MiB allocation budget.
Both vendor images documented in M5 fit. No loader/ADT low-DDR reservation or
GPU memory-layout change is needed.

Step 17 reads IDR0/1/2, computes register geometry and checks bounds against
the 1-MiB DT resource. It requires stage-1, stream matching, AArch64 4-KiB
page tables and at least 36-bit input/output physical widths. It rejects
CLIENTPD, EXID mode, invalid ID encodings and inaccessible/all-ones registers.
It scans **all** SMR/S2CR pairs and all CB SCTLRs. A candidate stream must have
SMR.VALID clear and a saved FAULT/BYPASS S2CR default. Any valid exact or masked
match for SID `0x1401` causes refusal. A candidate bank must have SCTLR.M clear,
be stage-1 capable and be unreferenced by **any** S2CR, including an invalid
stream or a bypass/fault entry. CB index is its ASID; enabled banks' TTBR0/1
ASIDs are checked for conflicts. Existing valid streams and enabled or
referenced banks are never written. Vacancy and ASID checks repeat after VFS
and allocation sleeps, immediately before programming. There is no general
apps-SMMU allocator in XNU; this is a single boot-thread ownership protocol.

The original M6 RC policy below is superseded by **M6b**. It checked the table
independently: CRC8 polynomial `0x07` over RID bytes
`01 00` yields slot `0x15`, and the bounded chain must resolve uniquely to
relative SID 1. SID_OFFSET must be zero. An existing enabled correct route
is retained. If the **entire 256-entry table is empty**, the driver owns only
PARF `+0x2054`, writes `0x01000100`, verifies it, then clears only CFG `+0x2c00`
BYPASS bit 0 with readback. A conflicting/nonempty unproven table, duplicate
RID, nonzero offset or readback fault stops at 17. No collision chain or
SID_OFFSET is changed. Original entry/config are saved for abort. This
small RC repair handles the dedicated WLAN RC's reset defaults; it is not a
replacement for general PCI enumeration. CFG/entry/SID_OFFSET and the owned
stream/context are rechecked before BME. The fixed port SID base follows the
live DT and Qualcomm's relative-SID table encoding; the BHI result remains
the endpoint proof of that hardware path.

### What is written

Step 20 builds ten 4-KiB tables in wired high RAM: L1 root, L2, and eight L3
pages. L1[0] references L2; L2[128..135] reference the L3 pages. Only buffer
leaf entries are valid, with RW unprivileged, NS, AF, nG, inner-shareable WB
and XN permissions (`PA | 0x60000000000f63`). Tables and outgoing buffers
are cache-cleaned page by page, followed by DSB SY. Table pointers use each
page's actual PA, including when allocations are physically noncontiguous.

For selected stream `s` and context `c`, let `G=0x15000000`,
`GR1=G+(1<<IDR1.PAGESIZE_shift)`, and `CB=runtime_CBbase+(c<<shift)`:

| Address | Value / purpose |
|---|---|
| `CB+0x00` | 0 while configuring; finally `0x1027` (ASIDPNE, CFRE, AFE, TRE, M; IRQ and stall disabled) |
| `GR1+0x800+4*c` | CBA2R=1, AArch64 format |
| `GR1+4*c` | CBAR=`0x1f300`, stage-1 / stage-2 bypass, weakest override attributes |
| `CB+0x10` | TCR2=`0x38011`, upstream SEP, 16-bit ASID, 36-bit PA |
| `CB+0x30` | TCR=`0x803520`, EPD1, 32-bit IOVA, 4-KiB granule, inner-shareable WB table walk |
| `CB+0x24/+0x20` | TTBR0 high then low: root PA plus ASID `c` in [63:48] |
| `CB+0x2c/+0x28` | TTBR1 high then low: ASID, zero base, disabled by EPD1 |
| `CB+0x38/+0x3c` | MAIR0=`0xff`, MAIR1=0 |
| `CB+0x58` | W1C defined FSR mask `0xc00001fe` once before DMA, only on this owned bank |
| `CB+0x610`, `CB+0x7f0` | TLBIASID=`c`, TLBSYNC=0; poll `CB+0x7f4.ACTIVE` for at most 100 ms |
| `G+0xc00+4*s` | S2CR=`c`, translate with default privilege policy |
| `G+0x800+4*s` | SMR=`0x80001401`, VALID last, exact SID mask zero |

All configuration registers have exact readback checks. TLB operations use
bounded completion polling. No global CR0/USFCFG, global TLB invalidation,
existing context, TBU halt, implementation ACTLR or unrelated SMMU is written.
Secure-firmware restrictions that reject writes stop before BME. Context
fault interrupts are not enabled; the host polls and terminates on faults.

### DMA proof, steps and bisection

The BHI status/cookie is an endpoint MMIO result, not a documented device
write to a host cookie buffer. There is no standalone harmless host-write
operation in the referenced PBL interface. M5's IDR0 also has ATOSNS set, so
M6 does not issue speculative address-translation operations. **BHI's first
512-KiB transfer at 21/22 is the first endpoint translation test**. Step 20
explicitly logs this fallback. A successful BHI/SBL transition with no CB
faults provides the first positive endpoint-read evidence; later MHI event
writes exercise the mapped context/ring buffers. A zero FSR alone is never
reported as DMA success.

M6 keeps steps **1–28** and `fuxiwlan=1` = all, `2..28` = inclusive limit:

| Step | M6 behavior |
|---|---|
| 17 | Runtime capability/ownership scan, exact SID policy, RC route verification or empty-table repair; BME remains off. |
| 18 | Existing endpoint TCSR firmware selection. |
| 19 | Existing root/file/ELF checks; wired high-RAM allocation with low IOVA. |
| 20 | Existing vectors/44-byte contexts/IRQ masking; build tables, re-prove ownership, program and read back stage-1 translation, TLB sync. |
| 21–22 | BHI IOVA transfer and completion: first actual DMA proof. |
| 23–25 | READY, IOVA context bases/event WPs, M0/SBL. |
| 26–28 | Full-image BHIe IOVAs, matching completion, mission EE/M0. |

After each step with an owned bank, log CB FSR/FAR/FSYNR0/1 plus read-only
global GFSR/GFSYNR1. Poll loops also check CB FSR, so a fault ends the current
wait without waiting for success/timeout. Defined fault bits `0xc00001fe`
abort; format-only FSR bits are logged without being misclassified as faults.
An unmatched-stream global fault naming SID `0x1401` also aborts. DMA fault
registers are preserved for diagnosis; shared global faults are never cleared.
The pre-BHIe TXVECSTATUS can be undefined (M5 showed upper status bits set
before any BHIe doorbell). Modes 0–2 therefore ignore that stale status; BHIe
error checks begin only after its own doorbell. The fake device starts with
that M5 value to cover this condition.
The `m6` report has magic `0x574c0006`, IDR2 in word 7, selected SMR/CB in
8/9, final fault snapshot in 80–86, and per-step FSR in 96–107. Existing step
results and firmware/state fields keep their M5 positions. The offline decoder
accepts m3/m4/m5/m6; historical M5 logs remain readable.

Expected lines (values and slot/bank numbers are runtime findings):

```
FUXI WLAN: M6 apps_smmu ... IDR2 ... NUMSMRG 105 NUMCB 70 CBbase 0x15080000
FUXI WLAN: SMMU free SMR ... CB ... SCTLR ... unreferenced; SID 0x1401 mask 0
FUXI WLAN: RC RID 0x100 SID cfg ... offset 0 hash 0x15 entry ... matches ...
FUXI WLAN: RC owned empty RID slot 0x15 -> DT SID 0x1401 (mask 0)
FUXI WLAN: SMMU owned stage1 SMR ... CB ... TTBR ... IOVA 0x10000000-... readback verified
FUXI WLAN: DMA self-test: no standalone BHI host-write cookie; step 21/22 BHI transfer + CB faults is first endpoint proof
FUXI WLAN: BHI doorbell seq ... bytes 0x80000 bus master on
FUXI WLAN: M6 step 22 CB ... FSR 0 FAR ... FSYNR ... GFSR ... GFSYNR1 ...
FUXI WLAN: M6 step 25 EE 0x1 (SBL) ... (M0 ready ...)
FUXI WLAN: BHIe doorbell seq ...
FUXI WLAN: M6 mission mode M0 confirmed ...
FUXI WLAN: m6 r[...]
```

The `RC owned empty` line is conditional; an existing correct route needs no
write. Reboot per limit: **16 → 17 → 18 → 19 → 20 → 21 → 22 → 23 → 24 → 25 →
26 → 27 → 28 → 1**. Inspect 17's ownership/route evidence and 20's readbacks
before proceeding to 21. Stop at the first failure or fault; never proceed
because FSR is zero after a timed-out transfer.

### Abort and undo

On any failure, snapshot endpoint and CB faults, request bounded MHI RESET,
clear endpoint BME and confirm by readback, assert PERST and confirm, then
wait 10 ms for transaction drain. Only after **both containment confirmations**:

1. Invalidate our SMR and verify VALID clear.
2. Restore that slot's saved FAULT/BYPASS S2CR exactly.
3. Disable our CB and verify SCTLR=0.
4. Invalidate our ASID and complete the bounded context TLB sync.
5. Restore the saved invalid SMR bits, then restore our optional RC CFG/entry.
6. Free table, context, vector and image allocations.

Cleanup accesses bypass the normal write-after-fault latch only for these
containment/owned addresses, using the fault-contained accessor. The original
fault counter stays latched. If containment or any undo readback/sync fails,
retain **all** allocations until reboot. No attempt to resume after a failed
partial setup is made. Disabled CB configuration values can remain after a
successful undo; no live route or table walker can reach freed memory.
Deliberate successful partial limits **17–27** use the same containment/undo,
including a route-only step-17 stop. Successful 28 retains the mappings and
all buffers for firmware paging, with no channels or QMI enabled.

### Build and verification

`tools/test-fuxi-wlan-m6.py` compiles the actual C with fake registers/VFS/wired
pages under ASan/UBSan. It checks full sequence and IOVA ABI, every 4-KiB leaf
against noncontiguous high PAs, unmapped guards/tables, occupied and wildcard
streams, active/referenced banks, late ownership changes, RC route conflicts,
readback mismatch, TLB timeout, fault abort, reverse undo ordering, quarantined
allocations, bounded firmware polls and offline decoding. The old M5 test
command forwards to this current-driver suite.
`tools/test-fuxi-wlan-m6-artifacts.py` rebuilds the payload offline, checks
the M5 ADT/cmdline/header, embedded ELF/boot bytes, pinned firmware section
size/hashes, and the complete artifact manifest.

Build only the dedicated WLAN native tree, then copy the stripped/unstripped
kernels to `xnu/out-fuxi-wlan6/`. Run
`tools/build-fuxi-wlan6-images.sh BASE_METAL2A_ARTIFACT_DIR`; it preserves the
M5 base ADT and appends the same single `fuxiwlan=1` argument (under 608 bytes),
using the same payload and header-v4 packing. Outputs:
`shim/out/fuxi-wlan-m6-{boot.img,loader.elf,payload.bin}` and
`xnu/out-fuxi-wlan6/SHA256SUMS`. Build logs and binaries remain ignored.

The loader script now requires **existing absolute paths** for MKBOOTIMG,
FUXI_GMU_FIRMWARE, FUXI_SQE_FIRMWARE and FUXI_GPU_ZAP_MBN. The SQE input is the
pinned `research/firmware/gpu/sheng-firmware/a740_sqe.fw`; GMU/SQE/ZAP hashes
remain pinned in the script. It fails if an input is unset, relative, missing
or has the wrong hash, and verifies the linked ELF `.gpu_fw` section is
**exactly `0x25ac8` bytes** before packing. Firmware is never committed.

Register/layout references: [generic arm-smmu programming](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.c),
[arm-smmu registers](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h),
[AArch64 I/O page tables](https://github.com/torvalds/linux/blob/master/drivers/iommu/io-pgtable-arm.c),
[Qualcomm relative RID-to-SID table](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-qcom.c),
and the MHI/BHI sources linked in M5. These are reference facts, not imported
driver code. Hardware write ownership and endpoint DMA remain to be verified
by the coordinator's fresh-boot bisection.

M6 validation completed: Linux and native-Mac ASan/UBSan host suites passed;
the dedicated native XNU build includes the inherited `93da876` ABI fix
and passed, and the final packed artifacts passed
the offline checks. `.gpu_fw` is `0x25ac8`; M5 ADT/cmdline/header comparisons
are byte-identical (apart from the boot header kernel-size field).

| Final artifact | SHA256 |
|---|---|
| `shim/out/fuxi-wlan-m6-boot.img` | `70fe6e2d129a1d046363604b50bd65e56044a598ddd3040f6b792198b52251ca` |
| `xnu/out-fuxi-wlan6/kernel.development.fuxi` | `67af671fe43aa8a38dc71736d5f41bbeef5c637ac72d871541c837c1040c817e` |
| `shim/out/fuxi-wlan-m6-loader.elf` | `7dfb1d7cf313d587582bc22dbccd09f113827a1755c0a8126892a5f74b784ee1` |

The complete seven-file manifest is in the ignored M6 output directory.
No firmware, captured device console, private input path or host address is
part of this change. These build/host results do not establish device DMA.

## Milestone 6b — initialize the bypassed RC SID table

The coordinator's M6 run reached step 17 after steps 1–16 passed, then refused
a populated RC table with bypass still enabled. The SMMU scan selected an
unreferenced stream/context; no SMMU programming or endpoint DMA occurred.
The failure was the RC table guard, followed by clean containment and buffer
cleanup. M6's requirement for an already-zero reset table was too strict:
contents are undefined while the table is bypassed.

### Encoding and ownership

Linux v6.12's
[qcom_pcie_config_sid_1_9_0](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/dwc/pcie-qcom.c)
zeros all 256 words, hashes the big-endian BDF with MSB CRC8 polynomial `0x07`,
and encodes `BDF << 16 | (SID - iommu-map base) << 8 | next`.
The [SM8550 PCIe0 DT](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/sm8550.dtsi)
maps root BDF 0 to SID `0x1400` and endpoint BDF `0x100` to `0x1401`, both
length 1. This agrees with the saved live/vendor DT's base `0x1400`.
The [downstream PCIe driver](https://android.googlesource.com/kernel/msm/+/41d4470112f489d6824ada4cf0a04587c6f4555d/drivers/pci/controller/pci-msm.c)
also computes its table SID by subtracting `qcom,smmu-sid-base` from the SMMU
SID. Thus the table contains **relative SID 1**, not absolute SID `0x1401`.
The expected SMMU-visible endpoint SID is base `0x1400` plus offset 1.
This follows the source/DT contract; successful endpoint DMA remains the
hardware proof and has not been measured for M6b.

| RID | CRC8 slot | SID offset | Table word | Expected SMMU SID |
|---|---|---|---|---|
| `0` | `0` | `0` | `0` | `0x1400` |
| `0x100` | `0x15` | `1` | `0x01000100` | `0x1401` |

The zero root entry is Linux's encoding, including NEXT=0. It is retained
explicitly by the full-table zero/readback and checked again before SMMU
programming and BME. These two BDFs do not collide, so no chain is needed.
The root SID gets no new SMMU stream or context; only endpoint SID `0x1401`
is translated by this driver. SID_OFFSET must remain zero and is never written.

Step 17 now requires CFG bit 0 **set**, endpoint BME off, disabled MSI/MSI-X
capabilities on root and endpoint, and zero DWC MSI vector enables in all
eight blocks (`DBI+0x828+12*n`). Invalid, cyclic or inaccessible capability
chains also refuse ownership. CFG bit 0 clear refuses **every** table,
including an empty table or an apparently correct existing endpoint route.
The old M6 route-adoption behavior is removed.

With these prerequisites, claim the entire bypassed table, zero all 256
entries, and verify every word. Install and verify the endpoint word at
PARF `+0x2054`, keep root slot 0 zero, then clear CFG bit 0 **last**, preserving
all other CFG bits. Power-on garbage is discarded rather than restored.
Only a successfully installed route permits step 20's SMMU programming.
The entire table, CFG and SID_OFFSET are checked before allocation of SMMU
tables, immediately before SMMU writes after any sleeps, and before step 21 BME.

After endpoint containment and the existing SMMU undo, RC undo restores and
verifies saved CFG with bypass set **before** clearing any owned table word.
It then zeros/verifies all 256 words, including partial scrub/install failures.
A failed bypass restore leaves the table untouched; failed undo quarantines
allocations. Successful step-20 bisection also performs this complete undo.

Step 17 still maps any RC failure to reason 5 (`0x1105`); this is unrelated
to step 20 reason 5's SMMU sync/enable path. A named `RC route refused` line
and report word 25 now preserve the RC subreason; the offline decoder labels it:

| RC subreason | Meaning |
|---|---|
| 1 | Config/offset inaccessible, unsupported or changed |
| 2 | Table in use: bypass clear |
| 3 | MSI/other user active or capability chain unproven |
| 4 | Full-table zero readback failed |
| 5 | Entry or bypass-clear readback failed |

### Build, host verification and expected lines

Linux and native-Mac ASan/UBSan suites passed. They cover bypassed garbage
(including duplicate RID bits), refusal of active/nonempty and active/empty
tables, MSI/MSI-X and DWC vector users, all-word zero verification, install and
CFG readback failures, route changes before SMMU writes, bypass-first undo,
and failed-undo retry. Both C files embedded in patch 0099 match their source.
The native kernel build passed in the dedicated `ci-lane-wlan` tree.

`tools/build-fuxi-wlan6b-images.sh BASE_METAL2A_ARTIFACT_DIR` consumes kernels
in `xnu/out-fuxi-wlan6b/` and the same required absolute-path firmware/toolchain
environment as M6. It packs two images from the same kernel and ADT: full
`fuxiwlan=1` and pre-DMA `fuxiwlan=20` (474/475-byte command lines).
`python3 tools/test-fuxi-wlan-m6-artifacts.py --milestone m6b` passed for both:
payload reconstruction, embedded ELF/boot bytes, base ADT/header, command-line
limits, pinned firmware hashes and `.gpu_fw` size `0x25ac8`.
The eleven-file manifest is `xnu/out-fuxi-wlan6b/SHA256SUMS`.

| Artifact | SHA256 |
|---|---|
| `shim/out/fuxi-wlan-m6b-boot.img` | `4fac2e4687731c4cd2c1a336cb42146da333134034bd7685a9d3f05844cf78de` |
| `shim/out/fuxi-wlan-m6b-s20-boot.img` | `78897e6a63e20121f58c622a6feb436e7985ba528e0cbed4505b1b298a5b9c79` |
| `xnu/out-fuxi-wlan6b/kernel.development.fuxi` | `9be8866b2a115959993ff3d9d1e6de4286ba60832644ddaf254363d07331cc85` |

Coordinator: use the **s20 image first** on a fresh boot. Expected templates:

```text
FUXI WLAN: M6b RC bypassed table zeroed 256 entries, readback verified; MSI idle
FUXI WLAN: M6b RC route installed: RID 0 -> DT SID 0x1400 offset 0 slot 0; RID 0x100 -> DT SID 0x1401 offset 1 slot 0x15 entry 0x1000100; bypass clear last
FUXI WLAN: step 17 ok code 0x1011 (... ms, faults 0)
FUXI WLAN: SMMU owned stage1 SMR ... CB ... TTBR ... IOVA ... readback verified
FUXI WLAN: step 20 ok code 0x1014 (... ms, faults 0)
FUXI WLAN: SMMU undo SMR invalid, S2CR restored, CB disabled, TLBI synced
FUXI WLAN: M6b RC undo bypass restored first, 256 owned entries zeroed
FUXI WLAN: abort buffers freed
```

The s20 image must show no BHI doorbell or bus-master-on line. Check all CB
fault snapshots and cleanup before continuing with fresh-boot limits 21–28
and finally the full image. On a pre-existing active table expect
`RC route refused: RC table in use (bypass clear)` and step 17 `0x1105`,
with no table/SMMU programming. No M6b phone run was performed by this lane.

## Milestone 6c — diagnose step 20 and probe before claiming a CB

M6b reached step 20's configuration-readback failure after steps 17–19
succeeded; containment and undo completed. That failure does **not** identify
which register rejected the write. No M6c device run was performed. The new
s20 image is for a fresh-boot pre-DMA diagnosis.

### Sharing, ownership and bank selection

[Qualcomm's access-control description, pp. 6–7](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/an-introduction-to-access-control-on-qualcomm-snapdragon-platforms.pdf)
assigns secure banks to TrustZone, non-secure stream mapping and stage 2 to
the hypervisor, and the OS's stage-1 contexts to HLOS. Thus an address inside
the CB aperture, or SCTLR.M=0, does not grant HLOS write access. Secure or
hyp-reserved banks must not be reconfigured. This is the Qualcomm hyp/Gunyah
sharing concern; the WLAN evidence does not identify the installed hypervisor
or establish its per-bank access policy.

There is **no proven numeric HLOS CB allowlist** for this boot. NUMCB=70 is
an architectural count, not an ownership grant. In
[Linux v6.12's generic driver](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu.c),
stage 1 allocates the first zero bit in `context_map`, starting at NUMS2CB;
its ASID is the selected CB index. Probe order and existing reservations
therefore affect the resulting bank number. It does not imply “WLAN uses
CB 6” or “all higher CBs are HLOS-owned.”

The [Qualcomm downstream driver](https://android.googlesource.com/kernel/msm.git/+/89594f79eb3779e02c47b5fd47427c55497cd5c9/drivers/iommu/arm-smmu.c)
implements `arm_smmu_handoff_cbs`: read valid SMRs and their S2CRs, reserve
translating CBs in the bitmap, and retain handoff metadata. Allocation can
reuse a matching handoff or find the first free bank after NUMS2CB. Its
`qcom,enable-static-cb` path instead requires the firmware assignment. These
are distinct policies; do not conflate either with a fixed SoC bank partition.

In the [upstream Qualcomm implementation reviewed at this revision](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c),
`qcom_smmu_cfg_probe` tests an S2CR BYPASS write. Some hyp versions ignore
FAULT writes and turn BYPASS into FAULT; Linux reserves the last CB to emulate
bypass when required. It imports valid bootloader SMRs into software, with
initial software S2CR bypass metadata. **It does not test CBAR writeability or
reserve every bootloader CB through that loop.** The source describes hyp
trap/emulation problems, not a CBAR-based HLOS discovery algorithm. M6c does
not copy Linux's global reset or destructive shared-register quirk probe.
A successful Linux CB assignment would be evidence for that runtime, but
there is no such assignment trace in this lane's input.

The downstream `qcom,actlr` property contains SID/mask/value triples used to
select ACTLR for an assigned domain, followed by a context TLB flush when
changed. It is a tuning policy, not a CB ownership list. The
[SM8550/Kalama vendor DT](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/43b5ada5654ed84c25f74789bc8f6326b7bacd20/qcom/msm-arm-smmu-kalama.dtsi)
contains camera/compute/display tuning and a handoff-stream property; its
apps ACTLR table has no match for WLAN SID `0x1401`. The reviewed upstream
implementation selects ACTLR through a client-compatible table where
configured. M6c leaves ACTLR and all shared implementation registers alone.

The [MMU-500 TRM, §3.1–3.2](https://documentation-service.arm.com/static/5e908c70c8052b1608761996)
specifies UNKNOWN reset values unless stated otherwise and distinguishes
undefined bits from writable fields. Garbage SCTLR/FAR/FSYNR values alone
therefore do not prove secure/hyp ownership. Conversely, failure to read back
a legal write is grounds to reject a candidate, regardless of the cause.
The TRM permits word-aligned 32-bit writes to either word of a 64-bit register;
our split TTBR writes occur with M=0. Linux uses `writeq` for long descriptors.

### Register-format audit

Compared against [Linux's register definitions and TCR constructors](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu.h)
and [SMMUv2 IHI0062D.c, chapters 10 and 16](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb):

| Register | M6c value / interpretation |
|---|---|
| TCR | `0x803520`: EPD1=1, A1=0, TG0=4 KiB, SH0=inner, ORGN0/IRGN0=WBWA, T0SZ=32. No AArch32 EAE bit. |
| TCR2 | `0x38011`: SEP=7 upstream, AS=1 (16-bit ASIDs), PASIZE=1 (36-bit output). Reserved bits zero. |
| TTBR0 | Root PA OR `CB << 48`; table aligned to 4 KiB, ASID in bits 63:48. |
| TTBR1 | Same ASID, base zero; walks disabled by EPD1. |
| MAIR0/1 | `0xff` / `0`: AttrIndx 0 is normal inner/outer WBWA; every mapped leaf selects 0. Unused slots zero. |
| CBA2R | `1`: VA64 set; VMID16 and MONC zero. |
| CBAR | `0x1f300`: TYPE=1 (S1 translate/S2 bypass), MEMATTR=15, BPSHCFG=3, VMID=0, IRPTNDX=0. |
| SCTLR | Disable `0`; enable `0x1027`: ASIDPNE, CFRE, AFE, TRE, M; IRQ/stall off. |

These CBAR/CBA2R fields follow Linux's stage-1 setup. SMMUv2 fixes the
context interrupt index to the CB, so Linux sets CBAR.IRPTNDX only on v1.
Linux enables CFIE; this polling lane omits it. Its
[page-table implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/io-pgtable-arm.c)
uses the same cache/shareability encodings; M6c needs only one MAIR entry.
No format change or relaxed full-word readback is justified by the unnamed
M6b failure. Hyp may still reject GR1 writes despite a writable CB register.

### Bounded probe, diagnostics and undo

Step 17 still selects a tentative CB with read-only SMMU access. In step 20,
after table allocation and the existing late-vacancy checks, try at most
**eight eligible candidates**, in ascending order. Before each probe, recheck
all SMRs/S2CRs, saved invalid slot, global config, RC table, BME-off state,
SCTLR.M=0, NUMS2CB exclusion and active-bank ASID use. Any S2CR reference,
even with an invalid SMR or non-translating type, excludes that CB. Valid SMRs
are never changed. Shared-state uncertainty aborts.

Write **zero only to CB_TCR2**, then read it back. This neither enables a
walker nor changes stream/GR1 ownership. On mismatch, mark that bank unusable
for the boot, log it, and try the next eligible CB. A fault-contained MMIO
exception stops immediately. A rejected bank receives no other writes or
claimed-bank undo; its only attempted change is the harmless TCR2 zero.
After a passing probe, recheck vacancy and unchanged SCTLR before claiming.
A zero readback can also come from RAZ/WI emulation: it is a filter, not proof
of ownership. Full programming readbacks are mandatory; failure after claim
aborts with the existing containment/undo, without trying another bank.

All ordinary configuration writes now name their register, SMMU-relative
byte offset (including page/bank), written value, actual readback and fault
count on mismatch. Diagnostics cap at 16 lines; this path normally produces
at most eight probe mismatches plus one claimed-bank mismatch. FSR W1C checks
that defined fault bits clear, excluding format bits. TLBI/TLBSYNC are command
registers: retain the bounded TLBSTATUS completion check rather than comparing
a command readback to the written value. The accessor uses `str w4`, not
`str wzr`, so zero writes do not depend on Qualcomm's documented zero-register
trap quirk. All prior containment, bypass-first RC undo and allocation
quarantine rules remain in force.

Step-20 reason 6 (`0x1406`) means no candidate passed within the bound, or a
probe MMIO fault. Reasons 3 (changed ownership), 4 (CB/GR1 config mismatch)
and 5 (fault/sync/enable/stream install) retain their meanings. Report word 9
is updated to the accepted CB; existing M6 report/decoder layout is unchanged.

### Verification, artifacts and expected lines

Linux and native-Mac ASan/UBSan host suites pass, including eight-rejection
bounds, rejected-bank write isolation, per-boot exclusion, references/M/ASIDs
changing between candidates, ownership changing during a probe, probe MMIO
faults, RAZ/WI false acceptance, every named configuration mismatch (including
zero writes), FSR W1C and the diagnostic cap. Existing routing, mapping, DMA
containment, undo, quarantine and decoder tests still pass. Patch 0099 embeds
both exact C sources. The native kernel build passed in `ci-lane-wlan`.

`tools/build-fuxi-wlan6c-images.sh BASE_METAL2A_ARTIFACT_DIR` consumes the
stripped/unstripped kernels in `xnu/out-fuxi-wlan6c/`. It uses the same required
absolute firmware/tool paths as M6b, preserves ADT/header, and packs `fuxiwlan=1`
and `fuxiwlan=20` variants. Both pass
`python3 tools/test-fuxi-wlan-m6-artifacts.py --milestone m6c`, including payload
reconstruction, pinned firmware hashes, `.gpu_fw=0x25ac8` and the eleven-file
`xnu/out-fuxi-wlan6c/SHA256SUMS` manifest. Binaries/logs/private inputs are ignored.

Use the **s20 image first**. Expected templates (bank numbers and readbacks
are runtime findings, not predictions):

```text
FUXI WLAN: M6c CB probe 1/8 CB ... SCTLR ... M=0 unreferenced; TCR2 zero
FUXI WLAN: M6c SMMU mismatch CB ... CB_TCR2 probe offset ... wrote 0 read ... faults 0
FUXI WLAN: M6c CB ... unusable; probe failed; next candidate
FUXI WLAN: M6c CB probe 2/8 CB ... SCTLR ... M=0 unreferenced; TCR2 zero
FUXI WLAN: M6c CB ... probe accepted; claim pending full CB/GR1 readbacks
FUXI WLAN: SMMU owned stage1 SMR ... CB ... TTBR ... IOVA ... readback verified
FUXI WLAN: step 20 ok code 0x1014 (... ms, faults 0)
FUXI WLAN: SMMU undo SMR invalid, S2CR restored, CB disabled, TLBI synced
FUXI WLAN: M6b RC undo bypass restored first, 256 owned entries zeroed
FUXI WLAN: abort buffers freed
```

Rejected-bank lines are conditional. If a probe passes but configuration
fails, expect `M6c SMMU mismatch ... CB_SCTLR disable|GR1_CBA2R|GR1_CBAR|...`
and step 20 `0x1404`, then undo. If all eight probes fail, expect
`M6c CB probe exhausted 8 candidates (limit 8); no bank claimed`, `0x1406`,
RC undo and buffer release. The s20 image must produce no BHI doorbell or
bus-master-on line. Inspect the named failure, CB fault snapshot and complete
undo before any fresh-boot step 21+ test. Host/build success proves no DMA.

| M6c artifact | SHA256 |
|---|---|
| `xnu/out-fuxi-wlan6c/kernel.development.fuxi` | `ea1bf3582173cda80b48dea738e4cfc547ec7b595e0277eef92926e9a3857fe1` |
| `shim/out/fuxi-wlan-m6c-boot.img` | `17ae60ae0870778ce4cc1fe80859b981bf09586e224418edc038d0cddbbbcc3a` |
| `shim/out/fuxi-wlan-m6c-s20-boot.img` | `498b5c26167eb48fc9af0aae6a840e4d94046de50e1a08476073c12fcb48fc7c` |

## Milestone 6d — verify writable fields, retain raw readbacks

This supersedes M6c's exact-word probe/configuration criterion. The supplied
M6c s20 result rejected eight banks with the same TCR2 readback, `0x60`, after
writing zero. That establishes the readback problem; it does not establish
that those banks are unwritable. M6b's unnamed step-20 reason 4 could have
been the same check, but that remains an inference. No M6d device run was
performed by this lane.

### Architectural and implementation evidence

[Arm IHI 0062D.c §16.5.39, pp. 16-336–338](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb)
defines the SMMUv2 TCR2 fields: NSCFG1[30], SEP[17:15], NSCFG0[14], HD[11],
HA[10], HAD1[9], HAD0[8], **TBI1[6], TBI0[5]**, AS[4], PASize[2:0]. Other
positions are reserved. TBI controls top-byte ignoring for TTBR1/TTBR0;
the specification limits its effect to address-translation operations that
can supply full tagged addresses. These are named architectural fields,
not universal RES1 bits. This lane uses untagged 32-bit IOVAs and no ATS.

[MMU-500 r2p4 TRM, DDI 0517F §3.1](https://documentation-service.arm.com/static/5e908ca6c8052b1608761ab4)
delegates architectural registers to the SMMU specification and directs
software to ignore undefined bits on reads; its public register descriptions
do not specify a Qualcomm TBI implementation. Concrete implementation
evidence is [AMD UG1087's MMU-500 TCR2 description](https://docs.amd.com/r/en-US/ug1087-zynq-ultrascale-registers/SMMU_CB4_TCR2-SMMU500-Register?contentId=cRVpcVV~QxLqktcjeot3sQ):
TBI1 and TBI0 are read-only, reset one, with register reset `0x60`. This
corroborates the supplied behavior without claiming AMD's register map is
the SM8550 contract. M6d therefore excludes both bits from comparisons.

[Linux v6.12 arm-smmu.h](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu.h)
defines `ARM_SMMU_TCR2_SEP` as [17:15], `ARM_SMMU_TCR2_AS` as bit 4 and
`ARM_SMMU_TCR2_PASIZE` as [3:0]. Its constructor sets SEP/PASize, and AArch64
setup adds AS; it defines no TBI controls. Linux's PASIZE macro includes
reserved bit 3; M6d follows the architectural [2:0] field instead.
[arm_smmu_write_context_bank](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu.c)
writes TCR2 before TCR/TTBRs, with TBI bits zero, and performs no TCR2 read
or readback verification. This statement concerns the reviewed generic
initialization path, not every downstream debug/register-dump facility.

### Per-register audit and behavior

The following masks apply to the lane's Non-secure AArch64 stage-1 format.
Field layouts follow IHI 0062D.c §§10.2, 16.5.30, 16.5.38–40 above.

| Register | Compared mask / treatment |
|---|---|
| CB_TCR2, including probe | `0x38017`: SEP, AS, PASize; excludes `0x60`, Secure-only NSCFG and reserved positions. Add HAD[1:0] only with IDR2.HADS; HA/HD only with IDR1.HAFDBS support. Reject reserved HAFDBS encoding 2. |
| GR1_CBAR, TYPE=1 | `0x00ffffff`; omit implementation-dependent v2 IRPTNDX storage. With VMID16 enabled, omit RES0 VMID[7:0] (`0x00ffff00`). |
| GR1_CBA2R | VA64 (`0x1`); add VMID16[31:16] only when implemented and globally enabled. MONC is ignored for Non-secure banks; [15:2] are reserved. |
| CB_TCR | `0xffffffbf`, excluding reserved bit 6 in AArch64 format. |
| CB_TTBR0/1 low | `0xffffffe0`: both regions now have TnSZ=32, 4-KiB granule, four L1 entries and x=5. Check address bits [11:5] too; allocation alignment alone does not make them safe to ignore. |
| CB_TTBR0/1 high | All ASID[15:0], plus implemented PA bits through min(IAS,OAS); `0xffff000f` on a 36-bit implementation. |
| CB_MAIR0/1 | `0xffffffff`: every attribute byte is writable; no relaxation. |
| CB_SCTLR | `0x7fdfc7f9`: omit reserved [31,21,13,11], optional ASIDPNE hint and long-descriptor UNK/SBOP AFE/TRE. M, fault, stall, interrupt and endian controls remain checked. |
| CB_CONTEXTIDR | Ignored for AArch64 ASIDs; untouched. ASIDs remain in TTBR[63:48]. |
| CB_ACTLR | Implementation tuning; untouched. |

The [MMU-500 SCTLR integration reference](https://docs.amd.com/r/en-US/ug1087-zynq-ultrascale-registers/SMMU_CB5_SCTLR-SMMU500-Register?contentId=X0v~MEaZMBYXfMuBn6LauQ)
also marks ASIDPNE read-only zero. Its optional private-namespace hint is
excluded without weakening ASID isolation: the existing enabled-bank ASID
collision checks remain mandatory. AFE/TRE writes stay Linux-compatible.
M6d changes TCR from `0x803520` to `0x80a03520` solely to give the disabled
TTBR1 region legal TG1=4-KiB and T1SZ=32 settings. EPD1 remains set; the
active TTBR0 format and mapping are unchanged.

Ordinary verification compares `(read & mask) == (wrote & mask)` and prints
name, offset, written value, **full raw read**, mask and fault count on both
success and failure. FSR keeps its separate W1C clear check; TLBI/TLBSYNC
keep completion polling. Shared GR0 stream installation/restore and RC
ownership checks retain their exact comparisons. SCTLR uses its mask at
disable, enable, pre-DMA validation and cleanup. Cleanup still tolerates an
already latched MMIO fault but rejects a new read fault, a writable-bit
mismatch or an uncleared M bit, retaining allocations in quarantine.

A masked-zero TCR2 probe still filters candidates rather than proving
ownership: RAZ/WI can pass zero. All full-programming checks remain required
before a stream is installed. A writable mismatch still rejects a candidate;
a mismatch after claim still aborts without fallback. The eight-candidate
bound, repeated ownership checks, DMA gating and undo order are unchanged.

### Verification and artifacts

Linux and native-Mac ASan/UBSan host suites pass. New cases cover TBI=one
on all eight candidates (first eligible bank accepted), raw `0x60` and
`0x38071` logs, every compared bit in the CB/GR1 programming sequence,
ignored bits, zero writes, optional extension/VMID16 fields, RAZ/WI after
probe acceptance, SCTLR pre-DMA/cleanup and stuck-M quarantine. Historical
routing, mapping, bounded retry, fault, containment and decoder cases pass.
Patch 0099 embeds the exact tested C sources. The native kernel build passed
in `ci-lane-wlan`; this is build/host verification, not endpoint DMA proof.

`tools/build-fuxi-wlan6d-images.sh BASE_METAL2A_ARTIFACT_DIR` consumes the
stripped/unstripped kernels in `xnu/out-fuxi-wlan6d/`, preserves the M6c
ADT/header/base command line and requires the same pinned GPU firmware via
absolute environment paths. It packs `fuxiwlan=20` and `fuxiwlan=1` images.
`python3 tools/test-fuxi-wlan-m6-artifacts.py --milestone m6d` verifies payload
reconstruction, both boot headers, firmware hashes, `.gpu_fw=0x25ac8` and
every entry in `xnu/out-fuxi-wlan6d/SHA256SUMS`. Private input, firmware,
binaries and build logs remain ignored.

Use the **s20 image first**, on a fresh boot. Expected templates if the
reported TBI read behavior persists and writable fields program correctly:

```text
FUXI WLAN: M6d CB probe 1/8 CB ... SCTLR ... M=0 unreferenced; TCR2 zero
FUXI WLAN: M6d SMMU readback CB ... CB_TCR2 probe offset ... wrote 0 read 0x60 mask 0x38017 faults 0
FUXI WLAN: M6d CB ... probe accepted; claim pending full CB/GR1 readbacks
FUXI WLAN: M6d SMMU readback CB ... CB_TCR2 offset ... wrote 0x38011 read 0x38071 mask 0x38017 faults 0
FUXI WLAN: SMMU owned stage1 SMR ... CB ... TTBR ... IOVA ... readback verified
FUXI WLAN: step 20 ok code 0x1014 (... ms, faults 0)
FUXI WLAN: M6d SMMU undo CB ... CB_SCTLR offset ... wrote 0 read ... mask 0x7fdfc7f9 faults 0
FUXI WLAN: SMMU undo SMR invalid, S2CR restored, CB disabled, TLBI synced
FUXI WLAN: M6b RC undo bypass restored first, 256 owned entries zeroed
FUXI WLAN: abort buffers freed
```

A writable-bit failure instead produces `M6d SMMU mismatch ... mask ...`:
probe exhaustion remains `0x1406`, full configuration failure `0x1404`,
and enable/fault/sync/stream failure `0x1405`. The s20 run must contain no
BHI doorbell or bus-master-on line. Inspect all readbacks and complete undo
before considering the full image; full-image transfer/M0 behavior remains
unmeasured in this lane.

| M6d artifact | SHA256 |
|---|---|
| `xnu/out-fuxi-wlan6d/kernel.development.fuxi` | `23524a3a3f9fc6d79256f43734f09b7731be097d89db2eb3d5c0244b2143387a` |
| `xnu/out-fuxi-wlan6d/kernel.development.fuxi.unstripped` | `326a1d80d40242043482a95ef9aa4465a2fe461d74ab78525a76d75ebe49c125` |
| `shim/out/fuxi-wlan-m6d-s20-boot.img` | `a4cd9c45f912eac1c8da1b097691e82d0db63b2704873a27ad5c0ea328c65838` |
| `shim/out/fuxi-wlan-m6d-boot.img` | `ed23bd7105feaaa2515af63beb3febbd28bd9651dde0e8c1eb30a3cb59dd8a98` |

## 16. Milestone 6e: distinguish PBL rejection from inaccessible transport

M6e adds BHI setup verification and two read-only step-22 snapshots. It does
not claim to fix the firmware rejection or prove successful endpoint DMA.
No device run is part of this milestone.

### Error interpretation and source comparison

Linux defines BHI_STATUS bits [31:30] as the transfer status: reset 0,
success 2, error 3. Thus raw all-ones extracts as error 3, but it also has
the usual inaccessible-MMIO ambiguity. Linux prints ERRCODE and ERRDBG1–3
without decoding the private ROM error namespace. The public upstream and
Xiaomi vendor sources examined contain no decode of `0xef0e0003`; exact-code
public searches found none either. Authentication, ELF validation and DMA
failure remain possible explanations, not identified meanings of that code.
See [Linux v6.6 BHI definitions](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/common.h#L42-L71),
[upstream BHI loader](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/boot.c#L210-L275)
and [Xiaomi SM8550 vendor BHI loader](https://github.com/LineageOS/android_kernel_xiaomi_sm8550/blob/2cac706017fd8fd44a1d6a27855ef1b685ce4ea2/drivers/bus/mhi/host/boot.c).
The vendor function is named `mhi_fw_load_bhi`; older vendor trees use
`mhi_fw_load_sbl` for the same BHI download, not a different image format.

The supplied M6d evidence has non-all-ones EXECENV, MHISTATUS, MHICTRL,
ERRCODE, ERRDBG and endpoint command/status in the same snapshot as the
all-ones BHI_STATUS. This supports a PBL-reported failure with other registers
still readable, rather than a sustained whole-BAR/config disappearance.
Those reads were sequential: a transient link loss/reset remains unexcluded.
Neither the unchanged address/size in ERRDBG nor the lack of a translation
fault proves that all 512 KiB were fetched correctly. CB FSR bit 10 is the
format indicator; only the existing fault mask is used for failure detection.
Raw device logs remain outside git.

### Firmware, layout and ordering audit

The filename rule is now supported by
[`cnss_pci_update_fw_name` and `cnss_get_mhi_soc_info`](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/959e4811619b8392b0a95711f86a95e6c58872a2/qcom/opensource/wlan/platform/cnss2/pci.c):
KIWI major version 2 selects `kiwi/amss20.bin`; other majors select
`kiwi/amss.bin`. CNSS extracts bits [15:8], then keeps the low four bits of
the major version. This matches our selector. The startup message now says
`CNSS kiwi major-version rule` instead of filename inference.
Upstream [ath12k MHI configuration](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/mhi.c)
uses FBC, 512-KiB SBL size and 512-KiB BHIe segments. Its firmware directory
is [WCN7850/hw2.0](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/hw.c),
where the image is named `amss.bin`. The upstream basename is not evidence
that the phone's two different vendor binaries can be interchanged.

Offline analysis of the read-only inputs:

| Field | vendor `amss20.bin` | vendor `amss.bin` |
|---|---|---|
| Bytes | 5,890,112 | 7,458,880 |
| ELF | ELF32, little-endian, ET_EXEC, EM_ARM, version 1 | same |
| Header / program entry bytes | 52 / 32 | 52 / 32 |
| Program table offset / entries | 52 / 23 | 52 / 25 |
| Entry address | `0x17b4c00` | `0x1794c00` |
| Header pseudo-segment bytes | `0x314` | `0x354` |
| Hash pseudo-segment offset / bytes | `0x1000` / `0x308` | `0x1000` / `0x348` |
| Hash flags / alignment | `0x02200000` / `0x1000` | same |
| Embedded variant identity | WCN7850 eval v2 | WCN7850 eval v1 |

Each hash segment has a 40-byte version-5 header followed by one 32-byte
SHA-256 entry per program header. Every nonzero entry matches the corresponding
file range, including ELF/header metadata. The hash segment's own slot and an
empty segment's slot are zero. This checks internal file consistency, not ROM
acceptance or signing policy. Firmware bytes, OEM strings and build paths are
not included here or in git.

For FBC, [Linux's firmware handler](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/boot.c#L360-L496)
copies `min(sbl_size, firmware_size)` bytes from file offset zero into a DMA
buffer. Our full image is read from file offset zero into the IOVA allocation;
BHI receives its start and `0x80000` bytes, not an ELF PT_LOAD address, segment
contents, or the BHIe vector table. Both inputs exceed that size. The vendor
loader does the same first transfer. Its special TME path excludes the initial
SBL from the later BHIe transfer only for TME-enabled devices; CNSS enables
that path for PEACH, not KIWI.

BHI offsets relative to BHIOFF remain LOW `+8`, HIGH `+0xc`, SIZE `+0x10`,
TXDB `+0x18`, INTVEC `+0x20`, EE `+0x28`, STATUS `+0x2c`, ERRCODE `+0x30`,
DBG1–3 `+0x34..+0x3c`. Linux **does write STATUS=0**, then address HIGH,
LOW, size and finally doorbell. M6d already used those values and that order.
M6e additionally reads back address, size, INTVEC, STATUS and EE before BME;
a mismatch fails step 21 reason 7 without ringing the doorbell.

[Linux power-up and READY transitions](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/pm.c)
set INTVEC=0 before loading, check a load-capable EE (PBL or EDL), then wait
for BHI completion. They do not force MHICTRL to BHI or M0 before the first
transfer. After download they wait for reset-clear and READY, program contexts
and ring pointers, then request M0. SBL EE later triggers BHIe. Our PBL-only
precondition and subsequent bounded READY/SBL polling preserve this sequence.

Linux enables its host IRQ and normally gets an EE-change wakeup, whereas this
lane polls registers and implements the transitions itself. INTVEC=0 selects
vector zero; it does not disable MSI. Endpoint MSI/MSI-X and legacy INTx remain
disabled here. The public loader does not require a host interrupt ACK to fetch
the image or publish status, and the supplied error response is evidence of
PBL processing with interrupts disabled. There is no justification yet to add
MSI just to address this failure; successful later transitions still need device
validation.

### Cacheability, coherence and alignment

The vendor DT marks both
[PCIe0](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/kalama-pcie.dtsi)
and [apps_smmu](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/13913424c9d0af031a8b8f9981c003bbb9632f73/qcom/msm-arm-smmu-kalama.dtsi)
`dma-coherent`. These describe endpoint transactions and table walks separately;
MAIR alone does not establish coherence. Our MAIR0 index 0 is `0xff` (normal
inner/outer WB), PTEs select that index with inner shareability, and TCR requests
cacheable shareable walks. No speculative attribute change is made.

The image, vectors and contexts are flushed page by page using physical
addresses and `flush_dcache64`, followed by `dsb sy`. The page tables receive
the same treatment after every PTE has been populated and before TTBR/enable.
The image is cleaned again immediately before BME/doorbell. MMIO stores have
an outer-shareable store barrier, and the added setup readbacks drain/check
posted writes. Host tests now require **every backing page** of all four
allocations to have been cleaned before BME.

The IOVA image base is 4-KiB aligned (also 16-KiB and 512-KiB aligned for the
initial allocation); the first size is exactly 512 KiB. Wired physical pages
may be noncontiguous: the endpoint sees a contiguous IOVA range. The Linux
BHI loader has no extra ELF-segment alignment rule beyond its DMA allocation;
we retain the checked page mappings and explicitly reject a misaligned BHI
address. No CPU pointer or physical address is substituted for the IOVA.

### Step-22 diagnostics and report extension

After polling, step 22 takes sample 0, sleeps 10 ms, and takes sample 1 before
returning or entering cleanup. This repeats reads only, never the transfer.
Both samples print raw STATUS and its field, ERRCODE, ERRDBG1–3, EXECENV,
MHISTATUS/MHICTRL, REGLEN/MHICFG, PARF LTSSM, ELBI SYS_STTS, root-port link
control/status and command/status, endpoint ID/command/status/BAR0, PMCSR,
PERST GPIO and root/endpoint AER status. Capability traversal is bounded and
read-only; missing/unreadable optional PM/AER fields print all-ones and are
not interpreted as D3 or AER faults. No serial or OEM-key registers are read.
Offsets and ELBI link bit follow
[the vendor PCIe controller](https://github.com/LineageOS/android_kernel_xiaomi_sm8550/blob/2cac706017fd8fd44a1d6a27855ef1b685ce4ea2/drivers/pci/controller/pci-msm.c):
PARF LTSSM `+0x1b0`, ELBI SYS_STTS `+8`, XMLH_LINK_UP bit 10.

The classification is evidence, not a private-code decode:

| Flag | Meaning |
|---|---|
| `0x001` | BHI_STATUS alone is all-ones |
| `0x002` | essential BAR register read is all-ones |
| `0x004` | endpoint ID disappeared/changed |
| `0x008` | root link status unavailable, DLLLA clear, or ELBI link-up clear |
| `0x010` | endpoint memory decode/BAR assignment changed |
| `0x020` | PERST asserted/unreadable |
| `0x040` | present/readable PMCSR reports non-D0 |
| `0x080` | transport checks readable, EE=PBL, status field=error, real nonzero ERRCODE |
| `0x100` | fault-contained MMIO access fault latched |
| `0x200` | optional capability chain invalid/unreadable or traversal limit reached |

A transport flag in either sample fails step 22 reason 5 (`0x1605`), retaining
SMMU reason 3 if already present. A PBL error stays reason 1 (`0x1601`).
All-ones STATUS with readable transport and a real error code may therefore
produce flags `0x81`, not a spurious link-down conclusion. A late error/reset
or transport loss cannot turn an earlier poll success into step-22 OK.
Normal abort/containment and SMMU undo remain in force.

Report word 108 is extension marker `0x4d366500`; 109 is latest flags;
110–123 are LTSSM, ELBI, RC link, RC command/status, EP ID, BAR low/high,
PMCSR, RC AER uncorrectable/correctable/root, EP AER uncorrectable/correctable,
and PERST. Words 124/125 retain sample-0 BHI_STATUS/ERRCODE; 126 retains
sample-0 flags; 127 retains the original poll result. Existing words 64–73
hold the latest MHI/BHI snapshot. The offline decoder recognizes this marker
without changing historical M5/M6 reports.

### Validation, packing and expected output

Linux ASan/UBSan host tests pass. Added cases cover an isolated all-ones BHI
status with a real error, independently lost BAR/config/link/decode, ELBI link
down, PERST and D3, PM/AER traversal, cyclic capability chains, MMIO faults,
late link loss/error during the repeat, setup corruption before BME, the CNSS
v1/v2 selector, whole-image prefix and page-by-page cleaning. Diagnostics are
asserted to issue no hardware writes. The embedded integration-patch sources
must still match both standalone C sources byte for byte.

`tools/build-fuxi-wlan6e-images.sh BASE_METAL2A_ARTIFACT_DIR` packs the kernels
from `xnu/out-fuxi-wlan6e/` with the existing ADT/header and pinned GPU firmware.
As with M6d, all private packing/firmware environment variables must reference
existing absolute files. It emits the full `fuxiwlan=1` image and the
`fuxiwlan=22` stop-after-BHI image. The latter **does initiate DMA** and stops
after step 22 only on success; failure uses the normal abort path.
`tools/test-fuxi-wlan-m6-artifacts.py --milestone m6e` checks both payloads,
loader ELF sections, GPU hashes, boot header, command lines and every manifest
hash. Firmware and private build inputs stay ignored.

Expected new messages (templates, not captured device logs):

```text
FUXI WLAN: TCSR_SOC_HW_VERSION ... firmware ... (CNSS kiwi major-version rule)
FUXI WLAN: M6e BHI layout file offset 0 ELF ident 7f454c46 class 1 phoff 0x34 phnum 23 first 0x80000 bytes; no segment extraction
FUXI WLAN: M6e BHI preflight addr 0:10000000 size 0x80000 INTVEC 0 STATUS 0 EE 0 image/tables cache-clean + dsb sy
FUXI WLAN: M6e BHI sample 0 STATUS raw ... field ... ERRCODE ... ERRDBG ... EXECENV ... REGLEN ... MHICFG ... flags ...: ...
FUXI WLAN: M6e link sample 0 PARF_LTSSM ... ELBI_SYS_STTS ... RC_LNKCTLSTA ... RCcmd/status ... EPid ... EPcmd/status ... BAR0 ... PMCSR ... PERST_IO ...
FUXI WLAN: M6e AER sample 0 RC uncor/cor/root ... EP uncor/cor ...
FUXI WLAN: M6e BHI sample 1 ...
FUXI WLAN: M6e link sample 1 ...
FUXI WLAN: M6e AER sample 1 ...
```

Native kernel build, native-Mac host tests, final image packing and artifact
hashes are pending access to the dedicated Mac build host. The implementation
and Linux host verification above are complete; no M6e boot image is claimed
as built or device-validated yet.

## M6f: live BHI mapping verification and KIWI bootloader logs

M6e's result establishes that PBL processed the doorbell and copied the BHI
address/length into its error registers. It does **not** establish that a PCIe
memory read reached the intended DDR bytes, or identify an authentication
failure. All-ones STATUS remains ambiguous in isolation; the independent
transport checks and error code establish the narrower PBL-error hypothesis.
M6f does not label the private error as a bad signature or invalid ELF.

### Reference audit

The pinned sources used for this comparison are
[upstream MHI boot.c](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/boot.c),
[MHI pm.c](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/pm.c),
[ath12k mhi.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/mhi.c),
[ath12k pci.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/ath/ath12k/pci.c),
[Xiaomi CNSS pci.c](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/pci.c),
[CNSS register definitions](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/reg.h),
and the [Xiaomi MHI boot handler](https://github.com/LineageOS/android_kernel_xiaomi_sm8550/blob/2cac706017fd8fd44a1d6a27855ef1b685ce4ea2/drivers/bus/mhi/host/boot.c).
These are reference implementations, not evidence of a successful M6f boot.

| Operation | Reference behavior and M6f decision |
|---|---|
| BHI INTVEC | Power-up writes zero before the firmware handler. Zero selects vector zero; it does not disable MSI. Retained. |
| BHI programming | STATUS=0; IMGADDR_HIGH=0; IMGADDR_LOW=IOVA; IMGSIZE=0x80000; IMGTXDB=session. HIGH precedes LOW. Retained. |
| Sequence | Random nonzero bits [29:0], mask 0x3fffffff. Replaced the timer-derived odd value with kernel random bytes, masked; zero becomes 1 without an unbounded retry. |
| MHI state before BHI | A load-capable EE, not a forced READY/M0. PBL with RESET state and READY=0 is permitted. RESET state (MHISTATUS state field zero) differs from asserting MHICTRL.RESET bit 1. M6f retains PBL-only admission, reset request clear, no SYSERR, and no MHICTRL state write before BHI. |
| After BHI | Reset-clear/READY precedes context installation and M0. SBL precedes the BHIe transfer. Retained. |
| BHIe reset state | ath12k clears TXVECDB, TXVECSTATUS, RXVECDB, RXVECSTATUS. M6f clears and checks those four actual offsets, replacing M6e's RX-address/size/doorbell-only loop. No RX buffers are exposed. |
| Firmware | KIWI major 2 selects kiwi/amss20.bin. CNSS and ath12k enable FBC with sbl_size and seg_len both 512 KiB. The initial buffer is a copy from file offset zero; no separate SBL or PT_LOAD extraction. Retained. |
| QRTR instance | cnss_pci_store_qrtr_node_id only handles QCA6390/QCA6490, and only writes a nonzero configured instance. KIWI skips it. No KIWI scratch write added. |
| Wake | CNSS force-wake uses MHI device get operations for accesses/runtime PM; it is not a prerequisite to the PBL firmware handler. ath12k writes local wake at BAR0+0x3004 during power-down. No pre-PBL wake/M0 handshake added. |
| Global reset | ath12k software reset toggles local BAR0+0x3008 bit 0, waits 10 ms on each edge, then requests MHICTRL.RESET to clear possible SYSERR. It also clears warm-entry/debug and hot-reset state. CNSS's KIWI cold power-up does not replicate that software-reset sequence. M6f retains the current PERST/power sequence; no speculative global reset is mixed into a live image transfer. |
| Timeouts / image type | The audited CNSS/MHI KIWI path uses host software timeouts, not an extra BHI image-type or endpoint timeout-register write. CNSS config defaults to 10 s and scales power-up to 30/60 s; ath12k controller config is 2 s and its start path uses 90 s. M6f retains its bounded 5 s polls, since the observed failure is an immediate explicit error. |
| PBL/SBL logging | The CNSS diagnostic registers are read on failure; they are not pre-load configuration. M6f implements the bounded KIWI subset below. |

The public files do not supply an enum or decoder for `0xef0e0003`; public
exact-code searches also found none. Only the BHI status-field error meaning
is defined. BHIe TXVECSTATUS `0xeab00c75`, interpreted structurally, has status
3 and sequence bits `0x2ab00c75`. Before a host TXVECDB transaction it is not a
correlated BHIe result. M6f explicitly clears this stale/undefined state; the
value cannot identify a firmware error by itself.

The existing ELF metadata and Qualcomm hash segment fit inside the selected
prefix (ELF32 program-table end 0x314; hash offset 0x1000, length 0x308).
M6f now enforces program-table and hash-segment prefix bounds before DMA.
Other program segments extend beyond the prefix; FBC transfers those later
firmware stages through BHIe. Linux's DMA allocator supplies the
BHI buffer; neither audited loader specifies an additional ELF-segment
alignment constraint. Our initial IOVA is aligned to 512 KiB, 16 KiB and 4 KiB.
Internal hash consistency, already checked in M6e, does not prove ROM trust,
anti-rollback acceptance or the identity of bytes delivered by PCIe DMA.

### Checks and bounded changes

Immediately before BME, M6f hashes the 512-KiB source with FNV-1a-64, then
cleans/invalidates image and table pages to PoC with flush_dcache64 and dsb sy.
It reads the selected live CB's TTBR0 and walks L1/L2/L3 for all 128 4-KiB
IOVA pages. Table pointers can resolve only into the owned table allocation;
leaf pointers can resolve only into owned wired allocations. Reverse lookup
uses each actual XNU physical page, including noncontiguous 16-KiB backing.
No untrusted PA goes through phystokv, and no physical contiguity is assumed.
The CPU hashes bytes through the aliases of the resolved physical pages,
checks every leaf against its intended source page, and prints both hashes
and the page count in **one console line**. A bad pointer, descriptor, physical
page, count or hash fails step 21 reason 8 before bus mastering.

This catches a wrong/stale software mapping and forces a post-clean CPU reload.
It cannot independently prove the endpoint used that CB/SID, invalidate a
hypothetical endpoint address cache, or prove PCIe inbound translation and
coherency. The existing CB TLB synchronization, RC route readbacks and SMMU
fault checks remain required. FNV is a diagnostic checksum, not an
authentication check; the output artifacts use SHA-256.

Step 21 also checks the four cleared BHIe registers before BME (reason 9 on
failure). It retains the Linux BHI tuple/readbacks (reason 7), then uses the
random session ID and rings exactly one BHI doorbell. No automatic firmware
retry is added. Limits 20 and 22 remain usable for staged runs; 22 initiates
DMA and stops only after a successful BHI completion.

On step-22 failure with readable transport, M6f reads these endpoint-internal
registers through the sliding BAR window, with selector readback and restore
on every access, including contained fault paths:

| Register | Endpoint address |
|---|---|
| TCSR_PBL_LOGGING | 0x01b000f8 |
| PBL_WLAN_BOOT_CFG | 0x01e22b34 |
| KIWI_PBL_BOOTSTRAP_STATUS | 0x01a10008 |
| CNSS PCIE_BHI_ERRDBG2 / ERRDBG3 aliases | 0x01e0e238 / 0x01e0e23c |
| KIWI PBL log | Ten words starting at 0x01403d98 |

The aliases are printed alongside the existing dynamically located BHI debug
registers; they must not silently be assumed identical on every revision.
CNSS's SBL-log pointer/length aliases are validated as word-aligned SRAM in
0x01400000..0x01800000 and clamped to 48 bytes before reading. A DMA address or
zero echoed in DBG2 is not a valid log pointer and is explicitly skipped. The
maximum is 27 data words including register metadata. Reads stop at the first
access fault and are skipped outside PBL/SBL or on transport loss. The BAR
mapping covers the 512-KiB sliding aperture; addresses are still explicitly
bounded. No arbitrary SRAM, serial, key, firmware or calibration dump is added.
The audited CNSS KIWI dump names TCSR_PBL_LOGGING; it defines no separate
KIWI TCSR_PBL_ERROR register or private-code decoder. PEACH-only reset-cause
and BHI-debug dumps are not copied into the KIWI path.

All prior abort paths remain: reset request, BME off, PERST assertion, disable
and invalidate only the owned SMMU bank/route, TLB synchronization, then free.
Buffers remain retained when safe containment cannot be established. A log
access fault cannot override an earlier SMMU failure or allow later DMA.

### Hypotheses and validation

Ranked by the current evidence, with no claimed device validation of M6f:

1. PBL rejects its BHI transfer at an unidentified internal stage. Image
   acceptance/authentication and a failed target-side DMA read remain possible;
   the public error value does not distinguish them. The boot logs should
   identify how far it got before assigning a more specific cause.
2. Incorrect/stale DMA mapping or backing bytes. Register echoes do not exclude
   this. The new live-CB walk and post-clean hashes test the CPU-visible half;
   a MATCH lowers this hypothesis without proving endpoint DMA delivery.
3. Residual endpoint/BHIe boot state or the timer-derived session value.
   These are concrete differences from the reference, now normalized, but the
   evidence does not prove either caused the BHI error. A reset/READY ordering
   change, a different image size, or a QRTR write has weaker source support.

Linux ASan/UBSan host tests cover successful progression, noncontiguous
physical pages, altered leaf/table/root pointers, known FNV output, zero/full
random values, failed vector clears, bounded ELF/hash metadata, log bounds,
mission-mode skips, fault-time selector restore, transport loss and all existing
containment paths. The fake pmap model now returns a physical **XNU page** for
subpage VAs, so noncontiguous tests no longer accidentally move within one
16-KiB page. Embedded patch sources must match the standalone files byte for
byte. Native-Mac host tests and artifact verification are also required.

`tools/build-fuxi-wlan6f-images.sh` emits full and stop-at-22 variants.
`tools/test-fuxi-wlan-m6-artifacts.py --milestone m6f` verifies packed kernels,
ADT/cmdlines, GPU sections, headers and every SHA-256 manifest entry. Outputs
and machine-specific build records stay ignored.

Expected additional console templates (not captured device logs):

```text
FUXI WLAN: M6f FBC metadata fits prefix phend 0x314 hash segments 1 offset 0x1000 size 0x308
FUXI WLAN: M6f BHIe TX/RX doorbells/status cleared; no RX buffers
FUXI WLAN: M6f CB <bank> BHI IOVA 0x10000000+0x80000 FNV1a64 source <16 hex> walk <same 16 hex> pages 128/128 MATCH reason 0 (CPU view)
FUXI WLAN: M6f BHI doorbell random seq <nonzero> mask 0x3fffffff bytes 0x80000 bus master on
FUXI WLAN: M6f TCSR_PBL_LOGGING [0x1b000f8] <value>
FUXI WLAN: M6f PBL_WLAN_BOOT_CFG [0x1e22b34] <value>
FUXI WLAN: M6f KIWI_PBL_BOOTSTRAP_STATUS [0x1a10008] <value>
FUXI WLAN: M6f PCIE_BHI_ERRDBG2 [0x1e0e238] <value>
FUXI WLAN: M6f PCIE_BHI_ERRDBG3 [0x1e0e23c] <value>
FUXI WLAN: M6f PBL log [<one of ten fixed addresses>] <value>
FUXI WLAN: M6f SBL log skipped invalid SRAM tuple <start>/<size> (runtime DBG <start>/<size>)
```

The existing M6e transport samples still print twice. Boot-log lines appear
only on a failed/unusable BHI result; a successful BHI goes on to bounded
READY/SBL/BHIe/M0 steps. Failure still prints the existing containment/undo
lines, including buffer retention if containment fails.

M6f native kernel compilation, native-Mac and Linux ASan/UBSan suites, and both
Mac-packed artifact variants passed. Full image SHA-256:
`dbc931785517a3f9831bac1a2e83d03e341727540550ee29caf8c544bd0ced1b`.
Stop-at-22 SHA-256:
`576c77855ef7708955bc154e06cd5858e55d838f68d8cb9db23eb52dbfb602e2`.
These are build/host results only; no M6f phone test was performed.
