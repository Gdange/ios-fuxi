#!/usr/bin/env python3
"""Source-only M6l actual-C sequence, size, vectors and containment tests."""
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
m6k = module('m6k', 'test-fuxi-wlan-m6k.py')

TESTS = r'''
static void configure(const char *pre, const char *bhi, const char *vec) {
    pre_arg=pre; bhi_arg=bhi; vec_arg=vec; wl_probe_configure();
}
static void prepare(void) {
    for(unsigned s=17;s<=20;s++) assert(!wl5_step(s));
}
static int socwrite(uint32_t off, uint32_t v) {
    for(unsigned i=0;i<ne;i++) if(events[i].kind=='S' && events[i].pa==off && events[i].v==v) return i;
    return -1;
}
static unsigned dbcount(uint32_t off) {
    unsigned n=0;
    for(unsigned i=0;i<ne;i++) n+=events[i].kind=='W' && events[i].pa==WL_BAR0+off && events[i].v;
    return n;
}
static void set_soc(uint32_t off, uint32_t v) { setreg(0x200000000ull+off,v); }
static void observe(unsigned frees_expected) {
    unsigned start=sleeps;
    wl_stop22=true; /* explicitly exercise the retained BHI diagnostic */
    assert(wl5_step(22)==0x6c && sleeps-start==5000);
    assert(strstr(logbuf,"M6l result") && strstr(logbuf,"stop/contain"));
    assert(!dbcount(wl_bhie+0x3c)); /* never AMSS TX doorbell */
    assert(wl5_step(21)==0x6b && dbcount(wl_bhi+0x18)==1);
    wl5_abort(22); assert(frees==frees_expected);
}
int main(void) {
    /* Missing and explicit off leave all experiment MMIO/allocations absent. */
    for(unsigned explicit=0;explicit<2;explicit++) {
        init(); if(explicit) configure("off","off","off");
        assert(!wl_variant_active() && !wl_variant_invalid); prepare();
        assert(!wl5_step(21) && !wl5_step(22));
        assert(!strstr(logbuf,"pre-BHI") && !strstr(logbuf,"M6l result"));
        assert(!wl_rddm.va && !wl_rxvectors.va);
        for(unsigned i=0;i<ne;i++) assert(events[i].kind!='S');
        wl5_abort(22); assert(frees==4);
    }
    const char *bad[]={"RESET","reset,wake","reset+warm","1","abcdefghijklmnopreset"};
    for(unsigned a=0;a<3;a++) for(unsigned b=0;b<5;b++) {
        init(); configure(a==0?bad[b]:NULL,a==1?bad[b]:NULL,a==2?bad[b]:NULL);
        assert(wl_variant_invalid && wl5_step(17)==0x6c && !allocations && !ne);
    }
    for(unsigned probe=0;probe<2;probe++) {
        init(); probe_arg=probe?"pattern":"unmapped"; configure("warm",NULL,NULL);
        assert(wl_variant_invalid && wl5_step(17)==0x6c && !allocations);
    }
    const char *pres[]={"qfprom","warm","reset","wake"};
    for(unsigned mode=0;mode<4;mode++) {
        init(); configure(pres[mode],NULL,NULL);
        setreg(WL_BAR0+0x310c,0x80001203); /* preserve static windows */
        set_soc(0x1f8031c,0xa5); set_soc(0x1f80500,0xc0000000);
        set_soc(0x1f80504,0x12345678); set_soc(0x1f8060c,0x102);
        set_soc(0x1e40304,0x80); set_soc(0x1e081b0,0x100);
        prepare(); assert(wl_mrd(0x310c)==0x80001203);
        assert(strstr(logbuf,"complete BME off EE PBL"));
        if(mode==0) {
            assert(socwrite(0x1f8031c,0xa0)>=0); /* clear 4 and 1 */
            assert(socwrite(0x1f80504,0)<0 && !dbcount(0x3008));
        }
        if(mode==1 || mode==2) {
            assert(socwrite(0x1f80504,0)>=0);
            assert(socwrite(0x1f80500,0)<0 && socwrite(0x1f8060c,0)<0);
        }
        if(mode==2) {
            int lt=socwrite(0x1e081b0,0x111), hot=socwrite(0x1e40304,0x90);
            int intr=socwrite(0x1e08228,~0u), pwr=socwrite(0x1f8031c,0xa1);
            int clear=findwrite(WL_BAR0+wl_bhie+0x3c,0,0);
            int warm=socwrite(0x1f80504,0), on=findwrite(WL_BAR0+0x3008,1,0);
            int off=findwrite(WL_BAR0+0x3008,0,0), mhi=findwrite(WL_BAR0+0x38,2,0);
            assert(lt>=0 && lt<hot && hot<intr && intr<pwr && pwr<clear && clear<warm);
            assert(warm<on && on<off && off<mhi && sleeps==55);
            for(unsigned i=0;i<ne;i++) assert(!(events[i].kind=='S' && events[i].pa==0x1e38338));
        }
        if(mode==3) assert(findwrite(WL_BAR0+0x3004,1,0)>=0 && sleeps==5);
        assert(!wl5_step(21)); observe(4);
    }
    /* A reset/target register failure blocks all firmware DMA. Dynamic window
       gets restored on readback failure AND a latched synchronous exception. */
    for(unsigned kind=0;kind<5;kind++) {
        init(); configure(kind==4?"wake":"qfprom",NULL,NULL);
        assert(!wl5_step(17)); setreg(WL_BAR0+0x310c,0x40001203);
        if(kind==0) set_soc(0x1f8031c,~0u);
        if(kind==1) { mismatch=true; mismatch_pa=WL_BAR0+0x8031c; set_soc(0x1f8031c,5); }
        if(kind==2) read_fault_pa=WL_BAR0+0x8031c;
        if(kind==3) setreg(WL_EPCFG,0x110617cb);
        if(kind==4) { mismatch=true; mismatch_pa=WL_BAR0+0x3004; }
        assert(wl5_step(18)==0x6c && !dbcount(wl_bhi+0x18) && !allocations);
        assert(wl_mrd(0x310c)==0x40001203);
        read_fault_pa=0; wl5_abort(18);
    }
    for(unsigned kind=0;kind<2;kind++) {
        init(); configure("reset",NULL,NULL); assert(!wl5_step(17));
        stuck_mhi=kind==0; fault_soc_assert=kind==1;
        assert(wl5_step(18)==0x6c && !dbcount(wl_bhi+0x18));
        assert(findwrite(WL_BAR0+0x3008,0,0)>findwrite(WL_BAR0+0x3008,1,0));
        stuck_mhi=fault_soc_assert=false; wl5_abort(18);
    }
    /* Full-file BHI covers the unaligned tail, live walk rejects a corrupt
       tail PTE; one 512-KiB vector size remains independent of BHI size. */
    for(unsigned legacy=0;legacy<2;legacy++) {
        init(); old_iova_present=true; old_iova_arg=legacy; wl_iova_configure();
        configure(NULL,"full",NULL); noncontig=true; prepare();
        assert(wl_bhi_bytes()==sizeof(fixture)); assert(!wl5_step(21));
        assert(wl_mrd(wl_bhi+0x10)==sizeof(fixture));
        assert(strstr(logbuf,"pages 129/129 MATCH"));
        struct wl_vector *v=(void *)wl_vectors.va;
        assert(v[0].size==WL_CHUNK && v[1].size==37);
        observe(4);
    }
    init(); configure(NULL,"full",NULL); prepare();
    uint64_t addr=wl_image.iova+WL_CHUNK;
    uint64_t *l3=(void *)(wl_tables.va+(2+((addr-wl_iova_base)>>21))*4096);
    l3[(addr>>12)&511]=0;
    assert(wl5_step(21)==8 && !dbcount(wl_bhi+0x18)); wl5_abort(21);
    /* Early TX writes only address/length; RDDM writes a distinct zeroed sink
       table and arms RX before BHI, preserving all image/vector contents. */
    for(unsigned rx=0;rx<2;rx++) {
        init(); configure(NULL,NULL,rx?"rddm":"early"); noncontig=true; prepare();
        if(rx) {
            struct wl_vector *v=(void *)wl_rxvectors.va;
            assert(wl_rddm.size==0x780000 && wl_rxvectors.iova!=wl_vectors.iova);
            for(unsigned i=0;i<15;i++) {
                assert(v[i].addr==wl_rddm.iova+i*WL_CHUNK && v[i].size==WL_CHUNK);
                for(unsigned off=0;off<WL_CHUNK;off+=4096)
                    assert((walk(v[i].addr+off)&0xfffffffff000ull)==wl_phys(wl_rddm.va+i*WL_CHUNK+off));
            }
            for(unsigned i=0;i<WL_RDDM_SIZE;i++) assert(!((uint8_t *)wl_rddm.va)[i]);
        }
        assert(!wl5_step(21));
        int bhi=findwrite(WL_BAR0+wl_bhi+0x18,wl_seq,0);
        int vec=findwrite(WL_BAR0+wl_bhie+(rx?0x60:0x2c),rx?wl_rxvectors.iova:wl_vectors.iova,0);
        assert(vec>=0 && vec<bhi && !dbcount(wl_bhie+0x3c));
        if(rx) assert(findwrite(WL_BAR0+wl_bhie+0x70,0x12345,0)<bhi);
        observe(rx?6:4);
    }
    init(); probe_arg="civac"; configure(NULL,NULL,"rddm"); prepare(); assert(!wl5_step(21));
    struct wl_dma *bufs[]={&wl_image,&wl_vectors,&wl_contexts,&wl_tables,&wl_rddm,&wl_rxvectors};
    for(unsigned i=0;i<6;i++) assert(civac_lines[i]==bufs[i]->size/64);
    int db=findwrite(WL_BAR0+wl_bhi+0x18,wl_seq,0);
    assert(db>0 && events[db-1].kind=='B'); observe(6);
    /* Partial RDDM allocation, RX setup mismatch, and containment failures.
       Allocation count includes successfully allocated image/vector/contexts. */
    for(unsigned nth=4;nth<=6;nth++) {
        init(); configure(NULL,NULL,"rddm"); assert(!wl5_step(17) && !wl5_step(18) && !wl5_step(19));
        fail_nth_alloc=nth; assert(wl5_step(20));
        assert(!dbcount(wl_bhi+0x18)); wl5_abort(20); assert(frees==nth-1); fail_nth_alloc=0;
    }
    init(); configure(NULL,NULL,"rddm"); prepare();
    mismatch=true; mismatch_pa=WL_BAR0+wl_bhie+0x68;
    assert(wl5_step(21)==12 && !dbcount(wl_bhi+0x18));
    mismatch=false; wl5_abort(21); assert(frees==6);
    init(); configure(NULL,NULL,"rddm"); prepare(); assert(!wl5_step(21));
    fail_bm=fail_perst=true; wl5_abort(21); assert(!frees && wl_rddm.va && wl_rxvectors.va);
    fail_bm=fail_perst=false; wl5_abort(21); assert(frees==6);
    puts("M6l preconditions/reset order, sizes, TX/RX tables and containment tests passed");
    return 0;
}
'''


