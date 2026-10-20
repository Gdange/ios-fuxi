#!/usr/bin/env python3
"""Generate/check the 0098 NativeAdreno patch; 0096 remains standalone.

0096 remains the baseline. 0098 contains the delta to canonical driver/shared
sources plus ABI files and IOKit registration; 0097 is orthogonal.
"""
import argparse
import difflib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'xnu/fuxi-adreno'
FILES = {
    'iokit/Kernel/arm/FuxiAdrenoUserClient.cpp': 'FuxiAdrenoUserClient.cpp',
    'bsd/dev/fuxi_gpu_abi_backend.inc': 'fuxi_gpu_abi_backend.inc',
}
for folder in ('bsd/dev', 'iokit/Kernel/arm'):
    for name in ('NativeAdrenoABI.h', 'fuxi_abi_wire.h', 'fuxi_gpu_abi.h', 'LICENSE.NativeAdreno'):
        FILES[f'{folder}/{name}'] = name


def baseline(name):
    patch = (ROOT/'xnu/patches/0096-fuxi-adreno.patch').read_text()
    block = next(p for p in re.split(r'(?=^--- a/)', patch, flags=re.M)
                 if p.startswith(f'--- a/bsd/dev/{name}\n'))
    return ''.join(line[1:] for line in block.splitlines(True)
                   if line.startswith('+') and not line.startswith('+++'))


CONFIG_BASE = '\n'*13 + 'iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\niokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\niokit/Kernel/arm/SweetSurface.cpp                                optional iokitcpp\niokit/Kernel/arm/SweetFramebuffer.cpp                            optional iokitcpp\niokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\niokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp\niokit/Kernel/arm/hid/IOHIDEvent.cpp                              optional iokitcpp\n'


def content():
    line = 'iokit/Kernel/arm/SweetFramebuffer.cpp                            optional iokitcpp\n'
    changed = CONFIG_BASE.replace(line, line+'iokit/Kernel/arm/FuxiAdrenoUserClient.cpp                         optional iokitcpp\n')
    patch = ''.join(difflib.unified_diff(CONFIG_BASE.splitlines(True), changed.splitlines(True),
        fromfile='a/iokit/conf/files.arm64', tofile='b/iokit/conf/files.arm64'))
    for name in ('gpu.c', 'gpu-a740.h', 'gpu-zap.h', 'gpu-handoff.h', 'fuxi_adreno.c', 'fuxi_adreno.h'):
        folder = BASE if name.startswith('fuxi_') else ROOT/'shim/fuxi-probe'
        patch += ''.join(difflib.unified_diff(baseline(name).splitlines(True),
            (folder/name).read_text().splitlines(True), fromfile=f'a/bsd/dev/{name}', tofile=f'b/bsd/dev/{name}'))
    for target, source in FILES.items():
        patch += ''.join(difflib.unified_diff([], (BASE/source).read_text().splitlines(True),
            fromfile=f'a/{target}', tofile=f'b/{target}'))
    return patch


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    path = ROOT/'xnu/patches/0098-fuxi-native-adreno-abi.patch'
    expected = content()
    if args.check:
        assert path.read_text() == expected, '0098 differs; run sync-fuxi-metal.py'
    else:
        path.write_text(expected)
    print('PASS: 0098 ABI sources match canonical files')


if __name__ == '__main__':
    main()
