#!/usr/bin/env python3
"""Build the single-CPU, RAM-root Fuxi ADT after validating a live FDT.

Uses the same iPhone X userland identity and generic providers as ios-sweet.
Hardware values come from Fuxi probe-02/03, not from Sweet. No UFS node is emitted.
"""
import argparse
import importlib.util
import struct
from pathlib import Path

import adt
import mkadt

PHYS_BASE, MEM_SIZE = 0x8c0000000, 0x40000000
FB, FB_SIZE = 0xb8000000, 0x02b00000
GICD, GICR, GICR_SIZE = 0x17100000, 0x17180000, 0x200000


def setprop(node, name, value):
    node.props = [(k, f, value if k == name else v) for k, f, v in node.props]


def validate_live(path):
    spec = importlib.util.spec_from_file_location('fuxi_fdt', Path(__file__).with_name('fuxi-inspect-dtb.py'))
    fdt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fdt)
    nodes, reservations, _ = fdt.parse(path.read_bytes())
    if b'fuxi' not in nodes['/'].get('model', b'').lower():
        raise ValueError('live FDT does not identify a Fuxi board')
    memory = nodes.get('/memory', {}).get('reg', b'')
    if len(memory) % 16:
        raise ValueError('unexpected /memory cell layout')
    ranges = list(struct.iter_unpack('>QQ', memory))
    if not any(base <= PHYS_BASE and PHYS_BASE + MEM_SIZE <= base + size for base, size in ranges):
        raise ValueError('requested XNU RAM is outside advertised memory')
    inventory = fdt.describe(nodes, reservations)['nodes']
    for name, props in inventory.items():
        if name.startswith('/reserved-memory/'):
            for region in props.get('decoded-reg', []):
                base, size = int(region['address'], 16), int(region['size'], 16)
                if size and base < PHYS_BASE + MEM_SIZE and PHYS_BASE < base + size:
                    raise ValueError(f'XNU RAM overlaps {name}')
    for region in reservations:
        base, size = int(region['address'], 16), int(region['size'], 16)
        if size and base < PHYS_BASE + MEM_SIZE and PHYS_BASE < base + size:
            raise ValueError('XNU RAM overlaps FDT reserve map')
    gic = nodes.get('/soc/interrupt-controller@17100000', {})
    if gic.get('reg') != struct.pack('>4I', GICD, 0x10000, GICR, GICR_SIZE):
        raise ValueError('GIC layout differs from the validated Fuxi map')
    splash = nodes.get('/reserved-memory/splash_region', {}).get('reg')
    if splash != struct.pack('>QQ', FB, FB_SIZE):
        raise ValueError('splash reservation differs from the measured Fuxi map')


def build():
    mkadt.HIGH_PHYS_BASE, mkadt.HIGH_MEM_SIZE = PHYS_BASE, MEM_SIZE
    mkadt.GICD, mkadt.GICR, mkadt.GICR_SIZE = GICD, GICR, GICR_SIZE
    mkadt.FB, mkadt.FB_SIZE = FB, FB_SIZE
    mkadt.VTIMER_INTID = 27
    root = mkadt.build(highmem=True)
    setprop(root, 'compatible', b'D22AP\0iPhone10,3\0xiaomi,fuxi\0qcom,sm8550\0AppleARM\0')
    root.children = [n for n in root.children if n.name != 'sweet-ufs']
    chosen = next(n for n in root.children if n.name == 'chosen')
    setprop(chosen, 'firmware-version', b'fuxi-shim\0')
    setprop(chosen, 'system-firmware-version', b'fuxi-shim\0')
    setprop(chosen, 'unique-chip-id', struct.pack('<Q', 0x4655584900000001))
    cpus = next(n for n in root.children if n.name == 'cpus')
    cpus.children = cpus.children[:1]
    setprop(cpus.children[0], 'compatible', b'qcom,sm8550-cpu\0ARM,v8\0')
    setprop(cpus.children[0], 'clock-frequency', struct.pack('<I', 2016000000))
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--live-fdt', type=Path, required=True)
    args = ap.parse_args()
    try:
        validate_live(args.live_fdt)
        blob = adt.serialize(build())
    except (ValueError, KeyError, struct.error) as error:
        ap.exit(1, str(error) + '\n')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f'{args.out}: {len(blob)} bytes; RAM {PHYS_BASE:#x}+{MEM_SIZE:#x}; CPU0 only; no UFS')


if __name__ == '__main__':
    main()
