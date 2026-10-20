#!/usr/bin/env python3
"""Run/decode the Fuxi WCN7850 read-only snapshot (request 0x5c, value 0x574c).

Index 1 takes a fresh snapshot (no register writes); --read-report re-reads
the last one. --decode decodes a saved 512-byte report without USB access.
See docs-lanes/wlan-bt.md for the word layout and gating rules.
"""
import argparse
import struct
import sys
from pathlib import Path

PCIE_CBCR = ['slv_axi', 'mstr_axi', 'cfg_ahb', 'aux', 'pipe', 'slv_q2a',
             'phy_rchng', 'aggre_noc_pcie_axi', 'ddrss_pcie_sf_qtb', 'cnoc_pcie_sf_axi']
BCR = ['PCIE_0_BCR', 'PCIE_0_PHY_BCR', 'PCIE_0_LINK_DOWN_BCR', 'PCIE_0_NOCSR_COM_PHY_BCR', 'PCIE_0_PHY_NOCSR_COM_PHY_BCR']
CMDDB = ['ldob15', 'smpe4', 'smpg2', 'smpg4', 'smpg5', 'smpg6', 'ldoe1', 'ldoe3', 'clka1', 'vrm.wlan']
PINS = {76: 'BT_UART_CTS', 77: 'BT_UART_RFR', 78: 'BT_UART_TX', 79: 'BT_UART_RX',
        80: 'WLAN_EN', 81: 'BT_EN', 82: 'SW_CTRL', 83: 'WLAN_SW_CTRL',
        94: 'PCIE0_PERST', 95: 'PCIE0_CLKREQ', 96: 'PCIE0_WAKE'}
PARENT = {0: ('XO', 19_200_000), 1: ('GPLL0', 600_000_000), 2: ('GPLL7', None), 6: ('GPLL0_EVEN', 300_000_000)}


def cbcr(v):
    return f'{v:#010x} {"EN" if v & 1 else "--"} {"OFF" if v >> 31 else "on"}'


def tlmm(cfg, io):
    pull = ['none', 'down', 'keeper', 'up'][cfg & 3]
    return (f'func={(cfg >> 2) & 0xf} pull={pull} drive={((cfg >> 6) & 7) * 2 + 2}mA '
            f'oe={(cfg >> 9) & 1} out={(io >> 1) & 1} in={io & 1}')


def dfs_rate(cfg, m, n):
    src = (cfg >> 8) & 7
    name, rate = PARENT.get(src, (f'src{src}', None))
    hid = cfg & 0x1f
    mode = (cfg >> 12) & 3
    if rate is None:
        return f'{name} hid={hid} mode={mode}'
    if hid:
        rate = rate * 2 // (hid + 1)
    if mode:
        mm = m & 0xffff
        nn = ((~n) & 0xffff) + mm
        rate = rate * mm // nn if nn else 0
    return f'{name} -> {rate} Hz'


