#!/usr/bin/env python3
"""Host checks for launchd-on-demand.py on a synthetic launchd cache."""
import contextlib, importlib.util, io, plistlib, tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('ondemand', Path(__file__).with_name('launchd-on-demand.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def job(label, **keys):
    keys['Label'] = label
    keys.setdefault('ProgramArguments', ['/usr/libexec/' + label.rsplit('.', 1)[-1]])
    return keys


JOBS = {
    'backboardd': job('com.apple.backboardd', RunAtLoad=True, KeepAlive=True, MachServices={'com.apple.backboard.hid': True}),
    'healthd': job('com.apple.healthd', RunAtLoad=True, MachServices={'com.apple.healthd.server': True},
                   LaunchEvents={'com.apple.notifyd.matching': {'x': {'Notification': 'x'}}}),
    'locationd': job('com.apple.locationd', RunAtLoad=True, KeepAlive=True, StartInterval=300,
                     MachServices={'com.apple.locationd.registration': True}),
    'mediaserverd': job('com.apple.mediaserverd', KeepAlive=True, UserName='mobile',
                        MachServices={'com.apple.coremedia.decompressionsession': True}),
    'watchdogd': job('com.apple.watchdogd', KeepAlive={'SuccessfulExit': False},
                     LaunchEvents={'com.apple.iokit.matching': {}}),
    'lockdownd': job('com.apple.mobile.lockdown', RunAtLoad=True, Sockets={'s': {'SockPathName': '/var/run/lockdown.sock'}}),
    'cron': job('com.apple.pluginkit.pkreporter', StartCalendarInterval={'Hour': 3}, WatchPaths=['/x'],
                QueueDirectories=['/y'], StartOnMount=True),
    'tccd': job('com.apple.tccd', MachServices={'com.apple.tccd': True}),
    'bluetoothd': job('com.apple.bluetoothd', RunAtLoad=True, KeepAlive=True, Disabled=True, MachServices={'b': True}),
    'ours': job('org.fuxi.gpu-abi-test', RunAtLoad=True),
}


def cache(root, jobs=JOBS):
    path = root/m.CACHE
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {'LaunchDaemons': {'/System/Library/LaunchDaemons/%s.plist' % k: dict(v) for k, v in jobs.items()},
            'AppExtensions': {'a': {'x': 1}}, 'VersionNumber': 7}
    path.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
    return path


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        assert m.main([str(a) for a in argv]) == 0
    return out.getvalue()


def jobs(path):
    return {v['Label']: v for v in plistlib.loads(path.read_bytes())['LaunchDaemons'].values()}


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    path = cache(root)
    original = path.read_bytes()

    # Dry run: report only, cache bytes unchanged.
    text = run(root, '--dry-run')
    assert path.read_bytes() == original
    assert 'on-demand com.apple.healthd: removed RunAtLoad, LaunchEvents\n' in text, text
    assert 'on-demand com.apple.watchdogd: removed KeepAlive, LaunchEvents (no Mach service or socket: never starts)' in text
    assert 'on-demand com.apple.mobile.lockdown: removed RunAtLoad\n' in text  # socket-activated, so it can start
    assert 'on-demand com.apple.backboardd' not in text and 'on-demand com.apple.tccd' not in text
    assert 'bluetoothd' not in text and 'org.fuxi' not in text
    assert '(dry run)' in text and '6 jobs on demand (2 of them never start)' in text, text
    assert 'warning: keep-list job com.apple.SpringBoard is not in this cache' in text

    # Real run: keys moved under _FuxiOnDemand, everything else intact.
    run(root)
    after = jobs(path)
    h = after['com.apple.healthd']
    assert 'RunAtLoad' not in h and 'LaunchEvents' not in h and h['MachServices']
    assert h[m.SAVED] == {'RunAtLoad': True, 'LaunchEvents': {'com.apple.notifyd.matching': {'x': {'Notification': 'x'}}}}
    assert set(after['com.apple.locationd'][m.SAVED]) == {'RunAtLoad', 'KeepAlive', 'StartInterval'}
    assert set(after['com.apple.pluginkit.pkreporter'][m.SAVED]) == {'StartCalendarInterval', 'WatchPaths',
                                                                      'QueueDirectories', 'StartOnMount'}
    assert after['com.apple.mediaserverd'][m.SAVED] == {'KeepAlive': True}
    assert after['com.apple.backboardd'] == JOBS['backboardd']
    assert after['com.apple.tccd'] == JOBS['tccd']
    assert after['com.apple.bluetoothd'] == JOBS['bluetoothd']
    assert after['org.fuxi.gpu-abi-test'] == JOBS['ours']
    data = plistlib.loads(path.read_bytes())
    assert data['AppExtensions'] == {'a': {'x': 1}} and data['VersionNumber'] == 7

    # Idempotent: nothing left to strip.
    assert '0 jobs on demand' in run(root)
    assert jobs(path) == after

    # Keeping a stripped job later restores it; --drop takes one off the list.
    text = run(root, '--keep', 'com.apple.mediaserverd', '--drop', 'com.apple.powerd')
    assert 'restore com.apple.mediaserverd: KeepAlive (in the keep list)' in text
    assert jobs(path)['com.apple.mediaserverd'] == JOBS['mediaserverd']
    run(root)  # default list again: mediaserverd goes back on demand
    assert m.SAVED in jobs(path)['com.apple.mediaserverd']

    # Undo restores the exact stock jobs.
    text = run(root, '--undo', '--dry-run')
    assert '6 jobs restored (dry run)' in text and m.SAVED in jobs(path)['com.apple.healthd']
    run(root, '--undo')
    assert jobs(path) == {v['Label']: v for v in JOBS.values()}

    # Unknown --keep labels and --drop outside the list fail without writing.
    before = path.read_bytes()
    for argv in (['--keep', 'com.apple.nosuchd'], ['--drop', 'com.apple.healthd']):
        try:
            run(root, *argv)
            raise AssertionError(argv)
        except SystemExit as e:
            assert e.code and 'com.apple.' in str(e.code)
    assert path.read_bytes() == before

    # The keep list documents a reason for every job.
    listing = run('--list-keep').splitlines()
    assert len(listing) == len(m.KEEP) and all('\t' in line and line.split('\t')[1] for line in listing)
    assert {'com.apple.backboardd', 'com.apple.SpringBoard', 'com.apple.configd'} <= set(m.KEEP)
    assert 'com.apple.mediaserverd' not in m.KEEP

print('PASS: launchd on-demand pass (dry run, strip/save, keep/restore, undo, foreign/disabled untouched)')
