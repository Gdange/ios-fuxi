# Lane: Wi-Fi and Bluetooth (WCN7850 "kiwi")

Status:
- Milestone 1 (read-only snapshot): built.
- Milestone 3 (gated power-up, PCIe0 link, endpoint ID and BAR sizes): built, not yet run on the device.
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