def decode(r):
    o = [f'magic={r[0]:#x} complete={r[3]} faults={r[124]} last_esr={r[125]:#x}']
    o.append(f'GCC votes 0x52000={r[4]:#010x} 0x52008={r[5]:#010x} 0x52010={r[6]:#010x} gdsc_collapse 0x52020={r[7]:#010x}')
    o.append('-- PCIe0 clocks --')
    for i, name in enumerate(PCIE_CBCR):
        o.append(f'  {name:20s} {cbcr(r[8 + i])}')
    o.append(f'  pcie_0_gdsc={r[19]:#010x} (on={r[19] >> 31}) cfg={r[20]:#010x}  phy_gdsc={r[21]:#010x} (on={r[21] >> 31}) cfg={r[22]:#010x}')
    for i, name in enumerate(BCR):
        o.append(f'  {name:30s} {r[23 + i]:#010x} (asserted={r[23 + i] & 1})')
    o.append(f'  pipe_mux={r[28]:#x} (0 PHY pipe, 2 XO)  aux rcg cmd/cfg={r[29]:#x}/{r[30]:#x}  rchng cmd/cfg={r[31]:#x}/{r[32]:#x}')
    o.append(f'  tcsr pcie_0_clkref_en={r[33]:#x}')
    o.append('-- QUP2 / SE6 (BT UART) clocks --')
    for i, name in enumerate(['wrap2_m_ahb', 'wrap2_s_ahb', 'wrap2_core', 'wrap2_core2x', 'wrap2_s6']):
        o.append(f'  {name:20s} {cbcr(r[34 + i])}')
    o.append(f'  s6 rcg cmd={r[39]:#x} cfg={r[40]:#x} dfsr={r[41]:#x} (dfs_en={r[41] & 1})')
    for lvl in range(8):
        o.append(f'    level {lvl}: cfg={r[42 + lvl]:#x} {dfs_rate(r[42 + lvl], r[50 + lvl], r[58 + lvl])}')
    o.append('-- TLMM --')
    for i, pin in enumerate(range(76, 84)):
        o.append(f'  gpio{pin} {PINS[pin]:13s} {tlmm(r[66 + i], r[74 + i])}')
    for i, pin in enumerate(range(94, 97)):
        o.append(f'  gpio{pin} {PINS[pin]:13s} {tlmm(r[82 + i], r[85 + i])}')
    o.append(f'-- RPMh DRV id={r[88]:#x} cfg={r[89]:#x}; cmd-db magic={r[90]:#x} (expect 0xc0330db)')
    for i, name in enumerate(CMDDB):
        o.append(f'  {name:9s} {r[91 + i]:#x}')
    g = r[101]
    o.append(f'-- gated reads mask={g:#x} (1 PARF, 2 PHY, 4 DBI, 8 SE6) --')
    if g & 1:
        o.append(f'  PARF LTSSM={r[102]:#x} (state {r[102] & 0x3f}, en {(r[102] >> 8) & 1}) PHY_CTRL={r[103]:#x} DEVICE_TYPE={r[104]:#x}')
        o.append(f'       INT_ALL={r[105]:#x} DBI_BASE={r[106]:#x} ATU_BASE={r[107]:#x}')
    if g & 2:
        o.append(f'  PHY pcs_status={r[108]:#x} (PHYSTATUS {(r[108] >> 6) & 1}, 0=ready) pwrdn={r[109]:#x}')
    if g & 4:
        o.append(f'  DBI id={r[110]:#010x} bus={r[111]:#010x} lnkctl/sta={r[112]:#010x} (DLLLA {(r[112] >> 29) & 1})')
    if g & 8:
        o.append(f'  QUP2 hw_ver={r[113]:#x} SE6 fw_rev={r[114]:#x} (proto {(r[114] >> 8) & 0xff}, 2=UART) status={r[115]:#x}')
        o.append(f'       m_clk_cfg={r[116]:#x} clk_sel={r[117]} ios={r[118]:#x} (CTS {"asserted" if not r[118] & 2 else "deasserted"}) '
                 f'hw_param0={r[119]:#x} (tx fifo depth {(r[119] >> 16) & 0x3f}) dma_mode={r[120]:#x}')
    return '\n'.join(o)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--read-report', action='store_true', help='re-read the last snapshot')
    ap.add_argument('--timeout', type=int, default=5000)
    ap.add_argument('--save', type=Path)
    ap.add_argument('--decode', type=Path)
    args = ap.parse_args()
    if args.decode:
        blob = args.decode.read_bytes()
    else:
        import usb.core
        import usb.util
        devs = [d for d in usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001)
                if usb.util.get_string(d, d.iProduct) == 'Fuxi hardware probe']
        if len(devs) != 1:
            ap.error('expected exactly one Fuxi native probe (1209:0001)')
        blob = bytes(devs[0].ctrl_transfer(0xc0, 0x5c, 0x574c, 0x100 if args.read_report else 1, 512,
                                           timeout=args.timeout))
    if len(blob) != 512:
        raise SystemExit(f'expected 512 bytes, got {len(blob)}')
    r = list(struct.unpack('<128I', blob))
    if r[0] != 0x574c0001:
        raise SystemExit(f'bad magic {r[0]:#x} (probe lacks the 0x5c handler?)')
    if args.save:
        args.save.write_bytes(blob)
    print(decode(r))
    return 0


if __name__ == '__main__':
    sys.exit(main())
