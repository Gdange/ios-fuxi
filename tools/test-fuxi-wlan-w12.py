#!/usr/bin/env python3
"""W12 production WLFW handshake + independent synthetic MHI/QRTR peer; host only."""
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('w11', ROOT / 'tools/test-fuxi-wlan-w11.py')
w11 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w11)

# Reuse the independent ring/credit transport peer, replace its QMI endpoint.
# All new wire expectations below are literal, independent of production codecs.
PEER = w11.TESTS.split('int main(void) {', 1)[0]
PEER = PEER.replace('static void dev_work(void) {', r'''
static unsigned scenario, requests[4], device_stage;
static void ind(unsigned msg,const uint8_t *tlvs,unsigned n) {
    uint8_t p[2048]={4,0,0};put16(p+3,msg);put16(p+5,n);
    if(n)memcpy(p+7,tlvs,n);dev_qrtr(1,p,n+7,false,false);
}
static void reply(unsigned txn,unsigned msg,bool reject) {
    uint8_t p[]={2,0,0,0,0,7,0,2,4,0,0,0,0,0};
    put16(p+1,txn);put16(p+3,msg);
    if(reject){p[10]=1;p[12]=90;}
    if(scenario==28 && msg==0x20) {
        uint8_t status[29];memcpy(status,p,14);put16(status+5,22);
        const uint8_t tlvs[]={0x10,8,0,1,2,3,4,5,6,7,8,0x71,1,0,42};
        memcpy(status+14,tlvs,sizeof(tlvs));dev_qrtr(1,status,sizeof(status),false,false);return;
    }
    if(scenario==25){p[7]=0x70;}
    dev_qrtr(1,p,sizeof(p),false,false);
    if(scenario==26) {
        uint8_t dup[21];memcpy(dup,p,14);memcpy(dup+14,p+7,7);put16(dup+5,14);
        dev_qrtr(1,dup,sizeof(dup),false,false);
    }
}
static void mem_request(void) {
    if(scenario==30) {
        uint8_t p[472]={1,0xd5,1,52};
        for(unsigned i=0;i<52;i++){put32(p+4+9*i,1);put32(p+8+9*i,i+1);}
        ind(0x35,p,sizeof(p));return;
    }
    /* Two segments; second has a single nonsecure subrange. LE counts are u8. */
    uint8_t p[]={1,32,0,2,
        1,0,2,0, 1,0,0,0, 0,
        0,0,1,0, 9,0,0,0, 1,
        0,16,0,0,0,0,0,0, 0,16,0,0, 0};
    if(scenario==13){put32(p+4,0x4000000);put32(p+13,1);p[21]=0;put16(p+1,19);ind(0x35,p,22);return;}
    if(scenario==14)p[34]=1;
    if(scenario==15)p[29]=2; /* config size exceeds segment */
    if(scenario==16)p[0]=0x70;
    if(scenario==17)p[3]=0;
    if(scenario==18)p[3]=53;
    if(scenario==19)p[12]=3;
    if(scenario==20){put16(p+1,31);ind(0x35,p,34);return;}
    ind(0x35,p,sizeof(p));
}
static void full_cap(void) {
    uint8_t p[256];memcpy(p,cap_reply,sizeof(cap_reply));unsigned n=sizeof(cap_reply);
    p[1]=4;
    const uint8_t extra[]={
        0x15,1,0,2,
        0x16,4,0,0xe4,0xc,0,0,
        0x17,4,0,0,0xf8,0x24,1,
        0x18,4,0,0x44,0x33,0x22,0x11,
        0x19,4,0,9,0,0,0,
        0x1a,8,0,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0x1b,4,0,2,0,0,0,
        0x1c,64,0,
        1,2,3,4,5,6,7,8, 0x10,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0x77,1,0,42};
    memcpy(p+n,extra,sizeof(extra));n+=sizeof(extra);put16(p+5,n-7);
    if(scenario==27)p[sizeof(cap_reply)+4+7+7+1]=3; /* wrong OTP length */
    dev_qrtr(1,p,n,false,false);
}
static void qmi_peer(const uint8_t *q,unsigned len) {
    assert(len>=7 && q[0]==0 && (unsigned)(q[5]|(q[6]<<8))==len-7);
    unsigned msg=q[3]|(q[4]<<8),txn=q[1]|(q[2]<<8);
    assert(txn>=1 && txn<=4 && txn==device_stage+1);
    const unsigned ids[]={0x20,0x34,0x36,0x24};assert(msg==ids[device_stage]);
    requests[device_stage]++;device_stage++;
    if(msg==0x20) {
        const uint8_t expected[]={0,1,0,0x20,0,27,0,
            0x10,1,0,1,0x15,4,0,0x4c,0x45,0x4e,0x4b,
            0x16,1,0,1,0x17,1,0,1,0x18,1,0,1,0x1b,1,0,1};
        if(len!=sizeof(expected) || memcmp(q,expected,len)) {
            fprintf(stderr,"msg %x len %u expected %zu\n",msg,len,sizeof(expected));
            for(unsigned j=0;j<len;j++)fprintf(stderr,"%02x ",q[j]);fprintf(stderr,"\n");
        }
        assert(len==sizeof(expected) && !memcmp(q,expected,len));
        if(scenario==2){mem_request();ind(0x37,NULL,0);}
        if(scenario==6)return;
        if(scenario==24)reply(9,msg,false); /* stale */
        reply(txn,msg,scenario==12);
    }
    if(msg==0x34) {
        const uint8_t expected[]={0,2,0,0x34,0,85,0,
            0x10,4,0,1,0,0,0, 0x13,1,0,14, 0x14,1,0,1,
            0x16,1,0,1, 0x17,1,0,1, 0x1a,1,0,0, 0x1c,1,0,0,
            0x21,1,0,1, 0x22,2,0,0,0, 0x23,1,0,0,
            0x24,2,0,32,0, 0x25,1,0,1,
            0x26,18,0,0,2,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
            0x27,8,0,24,0,0,0,0,0,0,0};
        if(len!=sizeof(expected) || memcmp(q,expected,len)) {
            fprintf(stderr,"msg %x len %u expected %zu\n",msg,len,sizeof(expected));
            for(unsigned j=0;j<len;j++)fprintf(stderr,"%02x ",q[j]);fprintf(stderr,"\n");
        }
        assert(len==sizeof(expected) && !memcmp(q,expected,len));
        if(scenario==3){ind(0x7777,NULL,0);ind(0x38,NULL,0);}
        if(scenario==7)return;
        if(scenario==1)mem_request();
        reply(txn,msg,false);
        if(scenario!=1 && scenario!=8)mem_request();
        if(scenario==4)mem_request();
    }
    if(msg==0x36 && scenario==30) {
        assert(len==895 && q[7]==1 && q[8]==0x75 && q[9]==3 && q[10]==52);
        for(unsigned i=0;i<52;i++) {
            const uint8_t *v=q+11+17*i;
            assert(get32(v)==wl_target[i].iova && !get32(v+4) && get32(v+8)==1 && get32(v+12)==i+1 && !v[16]);
            assert(walk(get32(v))==(wl_phys(wl_target[i].va)|WL_DMA_PTE));
        }
        reply(txn,msg,false);ind(0x37,NULL,0);return;
    }
    if(msg==0x36) {
        assert(len==45 && q[7]==1 && q[8]==35 && q[9]==0 && q[10]==2);
        const unsigned sizes[]={131073,65536}, types[]={1,9};
        for(unsigned i=0;i<2;i++) {
            const uint8_t *v=q+11+17*i;uint64_t addr=get32(v)|((uint64_t)get32(v+4)<<32);
            assert(addr==wl_target[i].iova && get32(v+8)==sizes[i] && get32(v+12)==types[i] && v[16]==0);
            assert(wl_target[i].nc && addr>=wl_iova_base && addr+sizes[i]<=wl_iova_end);
            for(unsigned off=0;off<wl_target[i].size;off+=4096) {
                assert(walk(addr+off)==(wl_phys(wl_target[i].va+off)|WL_DMA_PTE));
                assert(!memcmp((void *)(wl_target[i].va+off),(uint8_t[4096]){0},4096));
            }
        }
        if(scenario==9)return;
        if(scenario==1)ind(0x37,NULL,0);
        reply(txn,msg,false);
        if(scenario==5){full_cap();return;} /* cannot substitute for READY */
        if(scenario!=1 && scenario!=10)ind(0x37,NULL,0);
        if(scenario==21)ind(0x37,NULL,0);
    }
    if(msg==0x24) {
        const uint8_t expected[]={0,4,0,0x24,0,0,0};
        assert(len==7 && !memcmp(q,expected,7));
        assert(wl12_ready && requests[0]==1 && requests[1]==1 && requests[2]==1);
        cap_requests++;
        if(scenario!=11)full_cap();
        if(scenario==23){ind(0x38,NULL,0);ind(0x8888,NULL,0);}
    }
}
static void dev_work(void) {
''')
PEER = PEER.replace('len<=4096', 'len<=4096').replace('uint8_t p[512]', 'uint8_t p[4096]')
PEER = PEER.replace('tre->d0<=256','tre->d0<=1024')
a=PEER.index('            assert(len==7 && !memcmp')
b=PEER.index('\n        }', a)
PEER=PEER[:a]+'            qmi_peer(p+32,len);'+PEER[b:]
a=PEER.index('        if(type==1 && mode!=6) {')
b=PEER.index('\n    }',a)
PEER=PEER[:a]+PEER[b:]
PEER=PEER.replace('qrtr_arg=1;', 'qrtr_arg=2;')
PEER=PEER.replace('static int finish_step(void) {return wl5_step(28);}', r'''
static int finish_step(void) {int e=wl5_step(28);return e;}
static void begin(unsigned behavior,bool keep) {
    scenario=behavior;device_stage=0;memset(requests,0,sizeof(requests));start(0,keep);
    assert(wl_w12 && wl_qrtr);
}
static void success(void) {
    int e=finish_step();if(e)fprintf(stderr,"%s",logbuf);assert(!e && wl_w11_ok);
    assert(cap_requests==1 && device_stage==4 && wl_target_n==2 && wl_target_total==196609);
    for(unsigned i=0;i<4;i++)assert(requests[i]==1);
    const char *lines[]={"W12 ind register ok","W12 host cap ok","W12 request mem n=2 total=196609",
        "W12 respond mem ok","W12 fw mem ready","W12 cap: valid 0x3fff", "otp 0x11223344",
        "fw caps 0x1122334455667788", "start 0x807060504030201", "W12 handshake complete"};
    for(unsigned i=0;i<sizeof(lines)/sizeof(lines[0]);i++)assert(strstr(logbuf,lines[i]));
    assert(!walk(wl_target[1].iova+wl_target[1].size));
}
''')
TESTS = PEER + r'''
int main(void) {
    begin(0,true);success();unsigned before=ne;wl_finish(28);assert(ne==before && !frees);
    assert(wl_w11_run() && cap_requests==1);contain();
    for(unsigned i=0;i<52;i++)assert(!wl_target[i].va);
    begin(0,false);success();wl_finish(28);assert(frees==allocations && stops==2);dev_on=false;
    const unsigned good[]={1,2,3,21,23,24,28};
    for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);i++) {
        begin(good[i],true);success();
        if(good[i]==1)assert(strstr(logbuf,"out-of-order REQUEST_MEM") && strstr(logbuf,"out-of-order FW_MEM_READY"));
        if(good[i]==2)assert(strstr(logbuf,"ignored before HOST_CAP") && strstr(logbuf,"FW_MEM_READY stage 1 ignored"));
        if(good[i]==3)assert(strstr(logbuf,"unknown/cal indication 0x7777"));
        if(good[i]==23)assert(strstr(logbuf,"indication 0x8888"));
        contain();
    }
    begin(30,true);assert(!finish_step() && wl_target_n==52 && wl_w11_ok && cap_requests==1);
    contain();assert(frees==allocations);
    const unsigned bad[]={4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,25,27};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        begin(bad[i],true);unsigned t=sleeps;
        int e=finish_step();if(!e)fprintf(stderr,"scenario %u\n%s",bad[i],logbuf);
        assert(e && !wl_w11_ok && sleeps-t<=12200 && device_stage<=4);
        if(bad[i]>=6 && bad[i]<=11)assert(strstr(logbuf,"W12 timeout"));
        if(bad[i]==12)assert(strstr(logbuf,"error 90 (INCOMPATIBLE_STATE)"));
        if(bad[i]==13)assert(strstr(logbuf,"64 MiB") && !requests[2]);
        contain();
    }
    /* Missing/duplicate/unknown response TLVs and indication lengths. */
    begin(0,true);success();dev_on=false;
    uint8_t response[]={2,1,0,0x20,0,14,0,2,4,0,0,0,0,0,2,4,0,0,0,0,0};
    wl12_stage=WL12_IND;wl12_txn=1;wl12_msg=0x20;wl12_reply=false;clear_error();
    assert(wl_qmi_w12(response,sizeof(response)) && !wl12_reply);
    for(unsigned cut=0;cut<sizeof(response);cut++) {
        clear_error();wl12_reply=false;assert(wl_qmi_w12(response,cut));
    }
    uint8_t unknown[]={4,0,0,0x77,0x77,4,0,0x70,1,0,42};
    clear_error();assert(!wl_qmi_w12(unknown,sizeof(unknown)));
    unknown[8]=2;assert(wl_qmi_w12(unknown,sizeof(unknown)));
    dev_on=true;clear_error();contain();

    /* Production parser: count 1..52, exact rounded cap, rounding over cap,
     * overflow-scale lengths, cfg subranges, unknown optional TLVs. */
    begin(0,true);success();dev_on=false;
    uint8_t segments[1+52*9]={52};
    for(unsigned i=0;i<52;i++){put32(segments+1+9*i,1);put32(segments+5+9*i,i+1);}
    clear_error();wl12_requested=false;assert(!wl12_request_mem(segments,sizeof(segments)) && wl_target_n==52);
    assert(wl12_request_mem(segments,sizeof(segments)-1));
    segments[0]=1;put32(segments+1,64u*1024*1024);clear_error();assert(!wl12_request_mem(segments,10));
    segments[0]=2;put32(segments+1,64u*1024*1024-1);put32(segments+10,1);
    clear_error();assert(wl12_request_mem(segments,19));
    segments[0]=1;put32(segments+1,~0u);clear_error();assert(wl12_request_mem(segments,10));
    segments[0]=1;put32(segments+1,4096);uint64_t end=wl_iova_next;wl_iova_next=wl_iova_end-PAGE_SIZE+1;
    clear_error();assert(wl12_request_mem(segments,10));wl_iova_next=end;
    dev_on=true;clear_error();contain();

    /* Target allocation and NC conversion failure must prevent RESPOND_MEM;
     * include partial allocations in cleanup and quarantine. */
    for(unsigned kind=0;kind<3;kind++) {
        begin(0,true);
        if(kind<2)fail_nth_alloc=allocations+1+kind;
        if(kind==2)cache_drop=true;
        assert(finish_step() && !requests[2] && !wl_w11_ok);
        fail_nth_alloc=0;cache_drop=false;contain();
    }
    begin(10,true);assert(finish_step() && wl_target[0].va && !wl_w11_ok);
    fail_bm=fail_perst=true;wl5_abort(28);assert(!frees && wl_target[0].va && wl_target[1].va);
    fail_bm=fail_perst=false;contain();

    /* Extend across L3 boundaries with noncontiguous backing, and refuse
     * collisions, changed parents/live TTBR, missing aliases and TLB timeout. */
    for(unsigned kind=0;kind<6;kind++) {
        begin(0,true);success();dev_on=false;clear_error();
        struct wl_dma *d=&wl_target[2];assert(!d->va && !wl_dma_alloc(d,0x210000) && !wl_dma_nc(d));
        uint64_t *l2=(void *)(wl_tables.va+4096);
        unsigned n=(d->iova-wl_iova_base)>>21;uint64_t *l3=(void *)(wl_tables.va+(2+n)*4096);
        if(kind==1)l3[(d->iova>>12)&511]=WL_DMA_PTE;
        if(kind==2)l2[(d->iova>>21)&511]=0;
        if(kind==3)setreg(wl_cb_addr(wl_cb)+0x20,0);
        if(kind==4)setreg(wl_cb_addr(wl_cb)+0x7f4,1);
        if(kind==5)setreg(WL_SMMU+0xc00+4*wl_slot,wl_cb+1);
        int e=wl12_map_target(d);assert(kind?e:!e);
        if(!kind)for(unsigned off=0;off<d->size;off+=4096)assert(walk(d->iova+off)==(wl_phys(d->va+off)|WL_DMA_PTE));
        setreg(wl_cb_addr(wl_cb)+0x7f4,0);dev_on=true;clear_error();contain();
    }
    puts("W12 literal QMI encodings/full capability, indication order, DMA limits/live mapping, deadlines and containment passed");
    return 0;
}
'''


def main():
    assert 'python3 tools/test-fuxi-wlan-w12.py' in (ROOT / 'tools/ci/build.sh').read_text()
    w11.TESTS = TESTS
    # More independent allocations than W11: 52 target segments are legal.
    preamble = w11.w10.m6o.m6.PREAMBLE
    preamble = preamble.replace('cache_attrs[32][1024]', 'cache_attrs[96][8192]')
    preamble = preamble.replace('allocs[32]', 'allocs[96]')
    w11.w10.m6o.m6.PREAMBLE = preamble
    w11.main()


if __name__ == '__main__':
    main()
