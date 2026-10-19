#!/usr/bin/env python3
"""Build the single-CPU, RAM-root Fuxi ADT after validating a live FDT.

Uses the same iPhone X userland identity and generic providers as ios-sweet.
Hardware values come from measured Fuxi probes. UFS is absent by default; an
explicit option adds the node used by the read-only storage diagnostic.
"""
import argparse
import importlib.util
import json
import struct
from pathlib import Path

import adt
import mkadt

PHYS_BASE, MEM_SIZE = 0x8c0000000, 0x40000000
FB, FB_SIZE = 0xb8000000, 0x02b00000
GICD, GICR, GICR_SIZE = 0x17100000, 0x17180000, 0x200000


def setprop(node, name, value):
    node.props = [(k, f, value if k == name else v) for k, f, v in node.props]


def validate_live(path, ufs_readonly=False):
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
    if ufs_readonly:
        ufs=nodes.get('/soc/ufshc@1d84000',{})
        if ufs.get('reg',b'')[:8] != struct.pack('>II',0x1d84000,0x3000):
            raise ValueError('UFS host layout differs from the measured Fuxi map')
    splash = nodes.get('/reserved-memory/splash_region', {}).get('reg')
    if splash != struct.pack('>QQ', FB, FB_SIZE):
        raise ValueError('splash reservation differs from the measured Fuxi map')


def root_extent_props(path, root_bytes):
    """/chosen props for the pinned-file root: u64 {file, partition, count} blocks."""
    table = json.loads(path.read_text())
    if root_bytes % 4096 or root_bytes > table['size']:
        raise ValueError('root size must be 4 KiB aligned and inside the pinned file')
    words, pos = [], 0
    for logical, physical, length in table['extents']:
        if logical != pos or physical % 4096 or length % 4096:
            raise ValueError('extent table is not contiguous 4 KiB extents')
        words += [logical // 4096, physical // 4096, length // 4096]
        pos += length
    if pos < root_bytes:
        raise ValueError('extents do not cover the root')
    return [('fuxi-root-extents', struct.pack(f'<{len(words)}Q', *words)),
            ('fuxi-root-blocks', struct.pack('<Q', root_bytes // 4096))]


def build(ufs_readonly=False, root_props=(), smp=False):
    mkadt.HIGH_PHYS_BASE, mkadt.HIGH_MEM_SIZE = PHYS_BASE, MEM_SIZE
    mkadt.GICD, mkadt.GICR, mkadt.GICR_SIZE = GICD, GICR, GICR_SIZE
    mkadt.FB, mkadt.FB_SIZE = FB, FB_SIZE
    mkadt.VTIMER_INTID = 27
    root = mkadt.build(highmem=True)
    setprop(root, 'compatible', b'D22AP\0iPhone10,3\0xiaomi,fuxi\0qcom,sm8550\0AppleARM\0')
    if ufs_readonly:
        ufs=next(n for n in root.children if n.name=='sweet-ufs')
        setprop(ufs,'name',b'fuxi-ufs\0')
        setprop(ufs,'compatible',b'qcom,sm8550-ufshc\0')
    else:
        root.children = [n for n in root.children if n.name != 'sweet-ufs']
    chosen = next(n for n in root.children if n.name == 'chosen')
    setprop(chosen, 'firmware-version', b'fuxi-shim\0')
    setprop(chosen, 'system-firmware-version', b'fuxi-shim\0')
    setprop(chosen, 'unique-chip-id', struct.pack('<Q', 0x4655584900000001))
    chosen.props += [(k, 0, v) for k, v in root_props]
    cpus = next(n for n in root.children if n.name == 'cpus')
    # MPIDR Aff1 = core. cpu@100 keeps running the loader's USB monitor, so XNU
    # gets core 0 and, with smp, cores 2-7 (logical IDs 0-6 in this order).
    cpus.children = [cpus.children[c] for c in ((0, 2, 3, 4, 5, 6, 7) if smp else (0,))]
    for cpu in cpus.children:
        setprop(cpu, 'compatible', b'qcom,sm8550-cpu\0ARM,v8\0')
        setprop(cpu, 'clock-frequency', struct.pack('<I', 2016000000))
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--live-fdt', type=Path, required=True)
    ap.add_argument('--ufs-readonly',action='store_true',help='include UFS node for the opt-in, read-only kernel probe')
    ap.add_argument('--root-extents',type=Path,help='pinned-file extent JSON (bytes, userdata-relative); needs --root-bytes')
    ap.add_argument('--root-bytes',type=int,help='root image size in bytes')
    ap.add_argument('--smp',action='store_true',help='add cores 2-7 (cpu@100 stays with the USB monitor)')
    args = ap.parse_args()
    try:
        root_props = ()
        if args.root_extents:
            args.ufs_readonly = True  # the extent root needs the UFS node
            root_props = root_extent_props(args.root_extents, args.root_bytes or 0)
        validate_live(args.live_fdt,args.ufs_readonly)
        blob = adt.serialize(build(args.ufs_readonly, root_props, args.smp))
    except (ValueError, KeyError, struct.error) as error:
        ap.exit(1, str(error) + '\n')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f'{args.out}: {len(blob)} bytes; RAM {PHYS_BASE:#x}+{MEM_SIZE:#x}; CPUs {"0,2-7" if args.smp else "0"}; UFS={"read-only probe" if args.ufs_readonly else "absent"}')


if __name__ == '__main__':
    main()
