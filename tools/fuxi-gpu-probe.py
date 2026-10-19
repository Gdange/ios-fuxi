#!/usr/bin/env python3
"""Run the lane-gpu staged A740 power/clock/cmd-db probe (USB request 0x56).

Talks to the isolated Fuxi native USB probe (1209:0001, 'Fuxi hardware probe')
that the fuxi-loader image runs before the XNU handoff. Request 0x56 value
0x4753 index=<stage> runs stages 1..<stage> and returns a fixed 128-word
report; this tool decodes it. Read-only stage 1 is always safe; each later
stage runs only if the previous proved its prerequisite, and a bad MMIO resets
the SoC, so the furthest decoded stage pins a hang. No handoff, no storage
access. See docs-lanes/gpu.md for the full word map and milestone-2 plan.
"""
import argparse
import struct
import sys

import usb.core
import usb.util


def decode(r):
    """r: list of 128 uint32. Prints a human-readable stage-by-stage report."""
    def on(cbcr):
        return 'EN' if cbcr & 1 else '--', 'OFF' if cbcr & (1 << 31) else 'on'
    out = []
    out.append(f'magic={r[0]:#x} stage_limit={r[1]}')
    # Stage 1 / 4 snapshot
    out.append('-- stage 1 read-only snapshot --')
    out.append(f'  gcc cfg_ahb={r[4]:#010x} memnoc={r[5]:#010x} dvm={r[6]:#010x} gpll0_vote={r[7]:#010x}')
    out.append(f'  gpucc_read={r[13]}  cx_gdscr={r[8]:#010x} gx_gdscr={r[9]:#010x}')
    out.append(f'          ahb_clk={r[10]:#010x} smmu_vote={r[11]:#010x} cx_gmu={r[12]:#010x}')
    out.append(f'  smmu_read={r[15]} scr0={r[16]:#010x} id0={r[17]:#010x} id1={r[18]:#010x} id2={r[19]:#010x} gfsr={r[20]:#010x}')
    if r[15]:
        out.append(f'          smr_valid={r[21] & 0xffff} first={r[21] >> 16} smr={r[22]:#010x} s2cr={r[23]:#010x}')
    out.append(f'  gpu_read={r[14]} rbbm_status={r[24]:#010x}')
    # Stage 2
    out.append('-- stage 2 gcc_gpu_cfg_ahb enable --')
    out.append(f'  wrote_enable={r[26]} cfg_ahb_after={r[27]:#010x}')
    # Stage 3
    out.append('-- stage 3 CX GDSC + ahb/smmu clocks --')
    out.append(f'  cx_gdscr before={r[28]:#010x} hw_ctrl={r[29]:#010x} after={r[30]:#010x} (on={bool(r[30] & (1 << 31))})')
    out.append(f'  ahb_clk={r[31]:#010x} smmu_vote={r[32]:#010x} smmu_on={r[33]}')
    # Stage 4
    out.append('-- stage 4 powered adreno SMMU GR0 + CB0/CB1 --')
    out.append(f'  done={r[35]} scr0={r[36]:#010x} id0={r[37]:#010x} id1={r[38]:#010x} id2={r[39]:#010x}')
    if r[35]:
        out.append(f'  CB0 cbar={r[40]:#010x} sctlr={r[41]:#010x} fsr={r[42]:#010x} tcr={r[43]:#010x}')
        out.append(f'  CB1 cbar={r[44]:#010x} sctlr={r[45]:#010x} fsr={r[46]:#010x} tcr={r[47]:#010x}')
    # Stage 5
    out.append('-- stage 5 GMU clock snapshot --')
    out.append(f'  pll1 mode={r[48]:#010x} opmode={r[49]:#x} L={r[50]:#x}')
    out.append(f'  gmu_rcg cmd={r[51]:#010x} cfg={r[52]:#010x}  hub_rcg cmd={r[53]:#010x} cfg={r[54]:#010x}')
    out.append(f'  cx_gmu={r[55]:#010x} cxo={r[56]:#010x} hub_cx_int={r[57]:#010x} demet={r[58]:#010x} axi={r[59]:#010x} gpll0_vote={r[60]:#010x}')
    out.append(f'  gmu_regs_read={r[79]}')
    out.append(f'  (stage5 gmu state) sysreset={r[61]:#x} tcm_ret={r[62]:#x} rsc_busy={r[63]:#x} hfi_status={r[64]:#x}')
    # Stage 6
    STEP = {0: 'not-run', 7: 'ALL-PASS', 3: 'PLL1 lock timeout', 4: 'hub RCG timeout', 5: 'gmu RCG timeout', 6: 'cx_gmu CLK_OFF timeout'}
    out.append('-- stage 6 GMU clock bring-up --')
    out.append(f'  result={r[65]} ({STEP.get(r[65], "?")}) gpll0_voted={r[66]} pll1_lock_polls={r[67]} cx_gmu={r[68]:#010x} cx_gmu_polls={r[69]}')
    out.append(f'  pll1 mode={r[70]:#010x} opmode={r[71]:#x} gmu_cfg={r[72]:#05x} hub_cfg={r[73]:#05x}')
    out.append(f'  gmu cold-start preconds: sysreset={r[74]:#x}(want 1) tcm_ret={r[75]:#x}(want 0) rsc_busy={r[76]:#x}(want 0) hfi_status={r[77]:#x}(want bit0=0)')
    # Stage 7
    NAMES = ['gfx.lvl', 'mx.lvl', 'cx.lvl', 'gmxc.lvl', 'SH0', 'MC0', 'ACV', 'CN0']
    out.append('-- stage 7 cmd-db parse --')
    out.append(f'  version={r[80]:#x} magic={r[81]:#010x} (want 0x0c0330db) slaves={r[82]}')
    for i, nm in enumerate(NAMES):
        out.append(f'  {nm:<9} addr={r[83 + i]:#x}')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stage', type=int, default=1, choices=range(1, 8),
                    help='last stage to run (1 read-only .. 7 cmd-db parse). Default 1.')
    ap.add_argument('--timeout', type=int, default=8000, help='control-transfer timeout ms')
    ap.add_argument('--raw', action='store_true', help='also dump the 128 raw words')
    args = ap.parse_args()

    devs = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    devs = [d for d in devs if usb.util.get_string(d, d.iProduct) == 'Fuxi hardware probe']
    if len(devs) != 1:
        ap.error('expected exactly one Fuxi native probe (1209:0001)')
    dev = devs[0]

    data = bytes(dev.ctrl_transfer(0xc0, 0x56, 0x4753, args.stage & 0xff, 512, timeout=args.timeout))
    if len(data) != 512:
        raise SystemExit(f'short USB response: {len(data)} bytes')
    r = list(struct.unpack('<128I', data))
    if r[0] != 0xa740:
        raise SystemExit(f'bad report magic {r[0]:#x} (probe may not have the 0x56 handler)')
    print(decode(r))
    if args.raw:
        for i in range(0, 128, 8):
            print('  w%3d:' % i, ' '.join('%08x' % x for x in r[i:i + 8]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
