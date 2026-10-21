#!/usr/bin/env python3
"""Stage Metal into an already mounted writable root. No mounting or phone access."""
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


def validate_clients(path):
    raw=path.read_bytes()
    if len(raw)>8192 or b'\0' in raw:raise ValueError('invalid client config')
    seen={}
    for line in raw.split(b'\n'):
        line=line.strip(b' \t\r')
        if not line or line.startswith(b'#'):continue
        mode='allow'
        if line.startswith(b'census '):mode='census';line=line[7:].lstrip(b' \t')
        if not re.fullmatch(rb'[A-Za-z0-9_.-]{1,63}',line):raise ValueError('invalid client name')
        if line in seen and seen[line]!=mode:raise ValueError('conflicting client modes')
        seen[line]=mode


def stage(root,icd,bundle,smoke,conf,clients=Path(__file__).with_name('metal-clients.conf'),translator=None,metallib=None,source=None):
    if os.geteuid()!=0:raise PermissionError('run as root to set root:wheel 0:0')
    root=root.resolve()
    if not (root/'System/Library').is_dir():raise ValueError('expected mounted iOS root')
    if not re.fullmatch(rb'[ \t\r\n]*[0-4][ \t\r\n]*',conf.read_bytes()) or conf.stat().st_size>31:
        raise ValueError('config must contain max stage 0..4')
    if translator is None:translator=smoke.parent/'libmetal2vulkan.dylib'
    if metallib is None:metallib=smoke.parent/'metal-smoke.metallib'
    if source is None:source=smoke.parent/'metal2vulkan-source.tar.gz'
    validate_clients(clients)
    info=plistlib.loads((bundle/'Info.plist').read_bytes())
    if any(info.get(k)!=v for k,v in {'CFBundleExecutable':'FuxiMetal','NSPrincipalClass':'FuxiMTLDevice','CFBundleIdentifier':'org.fuxi.metal'}.items()):
        raise ValueError('unexpected plugin plist')
    macho(icd,6);macho(bundle/'FuxiMetal',6);macho(smoke,2);macho(translator,6)
    if not metallib.is_file() or metallib.stat().st_size>4*1024*1024 or metallib.read_bytes()[:4]!=b'MTLB':
        raise ValueError('expected owned-source metallib')
    if not source.is_file():raise ValueError('missing translator corresponding source archive')
    with source.open('rb') as archive:
        if archive.read(2)!=b'\x1f\x8b':raise ValueError('invalid translator corresponding source archive')
    artifacts=[(icd,'usr/local/lib/libvulkan_freedreno.dylib',0o755),
        (translator,'usr/local/lib/libmetal2vulkan.dylib',0o755),
        (metallib,'usr/local/share/fuxi/metal-smoke.metallib',0o644),
        (source,'usr/local/share/fuxi/metal2vulkan-source.tar.gz',0o644),
        (smoke,'usr/local/bin/fuxi-metal-smoke',0o755),
        (clients,'usr/local/share/fuxi/metal-clients.conf',0o644),(conf,'usr/local/share/fuxi/metal-smoke.conf',0o644)]
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
    ap.add_argument('--clients',type=Path,default=Path(__file__).with_name('metal-clients.conf'))
    ap.add_argument('--translator',type=Path)
    ap.add_argument('--metallib',type=Path)
    ap.add_argument('--source',type=Path,help='external library corresponding source and licence archive')
    a=ap.parse_args();stage(a.root,a.icd,a.bundle,a.smoke,a.conf,a.clients,a.translator,a.metallib,a.source)
