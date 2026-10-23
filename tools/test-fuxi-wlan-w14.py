#!/usr/bin/env python3
"""W14 original host peer: EI-derived BDF golden bytes, synthetic files and M3 DMA.

ath12k v6.6 protocol facts: BSD-3-Clause-Clear; The Linux Foundation
(2018-2021), Qualcomm Innovation Center, Inc. (2021-2022). No firmware fixtures.
"""
from pathlib import Path
import importlib.util
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('w13', ROOT / 'tools/test-fuxi-wlan-w13.py')
w13 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w13)
w12 = w13.w12
PEER = w13.PEER.replace('qrtr_arg=3;', 'qrtr_arg=4;')
PEER = PEER.replace('tre->d0<=1024', 'tre->d0<=8192').replace('0x1000+4096*dev_tx', '0x1000+8192*dev_tx')
PEER = PEER.replace('static unsigned trial, responses, snapshot_start;', 'static unsigned trial, responses, snapshot_start, dl[2], m3_requests;')
# Preserve W13 negotiation/transport peer, replace only the CAP response and
# add an independently checked download endpoint. Failure controls use io_case.
pos = PEER.index('    assert(msg==0x24 && len==7')
end = PEER.index('\n}\n', pos)
PEER = PEER[:pos] + r'''
    if(msg==0x25) {
        unsigned which=wl12_stage==WL14_REGDB?0:1, seg=dl[which]++;
        assert(which==0 || dl[0]==2);
        unsigned size=which?18439:12288, off=seg*6144;
        unsigned n=size-off>6144?6144:size-off;
        /* Literal EI layout: valid, enum file_id, remaining total, segment,
         * u16 array length, data, end u8, bdf_type u8. */
        assert(len==7+38+n);
        assert(!memcmp(q+7,(uint8_t[]){1,1,0,1,0x10,4,0,0,0,0,0,0x11,4,0},14));
        assert(get32(q+21)==size-off);
        assert(!memcmp(q+25,(uint8_t[]){0x12,4,0},3) && get32(q+28)==seg);
        assert(q[32]==0x13 && (unsigned)(q[33]|q[34]<<8)==n+2);
        assert((unsigned)(q[35]|q[36]<<8)==n);
        for(unsigned i=0;i<n;i++)assert(q[37+i]==synthetic(which+2,off+i));
        assert(!memcmp(q+37+n,(uint8_t[]){0x14,1,0,off+n==size,0x15,1,0,which?1:4},8));
        if(io_case==5 && which==0 && seg==1){reply(txn,msg,true);return;}
        if(io_case==6 && which==1 && seg==1)return;
        if(io_case==7 && which==0 && seg==0) { /* missing common result */
            uint8_t p[]={2,0,0,0x25,0,0,0};put16(p+1,txn);dev_qrtr(1,p,7,false,false);return;
        }
        if(io_case==8 && which==0 && seg==0)reply(txn-1,msg,false); /* stale */
        if(io_case==18 && which==0 && seg==0) {
            uint8_t p[]={2,0,0,0x25,0,14,0,2,4,0,0,0,0,0,2,4,0,0,0,0,0};
            put16(p+1,txn);dev_qrtr(1,p,sizeof(p),false,false);return;
        }
        if(io_case==19 && which==0 && seg==0) {
            uint8_t p[]={2,0,0,0x25,0,7,0,2,4,0,0,0,90,0};
            put16(p+1,txn);dev_qrtr(1,p,sizeof(p),false,false);return;
        }
        ind(0x21,NULL,0); /* premature ready must not satisfy final wait */
        if(which && seg==3) {
            if(io_case==25)fail_nth_alloc=allocations+1;
            if(io_case==26)cache_drop=true;
        }
        reply(txn,msg,false);return;
    }
    if(msg==0x3c) {
        m3_requests++;assert(dl[0]==2 && dl[1]==4);
        assert(len==25 && !memcmp(q+7,(uint8_t[]){1,8,0},3));
        uint64_t addr=get32(q+10)|((uint64_t)get32(q+14)<<32);
        assert(addr==wl14_m3.iova && addr!=wl14_m3.pa && addr!=wl14_m3.va && !(addr&16383));
        assert(!memcmp(q+18,(uint8_t[]){2,4,0,0x39,0x30,0,0},7));
        assert(wl14_m3.nc && wl14_m3.size==16384);
        for(unsigned i=0;i<12345;i++)assert(*(uint8_t *)(wl14_m3.va+i)==synthetic(4,i));
        for(unsigned i=12345;i<16384;i++)assert(!*(uint8_t *)(wl14_m3.va+i));
        for(unsigned i=0;i<16384;i+=4096)assert(walk(addr+i)==(wl_phys(wl14_m3.va+i)|WL_DMA_PTE));
        if(io_case==9){reply(txn,msg,true);return;}
        if(io_case==10)return;
        if(io_case==11){reply(txn,msg,false);return;}
        if(io_case==12){reply(txn,msg,false);ind(0x38,NULL,0);return;}
        ind(0x21,NULL,0);ind(0x38,NULL,0); /* race before reply is legal */
        reply(txn,msg,false);return;
    }
    assert(msg==0x24 && len==7 && txn==5 && wl12_ready);cap_requests++;
    uint8_t p[1024];memcpy(p,cap_reply,sizeof(cap_reply));unsigned n=sizeof(cap_reply);
    put16(p+1,txn);put32(p+28,0); /* reported board 0 */
    const uint8_t scalar[]={0x1e,4,0,0x78,0x56,0x34,0x12,0x1f,2,0,0x61,0};
    memcpy(p+n,scalar,sizeof(scalar));n+=sizeof(scalar);
    p[n++]=0x22;put16(p+n,433);n+=2;p[n++]=9;
    for(unsigned j=0;j<9;j++) {
        memset(p+n,0,48);memcpy(p+n,"VDD_TEST",8);put32(p+n+32,1);
        put32(p+n+36,900+j);put32(p+n+40,1);put32(p+n+44,600+j);n+=48;
    }
    if(io_case==20)p[n-433]=17;
    if(io_case==21)p[25]=0x71; /* absent board */
    if(io_case==23){memcpy(p+n,p+sizeof(cap_reply),7);n+=7;} /* duplicate hang offset */
    if(io_case==24)p[sizeof(cap_reply)+8]=1; /* hang length wrong */
    put16(p+5,n-7);dev_qrtr(1,p,n,false,false);
''' + PEER[end:]
# RESPOND_MEM optional share_mem, exactly 61 bytes for three entries.
PEER = PEER.replace('reply(txn,msg,false);ind(0x37,NULL,0);', r'''
        uint8_t shared[78]={2,0,0,0x36,0,71,0,2,4,0,0,0,0,0,0x10,61,0,3};
        put16(shared+1,txn);
        for(unsigned j=0;j<3;j++){put32(shared+18+20*j,j+1);put32(shared+22+20*j,0x4000+j*4096);put32(shared+30+20*j,4096);}
        if(io_case==22)shared[17]=9;
        dev_qrtr(1,shared,sizeof(shared),false,false);ind(0x37,NULL,0);
''')
TESTS = PEER + r'''
static void boot14(unsigned c,bool keep) {
    io_case=0;trial=0;responses=dl[0]=dl[1]=m3_requests=0;
    begin(0,keep);assert(wl_w14);target_cursor=wl_iova_next;io_case=c;
}
static void golden14(void) {
    static struct wl12_encoder e;const uint8_t data[]={0xa5,0x5a,0x11};
    const uint8_t expected[]={1,1,0,1,0x10,4,0,0x78,0x56,0x34,0x12,
        0x11,4,0,3,0,0,0,0x12,4,0,2,0,0,0,0x13,5,0,3,0,0xa5,0x5a,0x11,
        0x14,1,0,1,0x15,1,0,4};
    wl14_board=0x12345678;e.len=7;
    assert(!wl14_encode_bdf(&e,data,3,2,4) && e.len==sizeof(expected)+7);
    assert(!memcmp(e.data+7,expected,sizeof(expected)));
    e.len=7;assert(wl14_encode_bdf(&e,data,0,0,4));clear_error();
}
int main(void) {
    golden14();
    const char *good[]={"bd_m3.elf","bd_m3gl.elf","bdwlan.e00","bdwlan.b01","a-b_2.bin"};
    const char *bad[]={"",".","..","../x","a/../x","a..b","/x","x\\y","a b","a:b","%s","-x"};
    for(unsigned i=0;i<sizeof(good)/sizeof(*good);i++)assert(wl14_basename(good[i]));
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!wl14_basename(bad[i]));
    char longname[128];memset(longname,'x',127);longname[127]=0;assert(!wl14_basename(longname));
    char maxname[64];memset(maxname,'x',63);maxname[63]=0;assert(wl14_basename(maxname));
    bdf_arg=maxname;qrtr_arg=4;wl_probe_configure();assert(!wl_variant_invalid && !strcmp(wl14_bdf_name,maxname));
    bdf_arg="../x";qrtr_arg=4;wl_probe_configure();assert(wl_variant_invalid);
    bdf_arg=longname;wl_probe_configure();assert(wl_variant_invalid);
    bdf_arg="bd_m3gl.elf";wl_probe_configure();assert(!wl_variant_invalid && !strcmp(wl14_bdf_name,bdf_arg));
    bdf_arg=NULL;
    const unsigned success[]={0,8};
    for(unsigned i=0;i<2;i++) {
        boot14(success[i],true);assert(!strcmp(wl14_bdf_name,"bd_m3gl.elf"));
        int e=finish_step();if(e)fprintf(stderr,"%s",logbuf);assert(!e && wl_w11_ok);
        assert(wl12_txn==12 && m3_requests==1 && wl14_ready && wl14_init_done);
        assert(strstr(logbuf,"W14 regdb ok") && strstr(logbuf,"W14 bdf bd_m3gl.elf ok") && strstr(logbuf,"W14 m3 ok") && strstr(logbuf,"W14 fw ready"));
        assert(strstr(logbuf,"hang_data_addr_offset 0x12345678") && strstr(logbuf,"hang_data_length 97"));
        assert(strstr(logbuf,"ol_cpr_cfg pmu_param_len 9") && strstr(logbuf,"wake_volt 908") && strstr(logbuf,"share_mem_len 3"));
        unsigned before=ne;wl_finish(28);assert(ne==before && !frees);contain();
    }
    /* Unprogrammed OTP (no board_info TLV, observed on the device): continue with the named BDF. */
    boot14(21,true);{int e=finish_step();if(e)fprintf(stderr,"%s",logbuf);assert(!e && wl_w11_ok && wl14_ready && strstr(logbuf,"no board id (OTP unset)"));}
    {unsigned before=ne;wl_finish(28);assert(ne==before && !frees);contain();}
    boot14(0,false);assert(!finish_step());wl_finish(28);assert(frees==allocations);dev_on=false;
    for(unsigned c=1;c<=26;c++) {
        if(c==8||c==21)continue;
        boot14(c,true);unsigned ticks=sleeps;int e=finish_step();
        if(!e)fprintf(stderr,"unexpected success case %u\n",c);assert(e && !wl_w11_ok && sleeps-ticks<23000);
        if(c<=3)assert(strstr(logbuf,"missing/unreadable file"));
        if(c==1)assert(!dl[0] && !dl[1]);
        if(c==2)assert(dl[0]==2 && !dl[1]);
        if(c==3)assert(dl[1]==4 && !m3_requests && strstr(logbuf,"m3.bin"));
        if(c==6 || c==10 || c==11 || c==12)assert(strstr(logbuf,"timeout"));
        if(c==11 || c==12)assert(!wl14_ready); /* early 0x21 and 0x38 insufficient */
        fail_nth_alloc=0;cache_drop=false;contain();
    }
    /* Published M3 allocation remains owned on failed containment. */
    boot14(11,true);assert(finish_step() && wl14_m3.va);
    fail_bm=fail_perst=true;wl5_abort(28);assert(!frees && wl14_m3.va);
    fail_bm=fail_perst=false;contain();
    /* NC/map failures are fatal before M3_INFO. */
    boot14(0,true);dev_on=false;clear_error();
    unsigned size;assert(!wl14_read("m3.bin",&wl14_m3,WL14_M3_MAX,&size));
    cache_drop=true;assert(wl_dma_nc(&wl14_m3));cache_drop=false;dev_on=true;contain();
    boot14(0,true);dev_on=false;clear_error();
    assert(!wl14_read("m3.bin",&wl14_m3,WL14_M3_MAX,&size));
    wl_dma_clean(&wl14_m3);assert(!wl_dma_nc(&wl14_m3));
    unsigned table=2+((wl14_m3.iova-wl_iova_base)>>21);
    ((uint64_t *)(wl_tables.va+table*4096))[(wl14_m3.iova>>12)&511]=3;
    assert(wl12_map_target(&wl14_m3) && strstr(logbuf,"occupied leaf"));
    dev_on=true;contain();
    puts("W14 BDF EI golden/6144/last/end, vendor TLVs, safe files/args, errors, M3 NC/DMA, ready races/timeouts and quarantine passed");
    return 0;
}
'''


