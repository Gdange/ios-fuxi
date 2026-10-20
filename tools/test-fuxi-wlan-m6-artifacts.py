#!/usr/bin/env python3
"""Offline verification of M6 packing against the dedicated lane's M5 outputs."""
import argparse
import hashlib
from pathlib import Path
import struct

import mkpayload


def sections(elf):
    assert elf[:6] == b'\x7fELF\x02\x01'
    offset = struct.unpack_from('<Q', elf, 40)[0]
    stride, count, strings_index = struct.unpack_from('<HHH', elf, 58)
    assert stride == 64 and strings_index < count
    headers = [struct.unpack_from('<IIQQQQIIQQ', elf, offset + i * stride) for i in range(count)]
    strings = headers[strings_index]
    names = elf[strings[4]:strings[4] + strings[5]]
    result = {}
    for header in headers:
        name = names[header[0]:].split(b'\0', 1)[0].decode()
        result[name] = (header[3], elf[header[4]:header[4] + header[5]])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--milestone', choices=('m6', 'm6b', 'm6c', 'm6d'), default='m6')
    args = parser.parse_args()
    root = args.root
    lane = 'wlan' + args.milestone[1:]
    out, old = root / f'xnu/out-fuxi-{lane}', root / 'xnu/out-fuxi-wlan5'
    shim = root / 'shim/out'
    adt, cmdline = (out / 'fuxi.adt').read_bytes(), (out / 'cmdline.txt').read_text().strip()
    assert adt == (old / 'fuxi.adt').read_bytes(), 'M5 ADT must remain byte-identical'
    assert cmdline == (old / 'cmdline.txt').read_text().strip(), 'M5 boot arguments changed'
    assert cmdline.split().count('fuxiwlan=1') == 1 and len(cmdline.encode()) < 608
    verify_variant(root, out, old, args.milestone, '', 1)
    if args.milestone in ('m6b', 'm6c', 'm6d'):
        verify_variant(root, out, old, args.milestone, '-s20', 20)
    for line in (out / 'SHA256SUMS').read_text().splitlines():
        digest, filename = line.split(maxsplit=1)
        assert hashlib.sha256((root / filename).read_bytes()).hexdigest() == digest
    print(args.milestone, 'all manifest hashes verified')


def verify_variant(root, out, old, milestone, suffix, limit):
    shim = root / 'shim/out'
    adt = (out / 'fuxi.adt').read_bytes()
    cmdline = (out / f'cmdline{suffix}.txt').read_text().strip()
    expected = (old / 'cmdline.txt').read_text().strip().split()
    expected[expected.index('fuxiwlan=1')] = f'fuxiwlan={limit}'
    assert cmdline.split() == expected and len(cmdline.encode()) < 608
    stem = f'fuxi-wlan-{milestone}{suffix}'
    payload = (shim / f'{stem}-payload.bin').read_bytes()
    rebuilt, _ = mkpayload.build(out / 'kernel.development.fuxi', out / 'fuxi.adt', cmdline)
    assert payload == rebuilt, 'payload differs from packed kernel/ADT/cmdline'
    assert struct.unpack_from('<QQ', payload, 56) == (0x8c0000000, 0x13e000000)
    assert b'M6 apps_smmu' in payload and b'SMMU owned stage1' in payload
    if milestone in ('m6b', 'm6c', 'm6d'):
        assert b'M6b RC bypassed table zeroed' in payload and b'RC table in use' in payload
    if milestone == 'm6c':
        assert b'M6c CB probe' in payload and b'M6c SMMU mismatch' in payload
    if milestone == 'm6d':
        assert b'M6d CB probe' in payload and b'M6d SMMU %s' in payload
        assert b' mask %#x faults' in payload
    elf = (shim / f'{stem}-loader.elf').read_bytes()
    sec = sections(elf)
    assert sec['.payload'][1] == payload
    gpu = sec['.gpu_fw'][1]
    assert len(gpu) == 0x25ac8
    sizes_hashes = [
        (66844, '1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98'),
        (75380, '6d38046654333a6a5a4ee77ab67abc24e3c0c5c68237513afe98e94da8fc9fcc'),
        (12088, '86e4c3f61d4ddbe581405f4cc496182cf385bee39aaefec61639a31d3619fd35'),
    ]
    offset = 0
    for size, digest in sizes_hashes:
        assert hashlib.sha256(gpu[offset:offset + size]).hexdigest() == digest
        offset += size
    boot = (shim / f'{stem}-boot.img').read_bytes()
    old_boot = (shim / 'fuxi-wlan-m5-boot.img').read_bytes()
    assert boot[:8] == b'ANDROID!' and struct.unpack_from('<I', boot, 40)[0] == 4
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<IIII', boot, 8)
    assert ramdisk_size == 0 and header_size == 1584
    assert struct.unpack_from('<I', boot, 1580)[0] == 0  # no boot signature
    # Exact M5 header except kernel byte count; padding/header format/OS unchanged.
    assert boot[:8] + boot[12:4096] == old_boot[:8] + old_boot[12:4096]
    image = boot[4096:4096 + kernel_size]
    assert len(boot) == 4096 + ((kernel_size + 4095) & ~4095)
    assert image[56:60] == b'ARM\x64' and struct.unpack_from('<Q', image, 16)[0] == len(image)
    for name in ('.gpu_fw', '.payload'):
        address, data = sec[name]
        assert image[address:address + len(data)] == data
    assert not any(boot[4096 + kernel_size:])
    print(stem, 'artifacts: kernel/payload/ELF/boot match; M5 ADT/cmdline/header preserved; .gpu_fw 0x25ac8 and all hashes verified')
    print('boot SHA256', hashlib.sha256(boot).hexdigest())


if __name__ == '__main__':
    main()
