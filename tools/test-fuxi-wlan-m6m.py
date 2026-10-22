#!/usr/bin/env python3
"""Linux tests of actual M6m C: stock input, L1SS, BHI controls and bounds."""
from pathlib import Path
import importlib.util
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / file)
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj


m6 = module('m6', 'test-fuxi-wlan-m6.py')
elf = module('elf_layout', 'wlan-elf-layout.py')

TESTS = r'''
static void prepare(void) { for(unsigned s=17;s<=20;s++) assert(!wl5_step(s)); }
static unsigned writes(uint64_t pa) {
    unsigned count=0;
    for(unsigned i=0;i<ne;i++) count+=events[i].kind=='W' && events[i].pa==pa;
    return count;
}
static void l1ss(void) {
    setreg(WL_EPCFG+0x100,0x0001001e); setreg(WL_DBI+0x100,0x0001001e);
    setreg(WL_EPCFG+0x108,0x12345f); setreg(WL_DBI+0x108,0x54321f);
    setreg(WL_EPCFG+0x10c,0x12345678); setreg(WL_DBI+0x10c,0x87654321);
    setreg(WL_EPCFG+0x80,0xa0000003); setreg(WL_DBI+0x80,0x20000003);
    /* Low-half config writes do not clear the read-only Link Status. */
    read_masks[nread_masks++]=(struct read_mask){WL_EPCFG+0x80,0xa0000000,0};
    read_masks[nread_masks++]=(struct read_mask){WL_DBI+0x80,0x20000000,0};
}
static void hash_fixture(unsigned cls, unsigned version, unsigned qti_sig,
    unsigned qti_cert, unsigned oem_sig, unsigned oem_cert) {
    memset(fixture,0,sizeof(fixture)); memcpy(fixture,"\177ELF\1\1\1",7);
    fixture[4]=cls; fixture[20]=1;
    unsigned phoff=cls==1?52:64;
    fixture[cls==1?28:32]=phoff;
    fixture[cls==1?40:52]=phoff; fixture[cls==1?42:54]=cls==1?32:56;
    fixture[cls==1?44:56]=1;
    uint32_t flags=0x02200000;
    uint64_t offset=0x1000, size=0x80;
    memcpy(fixture+phoff+(cls==1?24:4),&flags,4);
    memcpy(fixture+phoff+(cls==1?4:8),&offset,cls==1?4:8);
    memcpy(fixture+phoff+(cls==1?16:32),&size,cls==1?4:8);
    uint32_t h[10]={0,version,qti_sig,qti_cert,0x58,0x20,0,oem_sig,0,oem_cert};
    memcpy(fixture+0x1000,h,sizeof(h));
}
int main(void) {
    /* Absent/off selections preserve full mission path and inherited L1SS. */
    for(unsigned explicit=0;explicit<2;explicit++) {
        init(); l1ss(); if(explicit) { fw_arg=pre_arg="off"; wl_probe_configure(); }
        assert(!wl_variant_active()); prepare();
        assert(!strstr(wl_fw_path,"stock-"));
        for(unsigned s=21;s<=28;s++) assert(!wl5_step(s));
        assert(!writes(WL_EPCFG+0x108) && !writes(WL_DBI+0x108));
        wl5_abort(28); assert(frees==4);
    }
    const char *bad[]={"STOCK","amss","stock,full","stock/../x","stock-suffix-long"};
    for(unsigned i=0;i<5;i++) {
        init(); fw_arg=bad[i]; wl_probe_configure();
        assert(wl_variant_invalid && wl5_step(17)==0x6c && !ne && !allocations);
    }
    init(); pre_arg="l1ss"; aspm_arg=1; wl_probe_configure();
    assert(wl_variant_invalid && wl5_step(17)==0x6c && !ne);
    for(unsigned i=0;i<2;i++) {
        init(); fw_arg="stock"; probe_arg=i?"pattern":"unmapped"; wl_probe_configure();
        assert(wl_variant_invalid && wl5_step(17)==0x6c && !ne);
    }
    for(unsigned badchip=0;badchip<2;badchip++) {
        init(); fw_arg="stock"; wl_probe_configure(); assert(!wl5_step(17));
        if(badchip) setreg(WL_BAR0+0x80000,0x101);
        else setreg(WL_EPCFG,0x110317cb);
        assert(wl5_step(18)==0x6c && !allocations);
    }
    /* Missing stock is a single open, never a fallback to the known image. */
    init(); fw_arg="stock"; wl_probe_configure();
    assert(!wl5_step(17) && !wl5_step(18)); open_error=2; opens=0;
    assert(wl5_step(19)==2 && opens==1 && !strcmp(opened_path,wl_fw_path));
    assert(strstr(opened_path,"/stock-amss20.bin") && !allocations);
    assert(!writes(WL_BAR0+0x118)); wl5_abort(19);
    for(unsigned legacy=0;legacy<2;legacy++) for(unsigned full=0;full<2;full++) {
        init(); old_iova_present=true; old_iova_arg=legacy; wl_iova_configure();
        fw_arg="stock"; bhi_arg=full?"full":NULL; wl_probe_configure();
        noncontig=true; prepare(); assert(!wl5_step(21));
        assert(wl_mrd(wl_bhi+0x10)==(full?sizeof(fixture):WL_CHUNK));
        assert(!memcmp((void *)wl_image.va,fixture,sizeof(fixture)));
        struct wl_vector *v=(void *)wl_vectors.va;
        assert(v[0].addr==wl_image.iova && v[0].size==WL_CHUNK && v[1].size==37);
        assert(wl_seq && !(wl_seq&~WL_SEQ_MASK));
        wl_stop22=true; /* explicitly exercise the retained BHI diagnostic */
        assert(wl5_step(22)==0x6c && strstr(logbuf,"M6m result fw=stock"));
        assert(writes(WL_BAR0+0x118)==1 && !wl_mrd(wl_bhie+0x3c));
        assert(wl5_step(21)==0x6b && writes(WL_BAR0+0x118)==1);
        wl5_abort(22); assert(frees==4);
    }
    /* Both functions: exact mask, upper bits/CTL2 retained, before BME/DB. */
    init(); l1ss(); pre_arg="l1ss"; wl_probe_configure(); prepare();
    assert(!wl5_step(21));
    assert(wl_prd(WL_EPCFG+0x108)==0x123450 && wl_prd(WL_DBI+0x108)==0x543210);
    assert(wl_prd(WL_EPCFG+0x10c)==0x12345678 && wl_prd(WL_DBI+0x10c)==0x87654321);
    int ep=findwrite(WL_EPCFG+0x108,0x123450,0), rc=findwrite(WL_DBI+0x108,0x543210,0);
    int bm=findwrite(WL_EPCFG+4,0x406,0), db=findwrite(WL_BAR0+0x118,wl_seq,0);
    assert(ep>=0 && rc>ep && bm>rc && db>bm);
    wl_stop22=true; /* explicitly exercise the retained BHI diagnostic */
    assert(wl5_step(22)==0x6c && strstr(logbuf,"pre=l1ss"));
    fail_bm=fail_perst=true; wl5_abort(22); assert(!frees && wl_image.va);
    fail_bm=fail_perst=false; wl5_abort(22); assert(frees==4);
    /* Optional absence and already-disabled bits are observable no-ops. */
    for(unsigned present=0;present<2;present++) {
        init(); if(present) { l1ss(); setreg(WL_EPCFG+0x108,0); setreg(WL_DBI+0x108,0); }
        pre_arg="l1ss"; wl_probe_configure(); prepare(); assert(!wl5_step(21));
        assert(!writes(WL_EPCFG+0x108) && !writes(WL_DBI+0x108));
        assert(strstr(logbuf,present?"mask 0xf":"L1SS absent")); wl5_abort(21);
    }
    /* Readback failure, all ones, malformed chain and sync fault block DMA. */
    for(unsigned kind=0;kind<5;kind++) {
        init(); l1ss(); pre_arg="l1ss"; wl_probe_configure(); prepare();
        if(kind==0) { mismatch=true; mismatch_pa=WL_EPCFG+0x108; }
        if(kind==1) setreg(WL_EPCFG+0x108,~0u);
        if(kind==2) read_fault_pa=WL_EPCFG+0x108;
        if(kind==3) setreg(WL_EPCFG+0x100,0x10100002); /* misaligned next */
        if(kind==4) { mismatch=true; mismatch_pa=WL_DBI+0x108; }
        assert(wl5_step(21)==11 && !writes(WL_BAR0+0x118));
        assert(!(wl_prd(WL_EPCFG+4)&4));
        read_fault_pa=0; mismatch=false; wl5_abort(21); assert(frees==4);
    }
    /* v5 length diagnostics on actual ELF32/64; unknown versions unparsed.
       Corrupt lengths fail closed without dereferencing signature pointers. */
    for(unsigned cls=1;cls<=2;cls++) for(unsigned kind=0;kind<4;kind++) {
        init(); hash_fixture(cls,kind==3?6:5,kind==1?8:0,kind==1?8:0,
                            kind==2?~0u:(kind==1?8:0),kind==1?8:0);
        assert(!wl5_step(17) && !wl5_step(18));
        assert(wl5_step(19)==(kind==2?6:0));
        if(kind<3) assert(strstr(logbuf,"M6m MBNv5") && strstr(logbuf,"authentication unverified"));
        else assert(strstr(logbuf,"version 6 unparsed"));
        if(kind==0) assert(strstr(logbuf,"QTI sig/cert 0/0 OEM sig/cert 0/0"));
        assert(!writes(WL_BAR0+0x118)); wl5_abort(19); assert(frees==1);
    }
    puts("M6m stock input, v5 lengths, L1SS masks/order/failures and diagnostic containment passed");
    return 0;
}
'''


