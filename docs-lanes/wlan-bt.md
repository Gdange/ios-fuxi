# Lane: Wi-Fi and Bluetooth (WCN7850 "kiwi")

Status: milestone 1 (research + read-only probe). Nothing here powers the chip yet.

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
- **M3 (PCIe)**
  - Rails l1e/l3e, WLAN_EN.
  - Bring up PCIe0 in the Linux order in section 2.
  - iATU CFG0 read of bus 1; expect `0x110717cb`.

## 6. Firmware needed from the phone (coordinator pull; never committed)

Please send `ls -lR` of `/vendor/firmware_mnt/image`, `/vendor/firmware`, `/vendor/bt_firmware`, `/vendor/etc/wifi` and `/odm/etc/wifi`. From those, pull the following.

- **Bluetooth** (bluetooth partition mounted at `/vendor/bt_firmware/image/`): `hmtbtfw*.tlv` (rampatch) and `hmtnv*.bin` / `hmtnv*.b*` (NVM/board).
- **Wi-Fi**:
  - kiwi `amss.bin` / `amss20.bin`, `m3.bin`, `regdb.bin`, `bdwlan*.elf` / `bdwlan*.e*`, `phy_ucode*.elf`. They are probably under `/vendor/firmware/` or `firmware_mnt/image/`.
  - `/vendor/etc/wifi/kiwi*/WCNSS_qcom_cfg.ini`.
