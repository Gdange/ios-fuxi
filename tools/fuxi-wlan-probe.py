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


M3_STEPS = {1: 'precheck (cmd-db, RSC, GPLL0)', 2: 'WCN rails s5g/l15b/s2g/s4e/s4g/s6g',
            3: 'WLAN_EN gpio80 high + 50 ms', 4: 'PHY rails ldoe1/ldoe3',
            5: 'PERST# low, CLKREQ#/WAKE# pins', 6: 'PCIe0 core + PHY GDSC',
            7: 'PCIe0 clocks', 8: 'core BCR + PARF init', 9: 'QMP PHY init + PHYSTATUS',
            10: 'PERST# high', 11: 'LTSSM + link (DLLLA)', 12: 'iATU CFG0 + endpoint ID',
            13: 'BAR sizing'}
M3_REASONS = {
    1: {1: 'cmd-db magic', 2: 'cmd-db address mismatch', 3: 'RSC DRV id/TCS count', 4: 'GPLL0 not locked'},
    2: {i + 1: f'vote {n} failed' for i, n in enumerate(['s5g', 'l15b', 's2g', 's4e', 's4g', 's6g'])},
    3: {1: 'WLAN_EN readback low'}, 4: {1: 'ldoe1 vote', 2: 'ldoe3 vote'}, 5: {1: 'PERST# not low'},
    6: {1: 'core GDSC timeout', 2: 'PHY GDSC timeout'},
    7: {1: 'aux RCG update', 2: 'rchng RCG update', 3: 'branch(es) not running'},
    8: {1: 'PARF abort or DEVICE_TYPE readback'}, 9: {1: 'PHY register abort', 2: 'PHYSTATUS stuck (not ready)'},
    10: {1: 'PERST# readback low'}, 11: {1: 'no PCIe capability / DBI abort', 2: 'abort while polling', 3: 'link timeout'},
    12: {1: 'iATU enable / abort', 2: 'config read abort', 3: 'unexpected ID'}, 13: {1: 'abort during sizing'}}


def bar_sizes(r):
    out, b = [], 0
    while b < 6:
        orig, rb = r[81 + b], r[87 + b]
        if rb == 0:
            b += 1
            continue
        if orig & 1:
            out.append(f'BAR{b} io size={(~(rb & ~3) & 0xffffffff) + 1:#x}')
            b += 1
        elif (orig >> 1) & 3 == 2 and b < 5:
            mask = (r[88 + b] << 32) | (rb & ~0xf)
            out.append(f'BAR{b} mem64 base={(r[82 + b] << 32) | (orig & ~0xf):#x} size={((~mask) & (2**64 - 1)) + 1:#x}'
                       f' prefetch={(orig >> 3) & 1}')
            b += 2
        else:
            out.append(f'BAR{b} mem32 size={(~(rb & ~0xf) & 0xffffffff) + 1:#x} prefetch={(orig >> 3) & 1}')
            b += 1
    return out