def layout_tests():
    for cls in (1, 2):
        data = bytearray(0x80001)
        data[:7] = b'\x7fELF' + bytes([cls, 1, 1])
        hsize, psize = (52, 32) if cls == 1 else (64, 56)
        fmt = '<HHIIIIIHHHHHH' if cls == 1 else '<HHIQQQIHHHHHH'
        struct.pack_into(fmt, data, 16, 2, 40, 1, 0, hsize, 0, 0, hsize, psize, 2, 0, 0, 0)
        phfmt = '<IIIIIIII' if cls == 1 else '<IIQQQQQQ'
        def segment(index, offset, size, flags):
            values = (0, offset, 0, 0, size, size, flags, 4096) if cls == 1 else (0, flags, offset, 0, 0, size, size, 4096)
            struct.pack_into(phfmt, data, hsize + psize * index, *values)
        segment(0, 0x1000, 0x368, 0x02200000)
        segment(1, 0x80000, 1, 5)
        result = elf.inspect(data)
        assert result['metadata_fits'] and result['hash_count'] == 1
        assert result['elf_class'] == cls * 32
        assert not result['segments'][1]['fits_prefix']
        segment(0, 0x7ffff, 2, 0x02200000)
        assert not elf.inspect(data)['metadata_fits']
        segment(0, 0x80000, 2, 0x02200000)
        try:
            elf.inspect(data)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted out-of-file segment')
        for length in (0, 15, hsize - 1, hsize + psize):
            try:
                elf.inspect(data[:length])
            except ValueError:
                pass
            else:
                raise AssertionError('accepted truncated header/table')
    print('M6l synthetic ELF32/64 layout and prefix bounds passed')


