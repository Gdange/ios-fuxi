#!/usr/bin/env python3
"""Decode/run the Fuxi A740 staged GPU probe (request 0x56, value 0x4753).

--stage 8a snapshots all GPU SMMU streams/banks after stages1..7.
8 maps SID5 and loads externally embedded firmware; 9 starts GMU/HFI;
10 requests GX; 11 maps GPU SIDs0/1; 12 starts SQE/CP; 13 writes a fence. Run each on a fresh loader boot first.
--read-reports retrieves pages without replaying hardware setup.
See docs-lanes/gpu.md for memory reservations, risks and report words.
"""
import argparse
import struct
import sys

from pathlib import Path


def decode(r, pages=None):
    """r: list of 128 uint32. Prints a human-readable stage-by-stage report."""
    def on(cbcr):
        return 'EN' if cbcr & 1 else '--', 'OFF' if cbcr & (1 << 31) else 'on'
    out = []
    out.append(f'magic={r[0]:#x} stage_limit={r[1]:#x} furthest={r[2]:#x} gmu_started={r[3]}')
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
    if pages and pages[0][0] in (0xa7400002, 0xa7400003):
        e = pages[0]
        errors = {
            0: 'not-run', 1: 'PASS',
            0x801: 'unsupported SMMU geometry/state', 0x802: 'no embedded firmware',
            0x803: 'bad/truncated/unmapped firmware block', 0x804: 'SID5 already matched',
            0x805: 'no unused disabled context/slot/ASID', 0x806: 'not a cold GMU',
            0x807: 'scratch overlaps image/FDT', 0x808: 'TLB sync timeout',
            0x809: 'SMMU readback mismatch', 0x80a: 'firmware readback mismatch',
            0x80b: 'page-table capacity/overlap', 0x80c: 'context fault after load', 0x901: 'FW init timeout/fault',
            0x902: 'HFI init timeout/fault', 0x903: 'HFI ACK timeout',
            0x904: 'bad queue indices/packet size/space', 0x905: 'GMU firmware error/fault',
            0x906: 'unexpected ACK/header/sequence', 0x907: 'ACK error',
            0x908: 'missing cmd-db aux/ARC/BW resource', 0x909: 'cold precondition lost',
            0xa01: 'GPU_SET timeout/fault', 0xa02: 'GX power/clock timeout/fault',
            0xa03: 'GX GDSC off', 0xa04: 'context fault before GX',
        }
        errors.update({
            0xb01: 'SMMU MMIO fault/unreadable stream ownership', 0xb02: 'ownership changed/live bank',
            0xb03: 'SID0/1 already matched', 0xb04: 'no free GPU bank/two slots/ASID',
            0xb05: 'GPU page-table pool/overlap', 0xb06: 'GPU TLB sync timeout',
            0xb07: 'GPU context/stream readback mismatch', 0xb08: 'SMMU fault at stage11',
            0xc01: 'no embedded SQE', 0xc02: 'invalid SQE size', 0xc03: 'SQE readback mismatch',
            0xc04: 'GPU_SET/GX timeout or MMIO fault', 0xc05: 'hw_init/CP MMIO fault',
            0xc06: 'CP/RBBM/SMMU fault', 0xc07: 'CP init rptr/idle timeout',
            0xc08: 'CP ring not in reset state', 0xc09: 'SECVID access fault',
            0xc0a: 'SECVID_TRUST_CNTL did not clear',
            0xd01: 'fence MMIO fault', 0xd02: 'fence CP/RBBM/SMMU fault',
            0xd03: 'fence/rptr/idle timeout', 0xd04: 'ring/fence precondition lost',
            0xd05: 'fence guard overwritten',
        })
        result = lambda v: f'{v:#x} ({errors.get(v, "unknown")})'
        out.append('-- stage 8a SMMU snapshot (frozen before stage8 writes) --')
        out.append(f'  snapshot={result(e[1])} SMRs={e[5]} CBs={e[6]} stride={e[7]:#x} CBbase={e[8]:#x} GFSR={e[9]:#x}')
        if len(pages) >= 5 and e[1] == 1:
            snap = pages[1]
            out.append(f'  safe snapshot faults={snap[48]} ESR={snap[49]:#x} offset={snap[50]:#x} SMRmask={snap[51]:#x} CBmask={snap[52]:#x}')
            for i in range(min(e[5], 32)):
                smr, s2cr = pages[1][i*2:i*2+2]
                out.append(f'  SMR{i:02}: {smr:#010x} S2CR={s2cr:#010x} valid={smr>>31} SID={smr&0x7fff:#x} mask={(smr>>16)&0x7fff:#x} type={(s2cr>>16)&3} CB={s2cr&255}')
            for b in range(min(e[6], 24)):
                d = pages[2+b//8][b%8*16:b%8*16+16]
                out.append(f'  CB{b:02}: ACTLR={pages[1][64+b]:#x} CONTEXTIDR={pages[1][88+b]:#x} CBAR={d[0]:#x} CBA2R={d[1]:#x} SCTLR={d[2]:#x} TCR2={d[3]:#x} TCR={d[8]:#x} MAIR={d[9]:#x}/{d[10]:#x}')
                out.append(f'        TTBR0={d[5]:08x}{d[4]:08x} TTBR1={d[7]:08x}{d[6]:08x} FSR={d[11]:#x} FAR={d[13]:08x}{d[12]:08x} FSYNR={d[14]:#x}/{d[15]:#x}')
        out.append('-- stage 8 SID5 translation + firmware --')
        out.append(f'  result={result(e[2])} slot={e[10]} CB={e[11]} ASID={e[12]} table_PA={(e[14]<<32)|e[13]:#x} tables={e[33]} programmed={e[15]} TLBstatus={e[16]:#x}')
        out.append(f'  fw bytes={e[17]} data_blocks={e[18]} prealloc_headers={e[19]} data_bytes={e[20]} bad_addr={e[21]:#x} bad_size={e[22]:#x}')
        out.append(f'  CBAR={e[23]:#x} CBA2R={e[24]:#x} TCR2={e[25]:#x} TCR={e[26]:#x} TTBR0={e[28]:08x}{e[27]:08x} MAIR0={e[29]:#x} SCTLR={e[30]:#x} SMR={e[31]:#x} S2CR={e[32]:#x}')
        out.append('-- stage 9 GMU firmware + HFI startup --')
        out.append(f'  result={result(e[3])} handoff_locked={e[4]} DTCM_version={e[34]:#x} expected={e[35]:#x} init={e[36]:#x} HFIstatus={e[37]:#x} HFIversion={e[38]:#x}')
        for i, name in enumerate(['PERF_TABLE', 'BW_TABLE', 'CORE_FW_START', 'START', 'GX_BW_PERF_VOTE']):
            d = e[40+i*4:44+i*4]
            out.append(f'  {name:<16} sent={d[0]:#010x} response={d[1]:#010x} returned={d[2]:#010x} error={d[3]:#x}')
        out.append('  GPU ARC votes (OFF,220,295,348,401,475,550,615,680,719 MHz): ' + ' '.join(f'{x:#x}' for x in e[100:110]))
        out.append('  GMU ARC votes (OFF,200,500 MHz): ' + ' '.join(f'{x:#x}' for x in e[110:113]))
        out.append('-- stage 10 GPU_SET / GX --')
        out.append(f'  result={result(e[60])} OOB_intr={e[61]:#x} SPTPRAC={e[62]:#x} GX_GDSCR={e[63]:#x} RBBM_read={e[65]} RBBM_STATUS={e[64]:#x}')
        out.append(f'  live: intr={e[84]:#x} AOirq={e[85]:#x} fence={e[86]:#x} GFSR={e[87]:#x} CB_FSR={e[88]:#x} FAR={e[90]:08x}{e[89]:08x} FSYNR={e[91]:#x}/{e[92]:#x}')
        out.append(f'  queues: command r/w={e[93]}/{e[94]} response r/w={e[95]}/{e[96]}')
        if len(pages) >= 7 and e[0] == 0xa7400003:
            m = pages[5]
            out.append('-- stage 11 GPU SID0/1 context --')
            out.append(f'  result={result(m[2])} slots={m[10]}/{m[34]} CB={m[11]} ASID={m[12]} GMU_CB/ASID={m[8]}/{m[9]} tables={m[33]} PT={(m[14]<<32)|m[13]:#x}')
            out.append(f'  CBAR={m[23]:#x} CBA2R={m[24]:#x} TCR2={m[25]:#x} TCR={m[26]:#x} TTBR0={m[28]:08x}{m[27]:08x} MAIR={m[29]:#x} SCTLR={m[30]:#x}')
            out.append(f'  SMR/S2CR SID0={m[31]:#x}/{m[32]:#x} SID1={m[35]:#x}/{m[36]:#x}; GMU FSR before/after={m[74]:#x}/{m[75]:#x} GPU FSR={m[76]:#x}/{m[77]:#x}')
            for i, name in enumerate(['SQE', 'ring', 'memptrs', 'fence']):
                out.append(f'  {name} VA={m[37+i]:#x} PA={m[65+2*i]:08x}{m[64+2*i]:08x}')
            out.append('-- stage 12 SQE / a7xx CP init / no-zap fallback --')
            out.append(f'  result={result(m[3])} step={m[60]} OOB_held={m[7]} SQE_started={m[59]} GX={m[63]:#x}')
            out.append(f'  SQE file/copy={m[17]}/{m[18]} header={m[19]:#x} first_payload={m[20]:#x} readback_dwords={m[21]} bad_offset={m[22]:#x}')
            out.append(f'  CP rptr before/after={m[48]}/{m[49]} wptr={m[50]} polls={m[51]} trust before/after={m[44]:#x}/{m[45]:#x} write_ok={m[43]} TSBstatus={m[47]:08x}{m[46]:08x}')
            out.append('-- stage 13 CP_MEM_WRITE fence --')
            out.append(f'  result={result(m[4])} fence before/after={m[52]:#x}/{m[53]:#x} expected={m[54]:#x} CP rptr before/after={m[55]}/{m[56]} wptr={m[57]} polls={m[58]}')
            out.append(f'  safe MMIO faults={m[116]} first_PA={m[118]:08x}{m[117]:08x} last_PA={m[111]:08x}{m[110]:08x} op={m[115]} ESR={m[112]:#x} FAR={m[114]:08x}{m[113]:08x}')
            out.append('  init packet: ' + ' '.join(f'{v:08x}' for v in m[80:90]))
            out.append('  fence packet: ' + ' '.join(f'{v:08x}' for v in m[90:94]))
            for start in (0, 64):
                d = pages[6][start:start+64]
                if not d[0]:
                    continue
                out.append(f'  stage{d[0]} frozen diagnostics: rptr/wptr={d[1]}/{d[2]} shadows={d[3]}/{d[4]} fence={d[5]:#x} RBBM={d[6]:#x} SQE_CNTL={d[7]:#x} SQE_STAT[1]={d[8]:#x}')
                out.append(f'    CP_HW_FAULT={d[9]:#x} CP_INT={d[10]:#x} PROTECT={d[11]:#x} RBBM_INT={d[12]:#x} TRUST={d[13]:#x} TSB={d[15]:08x}{d[14]:08x} GFSR={d[16]:#x}')
                for at, name in ((17, 'GPU'), (23, 'GMU')):
                    out.append(f'    {name} CB{d[at]} FSR={d[at+1]:#x} FAR={d[at+3]:08x}{d[at+2]:08x} FSYNR={d[at+4]:#x}/{d[at+5]:#x}')
                out.append(f'    fence guards={d[32]:#x}/{d[33]:#x} MMIO_faults={d[31]}')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stage', default='1', choices=[str(i) for i in range(1, 14)] + ['8a'],
                    help='last stage (default 1); 8a is the powered SMMU snapshot')
    ap.add_argument('--timeout', type=int, default=15000, help='control-transfer timeout ms')
    ap.add_argument('--raw', action='store_true', help='also dump every raw report page')
    ap.add_argument('--read-reports', action='store_true', help='read all pages without running a stage')
    ap.add_argument('--save', type=Path, help='save raw concatenated little-endian report pages')
    ap.add_argument('--decode', type=Path, help='decode a saved report offline (no USB access)')
    args = ap.parse_args()
    if args.decode:
        blob = args.decode.read_bytes()
        if len(blob) not in (512, 3072, 4096):
            ap.error('report must be 512 (legacy), 3072 (v2), or 4096 (v3) bytes')
    else:
        import usb.core
        import usb.util
        devs = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
        devs = [d for d in devs if usb.util.get_string(d, d.iProduct) == 'Fuxi hardware probe']
        if len(devs) != 1:
            ap.error('expected exactly one Fuxi native probe (1209:0001)')
        dev = devs[0]

        def read(index):
            data = bytes(dev.ctrl_transfer(0xc0, 0x56, 0x4753, index, 512, timeout=args.timeout))
            if len(data) != 512:
                raise SystemExit(f'short USB response: {len(data)} bytes at index {index:#x}')
            return data

        stage = 0x8a if args.stage == '8a' else int(args.stage)
        blob = read(0x100 if args.read_reports else stage)
        if args.read_reports or stage >= 8:
            extra = read(0x101)
            version = struct.unpack_from('<I', extra)[0]
            page_count = 8 if version == 0xa7400003 else 6
            blob += extra + b''.join(read(0x100 + i) for i in range(2, page_count))
    pages = [list(struct.unpack('<128I', blob[i:i+512])) for i in range(0, len(blob), 512)]
    if pages[0][0] != 0xa740:
        raise SystemExit(f'bad report magic {pages[0][0]:#x} (probe may not have the 0x56 handler)')
    print(decode(pages[0], pages[1:]))
    if args.save:
        args.save.write_bytes(blob)
    if args.raw:
        for p, r in enumerate(pages):
            print(f'page {p}:')
            for i in range(0, 128, 8):
                print('  w%3d:' % i, ' '.join('%08x' % x for x in r[i:i + 8]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
