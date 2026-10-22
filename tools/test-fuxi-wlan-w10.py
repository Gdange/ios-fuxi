#!/usr/bin/env python3
"""Host-only W10: compile production C using the M6o PCI/DMA/VFS peer.

Synthetic bytes only; no firmware, device, network, kernel build or CI use.
"""
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('m6o', ROOT / 'tools/test-fuxi-wlan-m6o.py')
m6o = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m6o)

TESTS = r'''
/* Timed peer changes happen during the production polling sleep. */
static unsigned tick_after, tick_kind;
static void w10_tick(unsigned ms) {
    if(ms!=25 || !tick_after || --tick_after) return;
    if(tick_kind==1) setreg(WL_BAR0+0x268,(2u<<30)|wl_bhie_seq);
    if(tick_kind==2) {setreg(WL_BAR0+0x128,2); setreg(WL_BAR0+0x48,0x200);}
    if(tick_kind==3) setreg(WL_BAR0+0x128,3);
}
static void start(const char *flags, const char *probe) {
    setup(flags); stop22_arg=keep_arg=0; probe_arg=probe; wl_probe_configure();
    tx_behavior=0; tick_after=tick_kind=0;
    file_size=WL_CHUNK+37;
    assert(!wl_stop22 && !wl_keep && !wl_variant_invalid);
}
static void until(unsigned first, unsigned last) {
    for(unsigned s=first;s<=last;s++) {
        int e=wl5_step(s);
        if(e) fprintf(stderr,"step %u reason %#x\n%s",s,e,logbuf);
        assert(!e);
    }
}
static unsigned tx_doorbells(void) {
    unsigned n=0;
    for(unsigned i=0;i<ne;i++) n+=events[i].kind=='W' &&
        events[i].pa==WL_BAR0+wl_bhie+0x3c && (events[i].v&WL_SEQ_MASK);
    return n;
}
static void no_channels(void) {
    for(unsigned i=0;i<ne;i++) if(events[i].kind=='W') {
        assert(events[i].pa<WL_BAR0+0x300 || events[i].pa>=WL_BAR0+0x700);
        assert(events[i].pa!=WL_BAR0+0x70 && events[i].pa!=WL_BAR0+0x74);
    }
    if(wl_contexts.va) {
        for(unsigned i=0;i<128*sizeof(struct wl_context);i++)
            assert(((uint8_t *)wl_contexts.va)[i]==0);
        struct wl_context *c=(void *)(wl_contexts.va+WL_CCTX);
        assert(c->wp==c->rp); /* no START_CHANNEL command */
    }
}
static void contained(unsigned step) {
    no_channels(); wl_finish(step);
    assert(frees==allocations && !wl_amss.va && !wl_image.va);
    assert(!(regs_peek(WL_EPCFG+4)&4));
    assert(!(regs_peek(TS_TLMM+0x1000*94+4)&2));
}
int main(void) {
    /* W9 observed result, including prefix, 32-bit MSI and stale TXSTATUS.
     * Full synthetic file is read once; vectors cover all bytes, not prefix. */
    start("cycle,parf,dbi,eq,hostirq,msi,por,prefix","civac");
    setreg(WL_DBI+0x7c,3); setreg(WL_EPCFG+0x50,0x010a7005);
    setreg(WL_EPCFG+0x5c,~0u); file_size=6090816; noncontig=true;
    random_value=0x0c170601;
    until(17,19); setreg(WL_BAR0+0x120,~0u); until(20,21);
    assert(wl_image.size==WL_CHUNK && wl_amss.size>=file_size);
    assert(vfs_reads==1 && vfs_bytes==file_size);
    assert(!memcmp((void *)wl_image.va,(void *)wl_amss.va,WL_CHUNK));
    struct wl_vector *v=(void *)wl_vectors.va;
    assert(wl5_report[19]==12*16 && v[11].size==file_size-11*WL_CHUNK);
    for(unsigned i=0;i<12;i++) assert(v[i].addr==wl_amss.iova+i*WL_CHUNK);
    for(unsigned off=0;off<wl_amss.size;off+=4096)
        assert((walk(wl_amss.iova+off)&0xfffffffff000ull)==wl_phys(wl_amss.va+off));
    assert(regs_peek(WL_BAR0+0x12c)==0x8c170601);
    until(22,28);
    assert(wl_sbl_ready && wl_bhie_fired && wl_amss_reached && tx_doorbells()==1);
    assert(wl_seq==0x0c170601 && wl_bhie_seq!=wl_seq);
    assert(strstr(logbuf,"W10 SBL READY success BHI STATUS 0x8c170601"));
    assert(strstr(logbuf,"W10 AMSS EE reached; M0 confirmed"));
    assert(!wl5_step(27)); /* owned completion remains valid */
    assert(wl5_step(26)==1 && tx_doorbells()==1); /* never retry */
    contained(28);
    assert(!(regs_peek(WL_EPCFG+0x50)&0x10000));
    assert(regs_peek(WL_EPCFG+0x54)==0x12340000 && !wl_seq_undos);

    /* Default non-prefix and NC modes can also continue. Keep retains only
     * after final success, and every explicit partial limit still contains. */
    const char *probes[]={NULL,"nc"};
    for(unsigned i=0;i<2;i++) {
        start(NULL,probes[i]); keep_arg=1; wl_probe_configure();
        until(17,28); assert(!wl_amss.va && wl_keep);
        unsigned before=ne; wl_finish(28);
        assert(ne==before && !frees && wl_enabled && wl_image.va);
        no_channels(); cleanup(28);
    }
    start(NULL,NULL); keep_arg=2; wl_probe_configure(); assert(!wl_keep);
    until(17,28); contained(28);
    start("prefix",NULL); keep_arg=1; wl_probe_configure();
    until(17,22); contained(22); assert(!tx_doorbells());

    /* Diagnostic boot arg preserves the old zero-vector prefix experiment. */
    setup("msi,por,prefix"); assert(wl_stop22);
    until(17,21); assert(!wl_amss.va && !wl_vectors.va);
    assert(wl5_step(22)==0x6c && strstr(logbuf,"W10 diagnostic stop at 22"));
    assert(!tx_doorbells()); cleanup(22);
    start(NULL,NULL); stop22_arg=1; wl_probe_configure(); until(17,21);
    assert(wl5_step(22)==0x6b && !tx_doorbells()); cleanup(22);

    /* BHI success must have the owned seq AND SBL READY. EE alone, stale
     * success and a non-ready state each time out without publishing contexts. */
    for(unsigned kind=0;kind<4;kind++) {
        start(NULL,NULL); until(17,21);
        if(kind==0) setreg(WL_BAR0+0x12c,0);
        if(kind==1) setreg(WL_BAR0+0x12c,(2u<<30)|(wl_seq+1));
        if(kind==2) setreg(WL_BAR0+0x48,0x100);
        if(kind==3) setreg(WL_BAR0+0x128,0);
        assert(wl5_step(22)==2 && !wl_sbl_ready && !tx_doorbells());
        assert(strstr(logbuf,"timeout")); cleanup(22);
    }
    start(NULL,NULL); until(17,21); diag_repeat_kind=2;
    assert(wl5_step(22)==1 && !wl_sbl_ready); cleanup(22);

    /* MHI error EEs and SYS_ERR at SBL and after BHIe are explicit errors. */
    for(unsigned after=0;after<2;after++) for(unsigned kind=0;kind<5;kind++) {
        start("msi,por,prefix",NULL); until(17,after?26:21);
        const unsigned ee[]={3,6,4,7,1};
        setreg(WL_BAR0+0x128,ee[kind]);
        if(kind==4) setreg(WL_BAR0+0x48,0xff04);
        assert(wl5_step(after?27:22)==1 && !wl_amss_reached);
        assert(strstr(logbuf,kind==0?"RDDM requested":kind==4?"SYS_ERR":"error EE") ||
            strstr(logbuf,"unsupported EE"));
        cleanup(after?27:22);
    }

    /* BHIe TXSTATUS mismatch never becomes success from EE=AMSS alone.
     * A matching later result is accepted without ringing another doorbell. */
    start("prefix",NULL); tx_behavior=2; until(17,26);
    unsigned before=sleeps;
    assert(wl5_step(27)==2 && sleeps-before==5000 && tx_doorbells()==1);
    assert(strstr(logbuf,"BHIe TXSTATUS sequence mismatch"));
    tick_after=3; tick_kind=1;
    assert(!wl5_step(27) && tx_doorbells()==1); until(28,28); contained(28);
    start(NULL,NULL); tx_behavior=3; until(17,26);
    assert(wl5_step(27)==1 && strstr(logbuf,"BHIe TXSTATUS error")); cleanup(27);
    start(NULL,NULL); tx_behavior=1; until(17,26);
    assert(wl5_step(27)==2 && !wl_amss_reached); cleanup(27);

    /* TX completion and AMSS arrival are separate waits. */
    start(NULL,NULL); tx_behavior=4; until(17,27);
    assert(wl5_step(28)==2 && !wl_amss_reached); /* TX good, stays SBL */
    tick_after=2; tick_kind=2; assert(!wl5_step(28)); contained(28);
    start(NULL,NULL); until(17,27); setreg(WL_BAR0+0x48,0x101);
    assert(wl5_step(28)==2 && strstr(logbuf,"AMSS EE reached state 0x1; waiting M0"));
    assert(!wl_amss_reached); cleanup(28); /* no second M0 request after AMSS */
    start(NULL,NULL); tx_behavior=4; until(17,27); tick_after=1; tick_kind=3;
    assert(wl5_step(28)==1 && strstr(logbuf,"RDDM requested")); cleanup(28);

    /* READY -> contexts -> M0 timeout and register-discovery guards. */
    start(NULL,NULL); until(17,24); progress=false;
    assert(wl5_step(25)==2 && !tx_doorbells()); cleanup(25);
    start(NULL,NULL); until(17,22); setreg(WL_BAR0+0x48,0);
    assert(wl5_step(23)==2 && !wl_contexts_published); cleanup(23);
    start(NULL,NULL); until(17,23); setreg(WL_BAR0+0x20,0x300);
    assert(wl5_step(24)==1 && !writes(WL_BAR0+0x300)); cleanup(24);

    /* Full image/vector allocation/read failures and malformed vectors stop
     * before a TX doorbell; readback, faults and second-TX attempts are bounded. */
    for(unsigned nth=1;nth<=5;nth++) {
        start("prefix",NULL); fail_nth_alloc=nth;
        assert(run(20)); fail_nth_alloc=0; cleanup(20);
    }
    start("prefix",NULL); read_resid=1; assert(run(19)==19); cleanup(19);
    for(unsigned kind=0;kind<4;kind++) {
        start("prefix",NULL); until(17,25);
        if(kind==0) ((struct wl_vector *)wl_vectors.va)[0].size--;
        if(kind==1) {mismatch=true; mismatch_pa=WL_BAR0+0x258;}
        if(kind==2) read_fault_pa=WL_BAR0+0x250;
        if(kind==3) setreg(WL_BAR0+0x2c,0x228);
        assert(wl5_step(26) && !tx_doorbells());
        assert(strstr(logbuf,"W10 BHIe TX refused"));
        mismatch=false; read_fault_pa=0; cleanup(26);
    }
    start(NULL,NULL); tx_behavior=5; until(17,25);
    assert(wl5_step(26)==4 && wl_bhie_fired && tx_doorbells()==1);
    assert(wl5_step(26)==1 && tx_doorbells()==1); cleanup(26);
    start(NULL,NULL); until(17,26); setreg(wl_cb_addr(wl_cb)+0x58,2);
    assert(wl5_step(27)==3 && strstr(logbuf,"SMMU fault")); cleanup(27);
    start(NULL,NULL); until(17,26); keep_arg=1; wl_keep=true;
    setreg(WL_BAR0+0x268,3u<<30); assert(wl5_step(27)==1);
    fail_bm=fail_perst=true; wl5_abort(27);
    assert(!frees && !wl_amss_reached && wl_image.va);
    fail_bm=fail_perst=false; cleanup(27);
    puts("W10 SBL READY, prefix/full vectors, BHIe sequence/error/timeouts, AMSS/M0, no channels, keep/contain/quarantine passed");
    return 0;
}
'''


