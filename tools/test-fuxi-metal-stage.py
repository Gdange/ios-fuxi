#!/usr/bin/env python3
"""Validate Metal staging on a temporary root with real signed artifacts."""
import argparse
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
        conf=d/'conf';conf.write_bytes(b'3\n')
        clients=d/'clients';clients.write_bytes(b'fuxi-metal-smoke\ncensus backboardd\n')
        def stage():m.stage(root,icd,bundle,smoke,conf,clients)
        with patch.object(m.os,'geteuid',return_value=0),patch.object(m.os,'chown') as chown:
            stage();stage()
            for src,rel,mode in ((icd,'usr/local/lib/libvulkan_freedreno.dylib',0o755),(smoke,'usr/local/bin/fuxi-metal-smoke',0o755),
                (conf,'usr/local/share/fuxi/metal-smoke.conf',0o644),(clients,'usr/local/share/fuxi/metal-clients.conf',0o644),(bundle/'FuxiMetal','System/Library/Extensions/FuxiMetal.bundle/FuxiMetal',0o755),
                (bundle/'Info.plist','System/Library/Extensions/FuxiMetal.bundle/Info.plist',0o644),
                (bundle/'_CodeSignature/CodeResources','System/Library/Extensions/FuxiMetal.bundle/_CodeSignature/CodeResources',0o644)):
                p=root/rel;assert p.read_bytes()==src.read_bytes() and p.stat().st_mode&0o777==mode
            assert all(c.args[1:]==(0,0) for c in chown.call_args_list)
            for raw in (b'',b'4',b'3\0',b'1'*32):
                conf.write_bytes(raw)
                try:stage();raise AssertionError('bad config accepted')
                except ValueError:pass
            conf.write_bytes(b'3\n')
            for raw in (b'x\0',b'census x y',b'census backboardd\nbackboardd',b'x'*8193,b'../bad'):
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
