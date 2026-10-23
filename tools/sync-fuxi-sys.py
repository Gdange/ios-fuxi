#!/usr/bin/env python3
"""Generate/check patch 0107 (fuxi-sys) against the preceding patch stack.

Canonical final files are in xnu/fuxi-sys/. The baseline of each file is
rebuilt with git apply from every earlier patch that touches it, so a change
to sweet_amfi.c by an earlier patch (0103 H1 or a later lane) is caught.
"""
import argparse
import difflib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'xnu/patches/0107-fuxi-sys.patch'
FILES = {
    'bsd/kern/sweet_amfi.c': 'sweet_amfi.c',
    'bsd/kern/fuxi_sys.h': 'fuxi_sys.h',
}


def baseline():
    with tempfile.TemporaryDirectory(prefix='fuxi-sys-stack-') as tmp:
        for patch in sorted(PATCH.parent.glob('*.patch')):
            if patch.name >= PATCH.name:
                break
            chunks = re.split(r'(?=^--- (?:a/|/dev/null))', patch.read_text(errors='surrogateescape'), flags=re.M)
            for chunk in chunks:
                match = re.search(r'^\+\+\+ b/(.+)$', chunk, re.M)
                if not match or match[1] not in FILES:
                    continue
                subprocess.run(['git', 'apply', '--whitespace=nowarn', '-'], input=chunk,
                               text=True, cwd=tmp, check=True, capture_output=True)
        return {name: (Path(tmp) / name).read_text() if (Path(tmp) / name).exists() else ''
                for name in FILES}


def expected_patch(before):
    return ''.join(''.join(difflib.unified_diff(
        before[name].splitlines(True), (ROOT / 'xnu/fuxi-sys' / local).read_text().splitlines(True),
        fromfile='a/' + name if before[name] else '/dev/null', tofile='b/' + name))
        for name, local in FILES.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    before = baseline()
    if not before['bsd/kern/sweet_amfi.c']:
        raise SystemExit('no earlier patch creates bsd/kern/sweet_amfi.c')
    expected = expected_patch(before)
    if args.check:
        assert PATCH.read_text() == expected, 'fuxi-sys sources differ; run tools/sync-fuxi-sys.py'
    else:
        PATCH.write_text(expected)
    with tempfile.TemporaryDirectory(prefix='fuxi-sys-check-') as tmp:
        for name, text in before.items():
            if text:
                dest = Path(tmp) / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(text)
        subprocess.run(['git', 'apply', '--check', '-'], input=expected, text=True,
                       cwd=tmp, check=True)
    print('PASS: fuxi-sys canonical sources match 0107; preceding patches and 0107 apply')


if __name__ == '__main__':
    main()
