#!/usr/bin/env python3
"""Validate Metal staging on a temporary root with real signed artifacts."""
import argparse
import os
import subprocess
import importlib.util
from pathlib import Path
import shutil
import struct
import plistlib
import tempfile
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('stage',Path(__file__).with_name('stage-fuxi-metal.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def policy_storage_tests():
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp);clients=root/'clients'
        clients.write_bytes(b'fuxi-ca-smoke\n')
        assert b'\nbackboardd\n' not in m.validate_clients(clients)
        assert m.validate_clients(clients,True)==b'fuxi-ca-smoke\nbackboardd\n'
        clients.write_bytes(b'backboardd\n')
        assert m.validate_clients(clients,True)==b'backboardd\n'
        for data in (b'backboardd\n',b'SpringBoard\n',b'census backboardd\n',b'census SpringBoard\n',
                     b'mediaserverd\n',b'census mediaserverd\n'):
            clients.write_bytes(data)
            try:m.validate_clients(clients);raise AssertionError('default compositor accepted')
            except ValueError:pass
            if data!=b'backboardd\n':
                try:m.validate_clients(clients,True);raise AssertionError('unsafe compositor accepted')
                except ValueError:pass
        job=root/'System/Library/LaunchDaemons/com.apple.backboardd.plist';job.parent.mkdir(parents=True)
        job.write_bytes(plistlib.dumps({}))
        assert m.backboard_account(root)==(0,0)
        job.write_bytes(plistlib.dumps({'UserName':'_render'}))
        try:m.backboard_account(root);raise AssertionError('missing target user accepted')
        except ValueError:pass
        passwd=root/'private/etc/master.passwd';passwd.parent.mkdir(parents=True)
        passwd.write_text('_render:*:501:502::0:0:render:/var/empty:/usr/bin/false\n')
        assert m.backboard_account(root)==(501,502)
        with patch.object(m.os,'chown') as chown:
            m.prepare_backboard_storage(root,(501,502))
            directory=root/'private/var/db/fuxi-metal'
            assert directory.stat().st_mode&0o7777==0o755
            assert (directory/'backboardd-501').stat().st_mode&0o7777==0o700
            assert any((Path(c.args[0]).resolve(),*c.args[1:])==((directory/'backboardd-501').resolve(),501,502) for c in chown.call_args_list)
        # Existing unsafe directories are refused, never made world writable/repaired.
        directory.chmod(0o777)
        try:m.prepare_backboard_storage(root,(501,502));raise AssertionError('unsafe parent accepted')
        except ValueError:pass
        shutil.rmtree(directory);directory.symlink_to(root/'private/etc',target_is_directory=True)
        try:m.prepare_backboard_storage(root,(501,502));raise AssertionError('linked storage accepted')
        except ValueError:pass
    print('PASS: staging opt-in/default deny, SpringBoard/census deny, target launchd UID resolution and 0755 root/0700 client storage')

def native_staging_tests():
    # Authored Mach-O command-layout models, not executable/signing evidence.
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);root=d/'root';(root/'System/Library').mkdir(parents=True)
        def binary(path,kind):
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(struct.pack('<8I',0xfeedfacf,0x100000c,0,kind,2,40,0,0)+
                             struct.pack('<6I',0x32,24,2,0x100000,0x100000,0)+struct.pack('<4I',0x1d,16,72,1)+b'S')
        bundle=d/'FuxiMetal.bundle';native=d/'FuxiNativeMetal.bundle'
        for directory,exe,identifier,principal in [(bundle,'FuxiMetal','org.fuxi.metal','FuxiMTLDevice'),
                                                    (native,'FuxiNativeMetal','com.fuxi.NativeMetal',None)]:
            binary(directory/exe,6)
            info={'CFBundleExecutable':exe,'CFBundleIdentifier':identifier}
            if principal:info['NSPrincipalClass']=principal
            (directory/'Info.plist').write_bytes(plistlib.dumps(info))
            (directory/'LICENSE.NativeMetal').write_text('MIT test notice')
            (directory/'_CodeSignature').mkdir();(directory/'_CodeSignature/CodeResources').write_text('model')
        for name,kind in [('icd',6),('libmetal2vulkan.dylib',6),('fuxi-metal-smoke',2),('fuxi-ca-smoke',2),('libnative_metal_bridge.dylib',6)]:binary(d/name,kind)
        (d/'metal-smoke.metallib').write_bytes(b'MTLB model')
        for name in ('metal2vulkan-source.tar.gz','nativemetal-source.tar.gz'):(d/name).write_bytes(b'\x1f\x8b model')
        (d/'conf').write_text('4\n');(d/'clients').write_text('fuxi-metal-smoke\n')
        def stage(**kwargs):
            m.stage(root,d/'icd',bundle,d/'fuxi-metal-smoke',d/'conf',d/'clients',ca_conf=d/'conf',**kwargs)
        with patch.object(m.os,'geteuid',return_value=0),patch.object(m.os,'chown'):
            native_args=dict(native_bundle=native,native_bridge=d/'libnative_metal_bridge.dylib',native_source=d/'nativemetal-source.tar.gz')
            stage(**native_args)
            assert (root/'usr/local/lib/libnative_metal_bridge.dylib').read_bytes()==(d/'libnative_metal_bridge.dylib').read_bytes()
            assert (root/'System/Library/Extensions/FuxiNativeMetal.bundle/Info.plist').read_bytes()==(native/'Info.plist').read_bytes()
            assert (root/'usr/local/share/fuxi/metal-clients.conf').read_bytes()==b'fuxi-metal-smoke\n'
            for key in native_args:
                incomplete=dict(native_args);del incomplete[key]
                with patch.object(m.shutil,'copyfile') as copied:
                    try:stage(**incomplete);raise AssertionError('incomplete native stage accepted')
                    except ValueError:pass
                    copied.assert_not_called()
            target=root/'usr/local/lib/libnative_metal_bridge.dylib';target.unlink();target.symlink_to(d/'escape')
            with patch.object(m.shutil,'copyfile') as copied:
                try:stage(**native_args);raise AssertionError('native destination escape accepted')
                except ValueError:pass
                copied.assert_not_called()
    print('PASS: NativeMetal offline staging, complete dylib/source trio, smoke-only policy and destination containment')

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,default=Path('tools/out'))
    ap.add_argument('--policy-storage-only',action='store_true');a=ap.parse_args()
    policy_storage_tests()
    native_staging_tests()
    if a.policy_storage_only:return
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
            # Exercise the actual staging flag/config writer without target-account assumptions.
            with patch.object(m,'backboard_account',return_value=(501,502)),patch.object(m,'prepare_backboard_storage') as storage:
                m.stage(root,icd,bundle,smoke,conf,clients,ca_conf=ca_conf,enable_backboardd=True)
                storage.assert_called_once_with(root.resolve(),(501,502))
                assert (root/'usr/local/share/fuxi/metal-clients.conf').read_bytes()==clients.read_bytes()+b'backboardd\n'
            stage() # Default restaging removes admission again.
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
