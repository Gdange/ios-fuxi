#!/usr/bin/env python3
"""Generate/check the observation-only backlight overlay and build-list hook."""
import argparse
import difflib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-backlight'
PATCH = ROOT / 'xnu/patches/0109-fuxi-backlight.patch'
FILES = ('FuxiBacklight.cpp', 'fuxi_backlight_logic.h')
BUILD = 'iokit/conf/files.arm64'


def expected():
    # 0016 carries upstream lines 5-7. The untouched first four lines are
    # placeholders; replay every subsequent build-list delta with git apply.
    with tempfile.TemporaryDirectory(prefix='fuxi-backlight-baseline-') as tmp:
        dest = Path(tmp) / BUILD
        dest.parent.mkdir(parents=True)
        dest.write_text('\n' * 5 + '# SMP\n' +
                        'iokit/Kernel/arm/AppleARMSMP.cpp    \t\t\t\t\toptional iokitcpp\n')
        for prior in sorted(PATCH.parent.glob('*.patch')):
            if prior.name >= PATCH.name:
                break
            for chunk in re.split(r'(?=^--- (?:a/|/dev/null))',
                                  prior.read_text(errors='surrogateescape'), flags=re.M):
                if re.search(r'^\+\+\+ b/' + re.escape(BUILD) + '$', chunk, re.M):
                    subprocess.run(['git', 'apply', '--whitespace=nowarn', '-'],
                                   input=chunk, text=True, cwd=tmp, check=True, capture_output=True)
        before = {BUILD: dest.read_text()}
    anchor = next(line for line in before[BUILD].splitlines(True)
                  if line.startswith('iokit/Kernel/arm/SweetFramebuffer.cpp '))
    after = {BUILD: before[BUILD].replace(anchor, anchor +
        'iokit/Kernel/arm/FuxiBacklight.cpp                                optional iokitcpp\n')}
    for name in FILES:
        path = 'iokit/Kernel/arm/' + name
        before[path] = ''
        after[path] = (SRC / name).read_text()
    patch = ''.join(''.join(difflib.unified_diff(
        before[path].splitlines(True), value.splitlines(True),
        fromfile='a/' + path if before[path] else '/dev/null', tofile='b/' + path))
        for path, value in after.items())
    return before, patch


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    before, patch = expected()
    if args.check:
        assert PATCH.read_text() == patch, 'run tools/sync-fuxi-backlight.py'
    else:
        PATCH.write_text(patch)
    with tempfile.TemporaryDirectory(prefix='fuxi-backlight-stack-') as tmp:
        for path, value in before.items():
            if value:
                dest = Path(tmp) / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(value)
        subprocess.run(['git', 'apply', '--check', '-'], input=patch, text=True,
                       cwd=tmp, check=True)
    print('PASS: backlight canonical sources and build hook match 0109; overlay applies')


if __name__ == '__main__':
    main()
