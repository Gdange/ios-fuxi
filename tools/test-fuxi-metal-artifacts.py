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
    ap.add_argument('--lane', choices=('metal1','metal2a','metal2b','metal2c','metal2e','metal2g','metal2h','metal3','metal4','metal5a'), default='metal1')
    ap.add_argument('--base-boot', type=Path, default=Path('shim/out/fuxi-metal2c-boot.img'))
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
    if args.lane in ('metal2g','metal2h','metal3','metal4','metal5a'):
        assert '_fuxi_gpu_nc_pointer' in symbols and '_pmap_sync_page_attributes_phys' in symbols
        assert b'FUXI GPU: cache sync outside RAM/NC windows:' in raw
    assert any('NativeAdrenoGpuPublished10gMetaClass' in name for name in symbols)
    assert not any('IOGPUFamily' in name for name in symbols)
    if args.lane in ('metal4','metal5a'):
        assert any('IOAcceleratorES10gMetaClass' in name for name in symbols)
        for text in (b'MetalPluginName',b'MetalPluginClassName',b'FuxiMetal',b'FuxiMTLDevice',b'IOAcceleratorES metaclass match PASS'):
            assert text in raw,text
    else:
        assert not any('IOAcceleratorES' in name for name in symbols)
    table=symbols['_copyio_recover_table'];end=symbols['_copyio_recover_table_end']
    seg=next(s for s in mach['segs'] if s['vmaddr']<=table and end<=s['vmaddr']+s['filesize'])
    offset=table-seg['vmaddr']+seg['fileoff'];entries=[]
    for n in range(0,end-table,24):
        a,b,c=struct.unpack_from('<qqq',raw,offset+n);entries.append((table+a,table+b,table+c))
    begin=symbols['_fuxi_gpu_access_begin'];stop=symbols['_fuxi_gpu_access_end']
    assert sum(a>=begin and b==stop and c>stop for a,b,c in entries)==2
    print('PASS: native ABI/service/discovery symbols, no IOGPUFamily, linked MMIO recovery')
    payload=Path(f'shim/out/fuxi-{args.lane}-payload.bin').read_bytes()
    image=Path(f'shim/out/fuxi-{args.lane}-boot.img').read_bytes()
    assert payload[:8]==b'SWEETXNU'
    cmdline=payload[72:680].split(b'\0')[0]
    basecmd=args.base.joinpath('cmdline.txt').read_bytes().strip()
    if args.lane in ('metal2b','metal2c','metal2e','metal2g','metal2h','metal3','metal4','metal5a'):
        if args.lane=='metal5a':
            expected=basecmd
            for file in ('kernel.development.fuxi','kernel.development.fuxi.unstripped','fuxi.adt','cmdline.txt'):
                assert (args.out/file).read_bytes()==(args.base/file).read_bytes(),file
            assert expected.split().count(b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-metal-smoke')==1
        elif args.lane=='metal4':
            args4=basecmd.split()
            old=b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-vk-smoke'
            assert args4.count(old)==1
            args4[args4.index(old)]=b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-metal-smoke'
            null=next(a for a in args4 if a.startswith(b'sweetnull='))
            args4[args4.index(null)]=null+b',fuxi-metal-smoke'
            expected=b' '.join(args4)
            for text in (b'CP_IB1_BASE_LO',b'CP_HW_FAULT',b'SMMU GPU CB',b'FUXI GPU HANG:'):
                assert text in raw,text
        elif args.lane=='metal3':
            args3=basecmd.split()
            old=b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-gpu-abi-test'
            assert args3.count(old)==1
            args3[args3.index(old)]=b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-vk-smoke'
            assert sum(a.startswith(b'sweetnull=') for a in args3)<=1
            args3=[a for a in args3 if not a.startswith(b'sweetnull=')]
            args3.append(b'sweetnull=backboardd,SpringBoard,fuxi-vk-smoke,peakpowermanagerd')
            expected=b' '.join(args3)
            for text in (b'CP_IB1_BASE_LO',b'CP_HW_FAULT',b'SMMU GPU CB',b'FUXI GPU HANG:'):
                assert text in raw,text
        elif args.lane in ('metal2g','metal2h'):
            expected=basecmd
            assert (args.out/'cmdline.txt').read_bytes()==(args.base/'cmdline.txt').read_bytes()
            assert expected.split().count(b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-gpu-abi-test')==1
            assert expected.split().count(b'sweetnoexec=bluetoothd,fairplayd.H2')==1
        elif args.lane=='metal2e':
            assert basecmd.split().count(b'sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test')==1
            assert basecmd.split().count(b'sweetnoexec=fairplayd.H2')==1
            expected=basecmd.replace(b'sweetexecswap=bluetoothd:',b'sweetexecswap=IOMFB_FDR_Loader:').replace(b'sweetnoexec=fairplayd.H2',b'sweetnoexec=bluetoothd,fairplayd.H2')
            assert b'sweetexecswap=IOMFB_FDR_Loader:/usr/local/bin/fuxi-gpu-abi-test' in expected.split()
            assert b'sweetnoexec=bluetoothd,fairplayd.H2' in expected.split()
            assert len(image)==args.base_boot.stat().st_size,'metal2e boot size differs from metal2c'
            print('PASS: metal2e boot size equals metal2c')
        else:
            expected=basecmd
            assert (args.out/'cmdline.txt').read_bytes()==(args.base/'cmdline.txt').read_bytes()
            assert b'sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test' in expected.split()
        assert (args.out/'cmdline.txt').read_bytes().strip()==expected
        assert b'fuxigpuabi=1' in expected.split() and not any(a.startswith(b'fuxiwlan=') for a in expected.split())
        assert '_fuxi_wlan_init' in symbols and b'SWEET EXECSWAP:' in raw and b'FUXI ABI: result' in raw
        assert b'FUXI GPU ABI: init check' in raw and b'FUXI GPU ABI: init write' in raw
    elif args.lane=='metal2a':
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
    tool=Path('tools/out/'+('fuxi-metal-smoke' if args.lane in ('metal4','metal5a') else 'fuxi-vk-smoke' if args.lane=='metal3' else 'fuxi-gpu-abi-test')).read_bytes()
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
    if args.lane in ('metal2e','metal2g','metal2h','metal3','metal4','metal5a'): assert all(firmware),'lane requires all three firmware inputs'
    if any(firmware):
        assert all(firmware),'provide all three firmware inputs'
        loader=Path(f'shim/out/fuxi-{args.lane}-loader.elf').read_bytes()
        assert loader[:6]==b'\x7fELF\x02\x01','expected ELF64 little-endian loader'
        shoff=struct.unpack_from('<Q',loader,40)[0]
        entsize,count,strings=struct.unpack_from('<3H',loader,58)
        sections=[struct.unpack_from('<II4QII2Q',loader,shoff+i*entsize) for i in range(count)]
        names=loader[sections[strings][4]:sections[strings][4]+sections[strings][5]]
        gpu=next(s for s in sections if names[s[0]:].split(b'\0')[0]==b'.gpu_fw')
        blobs=[path.read_bytes() for path in firmware]
        assert gpu[5]==sum(map(len,blobs))>0
        section=loader[gpu[4]:gpu[4]+gpu[5]]
        assert section==b''.join(blobs) and gpu[2]&2,'GPU firmware must be allocated in the Image'
        assert arm[gpu[3]:gpu[3]+gpu[5]]==section,'boot image lacks GPU firmware section'
        for path,blob in zip(firmware,blobs): assert loader.count(blob)==1 and arm.count(blob)==1,path.name
        print('PASS: allocated .gpu_fw section and exact external GMU/SQE/zap blobs in loader ELF and boot Image')
    for line in args.out.joinpath('SHA256SUMS').read_text().splitlines():
        digest,path=line.split(None,1)
        assert hashlib.sha256(Path(path.strip()).read_bytes()).hexdigest()==digest,path
    print('PASS: artifact SHA256SUMS')


if __name__=='__main__':
    main()