def decode_m3(r):
    o = [f'magic={r[0]:#x} limit={r[1]} last_step_entered={r[2]} result={r[3]:#x} faults={r[124]} esr={r[125]:#x} far={r[126]:#x}']
    for s in range(1, 14):
        v = r[3 + s]
        if not v:
            continue
        if v & 0x1000:
            o.append(f'  step {s:2} OK   {M3_STEPS[s]} ({r[100 + s]} ms)')
        else:
            why = M3_REASONS.get(s, {}).get(v & 0xff, f'reason {v & 0xff}')
            o.append(f'  step {s:2} FAIL {M3_STEPS[s]}: {why}')
    o.append(f'  cmd-db rails: ' + ' '.join(f'{n}={r[20 + i]:#x}' for i, n in enumerate(CMDDB[:8])) + f'  GPLL0 mode={r[28]:#x}')
    o.append(f'  rail votes (0 ok) s5g..s6g={r[29:35]} ldoe1={r[35]} ldoe3={r[42]}')
    o.append(f'  SW_CTRL/WLAN_SW_CTRL io before={r[36]:#x}/{r[37]:#x} after={r[40]:#x}/{r[41]:#x}  WLAN_EN cfg/io={r[38]:#x}/{r[39]:#x}')
    o.append(f'  PERST io={r[43]:#x} WAKE io={r[44]:#x}  GDSC polls core/phy={r[45]}/{r[46]} gdscr={r[47]:#x}/{r[48]:#x}')
    o.append('  clocks cfg_ahb slv_axi mstr_axi aux slv_q2a aggre ddrss rchng: ' + ' '.join(f'{x:#x}' for x in r[49:57]) + f' bad={r[57]:#x}')
    o.append(f'  PARF DEVICE_TYPE={r[58]:#x} PHY_CTRL={r[59]:#x} DBI_BASE={r[60]:#x} ATU_BASE={r[61]:#x}')
    o.append(f'  PHY polls={r[62]}us pcs_status={r[63]:#x} pipe_cbcr={r[64]:#x} pipe_mux={r[65]:#x}  PERST after={r[66]:#x}')
    o.append(f'  link: cap={r[67]:#x} {r[68]} ms LTSSM={r[69]:#x} (state {r[69] & 0x3f}) lnkctl/sta={r[70]:#010x} '
             f'(speed {(r[70] >> 16) & 0xf} width x{(r[70] >> 20) & 0x3f} DLLLA {(r[70] >> 29) & 1}) '
             f'states_seen={(r[72] << 32) | r[71]:#x} int_all={r[73]:#x}')
    o.append(f'  iATU ctrl2={r[74]:#x}  endpoint id={r[75]:#010x} (expect 0x110717cb) tries={r[76]} '
             f'class/rev={r[77]:#x} hdr={r[78]:#x} subsys={r[79]:#x} cmd/status={r[80]:#x}')
    o += ['  ' + x for x in bar_sizes(r)]
    return '\n'.join(o)


MHI_STATE = ['RESET', 'READY', 'M0', 'M1', 'M2', 'M3', 'M3_FAST', 'BHI']
MHI_EE = ['PBL', 'SBL', 'AMSS', 'RDDM', 'WFW', 'PTHRU', 'EDL', 'FP']
M4_STEPS = {14: 'iATU1 MEM + root-port window', 15: 'endpoint BAR0 assignment',
            16: 'MHI/BHI register read'}


def decode_m4(r):
    """XNU milestone-4 report (bsd/dev/fuxi_wlan.c wl4_report)."""
    o = [f'magic={r[0]:#x} limit={r[1]} last_step={r[2]} result={r[3]:#x}']
    for s in range(1, 17):
        c = r[40 + s]
        if c:
            name = M4_STEPS.get(s, 'M3 step (see m3 decode)')
            o.append(f'  step {s:2} {"OK  " if c == 0x1000 | s else f"FAIL reason {c & 0xff}"} {name}')
    if r[4] or r[8]:
        o.append(f'  iATU1 ctrl2={r[4]:#x} base={r[5]:#x} limit={r[6]:#x} target={r[7]:#x}  '
                 f'RC mem window={r[8]:#x} pref={r[9]:#x} cmd/status={r[10]:#x}')
    if r[11]:
        o.append(f'  BAR0 sizing readback={r[11]:#x} size={r[12]:#x} assigned={r[15] << 32 | (r[14] & ~0xf):#x} '
                 f'EP cmd/status {r[13]:#x} -> {r[16]:#x}')
    if any(r[20:30]):
        cfg, ctrl, st = r[22], r[28], r[29]
        o.append(f'  MHI REGLEN={r[20]:#x} MHIVER={r[21]:#x} MHICFG={cfg:#x} (nhwer {cfg >> 24} ner {(cfg >> 16) & 255} '
                 f'nhwch {(cfg >> 8) & 255} nch {cfg & 255})')
        o.append(f'  CHDBOFF={r[23]:#x} ERDBOFF={r[24]:#x} BHIOFF={r[25]:#x} BHIEOFF={r[26]:#x} DEBUGOFF={r[27]:#x}')
        s = (st >> 8) & 255
        o.append(f'  MHICTRL={ctrl:#x} MHISTATUS={st:#x} state={MHI_STATE[s] if s < 8 else ("SYS_ERR" if s == 255 else s)} '
                 f'ready={st & 1} syserr={(st >> 2) & 1}')
    if any(r[30:38]):
        ee = r[32]
        o.append(f'  BHI version {r[31]}.{r[30]} EXECENV={ee:#x} ({MHI_EE[ee] if ee < 8 else "?"}) STATUS={r[33]:#x} '
                 f'ERRCODE={r[34]:#x} ERRDBG={r[35]:#x} {r[36]:#x} {r[37]:#x}')
    if r[38]:
        o.append(f'  after: RC cmd/status={r[38]:#x} EP cmd/status={r[39]:#x}')
    return '\n'.join(o)


