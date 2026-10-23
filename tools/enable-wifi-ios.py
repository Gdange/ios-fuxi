#!/usr/bin/env python3
"""Inspect/opt in wifid in a mounted writable iOS root (dry-run by default).

Removes _LimitLoadToDeviceTree and clears Disabled only for /usr/sbin/wifid.
Edits launchd's cache plus the standalone job when present. No image mounting,
phone access or changes to boot arguments. Use launchd_unsecure_cache=1.
"""
import argparse
from copy import deepcopy
from pathlib import Path
import plistlib

KEYS = ('_LimitLoadToDeviceTree', 'Disabled')
CACHE = 'System/Library/xpc/launchd.plist'
JOB = 'System/Library/LaunchDaemons/com.apple.wifid.plist'
SUFFIX = '.fuxi-wifi-ios-backup.plist'


def checked_path(root, relative):
    path = root / relative
    # Refuse symlinks, including parents: never follow an iOS absolute symlink
    # into the host filesystem while editing a mounted root.
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError(f'symlink refused: {relative}')
    return path


def is_wifid(job):
    args = job.get('ProgramArguments') or ['']
    return (job.get('Program') or args[0]) == '/usr/sbin/wifid'


def prepare(root, restore=False):
    plans = []
    for relative in (CACHE, JOB):
        path = checked_path(root, relative)
        if not path.exists() and relative == JOB:
            continue
        raw = path.read_bytes()
        data = plistlib.loads(raw)
        jobs = data['LaunchDaemons'] if relative == CACHE else {'standalone': data}
        selected = {name: job for name, job in jobs.items() if isinstance(job, dict) and is_wifid(job)}
        if not selected:
            raise ValueError(f'no /usr/sbin/wifid job in {relative}')
        backup = checked_path(root, relative + SUFFIX)
        saved = plistlib.loads(backup.read_bytes()) if backup.exists() else None
        if restore:
            if saved is None or set(saved) != set(selected):
                raise ValueError(f'missing/mismatched backup for {relative}')
            for name, job in selected.items():
                for key in KEYS:
                    job.pop(key, None)
                    if key in saved[name]:
                        job[key] = saved[name][key]
        else:
            original = {name: {key: deepcopy(job[key]) for key in KEYS if key in job}
                        for name, job in selected.items()}
            if saved is not None and set(saved) != set(selected):
                raise ValueError(f'mismatched backup for {relative}')
            for job in selected.values():
                job.pop('_LimitLoadToDeviceTree', None)
                job['Disabled'] = False
            if saved is None:
                saved = original
        fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
        updated = plistlib.dumps(data, fmt=fmt, sort_keys=False)
        changed = plistlib.loads(raw) != data
        plans.append((relative, path, backup, saved, updated, changed))
    return plans


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root', type=Path)
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--enable-wifid', action='store_true', help='apply the opt-in root edit')
    group.add_argument('--restore-wifid', action='store_true', help='restore the two original job keys')
    args = ap.parse_args()
    root = args.root.resolve(strict=True)
    try:
        plans = prepare(root, args.restore_wifid)
        # Parse/validate every input before touching any output.
        for relative, path, backup, saved, updated, changed in plans:
            if args.enable_wifid or args.restore_wifid:
                if not backup.exists() and not args.restore_wifid:
                    with backup.open('xb') as stream:
                        stream.write(plistlib.dumps(saved, fmt=plistlib.FMT_BINARY))
                    backup.chmod(0o600)
                if changed:
                    # Preserve owner/mode/inode as in disable-launchd-jobs.py.
                    path.write_bytes(updated)
                if args.restore_wifid:
                    backup.unlink()
            print(f'{relative}: {"restore" if args.restore_wifid else "enable"} wifid'
                  f'{" (dry-run)" if not (args.enable_wifid or args.restore_wifid) else ""}'
                  f'{"; already matches" if not changed else ""}')
    except (OSError, ValueError, KeyError, TypeError, plistlib.InvalidFileException) as exc:
        ap.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
