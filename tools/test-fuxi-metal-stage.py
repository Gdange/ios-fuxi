#!/usr/bin/env python3
"""Validate Metal staging on a temporary root with real signed artifacts."""
import argparse
import os
import subprocess
import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('stage',Path(__file__).with_name('stage-fuxi-metal.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,default=Path('tools/out'));a=ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);root=d/'root';(root/'System/Library').mkdir(parents=True)
        icd=a.out/'libvulkan_freedreno.dylib';bundle=a.out/'FuxiMetal.bundle';smoke=a.out/'fuxi-metal-smoke'
        conf=d/'conf';conf.write_bytes(b'4\n')
        ca_conf=d/'ca-conf';ca_conf.write_bytes(b'4\n')
        clients=d/'clients';clients.write_bytes(b'fuxi-metal-smoke\nfuxi-ca-smoke\n')
        def stage():m.stage(root,icd,bundle,smoke,conf,clients,ca_conf=ca_conf)
        with patch.object(m.os,'geteuid',return_value=0),patch.object(m.os,'chown') as chown:
            stage();stage();conf.write_bytes(b'5\n');stage();conf.write_bytes(b'6\n');stage();conf.write_bytes(b'4\n');stage()
            if os.uname().sysname=='Darwin':
                subprocess.run(['codesign','--verify','--strict',str(root/'System/Library/Extensions/FuxiMetal.bundle')],check=True)
            for src,rel,mode in ((icd,'usr/local/lib/libvulkan_freedreno.dylib',0o755),(smoke,'usr/local/bin/fuxi-metal-smoke',0o755),
                (a.out/'fuxi-ca-smoke','usr/local/bin/fuxi-ca-smoke',0o755),
                (ca_conf,'usr/local/share/fuxi/ca-smoke.conf',0o644),
                (a.out/'libmetal2vulkan.dylib','usr/local/lib/libmetal2vulkan.dylib',0o755),
                (a.out/'metal-smoke.metallib','usr/local/share/fuxi/metal-smoke.metallib',0o644),
                (a.out/'metal2vulkan-source.tar.gz','usr/local/share/fuxi/metal2vulkan-source.tar.gz',0o644),
                (conf,'usr/local/share/fuxi/metal-smoke.conf',0o644),(clients,'usr/local/share/fuxi/metal-clients.conf',0o644),(bundle/'FuxiMetal','System/Library/Extensions/FuxiMetal.bundle/FuxiMetal',0o755),
                (bundle/'Info.plist','System/Library/Extensions/FuxiMetal.bundle/Info.plist',0o644),
                (bundle/'LICENSE.NativeMetal','System/Library/Extensions/FuxiMetal.bundle/LICENSE.NativeMetal',0o644),
                (bundle/'_CodeSignature/CodeResources','System/Library/Extensions/FuxiMetal.bundle/_CodeSignature/CodeResources',0o644)):
                p=root/rel;assert p.read_bytes()==src.read_bytes() and p.stat().st_mode&0o777==mode
            assert all(c.args[1:]==(0,0) for c in chown.call_args_list)
            # A missing third-party notice must fail preflight before any copy.
            original_is_file=Path.is_file
            with patch.object(Path,'is_file',lambda p:p.name!='LICENSE.NativeMetal' and original_is_file(p)),patch.object(m.shutil,'copyfile') as copied:
                try:stage();raise AssertionError('missing NativeMetal notice accepted')
                except ValueError:pass
                copied.assert_not_called()
            for raw in (b'',b'7',b'3\0',b'1'*32):
                conf.write_bytes(raw)
                try:stage();raise AssertionError('bad config accepted')
                except ValueError:pass
            conf.write_bytes(b'4\n')
            for raw in (b'',b'6',b'-1',b'4\0',b'4'*32):
                ca_conf.write_bytes(raw)
                try:stage();raise AssertionError('bad CA config accepted')
                except ValueError:pass
            for maximum in range(6):
                ca_conf.write_bytes(str(maximum).encode());stage()
            ca_conf.write_bytes(b'4\n')
            for raw in (b'x\0',b'census x y',b'census backboardd\nbackboardd',b'x'*8193,b'../bad',b'backboardd',b'SpringBoard',b'census backboardd',b'census SpringBoard'):
                clients.write_bytes(raw)
                try:stage();raise AssertionError('bad clients accepted')
                except ValueError:pass
            clients.write_bytes(b'fuxi-metal-smoke\n')
            target=root/'usr/local/bin/fuxi-metal-smoke';target.unlink();target.symlink_to(d/'escape')
            try:stage();raise AssertionError('escaping symlink accepted')
            except ValueError:pass
            target.unlink();shutil.rmtree(root/'System/Library/Extensions')
            escape=root/'System/Library/Extensions';escape.symlink_to(d)
            try:stage();raise AssertionError('escaping parent accepted')
            except ValueError:pass
        with patch.object(m.os,'geteuid',return_value=501):
            try:stage();raise AssertionError('non-root staging accepted')
            except PermissionError:pass
        unsigned=d/'unsigned';raw=bytearray(smoke.read_bytes());pos=32
        for _ in range(struct.unpack_from('<I',raw,16)[0]):
            cmd,size=struct.unpack_from('<2I',raw,pos)
            if cmd==0x1d:struct.pack_into('<I',raw,pos,0x7777)
            pos+=size
        unsigned.write_bytes(raw)
        try:m.macho(unsigned,2);raise AssertionError('unsigned accepted')
        except ValueError:pass
    print('PASS: actual artifact staging, idempotence, root:wheel file/directory modes, config validation, symlink containment, nonroot/unsigned refusal')
if __name__=='__main__':main()