def main():
    layout_tests()
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    patch = (ROOT / 'xnu/patches/0099-fuxi-wlan.patch').read_text()
    added = patch.split('+++ b/bsd/dev/fuxi_wlan.c\n', 1)[1].split('\n--- a/', 1)[0]
    assert int(re.match(r'@@ -0,0 \+1,(\d+) @@', added)[1]) == len(driver.splitlines())
    assert ''.join(l[1:] + '\n' for l in added.splitlines() if l.startswith('+')) == driver
    block = '/* ---- milestone 5:' + driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    block = block.replace('__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));', 'ctr = 4ull << 16;')
    block = block.replace('__asm__ volatile("dc civac, %0" :: "r"(va) : "memory");', 'test_civac(va);')
    block = block.replace('__asm__ volatile("dsb sy" ::: "memory");', "record('B',0,0);")
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* mocked privileged asm */;', block)
    recorder = m6k.RECORD_CACHE.replace('civac_lines[4]', 'civac_lines[6]').replace(
        '&wl_contexts,&wl_tables', '&wl_contexts,&wl_tables,&wl_rddm,&wl_rxvectors').replace('i<4;', 'i<6;')
    block = block.replace('static void\nwl_dma_civac(', recorder + '\nstatic void\nwl_dma_civac(')
    preamble = m6.PREAMBLE.replace('static bool cache_drop;', 'static bool cache_drop, stuck_mhi, fault_soc_assert;\nstatic unsigned fail_nth_alloc;')
    preamble = preamble.replace('if (pa==WL_BAR0+0x38 && v==2)', 'if (pa==WL_BAR0+0x38 && v==2 && !stuck_mhi)')
    preamble = preamble.replace("    record('W',pa,v);", "    record('W',pa,v);\n    if(fault_soc_assert && pa==WL_BAR0+0x3008 && (v&1)) fw_faults++;")
    preamble = preamble.replace('if(fail_alloc) return 1;', 'if(fail_alloc || (fail_nth_alloc && allocations+1==fail_nth_alloc)) return 1;')
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6l-') as tmp:
        source, binary = Path(tmp) / 'm6l.c', Path(tmp) / 'm6l'
        source.write_text(preamble + block + m6.TESTS.split('int main(void) {', 1)[0] + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
