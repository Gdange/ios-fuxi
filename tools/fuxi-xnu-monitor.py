#!/usr/bin/env python3
"""Collect early XNU progress from Fuxi's isolated native USB monitor.

The image must match --payload and --kernel. Only the selected 1 GiB XNU RAM
window can be read; no storage writes or unrestricted physical-memory requests.
Private output includes native reports, staged progress and console bytes.
"""
import argparse
import json
from pathlib import Path
import struct
import time

import usb.core
import usb.util

MON_FIELDS = 'magic delegate active usb_mpidr launch stage boot_args entry deadline image end'.split()
WANTED = {'_fuxi_early_status', '_sweet_log_ring', '_sweet_log_total'}


def symbols(path):
    b = path.read_bytes()
    magic, cpu, _, _, count, _, _, _ = struct.unpack_from('<8I', b)
    if magic != 0xfeedfacf or cpu != 0x100000c:
        raise ValueError('expected an arm64 Mach-O kernel')
    offset = 32
    result = {}
    for _ in range(count):
        command, size = struct.unpack_from('<II', b, offset)
        if command == 2:
            start, n, strings, _ = struct.unpack_from('<4I', b, offset + 8)
            for i in range(n):
                nameoff, kind, _, _, value = struct.unpack_from('<IBBHQ', b, start + 16 * i)
                if kind & 0x0e != 0x0e:
                    continue
                name_start = strings + nameoff
                name = b[name_start:b.find(b'\0', name_start)].decode(errors='replace')
                if name in WANTED:
                    result[name] = value
        offset += size
    if result.keys() != WANTED:
        raise ValueError(f'missing XNU diagnostic symbols: {WANTED - result.keys()}')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--payload', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--seconds', type=float, default=65)
    args = ap.parse_args()
    payload = args.payload.read_bytes()
    if payload[:8] != b'SWEETXNU':
        ap.error('invalid payload')
    virt, _, _, _, _, phys, size = struct.unpack_from('<7Q', payload, 16)
    if (phys, size) != (0x8c0000000, 0x40000000):
        ap.error('unexpected Fuxi RAM window')
    offsets = {name: value - virt for name, value in symbols(args.kernel).items()}
    if any(not 0 <= off < size - 0x400000 for off in offsets.values()):
        ap.error('symbol outside XNU window')
    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    devices = [d for d in devices if usb.util.get_string(d, d.iProduct) == 'Fuxi hardware probe']
    if len(devices) != 1:
        ap.error('expected exactly one Fuxi native monitor')
    dev = devices[0]
    args.out.mkdir(parents=True, exist_ok=False)

    def request(number, length, value=0, index=0):
        data = bytes(dev.ctrl_transfer(0xc0, number, value, index, length, timeout=3000))
        if len(data) != length:
            raise ValueError(f'short USB response: {number:#x}')
        return data

    def monitor(value=0):
        b = request(0x46, 88, value)
        if b[:8] != b'FXIMON01':
            raise ValueError('wrong native monitor protocol')
        return dict(zip(MON_FIELDS, struct.unpack('<11Q', b)))

    def read_ram(offset, length):
        if offset < 0 or length < 0 or offset + length > size:
            raise ValueError('RAM request outside XNU window')
        output = bytearray()
        for off in range(offset, offset + length, 512):
            output.extend(request(0x47, min(512, offset + length - off), off & 0xffff, off >> 16))
        return bytes(output)

    report = request(0x40, 152)
    if report[:8] != b'FXIPROBE':
        raise ValueError('wrong hardware report')
    (args.out / 'native-report.bin').write_bytes(report)
    core = struct.unpack('<10Q', request(0x45, 80))
    if core[2] or not core[3] or core[5] & 0xffffff != 0x100:
        raise ValueError(f'second CPU startup failed: {core}')
    monitor(1)
    for _ in range(100):
        time.sleep(.01)
        before = monitor()
        if before['active'] and before['usb_mpidr'] & 0xffffff == 0x100:
            break
    else:
        raise ValueError('USB polling did not move to cpu@100')
    (args.out / 'before.json').write_text(json.dumps(before, indent=2)+'\n')
    print('USB monitor active on cpu@100; requesting XNU handoff', flush=True)
    ufs=request(0x4b,96)
    names='magic version ahb axi unipro gdsc valid reserved cap hci_version ahit interrupt_status interrupt_enable host_status host_enable list_lo list_hi doorbell list_running task_list_lo task_list_hi task_doorbell task_running uic_command'.split()
    (args.out / 'ufs-before.json').write_text(json.dumps(dict(zip(names,struct.unpack('<24I',ufs))),indent=2)+'\n')
    (args.out / 'display-before.bin').write_bytes(request(0x42,432))
    request(0x49, 88, 0x584e)
    last_total = 0
    previous = None
    until = time.monotonic() + args.seconds
    with (args.out / 'progress.jsonl').open('w') as progress, (args.out / 'boot.log').open('wb') as log:
        while time.monotonic() < until:
            status = monitor()
            if status['stage'] == 99:
                status['error'] = request(0x48, 128).split(b'\0', 1)[0].decode(errors='replace')
            if status['stage'] >= 8 and status['stage'] != 99:
                early = read_ram(offsets['_fuxi_early_status'], 128)
                status['kernel_stage'] = struct.unpack_from('<Q', early)[0]
                status['kernel_label'] = early[8:].split(b'\0', 1)[0].decode(errors='replace')
                total = struct.unpack('<Q', read_ram(offsets['_sweet_log_total'], 8))[0]
                status['console_total'] = total
                if not last_total <= total < 0x10000000:
                    raise ValueError(f'invalid console counter: {total}')
                start = max(last_total, total - 65536)
                if start > last_total:
                    status['missed_console_bytes'] = start - last_total
                while start < total:
                    index = start % 0x400000
                    length = min(total-start, 0x400000-index)
                    log.write(read_ram(offsets['_sweet_log_ring']+index, length))
                    start += length
                log.flush()
                last_total = total
            progress.write(json.dumps(status)+'\n')
            progress.flush()
            brief = {k: status[k] for k in ('stage','kernel_stage','kernel_label','console_total','error') if k in status}
            if brief != previous:
                print(brief, flush=True)
                previous = brief
            if status['stage'] == 99:
                break
            time.sleep(.25)


if __name__ == '__main__':
    main()
