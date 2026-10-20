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


def stage(root, library, probe):
    if os.geteuid()!=0:
        raise PermissionError('run as root to set owner 0:0')
    root=root.resolve()
    if not (root/'System/Library').is_dir():
        raise ValueError('expected an already mounted iOS root')
    for source,(_,kind) in zip((library,probe),ARTIFACTS):
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
    for source,(name,_) in zip((library,probe),ARTIFACTS):
        target=root/name.lstrip('/')
        if not target.resolve().is_relative_to(root):
            raise ValueError('target symlink escapes mounted root: '+name)
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,target)
        os.chmod(target,0o755);os.chown(target,0,0)
        print('staged '+name+' mode 0755 owner 0:0')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root',type=Path);ap.add_argument('library',type=Path);ap.add_argument('probe',type=Path)
    args=ap.parse_args();stage(args.root,args.library,args.probe)


if __name__=='__main__': main()
