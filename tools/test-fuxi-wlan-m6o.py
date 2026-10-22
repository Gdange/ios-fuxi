#!/usr/bin/env python3
"""Compile production M6o C with synthetic PCI, RPMh, VFS and DMA; host only."""
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

PEER = r'''
static unsigned rail_addr[64], rail_data[64], nrail, fail_rail;
static unsigned special_writes, fault_special, drop_special;
static uint64_t special_pa[128];
static uint32_t special_mask[128];
static bool cycle_peer=true, l23_peer=true, reset_peer=true, reset_stuck;
static uint32_t eqvec[2], eqfmdc[2];
static unsigned fail_nth_alloc;
static unsigned test_rh_send(unsigned t, unsigned addr, unsigned data) {
    assert(t==0 && nrail<64);
    rail_addr[nrail]=addr; rail_data[nrail++]=data;
    return fail_rail && nrail==fail_rail;
}
static bool special(uint64_t p) {
    return (p>=WL_EPCFG && p<WL_EPCFG+0x100) ||
        (p>=WL_PARF && p<WL_PARF+0x400 && p!=WL_PARF+0x234) ||
        (p>=WL_DBI && p<WL_DBI+0x1000 && p!=WL_DBI+4) ||
        (p>=WL_BAR0+0x284 && p<=WL_BAR0+0x29c) || p==WL_BAR0+0x120 ||
        p==TS_TLMM+0x1000*80+4 || p==TS_TLMM+0x1000*94+4;
}
static void model_write(uint64_t pa, uint32_t v) {
    uint32_t requested=v;
    if(pa==WL_DBI+0x154 || pa==WL_DBI+0x188) assert(wl_prd(WL_DBI+0x8bc)&1);
    raw_pwr(pa,v);
    v=wl_prd(pa);
    if(pa==WL_DBI+0x8a8 || pa==WL_DBI+0x8ac || pa==WL_DBI+0x890) {
        unsigned bank=(wl_prd(WL_DBI+0x890)>>24)&3; assert(bank<2);
        if(pa==WL_DBI+0x8a8) eqvec[bank]=v;
        if(pa==WL_DBI+0x8ac) eqfmdc[bank]=v;
        setreg(WL_DBI+0x8a8,eqvec[bank]); setreg(WL_DBI+0x8ac,eqfmdc[bank]);
    }
    if(cycle_peer) {
        if(pa==WL_DBI+0xf24 && v==0x10 && l23_peer) setreg(WL_PARF+0x24,0x20);
        if(pa==WL_PARF+0x1b0) setreg(WL_DBI+0x80,(v&0x100)?1u<<29:0);
        if(pa==TS_TLMM+0x1000*94+4 && !(v&2)) {
            /* A real probe cycle loses EP state: restore must be substantive. */
            setreg(WL_EPCFG+0x10,4); setreg(WL_EPCFG+0x14,0);
            setreg(WL_EPCFG+0x78,0); setreg(WL_EPCFG+0x80,0);
        }
    }
    if(pa==WL_BAR0+0x38 && requested==2) {
        if(reset_peer) setreg(WL_BAR0+0x48,0);
        if(reset_stuck) setreg(WL_BAR0+0x38,2);
    }
}
static void wl_pwr(uint64_t pa, uint32_t v) {
    if(fw_faults) return;
    if(special(pa)) {
        special_writes++;
        if(fault_special==special_writes) { fw_faults++; return; }
        if(drop_special==special_writes) v^=special_mask[special_writes-1];
        if(special_writes<=128) special_pa[special_writes-1]=pa;
    }
    model_write(pa,v);
}

'''
TESTS = r'''
static unsigned writes(uint64_t pa) {
    unsigned n=0; for(unsigned i=0;i<ne;i++) n+=events[i].kind=='W' && events[i].pa==pa; return n;
}
static int firstwrite(uint64_t pa, uint32_t mask, uint32_t value) {
    for(unsigned i=0;i<ne;i++) if(events[i].kind=='W' && events[i].pa==pa && (events[i].v&mask)==value) return i;
    return -1;
}
static void setup(const char *arg) {
    init(); seq_arg=arg; wl_probe_configure();
    nrail=fail_rail=special_writes=fault_special=drop_special=fail_nth_alloc=0;
    cycle_peer=l23_peer=reset_peer=true; reset_stuck=false;
    memset(special_pa,0,sizeof(special_pa));
    eqvec[0]=0x23ab4501; eqvec[1]=0x34cd6702;
    eqfmdc[0]=0x95; eqfmdc[1]=0x8a;
    setreg(WL_DBI+0x7c,4); setreg(WL_DBI+0x8a8,eqvec[0]); setreg(WL_DBI+0x8ac,eqfmdc[0]);
    setreg(WL_DBI+0x890,1); setreg(WL_DBI+0x154,0x11223344); setreg(WL_DBI+0x188,0x22334455);
    setreg(WL_EPCFG+0x34,0x40); setreg(WL_EPCFG+0x40,0x5001); /* PM -> MSI */
    setreg(WL_EPCFG+0x44,0); setreg(WL_EPCFG+0x50,0x008a7005); /* MMC=5,64bit -> PCIe */
    setreg(WL_EPCFG+0x54,0x12340000); setreg(WL_EPCFG+0x58,0);
    setreg(WL_EPCFG+0x5c,0x20); setreg(WL_EPCFG+0x70,0x00020010);
    setreg(WL_EPCFG+0x78,0x1234); setreg(WL_EPCFG+0x80,0x4567);
    setreg(TS_TLMM+0x1000*80+4,2);
    const unsigned rails[]={0x40a00,0x40200,0x40400,0x40500,0x44000,0x40600};
    for(unsigned i=0;i<6;i++) wl3_report[20+i]=rails[i];
}
static unsigned run(unsigned last) {
    for(unsigned s=17;s<=last;s++) if(wl5_step(s)) return s;
    return 0;
}
static void cleanup(unsigned step) {
    fault_special=drop_special=0; wl5_abort(step);
    if(wl_cycle_powered && fw_faults && frees!=allocations) {
        assert(!frees); /* failed rail undo quarantines until access recovers */
        fw_faults=0; wl5_abort(step);
    }
    if(frees!=allocations) fprintf(stderr,"cleanup flags=%#x step=%u special=%u frees=%u alloc=%u\n%s",wl_seq_flags,step,special_writes,frees,allocations,logbuf);
    assert(frees==allocations && !wl_image.va && !wl_msi_sink.va);
    assert(!(wl_prd(WL_EPCFG+4)&4) && !(wl_prd(TS_TLMM+0x1000*94+4)&2));
}
int main(void) {
    setup(NULL); assert(!wl_seq_flags && !wl_seq_requested);
    ne=0; assert(!wl_m6o_host(8) && !wl_m6o_host(10) && !wl_m6o_host(11) && !wl_m6o_host(12) && !ne);
    setup("off"); assert(!wl_seq_flags && !wl_variant_invalid);
    const char *bad[]={"",",","parf,","parf,,dbi","off,msi","PARF","parf,parf","wat", "parfxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        setup(bad[i]); assert(wl_variant_invalid && wl5_step(17)==0x6c && !allocations && !ne);
    }
    setup("prefix"); bhi_arg="full"; wl_probe_configure(); assert(wl_variant_invalid);
    setup("prefix"); vec_arg="early"; wl_probe_configure(); assert(wl_variant_invalid);
    setup("msi"); probe_arg="unmapped"; wl_probe_configure(); assert(wl_variant_invalid);
    setup("por"); probe_arg="pattern"; wl_probe_configure(); assert(wl_variant_invalid);

    /* Every selector alone AND all 255 nonempty combinations use actual steps. */
    for(unsigned flags=1;flags<256;flags++) {
        char arg[96]={0};
        for(unsigned i=0;i<8;i++) if(flags&(1u<<i)) {
            if(*arg) strcat(arg,","); strcat(arg,wl_seq_names[i]);
        }
        setup(arg); assert(wl_seq_flags==flags && !wl_variant_invalid);
        assert(!wl_m6o_host(8) && !wl_m6o_host(10) && !wl_m6o_host(11) && !wl_m6o_host(12));
        if(flags&WL_O_PARF) assert(wl_prd(WL_PARF)==0x365e && !wl_prd(WL_PARF+0x350) && wl_prd(WL_PARF+0x358)==0x04000000);
        if(flags&WL_O_DBI) assert((wl_prd(WL_PARF+0x2c4)&0x101)==0x100 && (wl_prd(WL_DBI+0x714)&0x20) && (wl_prd(WL_DBI+0x78)&0x7000)==0x5000 && (wl_prd(WL_DBI+0x70c)&0x8000) && wl_prd(WL_DBI+0xb40)==20);
        if(flags&WL_O_EQ) assert((wl_prd(WL_DBI+0x80c)&0x1f00)==0x100 && (wl_prd(WL_DBI+0x890)&0x03000001)==0x01000000 && !(eqvec[0]&0x00ffff00) && !(eqvec[1]&0x00ffff00) && !(eqfmdc[0]&31) && !(eqfmdc[1]&31) && wl_prd(WL_DBI+0x154)==0x55555555 && wl_prd(WL_DBI+0x188)==0x55555555 && !(wl_prd(WL_DBI+0x8bc)&1));
        if(flags&WL_O_HOSTIRQ) assert(wl_prd(WL_PARF+0x22c)==0x3fc0d202);
        assert(!wl5_step(17));
        if(flags&WL_O_CYCLE) {
            assert(nrail==18 && wl_cycle_attempted);
            /* wl_vrm emits voltage then enable: six off + twelve on commands. */
            assert(wl_prd(WL_EPCFG+0x10)==(WL_BAR0|4));
            assert(wl_prd(WL_EPCFG+0x78)==0x1234 && wl_prd(WL_EPCFG+0x80)==0x4567);
        }
        assert(!wl5_step(18));
        if(flags&(WL_O_CYCLE|WL_O_MSI)) {
            assert(wl_enabled && (wl_prd(WL_EPCFG+4)&4));
            assert(!wl_contexts.va && !wl_image.va && !wl_vectors.va);
        } else assert(!wl_enabled && !(wl_prd(WL_EPCFG+4)&4));
        if(flags&WL_O_MSI) {
            assert((wl_prd(WL_EPCFG+0x50)&0x710000)==0x510000);
            assert((walk(wl_msi_sink.iova)&0xfffffffff000ull)==wl_msi_sink.pa);
            /* Simulate all vector message data values landing in the owned page. */
            for(unsigned data=0;data<32;data++) {
                uintptr_t va=wl_owned_alias(walk(wl_msi_sink.iova)&0xfffffffff000ull,4,false);
                assert(va==wl_msi_sink.va); *(uint32_t *)va=data;
            }
        }
        assert(!wl5_step(19));
        assert(wl_image.size==((flags&WL_O_PREFIX)?WL_CHUNK:round_page(sizeof(fixture))));
        assert(!wl5_step(20));
        assert(!!wl_vectors.va==!(flags&WL_O_PREFIX));
        assert(!memcmp((void *)wl_image.va,fixture,WL_CHUNK));
        if(flags&WL_O_POR) {
            assert(wl_rddm.va && wl_rxvectors.va);
            int hi=firstwrite(WL_BAR0+wl_bhie+0x64,~0u,0);
            /* First writes are complete RX clear. Find subsequent high->low->size->DB. */
            unsigned seen=0;
            for(unsigned i=0;i<ne;i++) if(events[i].kind=='W' && events[i].pa>=WL_BAR0+wl_bhie+0x60 && events[i].pa<=WL_BAR0+wl_bhie+0x78) {
                if(seen<7) assert(events[i].pa==WL_BAR0+wl_bhie+0x60+4*seen && !events[i].v);
                else {
                    const unsigned offsets[]={0x64,0x60,0x68,0x70};
                    assert(seen<11 && events[i].pa==WL_BAR0+wl_bhie+offsets[seen-7]);
                }
                seen++;
            }
            assert(seen==11 && hi>=0 && wl_mrd(wl_bhie+0x70)==random_value);
            assert(writes(WL_BAR0+wl_bhi+0x20)==1 && !writes(WL_BAR0+0x38));
        }
        { unsigned e=wl5_step(21); if(e) fprintf(stderr,"flags=%#x step21=%#x\n%s",flags,e,logbuf); assert(!e); }
        assert(writes(WL_BAR0+wl_bhi+0x18)==1);
        if(flags&WL_O_POR) assert(writes(WL_BAR0+wl_bhie+0x70)==2); /* clear then random, retained */
        assert(wl5_step(22)==0x6c && strstr(logbuf,"M6o result"));
        assert(wl5_step(21)==0x6b && writes(WL_BAR0+wl_bhie+0x3c)==1);
        cleanup(22);
        if(flags&WL_O_MSI) assert(!(wl_prd(WL_EPCFG+0x50)&0x10000) && wl_prd(WL_EPCFG+0x54)==0x12340000 && wl_prd(WL_EPCFG+0x5c)==0x20);
        assert(!wl_seq_undos);
        if(flags&WL_O_EQ) assert(eqvec[0]==0x23ab4501 && eqvec[1]==0x34cd6702 && eqfmdc[0]==0x95 && eqfmdc[1]==0x8a && wl_prd(WL_DBI+0x154)==0x11223344 && wl_prd(WL_DBI+0x188)==0x22334455 && !wl_prd(WL_DBI+0x8bc));
    }

    /* Cycle order and exact regulator commands, both before and during resume. */
    setup("cycle"); assert(!wl5_step(17));
    assert(nrail==18);
    const unsigned addr[]={0x40504,0x40604,0x40204,0x40404,0x44004,0x40a04,0x40a00,0x40a04,0x44000,0x44004,0x40400,0x40404,0x40200,0x40204,0x40600,0x40604,0x40500,0x40504};
    const unsigned data[]={0,0,0,0,0,0,1800,1,1000,1,980,1,950,1,1900,1,1350,1};
    for(unsigned i=0;i<18;i++) assert(rail_addr[i]==addr[i] && rail_data[i]==data[i]);
    int d3=firstwrite(WL_EPCFG+0x44,3,3), pme=firstwrite(WL_DBI+0xf24,~0u,0x10), off=firstwrite(TS_TLMM+0x1000*80+4,2,0);
    assert(d3>=0 && pme>d3 && off>pme);
    assert(wl_m6o_cycle() && nrail==18); cleanup(17);
    for(unsigned nth=1;nth<=18;nth++) {
        setup("cycle"); fail_rail=nth; assert(wl5_step(17)==0x6f); fail_rail=0; cleanup(17);
    }
    /* A dead link still aborts before any rail command. */
    setup("cycle"); cycle_peer=false; assert(wl5_step(17)==0x6f && !nrail); cleanup(17);
    /* A missing L23_READY alone is logged and the power cycle continues, as downstream. */
    setup("cycle"); l23_peer=false; assert(!wl5_step(17) && nrail==18); cleanup(17);
    setup("cycle"); wl3_report[20]=0x1234; assert(wl5_step(17)==0x6f); cleanup(17);

    /* Every new host write rejects a mismatch/fault and rolls back partial state. */
    const char *groups[]={"parf","dbi","eq","hostirq"};
    for(unsigned group=0;group<4;group++) {
        setup(groups[group]); assert(!wl_m6o_host(8) && !wl_m6o_host(10) && !wl_m6o_host(11) && !wl_m6o_host(12));
        unsigned count=wl_seq_undos;
        uint32_t masks[48]; for(unsigned i=0;i<count;i++) masks[i]=wl_seq_undo[i].mask;
        cleanup(11);
        for(unsigned nth=1;nth<=count;nth++) for(unsigned kind=0;kind<2;kind++) {
            setup(groups[group]);
            for(unsigned i=0;i<count;i++) special_mask[i]=masks[i]&-masks[i];
            if(kind) fault_special=nth; else drop_special=nth;
            assert(wl_m6o_host(8) || wl_m6o_host(10) || wl_m6o_host(11) || wl_m6o_host(12));
            cleanup(11);
        }
    }
    /* MSI capability malformed/absent/short, wrong count/width and read fault. */
    for(unsigned kind=0;kind<6;kind++) {
        setup("msi"); assert(!wl5_step(17));
        if(kind==0) setreg(WL_EPCFG+0x50,0x7005);
        if(kind==1) setreg(WL_EPCFG+0x50,0x00887005);
        if(kind==2) setreg(WL_EPCFG+0x40,0x4101);
        if(kind==3) setreg(WL_EPCFG+0x40,0x4001);
        if(kind==4) setreg(WL_EPCFG+0x40,1);
        if(kind==5) read_fault_pa=WL_EPCFG+0x50;
        assert(wl5_step(18)==0x6f && !writes(WL_BAR0+0x118)); read_fault_pa=0; cleanup(18);
    }
    /* Actual cycle/MSI/RX write faults must stop before the BHI image doorbell. */
    const char *paths[]={"cycle","msi","por","cycle,msi,por,prefix"};
    for(unsigned path=0;path<4;path++) {
        setup(paths[path]); assert(!run(20));
        unsigned count=special_writes; cleanup(20);
        for(unsigned nth=1;nth<=count;nth++) {
            setup(paths[path]); fault_special=nth; unsigned failed=run(20);
            assert(failed && fw_faults && !writes(WL_BAR0+0x118)); cleanup(failed);
        }
    }
    for(unsigned nth=1;nth<=6;nth++) {
        setup("cycle,msi,por,prefix"); fail_nth_alloc=nth; unsigned failed=run(20);
        assert(failed && !writes(WL_BAR0+0x118)); fail_nth_alloc=0; cleanup(failed);
    }
    setup("msi"); setreg(WL_EPCFG+0x50,0x018a7005); setreg(WL_EPCFG+0x60,~0u);
    assert(!run(21) && !wl_prd(WL_EPCFG+0x60)); cleanup(22); assert(wl_prd(WL_EPCFG+0x60)==~0u);
    setup("prefix"); file_size=WL_FW_MAX; assert(!run(21) && wl_image.size==WL_CHUNK && !wl_vectors.va); cleanup(22);
    setup("prefix"); read_resid=1; assert(run(19)==19); cleanup(19);
    setup("por"); random_value=0; assert(!run(20) && wl_mrd(wl_bhie+0x70)==1); cleanup(20);
    setup("por"); assert(!run(19)); setreg(WL_BAR0+0x48,0xff00); assert(!wl5_step(20));
    assert(writes(WL_BAR0+0x38)==1 && writes(WL_BAR0+0x120)==2); cleanup(20);
    setup("por"); assert(!run(19)); reset_peer=false; setreg(WL_BAR0+0x48,0xff00);
    assert(wl5_step(20) && writes(WL_BAR0+0x38)==1); reset_peer=true; cleanup(20);
    setup("por"); assert(!run(19)); reset_stuck=true; setreg(WL_BAR0+0x48,0xff00);
    assert(wl5_step(20) && sleeps==1000 && !writes(WL_BAR0+0x118)); reset_stuck=false; cleanup(20);
    setup("msi"); assert(!run(20)); setreg(WL_EPCFG+0x50,wl_prd(WL_EPCFG+0x50)&~0x10000);
    assert(wl5_step(21)==0x6f && !writes(WL_BAR0+0x118)); cleanup(21);
    setup("msi,prefix,por"); assert(!run(21));
    fail_bm=fail_perst=true; wl5_abort(22); assert(!frees && wl_msi_sink.va && wl_image.va);
    fail_bm=fail_perst=false; cleanup(22);
    setup("parf"); assert(!wl_m6o_host(8) && !run(21));
    corrupt_write_pa=WL_PARF+0x358; wl5_abort(22); assert(!frees && wl_seq_undos);
    corrupt_write_pa=0; cleanup(22);
    puts("M6o 255 combinations, cycle/rails, exact host writes, MSI sink/BME, RX/POR, prefix, faults and quarantine passed");
    return 0;
}
'''


