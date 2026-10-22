#!/usr/bin/env python3
"""Host-only W13: ath12k EI golden bytes, bounded negotiation and crash snapshots.

Wire facts: ath12k v6.6 qmi.c/qmi.h, BSD-3-Clause-Clear; credit The Linux
Foundation (2018-2021), Qualcomm Innovation Center, Inc. (2021-2022).
Independent fixtures/peer; no Linux implementation or firmware is included.
"""
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('w12', ROOT / 'tools/test-fuxi-wlan-w12.py')
w12 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w12)

PEER = w12.PEER
start = PEER.index('static void qmi_peer(')
end = PEER.index('static void dev_work(void) {', start)
# Retain the independently literal W12 IND_REGISTER/HOST_CAP expectations.
host = PEER[start:end]
ind_expected = host[host.index('const uint8_t expected[]'):host.index('        if(len!=sizeof(expected)')]
host = host[host.index('if(msg==0x34)'):]
host_expected = host[host.index('const uint8_t expected[]'):host.index('        if(len!=sizeof(expected)')]
PEER = PEER[:start] + r'''
static unsigned trial, responses, snapshot_start;
static uint64_t target_cursor;
static const unsigned chunk_size[]={2097152,2097152,2097152,983040,
    2097152,2097152,2097152,2097152,65536};
static void split_request(void) {
    uint8_t p[85]={1,82,0,9};
    for(unsigned i=0;i<9;i++) {put32(p+4+9*i,chunk_size[i]);put32(p+8+9*i,i<4?1:4);}
    if(trial==4)put32(p+4,2097153); /* second large request: bounded refusal */
    ind(0x35,p,sizeof(p));
}
static void large_request(void) {
    /* Literal observed lengths; no device addresses or captured packet bytes. */
    const uint8_t p[]={1,19,0,2,0,0,111,0,1,0,0,0,0,0,0,129,0,4,0,0,0,0};
    ind(0x35,p,sizeof(p));
}
static void type_request(void) {
    uint8_t p[49]={1,46,0,5};
    const unsigned types[]={1,3,4,9,2};
    for(unsigned i=0;i<5;i++){put32(p+4+9*i,65537);put32(p+8+9*i,types[i]);}
    ind(0x35,p,sizeof(p));
}
static void delayed_reply(unsigned txn) {
    /* ath12k allows result FAILURE=1 when error=0 only on its delayed path. */
    uint8_t p[]={2,0,0,0x36,0,7,0,2,4,0,1,0,0,0};put16(p+1,txn);
    if(trial==5)p[12]=90;
    if(trial==1)p[10]=0;
    dev_qrtr(1,p,sizeof(p),false,false);
}
static void crash_regs(unsigned ee,unsigned st) {
    setreg(WL_BAR0+0x128,ee);setreg(WL_BAR0+0x48,st);
    setreg(WL_BAR0+0x130,0xcafecace);setreg(WL_BAR0+0x134,0xcafecace);
    setreg(WL_BAR0+0x138,0x14feeb3);setreg(WL_BAR0+0x13c,0x496);
}
static void qmi_peer(const uint8_t *q,unsigned len) {
    assert(len>=7 && q[0]==0 && (unsigned)(q[5]|(q[6]<<8))==len-7);
    unsigned msg=q[3]|(q[4]<<8),txn=q[1]|(q[2]<<8);
    if(msg==0x20) {
''' + ind_expected + r'''
        assert(len==sizeof(expected) && !memcmp(q,expected,len));
        reply(txn,msg,false);return;
    }
    if(msg==0x34) {
''' + host_expected + r'''
        assert(len==sizeof(expected) && !memcmp(q,expected,len));
        if(trial==2)large_request(); /* races HOST_CAP reply */
        reply(txn,msg,false);
        if(trial==8)type_request();
        else if(trial!=2)large_request();
        return;
    }
    if(msg==0x36) {
        responses++;snapshot_start=ne;
        if(trial!=8 && responses==1) {
            const uint8_t zero[]={0,3,0,0x36,0,4,0,1,1,0,0};
            assert(len==sizeof(zero) && !memcmp(q,zero,len));
            assert(wl_iova_next==target_cursor);
            for(unsigned i=0;i<52;i++)assert(!wl_target[i].va && !wl_target[i].iova);
            if(trial==6)return; /* delayed response timeout */
            if(trial==2)split_request(); /* races zero-response reply */
            delayed_reply(txn);
            if(trial!=2 && trial!=3 && trial!=5)split_request();
            return;
        }
        assert(q[7]==1 && q[9]==0 && q[10]==(trial==8?5:9));
        const unsigned types[]={1,3,4,9,2};
        unsigned count=trial==8?5:9;
        assert(len==11+17*count && q[8]==1+17*count);
        for(unsigned i=0;i<count;i++) {
            const uint8_t *v=q+11+17*i;uint64_t addr=get32(v)|((uint64_t)get32(v+4)<<32);
            unsigned size=trial==8?65537:chunk_size[i], type=trial==8?types[i]:(i<4?1:4);
            assert(get32(v+8)==size && get32(v+12)==type && !v[16]);
            if(trial==8 && i==4) {assert(!addr && !wl_target[i].va);continue;}
            /* Independent power-of-two test, explicit write permission decode. */
            unsigned align=16384;while(align<size)align*=2;
            assert(!(addr&(align-1)) && addr==wl_target[i].iova && wl_target[i].nc);
            for(unsigned off=0;off<wl_target[i].size;off+=4096) {
                uint64_t pte=walk(addr+off);
                assert(pte==(wl_phys(wl_target[i].va+off)|WL_DMA_PTE));
                assert(((pte>>6)&3)==1 && !((pte>>2)&7) && ((pte>>8)&3)==2);
                assert(!memcmp((void *)(wl_target[i].va+off),(uint8_t[4096]){0},4096));
            }
        }
        if(trial>=9 && trial<=11) { /* observed AMSS stall, SYSERR, RDDM */
            crash_regs(trial==11?3:2,trial>=10?0xff05:0x201);return;
        }
        if(trial==7)return;
        if(trial==13){reply(txn,msg,true);return;}
        reply(txn,msg,false);ind(0x37,NULL,0);
        if(trial==12)split_request(); /* forbidden after real provisioning */
        return;
    }
    assert(msg==0x24 && len==7 && txn==(trial==8?4:5) && wl12_ready);
    cap_requests++;
    uint8_t p[sizeof(cap_reply)];memcpy(p,cap_reply,sizeof(p));put16(p+1,txn);
    dev_qrtr(1,p,sizeof(p),false,false);
}
''' + PEER[end:]
a = PEER.index('static void success(void)')
b = PEER.index('\n}\n', a) + 3
PEER = PEER[:a] + PEER[b:]  # Remove only W12-specific success assertions
PEER = PEER.replace('qrtr_arg=2;', 'qrtr_arg=3;')

