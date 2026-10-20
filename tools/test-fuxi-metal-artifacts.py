#!/usr/bin/env python3
"""Verify offline metal1 kernel, raw tool and boot-image packaging.

Optional firmware paths compare external inputs without copying them to git.
No device access. Requires llvm-nm-18 (or --nm) and the 0097b artifact directory.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import adt
import mkpayload


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--out', type=Path, default=Path('xnu/out-fuxi-metal1'))
    ap.add_argument('--lane', choices=('metal1','metal2a'), default='metal1')
    ap.add_argument('--nm', default='llvm-nm-18')
    ap.add_argument('--firmware', type=Path)
    ap.add_argument('--sqe', type=Path)
    ap.add_argument('--zap', type=Path)
    args=ap.parse_args()
    kernel=args.out/'kernel.development.fuxi.unstripped'
    raw=kernel.read_bytes();mach=mkpayload.parse_macho(raw)
    symbols={line.split()[-1]:int(line.split()[0],16)
             for line in subprocess.check_output([args.nm,str(kernel)],text=True).splitlines()
             if len(line.split())==3}
    for name in ('map','unmap','submit','poll','available','publish'):
        assert '_fuxi_gpu_abi_'+name in symbols,name
    assert any('NativeAdrenoGpuPublished10gMetaClass' in name for name in symbols)
    assert not any('IOAcceleratorES' in name or 'IOGPUFamily' in name for name in symbols)
    table=symbols['_copyio_recover_table'];end=symbols['_copyio_recover_table_end']
    seg=next(s for s in mach['segs'] if s['vmaddr']<=table and end<=s['vmaddr']+s['filesize'])
    offset=table-seg['vmaddr']+seg['fileoff'];entries=[]
    for n in range(0,end-table,24):
        a,b,c=struct.unpack_from('<qqq',raw,offset+n);entries.append((table+a,table+b,table+c))
    begin=symbols['_fuxi_gpu_access_begin'];stop=symbols['_fuxi_gpu_access_end']
    assert sum(a>=begin and b==stop and c>stop for a,b,c in entries)==2
    print('PASS: native ABI/service symbols, no built-in IOAcceleratorES/IOGPUFamily, linked MMIO recovery')
    payload=Path(f'shim/out/fuxi-{args.lane}-payload.bin').read_bytes()
    image=Path(f'shim/out/fuxi-{args.lane}-boot.img').read_bytes()
    assert payload[:8]==b'SWEETXNU'
    cmdline=payload[72:680].split(b'\0')[0]
    basecmd=args.base.joinpath('cmdline.txt').read_bytes().strip()
    if args.lane=='metal2a':
        expected=basecmd.replace(b'sweetnoexec=bluetoothd,',b'sweetnoexec=')+b' sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test'
        assert b'fuxigpuabi=1' in expected.split() and not any(a.startswith(b'fuxiwlan=') for a in expected.split())
        assert '_fuxi_wlan_init' in symbols and b'SWEET EXECSWAP:' in raw and b'FUXI ABI: result' in raw
    else: expected=basecmd+b' fuxigpuabi=1'
    assert cmdline==expected and len(cmdline)<608
    vb,entry,ao,an,ke,pb,ms=struct.unpack_from('<7Q',payload,16)
    assert (pb,ms)==(0x8c0000000,0x13e000000) and ke-vb<=ms-32768
    tree_bytes=payload[ao:ao+an]
    assert tree_bytes==args.base.joinpath('fuxi.adt').read_bytes()==args.out.joinpath('fuxi.adt').read_bytes()
    root,_=adt.parse(tree_bytes);chosen=next(x for x in root.children if x.name=='chosen')
    assert struct.unpack('<QQ',chosen.prop('fuxi-gpu-memory'))==(0x9fe000000,0x2000000)
    assert len(tree_bytes)<16384
    assert image[:8]==b'ANDROID!' and struct.unpack_from('<I',image,40)[0]==4
    size=struct.unpack_from('<I',image,8)[0];arm=image[4096:4096+size]
    assert arm[56:60]==b'ARM\x64' and struct.unpack_from('<Q',arm,16)[0]==len(arm)
    assert arm.count(payload)==1
    print('PASS: boot-v4/Image/payload geometry, full RAM bounds, unchanged base ADT and exact lane cmdline')
    tool=Path('tools/out/fuxi-gpu-abi-test').read_bytes()
    magic,cpu,sub,_,count=struct.unpack_from('<5I',tool)
    assert (magic,cpu,sub)==(0xfeedfacf,0x100000c,0)
    off=32;platform=None;signature=False
    for _ in range(count):
        op,size=struct.unpack_from('<II',tool,off)
        if op==0x32:
            platform,minos,sdk,_=struct.unpack_from('<4I',tool,off+8)
            assert platform==2 and minos==0x100000,(platform,minos)
        if op==0x1d: signature=True
        off+=size
    assert platform==2 and signature
    print('PASS: signed arm64 Mach-O, LC_BUILD_VERSION iOS 16.0')
    firmware=[args.firmware,args.sqe,args.zap]
    if any(firmware):
        assert all(firmware),'provide all three firmware inputs'
        loader=Path(f'shim/out/fuxi-{args.lane}-loader.elf').read_bytes()
        for path in firmware: assert loader.count(path.read_bytes())==1,path.name
        print('PASS: exact external GMU/SQE/zap blobs appear once in loader ELF')
    for line in args.out.joinpath('SHA256SUMS').read_text().splitlines():
        digest,path=line.split(None,1)
        assert hashlib.sha256(Path(path.strip()).read_bytes()).hexdigest()==digest,path
    print('PASS: artifact SHA256SUMS')


if __name__=='__main__':
    main()
