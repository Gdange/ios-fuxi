#!/usr/bin/env python3
"""Copy signed ICD/probe into an already mounted writable iOS root, as root.

Offline only: no mounting, phone access, launchd job or automatic GPU execution.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct

ARTIFACTS=(('/usr/local/lib/libvulkan_freedreno.dylib',6),('/usr/local/bin/fuxi-vk-probe',2))


def stage(root, library, probe, smoke=None, conf=None):
    if os.geteuid()!=0:
        raise PermissionError('run as root to set owner 0:0')
    root=root.resolve()
    if not (root/'System/Library').is_dir():
        raise ValueError('expected an already mounted iOS root')
    artifacts=list(ARTIFACTS)
    sources=[library,probe]
    if smoke is not None:
        if conf is None: raise ValueError('smoke requires a stage config')
        import re
        raw=conf.read_bytes()
        if len(raw)>31 or not re.fullmatch(rb'[ \t\r\n]*[1-4][ \t\r\n]*',raw):
            raise ValueError('config must contain max stage 1..4')
        sources.append(smoke);artifacts.append(('/usr/local/bin/fuxi-vk-smoke',2))
    elif conf is not None:
        raise ValueError('config requires smoke')
    for source,(_,kind) in zip(sources,artifacts):
        raw=source.read_bytes()
        if len(raw)<32 or struct.unpack_from('<4I',raw)!=(0xfeedfacf,0x100000c,0,kind):
            raise ValueError('expected signed arm64 Mach-O artifact: '+str(source))
        ncmds=struct.unpack_from('<I',raw,16)[0];pos=32;signature=False
        for _ in range(ncmds):
            cmd,size=struct.unpack_from('<2I',raw,pos)
            if size<8 or pos+size>len(raw): raise ValueError('invalid load command')
            signature |= cmd==0x1d
            pos+=size
        if not signature: raise ValueError('missing code signature: '+str(source))
    if smoke is not None:
        sources.append(conf);artifacts.append(('/usr/local/share/fuxi/vk-smoke.conf',0))
    # Check every destination before the first copy.
    for name,_ in artifacts:
        if not (root/name.lstrip('/')).resolve().is_relative_to(root):
            raise ValueError('target symlink escapes mounted root: '+name)
    for source,(name,kind) in zip(sources,artifacts):
        target=root/name.lstrip('/')
        if not target.resolve().is_relative_to(root):
            raise ValueError('target symlink escapes mounted root: '+name)
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,target)
        mode=0o755 if kind else 0o644
        os.chmod(target,mode);os.chown(target,0,0)
        print('staged '+name+' mode %04o owner 0:0'%mode)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root',type=Path);ap.add_argument('library',type=Path);ap.add_argument('probe',type=Path)
    ap.add_argument('--smoke',type=Path);ap.add_argument('--conf',type=Path)
    args=ap.parse_args()
    conf=args.conf or (Path(__file__).with_name('vk-smoke.conf') if args.smoke else None)
    stage(args.root,args.library,args.probe,args.smoke,conf)


if __name__=='__main__': main()
