#!/usr/bin/env python3
"""Host checks for fuxi-thermal-summary.py on synthetic monitor output."""
import contextlib, importlib.util, io, json, tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('summary', Path(__file__).with_name('fuxi-thermal-summary.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def boot(root, name, base_c, busy, settle_hot=90.0):
    out = Path(root)/name
    out.mkdir()
    lines = []
    for i, since in enumerate((10, 100, 299.9, 300, 400, 500)):
        hot = since < 300
        temps = {'cpu-0-0': (settle_hot if hot else base_c) - 3, 'cpu-1-4': settle_hot if hot else base_c + i,
                 'cpuss-1': base_c - 1, 'gpuss-0': base_c - 5, 'ddr': base_c - 8, 'aoss-0': 30.0}
        lines.append(json.dumps({'since_handoff': since, 'temps': temps,
                                 'cpu': [{'domain': 'silver', 'state_mhz': 1228.8},
                                         {'domain': 'gold', 'state_mhz': 1401.6 + i}]}))
    lines.append(json.dumps({'since_handoff': 600, 'temps': {}}))  # no valid sensors
    lines.append('{"since_handoff": 700, "tem')  # cut off at the end of the run
    (out/'thermal.jsonl').write_text('\n'.join(lines) + '\n')
    log = ['FUXI IDLE: cpuload 10s up 100s avg 99% irq 1 ipi 1 tmr 1 | c0 99%',
           'FUXI IDLE: cpuload 10s up 310s avg %d%% irq 5000 ipi 9000 tmr 3000 | c0 -' % busy,
           'FUXI IDLE: cpuload 10s up 320s avg %d%% irq 5200 ipi 8000 tmr 3200 | c0 -' % (busy + 2),
           'FUXI IDLE: system sleep refused (fuxinosleep=1)',
           'FUXI IDLE: system sleep refused (fuxinosleep=1) reason 2 options 0x0 count 1',
           'FUXI IDLE: xpcproxy pid 88 exec keeps pidversion 4 (fuxixpcid=1)',
           'FUXI MTL: gate deny mediaserverd']
    (out/'boot.log').write_bytes(('\n'.join(log) + '\n\xff').encode('latin-1'))
    return out


with tempfile.TemporaryDirectory() as tmp:
    a = boot(tmp, 'stock', 52.0, 20)
    b = boot(tmp, 'ondemand', 47.0, 8)
    empty = Path(tmp)/'empty'
    empty.mkdir()

    r = m.summarise(a, 300)
    assert r['samples'] == 3, r  # 300, 400, 500
    assert r['cpu_max'] == 56.0 and r['gold'] == 56.0 and r['silver'] == 49.0, r  # gold 55, 56, 57
    assert r['cpuss'] == 51.0 and r['gpu'] == 47.0 and r['ddr'] == 44.0
    assert r['mhz'] == {'gold': 1405.6, 'silver': 1228.8}, r['mhz']
    assert r['cpuload_lines'] == 2 and r['busy_pct'] == 21.0 and r['irq_s'] == 5100.0 and r['tmr_s'] == 3100.0
    assert r['sleep_refused'] == 2 and r['xpcid_kept'] == 1 and r['mtl_deny_mediaserverd'] == 1
    assert m.summarise(a, 0)['samples'] == 6 and m.summarise(a, 0)['cpu_max'] == 73.5

    e = m.summarise(empty, 300)
    assert e['samples'] == 0 and e['cpu_max'] is None and e['busy_pct'] is None and e['mhz'] == {}

    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        assert m.main([str(a), str(b), str(empty)]) == 0
    text = out.getvalue()
    assert '%s: 3 samples after 300s, 2 cpuload lines' % a in text, text
    assert 'cpu_max 51 (-5)' in text and 'busy_pct 9 (-12)' in text, text
    assert 'cpu_max -,' in text and 'sleep_refused 2, xpcid_kept 1, mtl_deny_mediaserverd 1' in text
    with contextlib.redirect_stdout(io.StringIO()) as j:
        m.main([str(a), '--json', '--settle', '0'])
    assert json.loads(j.getvalue())[0]['samples'] == 6

print('PASS: thermal summary (settle window, sensor groups, EPSS MHz, cpuload lines, markers, A/B deltas)')
