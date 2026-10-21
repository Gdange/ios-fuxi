#!/usr/bin/env python3
"""Reject non-iOS16 arm64 slices, non-system loads and EL0 CTR_EL0 reads.

Load-path/minimum-OS checks are necessary but do not prove every imported
symbol exists on iOS 16; review the recorded undefined-symbol list as well.
"""
import argparse
import json
import re
from pathlib import Path
import struct
import subprocess


def audit(path, required_export="_vk_icdGetInstanceProcAddr"):
    raw=path.read_bytes()
    magic,cpu,subtype,kind,ncmds,size,flags,reserved=struct.unpack_from('<8I',raw)
    assert (magic,cpu,subtype)==(0xfeedfacf,0x100000c,0), 'plain arm64 Mach-O required'
    assert kind in (2,6), 'executable/dylib required'
    pos=32; minimum=[]; loads=[]; ids=[]; texts=[]; signature=False
    for _ in range(ncmds):
        cmd,sz=struct.unpack_from('<2I',raw,pos)
        assert sz>=8 and pos+sz<=32+size
        if cmd==0x32:
            platform,version,sdk=struct.unpack_from('<3I',raw,pos+8)
            minimum.append((platform,version,sdk))
        if cmd in (0xc,0x80000018,0x8000001f,0x80000023,0xd):
            off=struct.unpack_from('<I',raw,pos+8)[0]
            name=raw[pos+off:pos+sz].split(b'\0')[0].decode()
            (ids if cmd==0xd else loads).append(name)
        if cmd==0x8000001c:
            raise AssertionError('unexpected LC_RPATH')
        if cmd==0x1d: signature=True
        if cmd==0x19:
            nsects=struct.unpack_from('<I',raw,pos+64)[0]
            for i in range(nsects):
                off=pos+72+80*i
                name=raw[off:off+16].split(b'\0')[0]
                length=struct.unpack_from('<Q',raw,off+40)[0]
                fileoff=struct.unpack_from('<I',raw,off+48)[0]
                attr=struct.unpack_from('<I',raw,off+64)[0]
                if name==b'__text' or attr & 0x80000400:
                    texts.append(raw[fileoff:fileoff+length])
        pos+=sz
    assert len(minimum)==1 and minimum[0][0:2]==(2,0x100000), minimum
    assert signature, 'unsigned artifact'
    assert loads and all(s.startswith(('/usr/lib/','/System/Library/Frameworks/')) for s in loads), loads
    assert all((w[0]&0xffffffe0)!=0xd53b0020 for code in texts for w in struct.iter_unpack('<I',code)), 'CTR_EL0 read'
    print(path.name+': arm64; platform iOS; minos 16.0; SDK %d.%d; signed; no CTR_EL0 reads'%
          (minimum[0][2]>>16,(minimum[0][2]>>8)&255))
    for s in ids: print('  ID '+s)
    for s in loads: print('  LOAD '+s)
    if kind==6 and required_export:
        symbols=subprocess.check_output(['nm','-gU',str(path)],text=True)
        assert ' T '+required_export in symbols
    imports=subprocess.check_output(['nm','-um',str(path)],text=True)
    strong={};weak_dynamic=0
    for line in imports.splitlines():
        m=re.search(r'\(undefined\) (weak )?external (\S+) \((?:from (.+)|dynamically looked up)\)',line)
        if not m: continue
        if not m[1]:
            # Xcode 15's linker emits objc_msgSend selector stubs as a flat lookup;
            # accept only that symbol, and only when libobjc is linked.
            if not m[3] and m[2]=='_objc_msgSend' and '/usr/lib/libobjc.A.dylib' in loads:
                strong[m[2]]='libobjc'; continue
            assert m[3], 'strong dynamic lookup: '+line
            strong[m[2]]=m[3]
        elif not m[3]: weak_dynamic+=1
    print('  %d strong system imports; %d weak dynamic entrypoint references'%(len(strong),weak_dynamic))
    path.with_suffix(path.suffix+'.undefined.txt').write_text(imports)
    return strong


def check_cache(symbols, required):
    records=json.loads(symbols.read_text());needed=json.loads(required.read_text())
    def provider(lib, image):
        # The supplied iOS 16.7 libSystem explicitly reexports these providers.
        if lib=='libSystem':
            return image.startswith('libsystem_') or image in ('libdyld.dylib','libunwind.dylib','dyld',
                'libdispatch.dylib','libcommonCrypto.dylib')
        if lib=='libc++': return image in ('libc++.1.dylib','libc++abi.dylib')
        names={'libz':'libz.1.dylib','libobjc':'libobjc.A.dylib'}
        return image==names.get(lib,lib)
    found={r.get('name') for r in records if r.get('address',0) and
           'undefined' not in r.get('type','') and r.get('name') in needed and
           provider(needed[r['name']],r.get('image',''))}
    missing=set(needed)-found
    assert not missing, 'missing from expected system provider/reexports: '+str(sorted(missing))
    print('PASS: all %d required imports defined in the supplied cache system providers/reexports'%len(needed))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binaries',type=Path,nargs='*')
    ap.add_argument('--cache-symbols',type=Path,help='ipsw dyld symaddr --output JSON')
    ap.add_argument('--required',type=Path,help='M2-required-symbol-libs.json from the Mac audit')
    args=ap.parse_args()
    if args.cache_symbols or args.required:
        if not (args.cache_symbols and args.required): ap.error('supply both --cache-symbols and --required')
        check_cache(args.cache_symbols,args.required)
    if not args.binaries:
        if args.cache_symbols: return
        ap.error('supply binaries or a cache-symbol check')
    required={}
    for path in args.binaries: required.update(audit(path))
    # ipsw dyld symaddr --in expects a list of regex lookup objects. One
    # combined pattern scans the cache once; require nonzero defined addresses.
    target=args.binaries[0].parent/'M2-required-symbols.json'
    target.write_text(json.dumps([{'pattern':'^('+ '|'.join(re.escape(s) for s in sorted(required))+')$'}]))
    target.with_name('M2-required-symbol-libs.json').write_text(json.dumps(required,indent=2))


if __name__=='__main__': main()