def main():
    preamble = w12.w11.w10.m6o.m6.PREAMBLE
    preamble = preamble.replace('static bool PE_parse_boot_argn(', 'static const char *bdf_arg;\nstatic bool PE_parse_boot_argn(')
    preamble = preamble.replace('    assert(size==sizeof(int));', '    if(!strcmp(name,"fuxiwlanbdf")){if(!bdf_arg)return false;strncpy(out,bdf_arg,size-1);((char *)out)[size-1]=0;return true;}\n    assert(size==sizeof(int));')
    for name in ['vnode_open', 'vnode_getattr', 'vn_rdwr']:
        preamble = preamble.replace('static int '+name+'(', 'static int original_'+name+'(')
    preamble += r'''
static unsigned io_case;
static uint8_t synthetic(unsigned file,unsigned i) {
    if(file==3 && i<4)return (uint8_t[]){0x7f,'E','L','F'}[i];
    return (i*17+3)&255;
}
static int vnode_open(const char *p,int mode,int cmode,int flags,vnode_t *vp,vfs_context_t c) {
    unsigned f=strstr(p,"regdb_xiaomi.bin")?2:strstr(p,"bd_m3")?3:strstr(p,"m3.bin")?4:1;
    if(f==1)return original_vnode_open(p,mode,cmode,flags,vp,c);
    assert(mode==(FREAD|O_NOFOLLOW|O_NONBLOCK));
    assert(!strncmp(p,"/usr/local/share/fuxi/wlan/",27));
    if(io_case==f-1)return 2;*vp=(void *)(uintptr_t)f;return 0;
}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t c) {
    unsigned f=(uintptr_t)v;if(f==1)return original_vnode_getattr(v,a,c);
    a->va_data_size=f==2?12288:f==3?18439:12345;
    if(io_case==4 && f==2)a->va_data_size=1048577;
    if(io_case==13 && f==4)a->va_data_size=2097153;
    if(io_case==14 && f==3)a->va_data_size=0;
    if(io_case==16 && f==3)return 5;
    return 0;
}
static int vn_rdwr(int rw,vnode_t v,char *b,int len,off_t off,int seg,int io,void *cred,int *resid,void *proc) {
    unsigned f=(uintptr_t)v;if(f==1)return original_vn_rdwr(rw,v,b,len,off,seg,io,cred,resid,proc);
    for(int i=0;i<len;i++)b[i]=synthetic(f,i);*resid=io_case==15?1:0;
    return io_case==17?5:0;
}
'''
    w12.w11.w10.m6o.m6.PREAMBLE = preamble
    w12.TESTS = TESTS
    assert 'python3 tools/test-fuxi-wlan-w14.py' in (ROOT / 'tools/ci/build.sh').read_text()
    w12.main()


if __name__ == '__main__':
    main()