def main():
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    patch = (ROOT / 'xnu/patches/0099-fuxi-wlan.patch').read_text()
    for dest, source in [('fuxi_wlan.c', driver), ('wlan.c', (ROOT / 'shim/fuxi-probe/wlan.c').read_text())]:
        added = patch.split(f'+++ b/bsd/dev/{dest}\n', 1)[1].split('\n--- a/', 1)[0]
        assert int(re.match(r'@@ -0,0 \+1,(\d+) @@', added)[1]) == len(source.splitlines())
        assert ''.join(l[1:] + '\n' for l in added.splitlines() if l.startswith('+')) == source
    assert 'python3 tools/test-fuxi-wlan-m6o.py' in (ROOT / 'tools/ci/build.sh').read_text()
    shim = (ROOT / 'shim/fuxi-probe/wlan.c').read_text()
    assert 'if (!wl_m6o_parf())' in shim
    for step in (8, 10, 11, 12):
        assert f'wl_m6o_host({step})' in shim
    assert driver.index('wl_variant_invalid && wl_seq_requested') < driver.index('if (!e) { e = wl_icc_step(s); }')
    block = '/* ---- milestone 5:' + driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* mocked privileged asm */;', block)
    preamble = m6.PREAMBLE.replace('static void wl_pwr(', 'static void raw_wl_pwr(')
    preamble = preamble.replace('static unsigned fuxi_gpu_access(', 'static void model_write(uint64_t,uint32_t);\nstatic unsigned fuxi_gpu_access(')
    preamble = preamble.replace('if(wr) raw_pwr(pa,(uint32_t)*v);', 'if(wr) model_write(pa,(uint32_t)*v);')
    preamble = preamble.replace('static unsigned rh_send(unsigned t, unsigned addr, unsigned data) { return 0; }', 'static unsigned test_rh_send(unsigned,unsigned,unsigned);\nstatic unsigned rh_send(unsigned t, unsigned addr, unsigned data) { return test_rh_send(t,addr,data); }')
    # Failure injection keeps the production allocation/error handling intact.
    preamble = preamble.replace('static int kmem_alloc(', 'static unsigned fail_nth_alloc;\nstatic int kmem_alloc(')
    preamble = preamble.replace('if(fail_alloc) return 1;', 'if(fail_alloc || (fail_nth_alloc && allocations+1==fail_nth_alloc)) return 1;')
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6o-') as tmp:
        source, binary = Path(tmp) / 'm6o.c', Path(tmp) / 'm6o'
        source.write_text(preamble + PEER + block + m6.TESTS.split('int main(void) {', 1)[0] + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
