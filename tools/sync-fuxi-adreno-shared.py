#!/usr/bin/env python3
"""Refresh/check 0096's exact copies of the proven loader GPU sources.

XNU is an external patch-only tree. The snapshots let 0096 apply on its own;
this check makes the loader the single source for register/PM4/PAS changes.
Run after editing any shared file, then rebuild both environments.
"""
import argparse
import difflib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SHARED = ('gpu.c', 'gpu-a740.h', 'gpu-zap.h', 'gpu-handoff.h',
          'fuxi_adreno.c', 'fuxi_adreno.h')


def patch_files():
    patch = (ROOT / 'xnu/patches/0096-fuxi-adreno.patch').read_text()
    parts = re.split(r'(?=^--- a/)', patch, flags=re.M)
    return patch, parts


def added_file(name):
    _, parts = patch_files()
    block = next(p for p in parts if p.startswith(f'--- a/bsd/dev/{name}\n'))
    assert '@@ -0,0 ' in block, f'{name} must be a new file'
    return ''.join(line[1:] for line in block.splitlines(True) if line.startswith('+') and not line.startswith('+++'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    original, parts = patch_files()
    for name in SHARED:
        folder = 'xnu/fuxi-adreno' if name.startswith('fuxi_') else 'shim/fuxi-probe'
        source = (ROOT / folder / name).read_text()
        if args.check:
            assert added_file(name) == source, f'{name} differs; run {Path(__file__).name}'
        else:
            index = next(i for i, p in enumerate(parts) if p.startswith(f'--- a/bsd/dev/{name}\n'))
            parts[index] = ''.join(difflib.unified_diff([], source.splitlines(True),
                fromfile=f'a/bsd/dev/{name}', tofile=f'b/bsd/dev/{name}'))
    if not args.check:
        (ROOT / 'xnu/patches/0096-fuxi-adreno.patch').write_text(''.join(parts))
    print('PASS: 0096 driver/shared sources match their canonical files')


if __name__ == '__main__':
    main()
