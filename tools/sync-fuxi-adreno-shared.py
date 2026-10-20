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
import subprocess

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
    source = ''.join(line[1:] for line in block.splitlines(True) if line.startswith('+') and not line.startswith('+++'))
    overlay = ROOT/'xnu/patches/0098-fuxi-native-adreno-abi.patch'
    if overlay.exists():
        for delta in re.split(r'(?=^--- a/)', overlay.read_text(), flags=re.M):
            if not delta.startswith(f'--- a/bsd/dev/{name}\n'):
                continue
            lines, at, result = source.splitlines(True), 0, []
            for hunk in re.split(r'(?=^@@ )', delta, flags=re.M)[1:]:
                match = re.match(r'@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@[^\n]*\n', hunk)
                start = int(match[1])-1
                result.extend(lines[at:start]); at = start
                for line in hunk[match.end():].splitlines(True):
                    if line[0] in ' -':
                        assert lines[at] == line[1:], (name, at)
                        at += 1
                    if line[0] in ' +': result.append(line[1:])
            result.extend(lines[at:]); source = ''.join(result)
    return source


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    overlay = ROOT/'xnu/patches/0098-fuxi-native-adreno-abi.patch'
    if overlay.exists():
        subprocess.run(['python3', str(ROOT/'tools/sync-fuxi-metal.py')] + (['--check'] if args.check else []), check=True)
        for name in SHARED:
            folder = 'xnu/fuxi-adreno' if name.startswith('fuxi_') else 'shim/fuxi-probe'
            assert added_file(name) == (ROOT/folder/name).read_text(), f'{name} differs after 0098'
        subprocess.run(['python3', str(ROOT/'tools/sync-fuxi-metal2.py'), '--check'], check=True)
        print('PASS: 0096 + 0098 + 0100 driver/shared sources match canonical files')
        return
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