def main():
    for cls in (1, 2):
        data = bytearray(0x80000)
        data[:7] = b'\x7fELF' + bytes([cls, 1, 1])
        hsize, psize = (52, 32) if cls == 1 else (64, 56)
        struct.pack_into('<HHIIIIIHHHHHH' if cls == 1 else '<HHIQQQIHHHHHH',
                         data, 16, 2, 40, 1, 0, hsize, 0, 0, hsize, psize, 1, 0, 0, 0)
        ph = (0, 0x1000, 0, 0, 0x80, 0x80, 0x02200000, 4096) if cls == 1 else (
            0, 0x02200000, 0x1000, 0, 0, 0x80, 0x80, 4096)
        struct.pack_into('<8I' if cls == 1 else '<II6Q', data, hsize, *ph)
        for version, sig, fit in ((5, 0, True), (5, 16, True), (5, 0xffffffff, False), (6, 0, None)):
            struct.pack_into('<10I', data, 0x1000, 0, version, 0, 0, 0x58, 32, 0, sig, 0, 16)
            result = elf.inspect(data)['hash_headers'][0]
            assert result['parsed'] == (version == 5)
            if version == 5:
                assert result['lengths_fit'] == fit and result['authentication'] == 'unverified'
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    patch = (ROOT / 'xnu/patches/0099-fuxi-wlan.patch').read_text()
    added = patch.split('+++ b/bsd/dev/fuxi_wlan.c\n', 1)[1].split('\n--- a/', 1)[0]
    assert int(re.match(r'@@ -0,0 \+1,(\d+) @@', added)[1]) == len(driver.splitlines())
    assert ''.join(l[1:] + '\n' for l in added.splitlines() if l.startswith('+')) == driver
    assert 'python3 tools/test-fuxi-wlan-m6m.py' in (ROOT / 'tools/ci/build.sh').read_text()
    block = '/* ---- milestone 5:' + driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* mocked privileged asm */;', block)
    preamble = m6.PREAMBLE.replace('static int vnode_open(', 'static const char *opened_path; static unsigned opens;\nstatic int vnode_open(')
    preamble = preamble.replace('assert(mode == (FREAD|O_NOFOLLOW|O_NONBLOCK));',
                                'opens++; opened_path=p; assert(mode == (FREAD|O_NOFOLLOW|O_NONBLOCK));')
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6m-') as tmp:
        source, binary = Path(tmp) / 'm6m.c', Path(tmp) / 'm6m'
        source.write_text(preamble + block + m6.TESTS.split('int main(void) {', 1)[0] + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