def from_xnu_log(path):
    """Rebuild the m3/m4 reports from 'FUXI WLAN: m3 r[  8] ...' console lines."""
    reports = {'m3': [0] * 128, 'm4': [0] * 64}
    seen = set()
    for line in path.read_text(errors='replace').splitlines():
        at = line.find('FUXI WLAN: ')
        if at < 0:
            continue
        f = line[at + 11:].replace('r[', ' ').replace(']', ' ').split()
        if len(f) == 10 and f[0] in reports:
            base = int(f[1])
            for j, w in enumerate(f[2:]):
                if base + j < len(reports[f[0]]):
                    reports[f[0]][base + j] = int(w, 16)
            seen.add(f[0])
    return reports, seen


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu-log', type=Path,
                    help='decode the FUXI WLAN m3/m4 dump lines of an XNU console log (fuxiwlan=1)')
    ap.add_argument('--m3', type=int, metavar='LIMIT', choices=range(1, 14),
                    help='run gated M3 steps 1..LIMIT (writes hardware; see docs-lanes/wlan-bt.md)')
    ap.add_argument('--m3-report', action='store_true', help='re-read the M3 report')
    ap.add_argument('--read-report', action='store_true', help='re-read the last snapshot')
    ap.add_argument('--timeout', type=int, default=5000)
    ap.add_argument('--save', type=Path)
    ap.add_argument('--decode', type=Path)
    args = ap.parse_args()
    if args.xnu_log:
        reports, seen = from_xnu_log(args.xnu_log)
        if not seen:
            raise SystemExit('no "FUXI WLAN: m3/m4 r[...]" lines found')
        if 'm3' in seen:
            print(decode_m3(reports['m3']))
        if 'm4' in seen:
            print(decode_m4(reports['m4']))
        return 0
    if args.decode:
        blob = args.decode.read_bytes()
    else:
        import usb.core
        import usb.util
        devs = [d for d in usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001)
                if usb.util.get_string(d, d.iProduct) == 'Fuxi hardware probe']
        if len(devs) != 1:
            ap.error('expected exactly one Fuxi native probe (1209:0001)')
        if args.m3:
            index = 0x300 + args.m3
        elif args.m3_report:
            index = 0x3ff
        else:
            index = 0x100 if args.read_report else 1
        blob = bytes(devs[0].ctrl_transfer(0xc0, 0x5c, 0x574c, index, 512,
                                           timeout=max(args.timeout, 15000) if args.m3 else args.timeout))
    if len(blob) != 512:
        raise SystemExit(f'expected 512 bytes, got {len(blob)}')
    r = list(struct.unpack('<128I', blob))
    if r[0] not in (0x574c0001, 0x574c0003):
        raise SystemExit(f'bad magic {r[0]:#x} (probe lacks the 0x5c handler?)')
    if args.save:
        args.save.write_bytes(blob)
    print(decode(r) if r[0] == 0x574c0001 else decode_m3(r))
    return 0


if __name__ == '__main__':
    sys.exit(main())
