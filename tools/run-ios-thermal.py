#!/usr/bin/env python3
# Thermal lane runner (derived from research/run-ios-0091d.py). Kernel/payload 0091d,
# loader shim/out/lane-thermal/fuxi-xnu-boot.img (adds request 0x57) and the monitor
# copy shim/out/lane-thermal/fuxi-xnu-monitor.py (adds --thermal-*, --monitor-idle, --cpu-*).
# The device serial comes from FUXI_SERIAL (kept out of git). Run from /hdd2/fuxi-ios:
#   sudo FUXI_SERIAL=... python3 shim/out/lane-thermal/run-ios-thermal.py OUT --loader-only [monitor args]
#       Android -> loader only (no XNU); the loader resets the phone at its 90 s deadline.
#   sudo FUXI_SERIAL=... python3 shim/out/lane-thermal/run-ios-thermal.py OUT [monitor args]
#       OrangeFox -> iOS 0091d exactly like run-ios-0091d.py (--touch-power, 60 min deadline renewed).
import json, os, re, subprocess, sys, time

out = sys.argv[1]
extra = sys.argv[2:]
loader_only = '--loader-only' in extra
if loader_only:
    extra = [a for a in extra if a != '--loader-only'] + ['--no-handoff']
SER = os.environ['FUXI_SERIAL']
LANE = 'shim/out/lane-thermal'
if not loader_only:
    # The pinned root must still be exactly where the ADT says (checked from OrangeFox).
    t = json.load(open('research/device/pinfile-01/extents.json'))
    assert subprocess.check_output(['adb', '-s', SER, 'get-state'], text=True).strip() == 'recovery'
    fm = subprocess.check_output(['adb', '-s', SER, 'shell', '/tmp/f2fs_io fiemap 0 %d %s' % (t['size'] // 4096, t['file'])], text=True)
    ext = []
    for line in fm.splitlines():
        m = re.match(r'^\d+\s+([0-9a-f]{16})\s+([0-9a-f]{16})\s+([0-9a-f]{16})', line)
        if m:
            lo, ph, ln = [int(x, 16) for x in m.groups()]
            if lo < t['size']:
                ext.append([lo, ph, min(ln, t['size'] - lo)])
    assert ext == t['extents'], 'FIEMAP differs from the ADT extent table'
    assert subprocess.check_output(['adb', '-s', SER, 'shell', '/tmp/f2fs_io pinfile get ' + t['file']], text=True).startswith('get_pin_file: pinned')
    print('pinned root extents verified', flush=True)
assert subprocess.check_output(['adb', '-s', SER, 'shell', 'getprop', 'ro.product.device'], text=True).strip() == 'fuxi'
subprocess.run(['adb', '-s', SER, 'reboot', 'bootloader'], check=True)
subprocess.run(['timeout', '30', 'fastboot', '-s', SER, 'boot', LANE + '/fuxi-xnu-boot.img'], check=True)
for _ in range(100):
    p = subprocess.run(['lsusb', '-d', '1209:0001'], capture_output=True, text=True)
    if p.stdout:
        f = p.stdout.split()
        node = '/dev/bus/usb/' + f[1] + '/' + f[3].rstrip(':')
        break
    time.sleep(.1)
else:
    raise SystemExit('Native monitor did not enumerate')
code = '''import usb.backend.libusb1 as b,runpy,sys,os,json
from pathlib import Path
assert b.get_backend(find_library=lambda _: '/lib/x86_64-linux-gnu/libusb-1.0.so.0')
out=sys.argv[1]
extra=json.loads(sys.argv[2])
script='%s/fuxi-xnu-monitor.py'
base=[script,'--out',out,'--kernel','xnu/out-fuxi-0091d/kernel.development.fuxi.unstripped','--payload','shim/out/fuxi-0091d-payload.bin','--skip-clocks']
if '--no-handoff' in extra: sys.argv=base+extra
else: sys.argv=base+['--seconds','14400','--framebuffer','--reset-minutes','60','--touch-power']+extra
print('monitor args',sys.argv[1:],flush=True)
try:runpy.run_path(script,run_name='__main__')
finally:
 for p in Path(out).glob('*'):os.chown(p,1000,1000)
 if Path(out).exists():os.chown(out,1000,1000)
''' % LANE
cmd = ['docker', 'run', '--rm', '--device', node,
       '--mount', 'type=bind,src=/usr,dst=/usr,readonly', '--mount', 'type=bind,src=/lib,dst=/lib,readonly',
       '--mount', 'type=bind,src=/lib64,dst=/lib64,readonly', '--mount', 'type=bind,src=/hdd2/fuxi-ios,dst=/work',
       '--workdir', '/work', '--env', 'PYTHONPATH=/work/research/venv/lib/python3.12/site-packages',
       '--entrypoint', '/usr/bin/python3', 'gbl-chainload-build:latest', '-c', code, out, json.dumps(extra)]
raise SystemExit(subprocess.run(cmd).returncode)
