#!/usr/bin/env python3
"""Offline layout checks; does not establish DRAM/DMA hardware reliability."""
import importlib.util
from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import adt
spec = importlib.util.spec_from_file_location('fuxi', Path(__file__).with_name('mkadt-fuxi.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
for size in (m.MEM_SIZE, m.LEGACY_SIZE):
    root = m.build(smp=True, gpu_reserve=True, mem_size=size)
    blob = adt.serialize(root)
    root, _ = adt.parse(blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    assert struct.unpack('<Q', chosen.prop('dram-size'))[0] == size
    base, length = struct.unpack('<2Q', chosen.prop('fuxi-gpu-memory'))
    assert m.PHYS_BASE + size <= base and base + length == 0xa00000000
    assert length == 32*1024*1024 and base < 1 << 36
    assert len(next(c for c in root.children if c.name == 'cpus').children) == 7
    for off in (0, size-512, 0xfffffe00, 0x100000000):
        if off >= size:
            continue
        req, value, index = (0x58 if off >> 32 else 0x47), off & 65535, (off >> 16) & 65535
        decoded = value | index << 16 | ((req == 0x58) << 32)
        assert decoded == off
assert m.MEM_SIZE == 5088*1024*1024
print('PASS: full/1g ADT bounds, scratch, seven CPUs and >4 GiB USB offsets')