TESTS = PEER + r'''
static void golden(void) {
    /* EI widths/order give 1 + 2*17 bytes, not u16 count/native 24-byte stride.
     * Nontrivial high address halves and enum bits expose endian/padding bugs. */
    const uint8_t expected[]={1,35,0,2,
        0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x12,0x34,0x56,0x78,1,0,0,0,0,
        8,7,6,5,4,3,2,1,0xdd,0xcc,0xbb,0xaa,0xfe,0xff,0xff,0xff,0};
    struct wl12_encoder e={{0},7};wl_target_n=2;
    wl_target[0].iova=0x1122334455667788ull;wl_target_size[0]=0x78563412;wl_target_type[0]=1;
    wl_target[1].iova=0x0102030405060708ull;wl_target_size[1]=0xaabbccdd;wl_target_type[1]=0xfffffffe;
    assert(!wl12_encode_respond(&e,false) && e.len==45 && !memcmp(e.data+7,expected,sizeof(expected)));
    e.len=7;assert(!wl12_encode_respond(&e,true) && e.len==11);
    assert(!memcmp(e.data+7,(uint8_t[]){1,1,0,0},4));
    for(unsigned i=0;i<52;i++){wl_target[i].iova=0;wl_target_size[i]=1;wl_target_type[i]=4;}
    e.len=7;wl_target_n=52;assert(!wl12_encode_respond(&e,false) && e.len==895);
    assert(e.data[8]==0x75 && e.data[9]==3 && e.data[10]==52 && !e.data[894]);
    e.len=7;wl_target_n=53;clear_error();assert(wl12_encode_respond(&e,false));
    e.len=8;wl_target_n=1;clear_error();assert(wl12_encode_respond(&e,false));
    wl_target_n=0;clear_error();
}
static void boot(unsigned n,bool keep) {
    trial=n;responses=0;begin(0,keep);assert(wl_w13);target_cursor=wl_iova_next;
}
static void done(void) {
    int e=finish_step();if(e)fprintf(stderr,"trial %u\n%s",trial,logbuf);
    assert(!e && wl_w11_ok && cap_requests==1 && responses==(trial==8?1:2));
    assert(wl_target_total==(trial==8?5*65537:15728640));
    assert(strstr(logbuf,"AP[7:6]=1 RW") && strstr(logbuf,"count-u8"));
    if(trial!=8)assert(strstr(logbuf,"zero segments before any target allocation"));
}
int main(void) {
    golden();
    for(unsigned n=0;n<=2;n++) {
        boot(n,true);done();assert(wl13_negotiated && wl12_txn==5);
        if(n==2)assert(strstr(logbuf,"queued before delayed RESPOND_MEM reply"));
        unsigned before=ne;wl_finish(28);assert(ne==before && !frees);contain();
    }
    boot(8,false);done();assert(!wl13_negotiated && wl12_txn==4);
    assert(strstr(logbuf,"unsupported target type 2"));wl_finish(28);assert(frees==allocations);dev_on=false;
    const unsigned failures[]={3,4,5,6,7,9,10,11,12,13};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
        boot(failures[i],true);unsigned ticks=sleeps;
        int e=finish_step();assert(e && !wl_w11_ok && !cap_requests && responses<=2 && sleeps-ticks<23000);
        if(trial==3)assert(strstr(logbuf,"timeout smaller REQUEST_MEM"));
        if(trial==4)assert(strstr(logbuf,"negotiation exhausted"));
        if(trial==5 || trial==13)assert(strstr(logbuf,"WLFW setup rejected"));
        if(trial==6 || trial==7 || (trial>=9 && trial<=11)) {
            assert(strstr(logbuf,"W13 RESPOND_MEM diagnostic"));
            if(trial>=9) {
                assert(strstr(logbuf,"BHI_ERRCODE 0xcafecace BHI_ERRDBG1 0xcafecace BHI_ERRDBG2 0x14feeb3 BHI_ERRDBG3 0x496"));
                assert(strstr(logbuf,trial>=10?"SYSERR(bit2) 1 MHI_STATE 0xff":"SYSERR(bit2) 0 MHI_STATE 0x2"));
            }
            if(trial==11)assert(strstr(logbuf,"RDDM observed; dump not collected"));
            /* Diagnostics cause no MMIO writes/dump doorbells after submission. */
            for(unsigned j=snapshot_start;j<ne;j++)if(events[j].kind=='W')
                assert(events[j].pa<WL_BAR0+wl_bhie+0x60 || events[j].pa>WL_BAR0+wl_bhie+0x78);
        }
        if(trial==12)assert(strstr(logbuf,"duplicate REQUEST_MEM; no reprovision"));
        contain();
    }
    /* Allocation failure after a negotiated request stays contained, with no
     * second empty response or freeing live mappings before reset/PERST. */
    boot(0,true);fail_nth_alloc=allocations+2;
    assert(finish_step() && responses==1 && wl_target[0].va && !wl_w11_ok);
    fail_nth_alloc=0;fail_bm=fail_perst=true;wl5_abort(28);assert(!frees && wl_target[0].va);
    fail_bm=fail_perst=false;contain();

    /* Alignment gaps count against IOVA space before allocating any segment. */
    boot(8,true);dev_on=false;wl_target_n=0;
    uint8_t seg[]={1,0,0,1,0,1,0,0,0,0}; /* 65536, DDR */
    wl_iova_next=wl_iova_end-65536-PAGE_SIZE;clear_error();
    put32(seg+1,65537); /* rounded bytes fit, aligned base + size does not */
    assert(wl12_request_mem(seg,sizeof(seg)) && strstr(logbuf,"aligned IOVA exhaustion"));
    clear_error();dev_on=true;contain();
    /* A snapshot is exactly six named register reads, no writes/transfers,
     * no BHI serial register. A read fault must visibly invalidate it. */
    boot(8,true);dev_on=false;crash_regs(3,0xff05);
    unsigned before=ne;nreads=0;track_reads=true;wl13_respond_diagnostics();track_reads=false;
    const unsigned offsets[]={0x128,0x48,0x130,0x134,0x138,0x13c};
    assert(ne==before && nreads==6);
    for(unsigned i=0;i<6;i++)assert(reads[i]==WL_BAR0+offsets[i]);
    read_fault_pa=WL_BAR0+0x138;wl13_respond_diagnostics();
    assert(ne==before && fw_faults && strstr(logbuf,"access-faults 1 valid 0"));
    read_fault_pa=0;fw_faults=0;crash_regs(2,0x201);dev_on=true;contain();
    puts("W13 EI golden bytes/HOST_CAP, one delayed negotiation and races, types/aligned RW DMA, timeout/SYSERR/RDDM snapshots and containment passed");
    return 0;
}
'''


def main():
    assert 'python3 tools/test-fuxi-wlan-w13.py' in (ROOT / 'tools/ci/build.sh').read_text()
    w12.TESTS = TESTS
    w12.main()


if __name__ == '__main__':
    main()
