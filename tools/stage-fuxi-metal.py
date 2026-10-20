#!/usr/bin/env python3
"""Stage M4 into an already mounted writable root. No mounting or phone access."""
import argparse
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct


def macho(path,kind):
    raw=path.read_bytes()
    if len(raw)<32 or struct.unpack_from('<4I',raw)!=(0xfeedfacf,0x100000c,0,kind):
        raise ValueError('expected plain arm64 Mach-O')
    ncmds,total=struct.unpack_from('<2I',raw,16);pos=32;signed=False;minimum=False
    if total>len(raw)-32:raise ValueError('truncated commands')
    for _ in range(ncmds):
        if pos+8>32+total:raise ValueError('truncated command')
        cmd,size=struct.unpack_from('<2I',raw,pos)
        if size<8 or pos+size>32+total:raise ValueError('invalid command')
        if cmd==0x1d:
            if size<16:raise ValueError('invalid signature command')
            off,length=struct.unpack_from('<2I',raw,pos+8)
            signed=length>0 and off+length<=len(raw)
        if cmd==0x32:minimum=struct.unpack_from('<2I',raw,pos+8)==(2,0x100000)
        pos+=size
    if pos!=32+total or not signed or not minimum:raise ValueError('requires signed iOS 16.0 artifact')


def stage(root,icd,bundle,smoke,conf):
    if os.geteuid()!=0:raise PermissionError('run as root to set root:wheel 0:0')
    root=root.resolve()
    if not (root/'System/Library').is_dir():raise ValueError('expected mounted iOS root')
    if not re.fullmatch(rb'[ \t\r\n]*[0-3][ \t\r\n]*',conf.read_bytes()) or conf.stat().st_size>31:
        raise ValueError('config must contain max stage 0..3')
    info=plistlib.loads((bundle/'Info.plist').read_bytes())
    if any(info.get(k)!=v for k,v in {'CFBundleExecutable':'FuxiMetal','NSPrincipalClass':'FuxiMTLDevice','CFBundleIdentifier':'org.fuxi.metal'}.items()):
        raise ValueError('unexpected plugin plist')
    macho(icd,6);macho(bundle/'FuxiMetal',6);macho(smoke,2)
    artifacts=[(icd,'usr/local/lib/libvulkan_freedreno.dylib',0o755),
        (smoke,'usr/local/bin/fuxi-metal-smoke',0o755),(conf,'usr/local/share/fuxi/metal-smoke.conf',0o644)]
    for src in ('FuxiMetal','Info.plist','_CodeSignature/CodeResources'):
        source=bundle/src
        if not source.is_file() or source.is_symlink():raise ValueError('missing/linked bundle member')
        artifacts.append((source,'System/Library/Extensions/FuxiMetal.bundle/'+src,0o755 if src=='FuxiMetal' else 0o644))
    for source,name,mode in artifacts:
        target=root/name
        if not target.resolve().is_relative_to(root):raise ValueError('target escapes root')
        if target.is_symlink():raise ValueError('refusing linked destination')
        if not source.is_file():raise ValueError('missing artifact')
    for source,name,mode in artifacts:
        target=root/name
        missing=[];parent=target.parent
        while not parent.exists():
            missing.append(parent);parent=parent.parent
        for directory in reversed(missing):
            directory.mkdir();os.chmod(directory,0o755);os.chown(directory,0,0)
        shutil.copyfile(source,target);os.chmod(target,mode);os.chown(target,0,0)
        print('staged /'+name+' root:wheel %04o'%mode)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root',type=Path);ap.add_argument('icd',type=Path);ap.add_argument('bundle',type=Path);ap.add_argument('smoke',type=Path)
    ap.add_argument('--conf',type=Path,default=Path(__file__).with_name('metal-smoke.conf'))
    a=ap.parse_args();stage(a.root,a.icd,a.bundle,a.smoke,a.conf)
