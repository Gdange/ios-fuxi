#!/usr/bin/env python3
"""Host-only M6k production C probes, cache policy and ASPM regression tests.

All ELF bytes, MMIO and faults are synthetic. Cache instructions are recorded;
this proves selection, ordering and containment, not physical DMA visibility.
"""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('m6', ROOT / 'tools/test-fuxi-wlan-m6.py')
m6 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m6)

RECORD_CACHE = r'''
static unsigned civac_lines[4];
static void test_civac(uintptr_t va) {
    struct wl_dma *bufs[]={&wl_image,&wl_vectors,&wl_contexts,&wl_tables};
    for(unsigned i=0;i<4;i++) if(va>=bufs[i]->va && va<bufs[i]->va+bufs[i]->size) {
        assert(va==bufs[i]->va+64*civac_lines[i]);
        if(!civac_lines[i]) record('V',va,0);
        civac_lines[i]++; return;
    }
    assert(0);
}
'''

TESTS = r'''
static void select_probe(const char *arg) {
    probe_arg=arg; wl_probe_configure(); memset(civac_lines,0,sizeof(civac_lines));
}
static void prepare(void) {
    for(unsigned s=17;s<=20;s++) assert(!wl5_step(s));
}
static unsigned doorbells(void) {
    unsigned n=0;
    for(unsigned i=0;i<ne;i++) n+=events[i].kind=='W' && events[i].pa==WL_BAR0+0x118;
    return n;
}
static void pbl_error(void) {
    setreg(WL_BAR0+0x12c,~0u); setreg(WL_BAR0+0x130,0xef0e0003);
    setreg(WL_BAR0+0x134,(uint32_t)wl_image.iova);
    setreg(WL_BAR0+0x138,0); setreg(WL_BAR0+0x13c,WL_CHUNK);
    setreg(WL_BAR0+0x128,0); setreg(WL_BAR0+0x48,0);
}
static void fault(uint32_t fsr, uint64_t far, uint32_t sid) {
    uint64_t cb=wl_cb_addr(wl_cb);
    setreg(cb+0x58,fsr); setreg(cb+0x60,far); setreg(cb+0x64,far>>32);
    setreg(cb+0x68,0x11); setreg(cb+0x6c,0x22);
    setreg(wl_gr1+0x400+4*wl_cb,sid);
}
int main(void) {
    /* Missing/explicit off preserves real image and the normal 28-step path. */
    init(); assert(wl_probe==WL_PROBE_OFF); select_probe("off");
    prepare(); assert(!wl5_step(21) && !memcmp((void *)wl_image.va,fixture,sizeof(fixture)));
    assert(!wl5_step(22) && !strstr(logbuf,"M6k result")); release();
    const char *bad[]={"pattern,nc","unmapped+nc","PATTERN","n","012345678901234567890"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        init(); select_probe(bad[i]); assert(wl_probe==WL_PROBE_INVALID);
        assert(wl5_step(17)==0x6b && !allocations && !doorbells());
    }
    /* Every image leaf is a deliberate hole; other DMA buffers remain mapped.
       Both apertures and scattered physical pages obey the same contract. */
    for(unsigned legacy=0;legacy<2;legacy++) {
        init(); old_iova_present=true; old_iova_arg=legacy; probe_arg="unmapped";
        wl_iova_configure(); noncontig=true; prepare();
        assert(wl_image.iova==wl_iova_base);
        for(size_t off=0;off<wl_image.size;off+=4096) assert(!walk(wl_image.iova+off));
        assert(walk(wl_vectors.iova) && walk(wl_contexts.iova));
        assert(!wl5_step(21) && doorbells()==1);
        assert(strstr(logbuf,"invalid leaves 128/128 VERIFIED"));
        assert(wl5_step(21)==0x6b && doorbells()==1);
        pbl_error(); fault(0x402,wl_image.iova,0x1401);
        unsigned start=ne;
        assert(wl5_step(22)==0x6b && strstr(logbuf,"target-translation-fault"));
        assert(strstr(logbuf,"FSYNR 0x11/0x22") && strstr(logbuf,"CBFRSYNRA 0x1401 SID 0x1401"));
        for(unsigned i=start;i<ne;i++) assert(!(events[i].kind=='W' && events[i].pa==wl_cb_addr(wl_cb)+0x58));
        wl5_abort(22); assert(frees==4 && doorbells()==1);
    }
    /* Fail before BME/doorbell if even one supposedly absent leaf is mapped. */
    init(); select_probe("unmapped"); prepare();
    unsigned n=(wl_image.iova-wl_iova_base)>>21;
    uint64_t *l3=(void *)(wl_tables.va+(2+n)*4096);
    l3[(wl_image.iova>>12)&511]=wl_image.pa|WL_DMA_PTE;
    assert(wl5_step(21)==8 && !doorbells() && !(wl_prd(WL_EPCFG+4)&4));
    wl5_abort(21); assert(frees==4);
    /* PBL rejects immediately, but a fault arrives later in the observation
       window: the experiment must not return at the first error sample. */
    init(); select_probe("unmapped"); prepare(); assert(!wl5_step(21)); pbl_error();
    late_cb=wl_cb_addr(wl_cb); late_iova=wl_image.iova;
    late_syndrome=wl_gr1+0x400+4*wl_cb; late_after=4;
    assert(wl5_step(22)==0x6b && !late_after && strstr(logbuf,"target-translation-fault"));
    wl5_abort(22); assert(frees==4 && doorbells()==1);
    /* FORMAT alone is not a translation fault. Wrong FAR or SID cannot prove
       that the intended WLAN image request reached this context bank. */
    for(unsigned kind=0;kind<4;kind++) {
        init(); select_probe("unmapped"); prepare(); assert(!wl5_step(21)); pbl_error();
        fault(kind?0x402:0x400,wl_image.iova+(kind==1?WL_CHUNK:0),kind==2?0x1400:0x1401);
        if(kind==3) setreg(WL_EPCFG,~0u);
        unsigned before=sleeps;
        assert(wl5_step(22)==0x6b && sleeps-before==5000);
        assert(strstr(logbuf,kind==0?"no-CB-translation-fault":kind==3?"transport-inconclusive":"other-translation-fault"));
        wl5_abort(22); assert(frees==4 && doorbells()==1);
    }
    /* Real firmware validation still occurs before deliberate corruption. */
    init(); select_probe("pattern"); prepare(); assert(!wl5_step(21));
    uint8_t *p=(void *)wl_image.va;
    assert(!memcmp(p,"\177ELF\377\377\0",7) && !memcmp(p+16,"FUXI-M6K-PATTERN",16));
    for(unsigned i=7;i<64;i++) if(i<16 || i>=32) assert(p[i]==0);
    for(size_t i=64;i<wl_image.size;i++) assert(p[i]==(uint8_t)(0xa5u^(i&255)));
    assert(!wl_bhi_mapping_check()); /* hash follows the substituted contents */
    assert(strstr(logbuf,"pattern first64") && strstr(logbuf,"MATCH"));
    assert(wl5_step(22)==0x6b); /* even unexpected BHI success stops */
    wl5_abort(22); assert(frees==4 && doorbells()==1);
    init(); select_probe("pattern"); fixture[0]=0;
    assert(!wl5_step(17) && !wl5_step(18) && wl5_step(19)==6 && !doorbells()); release();
    /* Actual C iterates CTR_EL0's 64-byte lines, including rounded tails, for
       every buffer; no buffer writes/cleans occur between final DSB and DB. */
    init(); select_probe("civac"); prepare(); assert(!wl5_step(21));
    struct wl_dma *bufs[]={&wl_image,&wl_vectors,&wl_contexts,&wl_tables};
    for(unsigned i=0;i<4;i++) assert(civac_lines[i]==bufs[i]->size/64);
    int db=findwrite(WL_BAR0+0x118,wl_seq,0); assert(db>0 && events[db-1].kind=='B');
    assert(strstr(logbuf,"DC CIVAC + DSB SY image/vectors/contexts/tables"));
    assert(wl5_step(22)==0x6b); wl5_abort(22); assert(frees==4);
    /* NC policy is installed before VFS fills the image; other buffers keep
       default attributes. Do not restore/free pages when containment fails. */
    init(); select_probe("nc"); noncontig=true; prepare();
    for(size_t off=0;off<wl_image.size;off+=PAGE_SIZE)
        assert(pmap_cache_attributes(pmap_find_phys(0,wl_image.va+off))==VM_WIMG_WCOMB);
    assert(pmap_cache_attributes(pmap_find_phys(0,wl_vectors.va))==VM_WIMG_DEFAULT);
    assert(!wl5_step(21) && !memcmp((void *)wl_image.va,fixture,sizeof(fixture)));
    assert(wl5_step(22)==0x6b); fail_bm=fail_perst=true; wl5_abort(22);
    assert(!frees && wl_image.nc && pmap_cache_attributes(pmap_find_phys(0,wl_image.va))==VM_WIMG_WCOMB);
    fail_bm=fail_perst=false; wl5_abort(22); assert(frees==4);
    for(unsigned i=0;i<allocations;i++) for(size_t off=0;off<allocs[i].size;off+=PAGE_SIZE)
        assert(cache_attrs[i][off/PAGE_SIZE]==VM_WIMG_DEFAULT);
    init(); select_probe("nc"); cache_drop=true;
    assert(!wl5_step(17) && !wl5_step(18) && wl5_step(19)==4 && !doorbells());
    wl5_abort(19); assert(frees==1);
    /* ASPM default off on EP and RC, keep W1C Link Status out of writes.
       L1SS controls are observed without mutation; exact revert is opt-in. */
    for(unsigned retain=0;retain<3;retain++) {
        init(); aspm_arg=retain;
        setreg(WL_EPCFG+0x80,0xaaaa0143); setreg(WL_DBI+0x80,0xbbbb0143);
        setreg(WL_EPCFG+0x100,0x1001e); setreg(WL_EPCFG+0x108,15);
        setreg(WL_DBI+0x100,0x1001e); setreg(WL_DBI+0x108,15);
        assert(!wl_download_aspm());
        assert((wl_prd(WL_EPCFG+0x80)&0xffff)==(retain==1?0x143:0x140));
        assert((wl_prd(WL_DBI+0x80)&0xffff)==(retain==1?0x143:0x140));
        for(unsigned i=0;i<ne;i++) if(events[i].kind=='W') {
            assert(retain!=1 && (events[i].pa==WL_EPCFG+0x80 || events[i].pa==WL_DBI+0x80));
            assert(events[i].v==0x140);
        }
        assert(wl_prd(WL_EPCFG+0x108)==15 && wl_prd(WL_DBI+0x108)==15);
    }
    init(); prepare(); setreg(WL_EPCFG+0x80,3); mismatch=true; mismatch_pa=WL_EPCFG+0x80;
    assert(wl5_step(21)==11 && !doorbells()); wl5_abort(21);
    init(); setreg(WL_EPCFG+0x70,0x7005); assert(wl_download_aspm());
    assert(!doorbells() && !ne); /* malformed/cyclic capability chain */
    puts("M6k probe selection, holes/fault outcomes, pattern, CIVAC, NC and ASPM tests passed");
    return 0;
}
'''


