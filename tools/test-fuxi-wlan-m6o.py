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
/* A trained link taken down without core/PHY reset froze on DBI read.
 * Only actual modeled reset completion and PHY readiness permit local DBI
 * before retraining; remote apertures require L0. This check is independent
 * of production wl_link_off/wl_dbi_ready, so relaxing the guard is detected. */
static bool link_was_up, core_asserted, core_reset, phy_asserted, phy_reset, phy_ready;
static bool phy_stuck, dll_stuck, train_read_fault;
static unsigned training_reads;
static unsigned reinit_writes;
static unsigned fail_reinit, drop_reinit;
static bool train_started;

static uint32_t regs_peek(uint64_t pa) {
    for (unsigned i=0;i<nr;i++) if(regs[i].pa==pa) return regs[i].v;
    return 0;
}
static void pcie_dead_check(uint64_t pa) {
    bool perst_low=!(regs_peek(TS_TLMM+0x1000*94+4)&2), ltssm_off=!(regs_peek(WL_PARF+0x1b0)&0x100);
    bool l0=(regs_peek(WL_PARF+0x1b0)&0x3f)==0x11;
    bool local_ready=core_reset && phy_reset && phy_ready && !train_started && pa<WL_DBI+0xf20;
    if(link_was_up && !l0 && !local_ready && pa>=0x60000000ull && pa<0x64000000ull) {fprintf(stderr,"DEAD pa %#llx perst_low %d ltssm_off %d after: %s\n",(unsigned long long)pa,perst_low,ltssm_off,logbuf+(loglen>300?loglen-300:0)); abort();}
}
static void pcie_model_read(uint64_t pa) {
    if(pa!=WL_PARF+0x1b0 || !train_started || !cycle_peer) return;
    if(train_read_fault && training_reads==2) { fw_faults++; return; }
    unsigned state=training_reads<2?5:training_reads==2?0x20:0x11;
    training_reads++;
    setreg(pa,0x100|state);
    if(state==0x11 && !dll_stuck) setreg(WL_DBI+0x80,1u<<29);
}
static const char *last_log_line(void) {
    size_t start=loglen;
    if(start && logbuf[start-1]=='\n') start--;
    while(start && logbuf[start-1]!='\n') start--;
    return logbuf+start;
}
static unsigned test_rh_send(unsigned t, unsigned addr, unsigned data) {
    assert(t==0 && nrail<64);
    char expected[128];
    if(addr&4) snprintf(expected,sizeof(expected),"FUXI WLAN: M6o begin rail enable addr %#x value %u\n",addr,data);
    else snprintf(expected,sizeof(expected),"FUXI WLAN: M6o begin rail voltage addr %#x mV %u\n",addr,data);
    assert(!strcmp(last_log_line(),expected));
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
    if(pa==TS_TLMM+0x1000*80+4) {
        /* The breadcrumb must already exist when the GPIO write is attempted. */
        if(v&2) assert(strstr(last_log_line(),"begin cycle WLAN_EN high"));
        else assert(strstr(last_log_line(),"begin cycle WLAN_EN low") ||
                    strstr(last_log_line(),"cycle undo begin WLAN_EN low"));
    }
    pcie_dead_check(pa);
    if(pa==0x16b000) {
        if(v&1) { core_asserted=true; core_reset=false; phy_ready=false; }
        else if(core_asserted) {
            core_reset=true;
            /* Reset really destroys RC setup; immutable caps survive. */
            const unsigned off[]={0x18,0x20,0x24,0x28,0x2c,0x1004,0x1204};
            for(unsigned i=0;i<sizeof(off)/sizeof(*off);i++) setreg(WL_DBI+off[i],0);
        }
    }
    if(pa==0x16c01c) {
        if(v&1) { phy_asserted=true; phy_reset=false; phy_ready=false; }
        else if(phy_asserted) phy_reset=true;
    }
    if(pa==WL_PHY+0x244 && (v&3)==3) {
        assert(core_reset && phy_reset && !(regs_peek(WL_PHY+0x200)&1));
        phy_ready=!phy_stuck; setreg(WL_PHY+0x214,phy_stuck?0x40:0);
    }
    if(pa==TS_TLMM+0x1000*94+4 && (v&2)) {
        assert(core_reset && phy_reset && phy_ready);
        assert(!(regs_peek(WL_PARF+0x1b0)&0x100));
    }
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
    if(pa==WL_PARF+0x1b0 && (v&0x100)) {
        assert(core_reset && phy_reset && phy_ready);
        train_started=true; link_was_up=true;
    }
    /* A dead peer never retrains: no L0 and no DLLLA after LTSSM resume. */
    if(pa==WL_PARF+0x1b0) {
        setreg(WL_DBI+0x80,0); setreg(WL_PARF+0x1b0,(v&~0x3fu)|5);
        if(!(v&0x100)) train_started=false;
    }
    if(cycle_peer) {
        if(pa==WL_DBI+0xf24 && v==0x10 && l23_peer) setreg(WL_PARF+0x24,0x20);
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
    if(wl_link_refused(pa)) { fw_faults++; return; }
    if(fw_faults) return;
    if((pa>=WL_PHY && pa<WL_PHY+0x2000) || pa==0x16b000 || pa==0x16c01c) {
        if(++reinit_writes==fail_reinit) { fw_faults++; return; }
        if(reinit_writes==drop_reinit) v^=1;
    }
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
    cycle_peer=l23_peer=reset_peer=true; reset_stuck=phy_stuck=dll_stuck=train_read_fault=false;
    core_asserted=core_reset=phy_asserted=phy_reset=phy_ready=train_started=false;
    reinit_writes=fail_reinit=drop_reinit=training_reads=0;
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
    /* Steps 10-11 left PERST high and the link trained in L0. */
    setreg(TS_TLMM+0x1000*94+4,2); setreg(WL_PARF+0x1b0,0x111); link_was_up=true;
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
        assert(!frees); /* failed containment quarantines until access recovers */
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
    setup("cyclerails,cycle"); assert(!wl_variant_invalid && wl_seq_flags==(WL_O_CYCLE|WL_O_CYCLERAILS));
    const char *bad[]={"",",","parf,","parf,,dbi","off,msi","PARF","parf,parf","cyclerails,cyclerails","cycle,cycle,cyclerails","wat", "parfxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        setup(bad[i]); assert(wl_variant_invalid && wl5_step(17)==0x6c && !allocations && !ne);
    }
    setup("prefix"); bhi_arg="full"; wl_probe_configure(); assert(wl_variant_invalid);
    setup("prefix"); vec_arg="early"; wl_probe_configure(); assert(wl_variant_invalid);
    setup("msi"); probe_arg="unmapped"; wl_probe_configure(); assert(wl_variant_invalid);
    setup("por"); probe_arg="pattern"; wl_probe_configure(); assert(wl_variant_invalid);

    /* All 511 token combinations, including cyclerails implying cycle, use actual steps. */
    for(unsigned tokens=1;tokens<512;tokens++) {
        unsigned flags=tokens|((tokens&WL_O_CYCLERAILS)?WL_O_CYCLE:0);
        char arg[96]={0};
        for(unsigned i=0;i<9;i++) if(tokens&(1u<<i)) {
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
            assert(nrail==((flags&WL_O_CYCLERAILS)?18u:0u) && wl_cycle_attempted);
            /* Only cyclerails sends six off + twelve on commands. */
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
        assert(nrail==((flags&WL_O_CYCLERAILS)?24u:0u));
        if(flags&WL_O_MSI) assert(!(wl_prd(WL_EPCFG+0x50)&0x10000) && wl_prd(WL_EPCFG+0x54)==0x12340000 && wl_prd(WL_EPCFG+0x5c)==0x20);
        assert(!wl_seq_undos);
        if(flags&WL_O_EQ) assert(eqvec[0]==0x23ab4501 && eqvec[1]==0x34cd6702 && eqfmdc[0]==0x95 && eqfmdc[1]==0x8a && wl_prd(WL_DBI+0x154)==0x11223344 && wl_prd(WL_DBI+0x188)==0x22334455 && !wl_prd(WL_DBI+0x8bc));
    }

    /* Plain cycle leaves all votes intact through success, abort and containment. */
    setup("cycle"); fail_rail=1; wl3_report[20]=0x1234;
    assert(!wl5_step(17) && !nrail && sleeps==54);
    assert(wl_cycle_attempted && !wl_m6o_rails(false) && !wl_m6o_rails(true) && !nrail);
    assert(strstr(logbuf,"cycle retains all six rails; WLAN_EN only"));
    assert(wl_m6o_cycle() && !nrail); cleanup(17); assert(!nrail);
    /* Exact regulator commands are reserved for the explicit cyclerails opt-in. */
    setup("cyclerails"); assert(wl_seq_flags==(WL_O_CYCLE|WL_O_CYCLERAILS));
    assert(!wl5_step(17) && nrail==18 && sleeps==54);
    const unsigned addr[]={0x40504,0x40604,0x40204,0x40404,0x44004,0x40a04,0x40a00,0x40a04,0x44000,0x44004,0x40400,0x40404,0x40200,0x40204,0x40600,0x40604,0x40500,0x40504};
    const unsigned data[]={0,0,0,0,0,0,1800,1,1000,1,980,1,950,1,1900,1,1350,1};
    for(unsigned i=0;i<18;i++) assert(rail_addr[i]==addr[i] && rail_data[i]==data[i]);
    assert(strstr(logbuf,"begin rail enable addr 0x40504 value 0") &&
           strstr(logbuf,"begin rail voltage addr 0x40a00 mV 1800"));
    int d3=firstwrite(WL_EPCFG+0x44,3,3), pme=firstwrite(WL_DBI+0xf24,~0u,0x10), off=firstwrite(TS_TLMM+0x1000*80+4,2,0);
    assert(d3>=0 && pme>d3 && off>pme);
    assert(wl_m6o_cycle() && nrail==18); cleanup(17);
    assert(nrail==24);
    for(unsigned i=0;i<6;i++) assert(rail_addr[18+i]==addr[i] && !rail_data[18+i]);
    for(unsigned nth=1;nth<=18;nth++) {
        setup("cyclerails"); fail_rail=nth; assert(wl5_step(17)==0x6f); fail_rail=0; cleanup(17);
    }
    /* A dead link still aborts before any rail command. */
    setup("cycle"); cycle_peer=false; assert(wl5_step(17)==0x6f && !nrail); cleanup(17);
    /* A missing L23_READY alone is logged and the power cycle continues, as downstream. */
    setup("cycle"); l23_peer=false; assert(!wl5_step(17) && !nrail && sleeps==154); cleanup(17); assert(!nrail);
    setup("cyclerails"); l23_peer=false; assert(!wl5_step(17) && nrail==18); cleanup(17);
    setup("cyclerails"); wl3_report[20]=0x1234; assert(wl5_step(17)==0x6f); cleanup(17);
    /* DBI opens only after reset + PHY ready; remote apertures wait for L0. */
    setup("cycle"); assert(!wl5_step(17));
    assert(strstr(logbuf,"cycle link down dwell 1 ms, no DBI access") &&
           strstr(logbuf,"begin wait 0x1c001b0 mask 0x3f expected 0x11 limit 1000 ms") &&
           !strstr(logbuf,"expected 0 limit 100 ms") && !strstr(logbuf,"PME_TURNOFF clear"));
    cleanup(17);
    /* No L0: abort, and the undo refuses DBI entries instead of touching a dead link. */
    setup("cycle,dbi"); cycle_peer=false; assert(wl5_step(17)==0x6f && !nrail);
    cleanup(17); assert(!nrail && strstr(logbuf,"refused while the PCIe link is down"));
    setup("cycle"); read_fault_pa=TS_TLMM+0x1000*80+4;
    assert(wl5_step(17)==0x6f && !nrail && !writes(read_fault_pa));
    assert(strstr(last_log_line(),"begin cycle WLAN_EN low"));
    read_fault_pa=0; cleanup(17); assert(!nrail);

    /* Real shared routines: every reset/table/start write is exercised and
       failures never reach the pre-training DBI exception or endpoint. */
    setup("cycle"); assert(!wl5_step(17));
    unsigned count_reinit=reinit_writes;
    assert(core_reset && phy_reset && phy_ready && !wl_dbi_ready && !wl_link_off);
    assert(clock_delays==104010); /* core 2ms, PHY 2.01ms, CFG0 settle 100ms */
    assert(strstr(logbuf,"LTSSM 0x111 seen 0x20020/0x1 PM_STTS 0x20 PHY_STATUS 0"));
    off=firstwrite(TS_TLMM+0x1000*80+4,2,0);
    int core=firstwrite(0x16b000,1,1), phyreset=firstwrite(0x16c01c,1,1);
    int start=firstwrite(WL_PHY+0x244,3,3), release=firstwrite(TS_TLMM+0x1000*94+4,2,2);
    int train=firstwrite(WL_PARF+0x1b0,0x100,0x100);
    assert(core>off && phyreset>core && start>phyreset && release>start && train>release);
    assert(wl_prd(WL_DBI+0x18)==0xff0100 && wl_prd(WL_DBI+0x1004)==0x80000000 &&
           wl_prd(WL_DBI+0x1204)==0x80000000 && wl_prd(WL_DBI+0x20)==0x60506040);
    for(unsigned i=0;i<sizeof(wl_phy_tbl)/sizeof(*wl_phy_tbl);i++)
        assert(firstwrite(WL_PHY+wl_phy_tbl[i][0],~0u,wl_phy_tbl[i][1])>=0);
    cleanup(17);
    for(unsigned nth=1;nth<=count_reinit;nth++) {
        setup("cycle"); fail_reinit=nth; assert(wl5_step(17)==0x6f);
        assert(wl_link_off && !wl_dbi_ready && !train_started && !nrail);
        assert(!writes(WL_BAR0+0x118)); fail_reinit=0; cleanup(17);
    }
    for(unsigned nth=3;nth<=6;nth++) {
        setup("cycle"); drop_reinit=nth; assert(wl5_step(17)==0x6f);
        assert(wl_link_off && !wl_dbi_ready && !train_started);
        drop_reinit=0; cleanup(17);
    }
    setup("cycle"); train_read_fault=true; assert(wl5_step(17)==0x6f && fw_faults);
    assert(wl_link_off && !wl_dbi_ready && strstr(logbuf,"M6q cycle link FAILED"));
    train_read_fault=false; cleanup(17);
    setup("cycle,dbi"); phy_stuck=true; assert(wl5_step(17)==0x6f);
    assert(wl_link_off && !wl_dbi_ready && !train_started && !nrail);
    assert(strstr(logbuf,"PHY init FAILED") && !strstr(logbuf,"local DBI allowed"));
    cleanup(17);
    setup("cycle,dbi"); cycle_peer=false; assert(wl5_step(17)==0x6f);
    assert(wl_link_off && !wl_dbi_ready);
    assert(strstr(logbuf,"link FAILED after 1000 ms LTSSM 0x105 seen 0x20/0 PM_STTS 0 PHY_STATUS 0"));
    cleanup(17);
    setup("cycle"); dll_stuck=true; assert(wl5_step(17)==0x6f);
    assert(wl_link_off && !wl_dbi_ready && strstr(logbuf,"M6q cycle link FAILED")); cleanup(17);
    /* The exception permits local DBI only. Never iATU, config or BAR. */
    setup("cycle"); wl_link_off=wl_dbi_ready=true;
    assert(!wl_link_refused(WL_DBI) && !wl_link_refused(WL_DBI+0xf1c));
    assert(wl_link_refused(WL_DBI+0xf20) && wl_link_refused(WL_DBI+0xffc) && wl_link_refused(WL_DBI+0x1000) && wl_link_refused(WL_EPCFG) && wl_link_refused(WL_BAR0));
    wl_dbi_ready=false; assert(wl_link_refused(WL_DBI));

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
    const char *paths[]={"cycle","cyclerails","msi","por","cycle,msi,por,prefix","cyclerails,msi,por,prefix","cycle,parf,dbi,eq,hostirq,msi,por,prefix"};
    for(unsigned path=0;path<sizeof(paths)/sizeof(*paths);path++) {
        setup(paths[path]); assert(!run(20));
        unsigned count=special_writes; cleanup(20);
        for(unsigned nth=1;nth<=count;nth++) {
            setup(paths[path]); fault_special=nth; unsigned failed=run(20);
            assert(failed && fw_faults && !writes(WL_BAR0+0x118)); cleanup(failed);
            if(!(wl_seq_flags&WL_O_CYCLERAILS)) assert(!nrail);
        }
    }
    for(unsigned nth=1;nth<=6;nth++) {
        setup("cycle,msi,por,prefix"); fail_nth_alloc=nth; unsigned failed=run(20);
        assert(failed && !writes(WL_BAR0+0x118)); fail_nth_alloc=0; cleanup(failed);
    }
    setup("msi"); setreg(WL_EPCFG+0x50,0x018a7005); setreg(WL_EPCFG+0x60,~0u);
    assert(!run(21) && !wl_prd(WL_EPCFG+0x60)); cleanup(22); assert(wl_prd(WL_EPCFG+0x60)==~0u);
    /* Device KIWI layout (observed cap 0x010a7005): 32-bit address, data at +8, mask at +0xc. */
    setup("msi"); setreg(WL_EPCFG+0x50,0x010a7005); setreg(WL_EPCFG+0x5c,~0u);
    assert(!run(21) && (wl_prd(WL_EPCFG+0x50)&0x710000)==0x510000);
    assert(!(wl_msi_sink.iova>>32) && wl_prd(WL_EPCFG+0x54)==(uint32_t)wl_msi_sink.iova);
    assert(!(wl_prd(WL_EPCFG+0x58)&0xffff) && !wl_prd(WL_EPCFG+0x5c) && !writes(WL_EPCFG+0x60));
    cleanup(22); assert(wl_prd(WL_EPCFG+0x5c)==~0u && wl_prd(WL_EPCFG+0x54)==0x12340000);
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
    puts("M6o 511 combinations, safe cycle/explicit rails, exact host writes, MSI sink/BME, RX/POR, prefix, faults and quarantine passed");
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
    for name, arg in [('wl3_step', 'unsigned s'), ('wl3_phy_init', 'unsigned us'), ('wl4_step', 'unsigned s')]:
        preamble = preamble.replace(f'static int {name}({arg}) {{ abort(); }}', f'static int {name}({arg});')
    shared = r'''
#define FUXI_WLAN_XNU 1
#define WL_GCC 0x100000u
#define WL_ATU1 0x60001200ull
#define WL_MEM_LO 0x60300000u
#define WL_MEM_HI 0x63ffffffu
#define WL_BAR0_END 0x605fffffu
static bool wl_m6o_parf(void);
static int wl_m6o_host(unsigned);
static void delay(unsigned us) { fw_delay(us); }
static uint64_t wl_now_us(void) { return clock_delays; }
static uint32_t usb_rd(uintptr_t base, unsigned off) { return wl_prd(base+off); }
static void wl_wr(uintptr_t base, unsigned off, uint32_t v) { wl_pwr(base+off,v); }
static void wl_set(uintptr_t base, unsigned off, uint32_t v) { wl_wr(base,off,usb_rd(base,off)|v); }
static void wl_clr(uintptr_t base, unsigned off, uint32_t v) { wl_wr(base,off,usb_rd(base,off)&~v); }
'''
    shared += 'static const uint16_t wl_phy_tbl' + shim.split('static const uint16_t wl_phy_tbl', 1)[1].split('/* PCI Express capability', 1)[0]
    shared += 'static int wl3_phy_init' + shim.split('static int wl3_phy_init', 1)[1].split('/* Returns 0 on success', 1)[0]
    shared += 'static int wl3_step(unsigned step) { uint32_t *r=wl3_report; switch(step) {\n'
    shared += ' case 8:' + shim.split(' case 8:', 1)[1].split(' case 9:', 1)[0]
    shared += ' case 12:' + shim.split(' case 12:', 1)[1].split(' case 13:', 1)[0]
    shared += 'default: abort(); }}\n'
    shared += 'static int wl4_step(unsigned step) { uint32_t *r=wl4_report; switch(step) {\n'
    shared += '\tcase 14:' + driver.split('\tcase 14:', 1)[1].split('\tcase 15:', 1)[0]
    shared += 'default: abort(); }}\n'

    preamble = preamble.replace('static unsigned fuxi_gpu_access(', 'static void model_write(uint64_t,uint32_t);\nstatic unsigned fuxi_gpu_access(')
    preamble = preamble.replace('if(wr) raw_pwr(pa,(uint32_t)*v);', 'if(wr) model_write(pa,(uint32_t)*v);')
    preamble = preamble.replace('static bool wl_link_refused(uint64_t pa);\n', 'static bool wl_link_refused(uint64_t pa);\nstatic void pcie_dead_check(uint64_t);\nstatic void pcie_model_read(uint64_t);\n', 1)
    preamble = preamble.replace('{fw_faults++; return;} if (!fw_faults) raw_pwr(pa,v); }', '{fw_faults++; return;} pcie_dead_check(pa); if (!fw_faults) raw_pwr(pa,v); }', 1)
    preamble = preamble.replace('    if(track_reads)', '    pcie_model_read(pa);\n    if(track_reads)', 1)
    preamble = preamble.replace('else *v=wl_prd(pa);', 'else {pcie_dead_check(pa); *v=wl_prd(pa);}')
    preamble = preamble.replace('    if (wl_link_refused(pa)) {fw_faults++; return 0;}\n', '    if (wl_link_refused(pa)) {fw_faults++; return 0;}\n    pcie_dead_check(pa);\n', 1)
    preamble = preamble.replace('static unsigned rh_send(unsigned t, unsigned addr, unsigned data) { return 0; }', 'static unsigned test_rh_send(unsigned,unsigned,unsigned);\nstatic unsigned rh_send(unsigned t, unsigned addr, unsigned data) { return test_rh_send(t,addr,data); }')
    # Failure injection keeps the production allocation/error handling intact.
    preamble = preamble.replace('static int kmem_alloc(', 'static unsigned fail_nth_alloc;\nstatic int kmem_alloc(')
    preamble = preamble.replace('if(fail_alloc) return 1;', 'if(fail_alloc || (fail_nth_alloc && allocations+1==fail_nth_alloc)) return 1;')
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6o-') as tmp:
        source, binary = Path(tmp) / 'm6o.c', Path(tmp) / 'm6o'
        source.write_text(preamble + PEER + shared + block + m6.TESTS.split('int main(void) {', 1)[0] + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
