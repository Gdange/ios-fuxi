#!/usr/bin/env python3
# Ported from HackiOS hackios-xnu-userland userland/mkjobs.py (MIT): the
# "--rest" pass that keeps a core job list and starts every other daemon on
# demand only. Adapted to edit the compiled iOS 16.7 launchd cache in place.
#
# Copyright (c) 2026 OOSEX GANG (HackiOS); adaptations copyright (c) 2026 bangprovn
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""Start every launchd daemon outside a core list on demand only (cooling).

  launchd-on-demand.py ROOT [--dry-run] [--keep LABEL]... [--drop LABEL]...
  launchd-on-demand.py ROOT --undo [--dry-run]
  launchd-on-demand.py --list-keep

Edits ROOT/System/Library/xpc/launchd.plist (the cache launchd reads when
booted with launchd_unsecure_cache=1), like disable-launchd-jobs.py. For every
com.apple job that is not in the keep list, the keys that make launchd start
it by itself are taken out: RunAtLoad, KeepAlive, StartInterval,
StartCalendarInterval, WatchPaths, QueueDirectories, StartOnMount and
LaunchEvents. The job then runs only when a program looks up one of its Mach
services (or connects to one of its sockets). A job with neither never runs.
HackiOS boots iOS 16.5 to SpringBoard this way with about 22 core jobs; the
stock 16.7 cache has 530 daemons, 334 of which start by themselves.

