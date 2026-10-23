#!/usr/bin/env python3
"""Generate/check patch 0105 (fuxi-idle) from xnu/fuxi-idle/.

xnu/fuxi-idle/hooks.diff holds the hunks for upstream files (FUXI-only hooks
in kern_exec.c, IOPMrootDomain.cpp, cpu_common.c, cpu.c, sleh.c,
files.arm64, sched_prim.c). The new files osfmk/arm64/fuxi_idle.c and
fuxi_idle_logic.h are snapshots of the canonical sources next to it.

  --check          the checked-in patch must equal the generated one
  --tree DIR       also dry-run the patch with `patch -p1` in DIR, an xnu
                   tree with every patch before 0105 applied (and not 0105)
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-idle'
PATCH = ROOT / 'xnu/patches/0105-fuxi-idle.patch'
NEW = ('fuxi_idle_logic.h', 'fuxi_idle.c')


def new_file(path, text):
    lines = text.splitlines(True)
    assert lines and text.endswith('\n'), path
    return '--- a/%s\n+++ b/%s\n@@ -0,0 +1,%d @@\n' % (path, path, len(lines)) + ''.join('+' + l for l in lines)


def expected():
    hooks = (SRC / 'hooks.diff').read_text()
    paths = re.findall(r'^--- a/(\S+)\n', hooks, re.M)
    assert paths == sorted(paths) and len(paths) == len(set(paths)), 'hooks.diff: one sorted section per file'
    assert not any(p.startswith('osfmk/arm64/fuxi_idle') for p in paths), 'hooks.diff: new files come from sources'
    return hooks + ''.join(new_file('osfmk/arm64/' + n, (SRC / n).read_text()) for n in NEW)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--tree')
    args = ap.parse_args()
    text = expected()
    if args.check:
        assert PATCH.read_text() == text, 'fuxi-idle sources differ; run tools/sync-fuxi-idle.py'
    else:
        PATCH.write_text(text)
    if args.tree:
        subprocess.run(['patch', '-p1', '--dry-run', '-s', '-N', '-F0'], input=text, text=True,
                       cwd=args.tree, check=True)
    print('PASS: fuxi-idle sources and hooks.diff match 0105' + ('; 0105 applies' if args.tree else ''))


if __name__ == '__main__':
    main()