def main():
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    patch = (ROOT / 'xnu/patches/0099-fuxi-wlan.patch').read_text()
    added = patch.split('+++ b/bsd/dev/fuxi_wlan.c\n', 1)[1].split('\n--- a/', 1)[0]
    assert int(re.match(r'@@ -0,0 \+1,(\d+) @@', added)[1]) == len(driver.splitlines())
    assert ''.join(l[1:] + '\n' for l in added.splitlines() if l.startswith('+')) == driver
    block = '/* ---- milestone 5:' + driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    # The loop/line calculation remains production C; only privileged asm is mocked.
    block = block.replace('__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));', 'ctr = 4ull << 16;')
    block = block.replace('__asm__ volatile("dc civac, %0" :: "r"(va) : "memory");', 'test_civac(va);')
    block = block.replace('__asm__ volatile("dsb sy" ::: "memory");', "record('B',0,0);")
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* host barrier */;', block)
    # Recorder needs the production DMA type/globals; insert just before its first caller.
    block = block.replace('static void\nwl_dma_civac(', RECORD_CACHE + '\nstatic void\nwl_dma_civac(')
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6k-') as tmp:
        source, binary = Path(tmp) / 'm6k.c', Path(tmp) / 'm6k'
        preamble = m6.PREAMBLE.replace('static unsigned diag_repeat_kind;', '''
static unsigned diag_repeat_kind, late_after;
static uint64_t late_cb, late_iova, late_syndrome;
''').replace('sleeps+=ms;', '''sleeps+=ms;
    if(ms==25 && late_after && --late_after==0) {
        setreg(late_cb+0x58,0x402); setreg(late_cb+0x60,late_iova);
        setreg(late_cb+0x64,late_iova>>32); setreg(late_syndrome,0x1401);
    }
''')
        source.write_text(preamble + block + m6.TESTS.split('int main(void) {', 1)[0] + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