def main():
    preamble = m6o.m6.PREAMBLE
    preamble = preamble.replace('static bool progress = true,', '''static unsigned tx_behavior, vfs_reads, vfs_bytes;
static void w10_tick(unsigned);
static bool progress = true,''')
    preamble = preamble.replace('static uint8_t fixture[0x80000 + 37];',
                                'static uint8_t fixture[0xf00000];')
    preamble = preamble.replace('sleeps+=ms;', 'sleeps+=ms; w10_tick(ms);')
    preamble = preamble.replace('memcpy(b,fixture,len); *resid=',
                                'memcpy(b,fixture,len); vfs_reads++; vfs_bytes=len; *resid=')
    preamble = preamble.replace('if (pa==WL_BAR0+0x260 && v) {', '''if (pa==WL_BAR0+0x260 && v) {
        if(tx_behavior==5) { fw_faults++; return; }
        if(tx_behavior==1) return;
        if(tx_behavior==2) {
            setreg(WL_BAR0+0x268,(2u<<30)|((v+1)&0x3fffffff));
            setreg(WL_BAR0+0x128,2); return;
        }
        if(tx_behavior==3) {setreg(WL_BAR0+0x268,(3u<<30)|(v&0x3fffffff)); return;}
        if(tx_behavior==4) {setreg(WL_BAR0+0x268,(2u<<30)|(v&0x3fffffff)); return;}
''')
    m6o.m6.PREAMBLE = preamble
    setup = m6o.TESTS.split('int main(void) {', 1)[0]
    setup = setup.replace('init(); stop22_arg=1;', 'init(); vfs_reads=vfs_bytes=0; stop22_arg=1;')
    m6o.TESTS = setup + TESTS
    m6o.main()


if __name__ == '__main__':
    main()
