#!/usr/bin/env python3
"""Finish a local 20H392 HFS+ root after copying OS + both cryptexes.

Run as root on the mounted image. Creates hard links and baseline writable data
from mkiosroot.sh. Does not access devices or install the image on a phone.
Apply sweet-root-tweaks.py separately after reviewing its userland changes.
"""
import argparse
import os
from pathlib import Path
import plistlib


def safe(root, relative, skip_alias=False):
    path = root / relative
    parent = path.parent
    while parent != root:
        if parent.is_symlink():
            if skip_alias:
                return None
            raise ValueError(f'symlink parent: {parent}')
        parent = parent.parent
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root.resolve()
    if os.geteuid() != 0 or root == Path('/'):
        parser.error('run as root against a mounted image, never /')
    version = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version.get('ProductBuildVersion') != '20H392':
        parser.error('expected iOS 20H392')
    if (root / 'sbin/launchd').stat().st_uid != 0:
        parser.error('source ownership was not preserved')
    count = 0
    aliases = 0
    for cryptex in ('OS', 'App'):
        source = root / 'private/preboot/Cryptexes' / cryptex
        if not (source / 'System').is_dir():
            parser.error(f'missing {cryptex} cryptex')
        for current, directories, files in os.walk(source, followlinks=False):
            # Include symlinks to directories without following them.
            files += [name for name in directories if (Path(current) / name).is_symlink()]
            for name in files:
                src = Path(current) / name
                dst = safe(root, src.relative_to(source), skip_alias=True)
                # Existing OS bundle aliases already select their cryptex subtree.
                # Never follow an absolute iOS symlink through the host's root.
                if dst is None:
                    aliases += 1
                    continue
                if os.path.lexists(dst):
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                if src.is_symlink():
                    os.symlink(os.readlink(src), dst)
                    os.lchown(dst, src.lstat().st_uid, src.lstat().st_gid)
                else:
                    os.link(src, dst)
                count += 1
    for name, mode in [('diagnostics', 0o751), ('uuidtext', 0o755), ('diagnostics/timesync', 0o755)]:
        directory = safe(root, 'private/var/db/' + name)
        directory.mkdir(parents=True, exist_ok=True)
        os.chown(directory, 272, 272)
        os.chmod(directory, mode)
        marker = directory / '.migrated'
        marker.touch()
        os.chown(marker, 272, 272)
        os.chmod(marker, 0o644)
    keybags = safe(root, 'private/var/keybags')
    keybags.mkdir(parents=True, exist_ok=True)
    os.chown(keybags, 0, 0)
    os.chmod(keybags, 0o700)
    bag = keybags / 'systembag.kb'
    if not os.path.lexists(bag):
        bag.write_bytes(plistlib.dumps({'KeyBagKeys': b'SWEET'}))
        os.chown(bag, 0, 0)
        os.chmod(bag, 0o600)
    print(f'Root staged: {count} cryptex links, {aliases} entries under existing aliases; log stores and software keybag prepared')


if __name__ == '__main__':
    main()
