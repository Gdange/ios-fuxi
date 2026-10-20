#!/usr/bin/env python3
"""Stage the signed raw test into an already mounted, writable iOS root.

Coordinator-only offline root-image edit; never mounts, installs or accesses
any phone. Embeds one RunAtLoad job in launchd's existing unsecure cache,
as sweet-root-tweaks.py does. --remove removes only this test and its job.
"""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import struct

LABEL = 'org.fuxi.gpu-abi-test'
LEGACY_JOB = f'/Library/LaunchDaemons/{LABEL}.plist'
JOB = f'/System/Library/LaunchDaemons/{LABEL}.plist'
BINARY = '/usr/local/bin/fuxi-gpu-abi-test'


def job(watchdog=False):
    return dict(Label=LABEL, Program=BINARY, ProgramArguments=[BINARY]+(['--watchdog'] if watchdog else []),
                UserName='root', RunAtLoad=True, KeepAlive=False,
                ProcessType='Background', StandardOutPath='/dev/console',
                StandardErrorPath='/dev/console')


def stage(root, binary=None, watchdog=False, remove=False):
    cache = root/'System/Library/xpc/launchd.plist'
    data = plistlib.loads(cache.read_bytes())
    jobs = data['LaunchDaemons']
    assert isinstance(jobs, dict)
    target = root/BINARY.lstrip('/')
    plist = root/JOB.lstrip('/')
    if not remove:
        assert binary and binary.is_file(), 'supply the signed iOS test binary'
        assert struct.unpack('<3I', binary.read_bytes()[:12]) == (0xfeedfacf, 0x100000c, 0), 'arm64 Mach-O required'
    # iOS bootstrap enumerates only configured system daemon directories.
    # Migrate the old cache key/file so future root edits cannot duplicate it.
    jobs.pop(LEGACY_JOB, None)
    (root/LEGACY_JOB.lstrip('/')).unlink(missing_ok=True)
    if remove:
        jobs.pop(JOB, None)
        target.unlink(missing_ok=True)
        plist.unlink(missing_ok=True)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(binary, target)
        os.chmod(target, 0o755)
        plist.parent.mkdir(parents=True, exist_ok=True)
        plist.write_bytes(plistlib.dumps(job(watchdog), fmt=plistlib.FMT_XML))
        os.chmod(plist, 0o644)
        jobs[JOB] = job(watchdog)
        if os.geteuid() == 0:
            os.chown(target, 0, 0)
            os.chown(plist, 0, 0)
    # Keep the original cache owner/mode by replacing its contents in place.
    cache.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
    print(('removed' if remove else 'staged') + ' one-shot root GPU ABI test')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root', type=Path)
    ap.add_argument('binary', nargs='?', type=Path)
    ap.add_argument('--watchdog', action='store_true')
    ap.add_argument('--remove', action='store_true')
    args = ap.parse_args()
    stage(args.root, args.binary, args.watchdog, args.remove)


if __name__ == '__main__':
    main()
