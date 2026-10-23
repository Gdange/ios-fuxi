#!/usr/bin/env python3
"""Check/regenerate 0097's canonical governor snapshots, preserving its hooks.

Uses the same snapshot format as sync-fuxi-idle.py and the reviewed-prefix
workflow of sync-fuxi-metal2.py.
"""
import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'xnu/patches/0097-fuxi-touch-boost.patch'


def expected():
    spec = importlib.util.spec_from_file_location('idle_sync', ROOT / 'tools/sync-fuxi-idle.py')
    sync = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sync)
    text = PATCH.read_text()
    prefix = text[:text.index('--- a/bsd/dev/fuxi_perf.c\n')]
    return prefix + ''.join(sync.new_file('bsd/dev/' + name,
        (ROOT / 'xnu/fuxi-perf' / name).read_text())
        for name in ('fuxi_perf.c', 'fuxi_perf_logic.h'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    text = expected()
    if args.check:
        assert PATCH.read_text() == text, '0097 differs; run tools/sync-fuxi-perf.py'
    else:
        PATCH.write_text(text)
    print('PASS: 0097 governor snapshots match canonical sources')


if __name__ == '__main__':
    main()
