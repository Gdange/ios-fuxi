#!/usr/bin/env python3
"""Set the launchd ProcessType of one daemon in a mounted, writable iOS root.

  set-launchd-process-type.py ROOT PROGRAM TYPE

Edits /System/Library/xpc/launchd.plist (the cache launchd reads when booted
with launchd_unsecure_cache=1). Exactly one job must run a program whose
basename is PROGRAM. The CA smoke is exec-swapped in for peakpowermanagerd
(sweetexecswap), so it inherits that job's Background type: launchd clamps the
process to throttled priority 4, a time-constraint request is accepted but not
applied, and sleeps get the background timer leeway (up to 100 ms, observed).
"""
import os
from pathlib import Path
import plistlib
import sys

TYPES = ('Background', 'Standard', 'Adaptive', 'Interactive')


def program(job):
    args = job.get('ProgramArguments') or [None]
    return job.get('Program') or args[0] or ''


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in TYPES:
        sys.exit(__doc__)
    root, name, kind = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    cache = root/'System/Library/xpc/launchd.plist'
    data = plistlib.loads(cache.read_bytes())
    jobs = [job for job in data['LaunchDaemons'].values() if os.path.basename(program(job)) == name]
    if len(jobs) != 1:
        sys.exit(f'expected one job running {name}, found {len(jobs)}')
    job = jobs[0]
    old = job.get('ProcessType', '(unset)')
    job['ProcessType'] = kind
    # Keep the cache's owner and mode by replacing its contents in place.
    cache.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
    print(f'{job.get("Label")}: ProcessType {old} -> {kind}')


if __name__ == '__main__':
    main()