The removed keys are saved in the job under _FuxiOnDemand, so --undo puts
every job back, and a later run whose keep list names a stripped job restores
that job. launchd accepts the extra underscore key (our _PanicOnCrasX rename
of _PanicOnCrash in the same cache boots, observed). Disabled jobs and jobs
whose label does not start with com.apple. (ours, e.g. org.fuxi.*) are never
touched. --dry-run prints the same report and writes nothing.
"""
import argparse
from pathlib import Path
import plistlib
import sys

CACHE = 'System/Library/xpc/launchd.plist'
SAVED = '_FuxiOnDemand'
AUTOSTART = ('RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval', 'WatchPaths',
             'QueueDirectories', 'StartOnMount', 'LaunchEvents')

# The jobs that keep their own start conditions. The first group is HackiOS's
# IOS_JOBS (userland/build.sh) plus wifid, with the 16.5 labels checked
# against the 16.7 cache; the second is what this port's boot needs in
# addition. Most of the first group are on demand in the stock cache anyway;
# they are listed so that a later Apple change cannot strip them silently.
KEEP = {
    # HackiOS core list
    'com.apple.notifyd': 'notifications; everything posts and waits on it',
    'com.apple.cfprefsd.xpc.daemon': 'preferences of mobile processes',
    'com.apple.cfprefsd.xpc.daemon.system': 'preferences of root processes',
    'com.apple.logd': 'os_log store (RunAtLoad, KeepAlive)',
    'com.apple.logd_helper': 'logd helper',
    'com.apple.syslogd': 'syslog socket and ASL (KeepAlive)',
    'com.apple.diagnosticd': 'log streaming clients',
    'com.apple.distnoted.xpc.daemon': 'distributed notifications (KeepAlive)',
    'com.apple.fseventsd': 'file system events (RunAtLoad, KeepAlive)',
    'com.apple.containermanagerd': 'app/data containers (RunAtLoad)',
    'com.apple.containermanagerd.system': 'system containers',
    'com.apple.runningboardd': 'process assertions and app lifecycle (RunAtLoad, KeepAlive)',
    'com.apple.backboardd': 'window server, HID, display (RunAtLoad, KeepAlive)',
    'com.apple.SpringBoard': 'home screen (RunAtLoad, KeepAlive)',
    'com.apple.mobile.usermanagerd': 'user session; SpringBoard waits for it (RunAtLoad)',
    'com.apple.mobilegestalt.xpc': 'device identity answers (iPhone X identity, mkadt.py)',
    'com.apple.lsd': 'LaunchServices database',
    'com.apple.powerd': 'power management, display sleep (KeepAlive)',
    'com.apple.analyticsd': 'kept by HackiOS; its LaunchEvents are cheap',
    'com.apple.iconservices.iconservicesagent': 'home screen icons',
    'com.apple.pluginkit.pkd': 'app extensions (widgets, wallpaper posters)',
    'com.apple.wifid': 'Wi-Fi lane (fuxiwifiios); loads only with a wlan ADT node',
    # this port
    'com.apple.configd': 'SCDynamicStore and network config; USB Ethernet (KeepAlive)',
    'com.apple.UserEventAgent-System': 'EventMonitor: delivers the LaunchEvents of the kept jobs',
    'com.apple.peakpowermanagerd': 'host of the fuxi-ca-smoke exec-swap (sweetexecswap; RunAtLoad one-shot)',
    'com.apple.mobile.keybagd': 'data-protection keybag at boot; exits after 15 s idle (-t 15)',
    'com.apple.usb.networking.addNetworkInterface': 'one-shot on IONetworkInterface matching (USB/Wi-Fi interfaces)',
}


def stripped_keys(job):
    return [key for key in AUTOSTART if key in job]


def plan(data, keep):
    """Return (strip, restore, report) without changing data."""
    strip, restore, report = [], [], {'kept': [], 'foreign': [], 'disabled': [], 'never': []}
    for path, job in sorted(data['LaunchDaemons'].items(), key=lambda kv: kv[1].get('Label', kv[0])):
        label = job.get('Label', path)
        if not label.startswith('com.apple.'):
            report['foreign'].append(label)
            continue
        if label in keep:
            if SAVED in job:
                restore.append((label, job))
            report['kept'].append(label)
            continue
        if job.get('Disabled') is True:
            report['disabled'].append(label)
            continue
        keys = stripped_keys(job)
        if keys:
            strip.append((label, job, keys))
            if not job.get('MachServices') and not job.get('Sockets'):
                report['never'].append(label)
    return strip, restore, report


def apply_strip(job, keys):
    saved = job.setdefault(SAVED, {})
    for key in keys:
        saved[key] = job.pop(key)


def apply_restore(job):
    for key, value in job.pop(SAVED).items():
        job[key] = value


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('root', nargs='?', type=Path, help='mounted, writable iOS root')
    ap.add_argument('--dry-run', action='store_true', help='print the changes, write nothing')
    ap.add_argument('--keep', action='append', default=[], metavar='LABEL', help='also keep this job')
    ap.add_argument('--drop', action='append', default=[], metavar='LABEL', help='remove a label from the keep list')
    ap.add_argument('--undo', action='store_true', help='restore every job this tool changed')
    ap.add_argument('--list-keep', action='store_true', help='print the keep list with reasons')
    a = ap.parse_args(argv)
    if a.list_keep:
        for label, why in KEEP.items():
            print(f'{label}\t{why}')
        return 0
    if a.root is None:
        ap.error('ROOT is required')
    unknown = [label for label in a.drop if label not in KEEP]
    if unknown:
        sys.exit(f'--drop names labels outside the keep list: {", ".join(unknown)}')
    cache = a.root/CACHE
    data = plistlib.loads(cache.read_bytes())
    labels = {job.get('Label') for job in data['LaunchDaemons'].values()}
    missing = [label for label in a.keep if label not in labels]
    if missing:
        sys.exit(f'no such job: {", ".join(missing)}')
    keep = (set(KEEP) - set(a.drop)) | set(a.keep)
    if a.undo:
        restore = [(job.get('Label'), job) for job in data['LaunchDaemons'].values() if SAVED in job]
        for label, job in sorted(restore, key=lambda x: x[0]):
            print(f'restore {label}: {", ".join(job[SAVED])}')
            if not a.dry_run:
                apply_restore(job)
        print(f'{len(restore)} jobs restored' + (' (dry run)' if a.dry_run else ''))
    else:
        for label in sorted(keep - labels):
            print(f'warning: keep-list job {label} is not in this cache')
        strip, restore, report = plan(data, keep)
        for label, job in restore:
            print(f'restore {label}: {", ".join(job[SAVED])} (in the keep list)')
            if not a.dry_run:
                apply_restore(job)
        for label, job, keys in strip:
            never = '' if job.get('MachServices') or job.get('Sockets') else ' (no Mach service or socket: never starts)'
            print(f'on-demand {label}: removed {", ".join(keys)}{never}')
            if not a.dry_run:
                apply_strip(job, keys)
        print(f'{len(strip)} jobs on demand ({len(report["never"])} of them never start), '
              f'{len(restore)} restored, {len(report["kept"])} kept, {len(report["disabled"])} disabled '
              f'left alone, {len(report["foreign"])} non-Apple left alone' + (' (dry run)' if a.dry_run else ''))
    if not a.dry_run:
        # Keep the cache's owner and mode by replacing its contents in place.
        cache.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
    return 0


if __name__ == '__main__':
    sys.exit(main())
