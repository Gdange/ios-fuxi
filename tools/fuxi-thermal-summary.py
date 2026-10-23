#!/usr/bin/env python3
"""Summarise the steady-state heat of one or more boots (cooling A/B).

  fuxi-thermal-summary.py OUT [OUT...] [--settle SECONDS] [--json]

Each OUT is a monitor output directory (fuxi-xnu-monitor.py --thermal-log N):
OUT/thermal.jsonl gives TSENS temperatures and EPSS clocks per sample and
OUT/boot.log the console. Only samples at least --settle seconds after the
handoff (default 300) count, so the boot's own work is left out. Printed per
boot: sample count, medians of the hottest CPU sensor and of the silver,
gold/prime, CPUSS, GPU and DDR groups, median EPSS MHz per domain, and from
the console the median of the patch-0105 "FUXI IDLE: cpuload" lines (avg busy
percent, irq/ipi/timer per second; booted with fuxicpuload=N) plus counts of
the cooling markers. The first OUT is the baseline; later ones also show the
difference of each median to it.
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import sys

GROUPS = (('cpu_max', None), ('silver', 'cpu-0-'), ('gold', 'cpu-1-'), ('cpuss', 'cpuss'),
          ('gpu', 'gpuss'), ('ddr', 'ddr'))
CPULOAD = re.compile(r'FUXI IDLE: cpuload (\d+)s up (\d+)s avg (\d+)% irq (\d+) ipi (\d+) tmr (\d+)')
MARKERS = {
    'sleep_refused': 'FUXI IDLE: system sleep refused',
    'xpcid_kept': 'exec keeps pidversion',
    'mtl_deny_mediaserverd': 'gate deny mediaserverd',
}


def median(values):
    values = [v for v in values if v is not None]
    return round(statistics.median(values), 1) if values else None


def group_max(temps, prefix):
    if prefix is None:
        values = [v for k, v in temps.items() if k.startswith(('cpu-', 'cpuss'))]
    else:
        values = [v for k, v in temps.items() if k.startswith(prefix)]
    return max(values) if values else None


def summarise(out, settle):
    out = Path(out)
    result = {'out': str(out), 'samples': 0}
    rows = []
    thermal = out/'thermal.jsonl'
    if thermal.exists():
        for line in thermal.read_text().splitlines():
            try:
                t = json.loads(line)
            except ValueError:
                continue  # a sample cut off by the end of the run
            if t.get('since_handoff', 0) >= settle and t.get('temps'):
                rows.append(t)
    result['samples'] = len(rows)
    for name, prefix in GROUPS:
        result[name] = median(group_max(t['temps'], prefix) for t in rows)
    mhz = {}
    for t in rows:
        for c in t.get('cpu', []):
            mhz.setdefault(c['domain'], []).append(c.get('state_mhz'))
    result['mhz'] = {d: median(v) for d, v in sorted(mhz.items())}
    log = out/'boot.log'
    text = log.read_bytes().decode('utf-8', 'replace') if log.exists() else ''
    loads = [tuple(int(x) for x in m.groups()) for m in CPULOAD.finditer(text)]
    loads = [l for l in loads if l[1] >= settle]
    result['cpuload_lines'] = len(loads)
    for i, name in ((2, 'busy_pct'), (3, 'irq_s'), (4, 'ipi_s'), (5, 'tmr_s')):
        result[name] = median(l[i] for l in loads)
    for name, marker in MARKERS.items():
        result[name] = text.count(marker)
    return result


FIELDS = ('cpu_max', 'silver', 'gold', 'cpuss', 'gpu', 'ddr', 'busy_pct', 'irq_s', 'ipi_s', 'tmr_s')


def show(value):
    return '-' if value is None else ('%g' % value)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('outs', nargs='+', help='monitor output directories; the first is the baseline')
    ap.add_argument('--settle', type=float, default=300, help='seconds after handoff to skip (default 300)')
    ap.add_argument('--json', action='store_true', help='print the summaries as JSON')
    a = ap.parse_args(argv)
    results = [summarise(o, a.settle) for o in a.outs]
    if a.json:
        print(json.dumps(results, indent=1))
        return 0
    base = results[0]
    for r in results:
        print('%s: %d samples after %gs, %d cpuload lines' % (r['out'], r['samples'], a.settle, r['cpuload_lines']))
        parts = []
        for f in FIELDS:
            part = '%s %s' % (f, show(r[f]))
            if r is not base and r[f] is not None and base[f] is not None:
                part += ' (%+g)' % round(r[f] - base[f], 1)
            parts.append(part)
        print('  ' + ', '.join(parts))
        if r['mhz']:
            print('  MHz ' + ', '.join('%s %s' % (d, show(v)) for d, v in r['mhz'].items()))
        print('  ' + ', '.join('%s %d' % (name, r[name]) for name in MARKERS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
