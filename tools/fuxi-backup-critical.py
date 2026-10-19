#!/usr/bin/env python3
"""Read and verify Fuxi critical partition backups from a rooted recovery.

No device writes. Requires an explicit serial and private output directory.
This is not a userdata backup. Metadata is included only when unmounted and
explicitly requested. Existing verified images are reused; mismatches are refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PARTITIONS = '''boot_a boot_b vendor_boot_a vendor_boot_b init_boot_a init_boot_b
recovery_a recovery_b dtbo_a dtbo_b vbmeta_a vbmeta_b vbmeta_system_a vbmeta_system_b
abl_a abl_b xbl_a xbl_b xbl_config_a xbl_config_b xbl_ramdump_a xbl_ramdump_b
persist persistbak modemst1 modemst2 fsg fsc misc devinfo'''.split()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--metadata', action='store_true')
    args = ap.parse_args()
    adb = ['adb', '-s', args.serial]

    def shell(*command):
        return subprocess.check_output(adb + ['shell', *command], text=True).strip()

    if shell('getprop', 'ro.product.device') != 'fuxi' or shell('id', '-u') != '0':
        ap.error('target must be Fuxi with a root recovery shell')
    if shell('getprop', 'ro.orangefox.boot') != '1':
        ap.error('expected the verified OrangeFox environment')
    names = set(shell('ls', '/dev/block/by-name').split())
    parts = list(PARTITIONS)
    if args.metadata:
        if any(line.split()[1] == '/metadata' for line in shell('cat', '/proc/mounts').splitlines()):
            ap.error('unmount /metadata before requesting its backup')
        parts.append('metadata')
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {'device': 'fuxi', 'scope': 'critical partitions; excludes userdata and super', 'images': {}}
    manifest_path = args.out / 'manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
    for name in parts:
        if name not in names:
            continue
        device = '/dev/block/by-name/' + name
        size = int(shell('blockdev', '--getsize64', device))
        if not 0 < size <= 512 * 1024**2:
            raise ValueError(f'unexpected partition size: {name} {size}')
        dest = args.out / (name + '.img')
        if not dest.exists():
            temp = dest.with_suffix('.img.part')
            if temp.exists():
                raise FileExistsError(f'partial backup needs review: {temp}')
            print(f'Reading {name}: {size} bytes', flush=True)
            with temp.open('xb') as out:
                p = subprocess.Popen(adb + ['exec-out', 'dd if=' + device + ' bs=4194304 2>/dev/null'], stdout=out, stderr=subprocess.PIPE)
                _, stderr = p.communicate()
                if p.returncode:
                    raise RuntimeError(stderr.decode(errors='replace'))
            if temp.stat().st_size != size:
                raise ValueError(f'short read: {name}')
            temp.rename(dest)
        if dest.stat().st_size != size:
            raise ValueError(f'existing image size mismatch: {name}')
        with dest.open('rb') as inp:
            local = hashlib.file_digest(inp, 'sha256').hexdigest()
        remote = shell('sha256sum', device).split()[0]
        if local != remote:
            raise ValueError(f'backup/source mismatch: {name}; source may have changed')
        manifest['images'][name] = {'size': size, 'sha256': local, 'source': device}
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        print(f'Verified {name}: {local}', flush=True)
    print(f'{len(manifest["images"])} verified images in {args.out}', flush=True)


if __name__ == '__main__':
    main()
