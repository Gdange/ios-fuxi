#!/usr/bin/env python3
"""Collect the standalone Fuxi probe report and live FDT over USB (requires pyusb)."""
import argparse
import json
import struct
import time
from pathlib import Path

import usb.core
import usb.util


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path, help='private output directory (use research/)')
    ap.add_argument('--wait', type=float, default=30)
    ap.add_argument('--display', action='store_true', help='also read the known SM8550 display registers')
    ap.add_argument('--paint', action='store_true', help='draw a diagnostic title using the validated scanout')
    ap.add_argument('--ram', action='store_true', help='sample and restore words in the advertised upper RAM bank')
    ap.add_argument('--core', action='store_true', help='request isolated cpu@100 diagnostic startup')
    ap.add_argument('--delegate', action='store_true', help='hand USB polling to the isolated diagnostic CPU')
    ap.add_argument('--ufs', action='store_true', help='read a clock-guarded UFS handoff snapshot without issuing storage commands')
    ap.add_argument('--ufs-wake', action='store_true', help='temporarily clear UFS AHB hardware gating, snapshot, then restore it')
    ap.add_argument('--display-clocks', action='store_true', help='read GCC/DispCC clock state, skipping a halted bus')
    args = ap.parse_args()
    deadline = time.monotonic() + args.wait
    dev = None
    while time.monotonic() < deadline:
        devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
        for candidate in devices:
            try:
                product = usb.util.get_string(candidate, candidate.iProduct)
            except (usb.core.USBError, ValueError) as error:
                ap.exit(1, f'Cannot open USB probe: {error}. Check access to its USB device node.\n')
            if product == 'Fuxi hardware probe':
                if dev is not None:
                    ap.exit(1, 'Multiple Fuxi probes found\n')
                dev = candidate
        if dev is not None:
            break
        time.sleep(.2)
    if dev is None:
        ap.exit(1, 'Fuxi probe did not enumerate within the wait interval\n')
    names = ('magic version midr mpidr mmfr0 mmfr1 current_el cntfrq sctlr '
             'fdt_address fdt_size gsnpsid gctl_before dctl_before dsts_before '
             'gcc_master gcc_cfg configured speed').split()
    raw = bytes(dev.ctrl_transfer(0xc0, 0x40, 0, 0, 8 * len(names), timeout=3000))
    if raw[:8] != b'FXIPROBE' or len(raw) != 8 * len(names):
        ap.exit(1, 'Unexpected probe report\n')
    report = dict(zip(names, struct.unpack('<' + 'Q' * len(names), raw)))
    if report['version'] != 1 or not 40 <= report['fdt_size'] <= 0x1000000:
        ap.exit(1, 'Unsupported report or invalid live FDT size\n')
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    tree = bytearray()
    for offset in range(0, report['fdt_size'], 512):
        length = min(512, report['fdt_size'] - offset)
        chunk = bytes(dev.ctrl_transfer(0xc0, 0x41, offset & 0xffff,
                                      offset >> 16, length, timeout=3000))
        if len(chunk) != length:
            ap.exit(1, f'Short transfer at {offset}\n')
        tree.extend(chunk)
    if tree[:4] != b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I', tree, 4)[0] != len(tree):
        ap.exit(1, 'Invalid live FDT header\n')
    (args.out / 'live.dtb').write_bytes(tree)
    if args.display:
        raw = bytes(dev.ctrl_transfer(0xc0, 0x42, 0, 0, 432, timeout=3000))
        if len(raw) != 432:
            ap.exit(1, 'Short display register snapshot\n')
        values = struct.unpack('<108I', raw)
        fields = ('base', 'source_size', 'output_size', 'address', 'stride',
                  'format', 'unpack', 'op_mode', 'multirect')
        display = {'sspp': [dict(zip(fields, values[i:i + 9])) for i in range(0, 90, 9)],
                   'ctl': [dict(zip(('base', 'top', 'start'), values[i:i + 3]))
                           for i in range(90, 108, 3)]}
        (args.out / 'display.json').write_text(json.dumps(display, indent=2) + '\n')
    if args.paint:
        raw = bytes(dev.ctrl_transfer(0xc0, 0x43, 0, 0, 4, timeout=3000))
        if raw != b'\x01\0\0\0':
            ap.exit(1, 'Display geometry did not match the validated layout; paint refused\n')
        print('Diagnostic title written and CTL0 START issued; visual confirmation needed')
    if args.ram:
        raw = bytes(dev.ctrl_transfer(0xc0, 0x44, 0, 0, 64, timeout=3000))
        names = ('base', 'size', 'stride', 'samples', 'pass_mask', 'first_error', 'image_base', 'image_end')
        ram = dict(zip(names, struct.unpack('<8Q', raw)))
        (args.out / 'ram.json').write_text(json.dumps(ram, indent=2) + '\n')
        if ram['first_error'] or ram['pass_mask'] != (1 << ram['samples']) - 1:
            ap.exit(1, 'Upper RAM sample/restore check failed\n')
        print(f"Upper RAM samples passed at {ram['base']:#x}+{ram['size']:#x}")
    if args.core or args.delegate:
        raw = bytes(dev.ctrl_transfer(0xc0, 0x45, 0, 0, 80, timeout=3000))
        fields = 'magic target result alive midr mpidr current_el sctlr heartbeat timestamp'.split()
        core = dict(zip(fields, struct.unpack('<10Q', raw)))
        time.sleep(.1)
        raw2 = bytes(dev.ctrl_transfer(0xc0, 0x45, 0, 0, 80, timeout=3000))
        later = dict(zip(fields, struct.unpack('<10Q', raw2)))
        (args.out / 'core.json').write_text(json.dumps({'first':core, 'later':later}, indent=2)+'\n')
        if not later['alive'] or later['result'] or later['heartbeat'] <= core['heartbeat']:
            ap.exit(1, f'CPU diagnostic startup failed: {later}\n')
        print(f"Secondary CPU alive: MPIDR={later['mpidr']:#x}, MIDR={later['midr']:#x}, heartbeat={later['heartbeat']}")
    if args.delegate:
        fields = 'magic delegate active usb_mpidr launch stage boot_args entry deadline image end'.split()
        dev.ctrl_transfer(0xc0, 0x46, 1, 0, 88, timeout=3000)
        for _ in range(50):
            time.sleep(.02)
            raw = bytes(dev.ctrl_transfer(0xc0, 0x46, 0, 0, 88, timeout=3000))
            monitor = dict(zip(fields, struct.unpack('<11Q', raw)))
            if monitor['active'] and monitor['usb_mpidr'] & 0xffffff == 0x100:
                break
        else:
            ap.exit(1, 'USB delegation did not complete on cpu@100\n')
        (args.out / 'monitor.json').write_text(json.dumps(monitor, indent=2)+'\n')
        print(f"USB requests now serviced by CPU MPIDR={monitor['usb_mpidr']:#x}")
    if args.ufs or args.ufs_wake:
        raw = bytes(dev.ctrl_transfer(0xc0,0x4d if args.ufs_wake else 0x4b,0x5546 if args.ufs_wake else 0,0,96,timeout=3000))
        names='magic version ahb axi unipro gdsc valid reserved cap hci_version ahit interrupt_status interrupt_enable host_status host_enable list_lo list_hi doorbell list_running task_list_lo task_list_hi task_doorbell task_running uic_command'.split()
        snapshot=dict(zip(names,struct.unpack('<24I',raw)))
        (args.out / 'ufs.json').write_text(json.dumps(snapshot,indent=2)+'\n')
        print('UFS snapshot:', {k:hex(v) for k,v in snapshot.items()})
    if args.display_clocks:
        raw=bytes(dev.ctrl_transfer(0xc0,0x4c,0,0,56,timeout=3000))
        names='magic version gcc_ahb gcc_hf_axi valid gdsc ahb mdp byte0 byte0_intf pclk0 esc0 vsync non_gdsc_ahb'.split()
        clocks=dict(zip(names,struct.unpack('<14I',raw)))
        (args.out/'display-clocks.json').write_text(json.dumps(clocks,indent=2)+'\n')
        print('Display clocks:',{k:hex(v) for k,v in clocks.items()})
    print(f"Saved {len(tree)} bytes of live FDT to {args.out}")
    print(f"MIDR={report['midr']:#x}, MPIDR={report['mpidr']:#x}, "
          f"MMFR0={report['mmfr0']:#x}, EL={report['current_el'] >> 2}")


if __name__ == '__main__':
    main()
