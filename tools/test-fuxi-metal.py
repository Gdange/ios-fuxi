#!/usr/bin/env python3
"""Offline NativeAdreno ABI/framing and actual C backend tests (ASan/UBSan).

Models page tables, register observations and scheduler callbacks. Does not
establish real CP/BR/BV execution, SMMU switching or DMA/cache coherency.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT/'xnu/fuxi-adreno'
WIRE = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "fuxi_abi_wire.h"
#define SZ(t,n) static_assert(sizeof(t)==n,#t)
SZ(NativeAdrenoAbiBoAllocReply,8); SZ(NativeAdrenoAbiBoMapReply,8);
SZ(NativeAdrenoAbiBoFree,8); SZ(NativeAdrenoAbiSubmitReply,8);
static_assert(offsetof(NativeAdrenoAbiSubmit,buffers)==32,"buffers offset");
static_assert(offsetof(NativeAdrenoAbiDeviceInfo,flags)==56,"flags offset");
static unsigned bits(uint32_t x) { return __builtin_popcount(x); }
static unsigned packet(const uint32_t *w,unsigned n,unsigned *op,unsigned *len) {
 unsigned type=w[0]>>28;
 assert(type==4 || type==7);
 *len=type==7 ? w[0]&0x3fff : w[0]&0x7f;
 *op=type==7 ? (w[0]>>16)&0x7f : (w[0]>>8)&0x3ffff;
 assert((bits(*len)+((w[0]>>(type==7?15:7))&1))%2==1);
 assert((bits(*op)+((w[0]>>(type==7?23:27))&1))%2==1);
 assert(*len<n); return type;
}
int main() {
 NativeAdrenoAbiParams query={};NativeAdrenoAbiDeviceInfo info={};
 assert(!deviceQuery(NativeAdrenoMethodQuery,nullptr,&query));
 assert(query.abi_version==1 && query.input_bits==31 && query.output_bits==36 &&
     query.flags==0 && query.max_bo_bytes==(1u<<28));
 assert(!deviceQuery(NativeAdrenoMethodGetDeviceInfo,nullptr,&info));
 /* Decode the actual page-granular 2b readback, restoring inclusive end. */
 const uint32_t min=0x1000000,max=0x12ff000;
 assert((max|0xfff)-min+1==0x300000);
 assert(info.chip_id==0x43050a01 && info.gmem_base==min &&
     info.gmem_bytes==(max|0xfff)-min+1 && info.gmem_bytes==3u*1024*1024);
 const unsigned is[]={0,0,16,32,8,416,16,0,0,80,80,8,8,8,16,0,16};
 const unsigned os[]={0,24,8,8,0,8,0,8,16,0,80,8,0,16,0,64,8};
 for(unsigned s=1;s<=16;s++) {assert(fg_abi_input_size(s)==is[s]);assert(fg_abi_output_size(s)==os[s]);}
 assert(!fg_abi_range(0x1ffff000,4096)); assert(!fg_abi_range(0x80000000,4096));
 assert(fg_abi_range(0x7ffff000,4096)); assert(!fg_abi_range(0x7ffff000,8192));
 assert(!fg_abi_range(UINT64_MAX-4095,8192)); assert(!fg_abi_range(0x20000000,0));
 assert(!fg_abi_range(0x20000001,4096)); assert(!fg_abi_range(0x20000000,4095));
 for(unsigned align=0;align<32;align++)
  for(unsigned contig=0;contig<4;contig++)
   assert(fg_abi_alloc_valid(4096,align,contig)==(align>=12 && align<=24 && contig<=1));
 assert(!fg_abi_alloc_valid(0,12,0)); assert(!fg_abi_alloc_valid(4097,12,0));
 assert(fg_abi_alloc_valid(1u<<28,24,1)); assert(!fg_abi_alloc_valid((1u<<28)+4096,12,0));
 for(unsigned perm=0;perm<8;perm++) for(unsigned wb=0;wb<3;wb++) for(unsigned sh=0;sh<5;sh++)
  assert(fg_abi_map_valid(0x20000000,4096,perm,wb,sh,0)==((perm==1||perm==3)&&wb<=1&&(sh==2||sh==3)));
 assert(!fg_abi_map_valid(0x20000000,4096,3,0,2,1));
 assert(fg_abi_ib_valid(0x20000000,4096,0x20000004,4092));
 assert(!fg_abi_ib_valid(0x20000000,4096,0x20000004,4096));
 assert(!fg_abi_ib_valid(0x20000000,4096,0x20000000,0));
 assert(!fg_abi_ib_valid(0x20000000,4096,0x20000001,4));
 assert(!fg_abi_ib_valid(0x20000000,4096,UINT64_MAX-3,8));
 uint64_t va[16],bytes[16]; uint32_t w[160];
 for(unsigned i=0;i<16;i++) {va[i]=0x20000000+i*4096;bytes[i]=4096;}
 for(unsigned reset=0;reset<2;reset++) for(unsigned count=1;count<=16;count++) {
  unsigned n=fg_abi_frame(w,va,bytes,count,0x10029080,13,14,reset);
  assert(n==11+26*reset+count*4+31+1 && n<=160);
  unsigned at=0,op=0,len=0,ibs=0,bv=0,br=0,sram=0,resets=0,ifpc=0,thread=0,waitBv=0;
  /* Independent decoder asserts ordering and address/value pairs. */
  while(at<n) {
   unsigned type=packet(w+at,n-at,&op,&len);
   const uint32_t *p=w+at+1;
   if(type==7 && op==0x17) thread=p[0]&3;
   if(type==7 && op==0x1f) {assert(p[0]==15 && thread==3);resets++;}
   if(type==7 && op==0x3c) {assert(len==6 && p[0]==3 && p[1]==0x50f && p[3]==1 && p[4]==1);sram++;}
   if(type==7 && op==0x65) {assert(p[0]==(ifpc?0x100u:0x101u));ifpc++;}
   if(type==7 && op==0x3f) {assert(ifpc==1 && thread==3 && sram==reset && resets==reset);assert(len==3 && p[0]==va[ibs] && !p[1] && p[2]==1024);ibs++;}
   if(type==7 && op==0x46 && len==4) {
    assert(p[3]==14 && !p[2]);
    if(thread==2) {assert(p[0]==0x08000031 && p[1]==0x1002908c);bv++;}
    else {assert(thread==1 && bv==1 && waitBv==1 && p[0]==0x88000031 && p[1]==0x10029084);br++;}
   }
   if(type==7 && op==0x14) {
    assert(thread==1 && len==4 && !p[0] && !p[2]);
    if(p[1]==0x1002908c) {assert(bv==1 && p[3]==14);waitBv++;}
    else {assert(reset && p[1]==0x10029084 && p[3]==13 && !ibs);}
   }
   at+=len+1;
  }
  assert(at==n && ibs==count && bv==1 && br==1 && waitBv==1 && ifpc==2 && thread==3);
  assert(w[n-1]==fg_abi_pkt7(0x26,0));
 }
 assert(!fg_abi_frame(w,va,bytes,0,0x10029080,0,1,1));
 assert(!fg_abi_frame(w,va,bytes,17,0x10029080,0,1,1));
 assert(!fg_abi_frame(w,va,bytes,1,0x10029080,0,0,1));
 bytes[0]=0; assert(!fg_abi_frame(w,va,bytes,1,0x10029080,0,1,1));
 puts("PASS: all 16 wire layouts/selectors, bounds/overflow/flags, independent PM4 decode, reset/SRAM/IFPC/BV/BR/IB framing");
}
'''
BACKEND_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "fuxi_abi_wire.h"
#define FG_GUARD
#define G_MEM 0x9fe000000ULL
#define G_MEM_SIZE 0x2000000
#define G3_PT (G_MEM+0x1200000)
#define G3_SQE_PA (G_MEM+0x1300000)
#define G3_FENCE_PA (G_MEM+0x1329000)
#define G3_SQE_VA 0x10000000u
#define G3_FENCE_VA 0x10029000u
#define G4_DST_PA (G_MEM+0x1501000)
#define G4_BYTES 0x40000
#define G5_META_PA (G_MEM+0x1600000)
#define G5_META_BYTES 0x2000
#define FG_HANDOFF (G_MEM+0x1800000)
#define FG_FW_BASE (FG_HANDOFF+0x4000)
#define FG_FW_SLOT 0x100000
#define G4_RB_NC 0x8e08
#define G4_TPL1_NC 0xb604
#define G4_SP_NC 0xae02
#define G4_GRAS_NC 0x8602
#define G3_APERTURE_HOST 0xa00
#define SMMU_ID2 0x28
#define PAGE_SHIFT 14
#define PAGE_MASK 16383u
#define NSEC_PER_MSEC 1000000
#define THREAD_CALL_PRIORITY_KERNEL 1
#define THREAD_CALL_OPTIONS_ONCE 1
#define kernel_pmap 0
static uint8_t scratch[G_MEM_SIZE] __attribute__((aligned(16384)));
#define G_PTR(pa) ((uintptr_t)(scratch+((pa)-G_MEM)))
#define G_PTR_RANGE(pa, bytes) G_PTR(pa)
typedef unsigned ppnum_t;
typedef void *thread_call_param_t;
typedef struct {void (*fn)(void *,void *);} thread_call_data_t;
static uint64_t ticks;
static uint64_t counter(const char *name) {return strcmp(name,"cntfrq_el0")?ticks:1000000;}
#define rsr(r) counter(r)
static unsigned fg_access_failed,fg_busy,fg_client_busy,g3_wptr,g3_bank=2,g3_asid=3;
static unsigned state=1,drains,drainGood=1,fault,tlbs,failTlb,doorbells,enqueued;
static uint32_t regs[0x10000],sregs[0x1000],ring[8192];
static unsigned readFault,sreadFault,writeFail,writeCount,logLines,dumpLines,hangLines;
static char lastLog[256],hangLog[16][256];
static int trace(const char *fmt,...) {
 va_list ap;va_start(ap,fmt);vsnprintf(lastLog,sizeof(lastLog),fmt,ap);va_end(ap);
 logLines++;if(strstr(lastLog,"init regs"))dumpLines++;if(strstr(lastLog,"FUXI GPU HANG:")){assert(hangLines<16);strcpy(hangLog[hangLines++],lastLog);}return 0;
}
#define printf trace
static unsigned g_cb(unsigned bank) {return bank*0x100;}
static uint32_t g3_gr(unsigned reg) {assert(reg<0x10000);if(reg==readFault)fg_access_failed=1;return regs[reg];}
static int g3_gw(unsigned reg,uint32_t value) {if(++writeCount==writeFail) {fg_access_failed=1;return 0;} regs[reg]=value; if(reg==0x807) doorbells++; return 1;}
static uint32_t g3_sr(unsigned reg) {if(sreadFault)fg_access_failed=1;return sregs[reg/4];}
static int g3_sw(unsigned reg,uint32_t value) {assert(!fg_busy);sregs[reg/4]=value;return 1;}
static void g_zero(uint64_t pa,unsigned bytes) {assert(pa>=G_MEM && pa+bytes<=G_MEM+G_MEM_SIZE);memset((void *)G_PTR(pa),0,bytes);}
static uint32_t g3_read(uint64_t pa) {return *(uint32_t *)G_PTR(pa);}
static void g_wr(uint64_t pa,unsigned off,uint32_t value) {*(uint32_t *)G_PTR(pa+off)=value;}
static int g3_faulted(void) {return fault;}
static int g4_tlb(uint32_t *d) {(void)d;tlbs++;return failTlb;}
static int fg_isolate(void) {assert(hangLines==16);drains++;return drainGood;}
static void fg_fail(const char *why,unsigned code) {(void)why;(void)code;state=2;}
static int fuxi_gpu_ready(void) {return state==1;}
static void g3_emit(uint32_t v) {ring[g3_wptr++&8191]=v;}
static ppnum_t pmap_find_phys(unsigned map,uintptr_t va) {(void)map; return (unsigned)(0x200000+(va>>14)*3);}
static void clock_interval_to_deadline(unsigned n,unsigned scale,uint64_t *d) {*d=ticks+(uint64_t)n*scale/1000;}
static void thread_call_setup_with_options(thread_call_data_t *c,void (*fn)(void *,void *),void *arg,unsigned pri,unsigned opt) {(void)arg;(void)pri;(void)opt;c->fn=fn;}
static int thread_call_enter_delayed(thread_call_data_t *c,uint64_t d) {(void)c;(void)d;enqueued++;return 0;}
'''
BACKEND_TEST = r'''
static void seed(void) {
 memset(scratch,0x5a,sizeof(scratch)); memset(regs,0,sizeof(regs));memset(sregs,0,sizeof(sregs));
 regs[0xe07]=regs[0xe09]=0xfffff000;regs[0xe08]=regs[0xe0a]=0x1ffff;regs[0x534]=1;regs[0xe0b]=0x1000000;regs[0xe0d]=0x12ff000;regs[0xe01]=3u<<21;
 sregs[SMMU_ID2/4]=0x5511;
 state=drainGood=1;fg_busy=fg_client_busy=fg_access_failed=0;g3_wptr=0;
 fg_abi_live=fg_abi_seq=fg_abi_completed=fg_abi_poisoned=fg_abi_timed_out=0;fg_abi_hang_dumped=0;
 ticks=tlbs=failTlb=doorbells=enqueued=drains=fault=0;
 readFault=sreadFault=writeFail=writeCount=logLines=hangLines=0;
}
static void reset(void) {
 seed();
 assert(fg_abi_init()); assert(fuxi_gpu_abi_available());
}
static void complete(void) {
 g_wr(G3_FENCE_PA,FG_ABI_FENCE_OFF+4,fg_abi_seq);
 regs[0x806]=regs[0x807]=g3_wptr;regs[0x210]=0;
}
int main(void) {
 /* Every mismatch, read failure and MMIO write failure uses the actual init.
  * The first failure still captures all registers; later attempts never dump.
  * Refusal must precede table construction and ABI publication. */
 const unsigned verified[]={0xe07,0xe08,0xe09,0xe0a,0xe0b,0xe0c,0xe0d,0xe0e,0xe01,0x534};
 const char *names[]={"UCHE_WRITE_THRU_BASE_LO","UCHE_WRITE_THRU_BASE_HI",
  "UCHE_TRAP_BASE_LO","UCHE_TRAP_BASE_HI","UCHE_GMEM_RANGE_MIN_LO",
  "UCHE_GMEM_RANGE_MIN_HI","UCHE_GMEM_RANGE_MAX_LO","UCHE_GMEM_RANGE_MAX_HI",
  "UCHE_MODE_CNTL_HBB","RBBM_NC_MODE_CNTL"};
 for(unsigned i=0;i<10;i++) {
  seed();regs[verified[i]]^=i==8 ? 1u<<21 : i<8 && !(i&1) ? 1u<<12 : 1;
  assert(!fg_abi_init() && !fg_abi_live && !writeCount);
  assert(strstr(lastLog,"init check") && strstr(lastLog,names[i]) && strstr(lastLog,"got 0x") && strstr(lastLog,"want 0x"));
  assert(dumpLines==11 && logLines==(i ? 1u : 12u));
  assert(*(uint64_t *)G_PTR(FG_ABI_PT)==0x5a5a5a5a5a5a5a5aULL);
 }
 seed();sregs[SMMU_ID2/4]=0x5501;
 assert(!fg_abi_init() && strstr(lastLog,"SMMU_ID2_OAS_min got 0x0 want 0x1"));
 for(unsigned i=0;i<10;i++) {
  seed();readFault=verified[i];
  assert(!fg_abi_init() && !writeCount && !fg_abi_live);
  assert(strstr(lastLog,"register_access got 0x1 want 0x0"));
 }
 seed();sreadFault=1;
 assert(!fg_abi_init() && !writeCount && !fg_abi_live);
 assert(strstr(lastLog,"register_access got 0x1 want 0x0") && logLines==1);
 const char *writes[]={"G4_RB_NC","G4_TPL1_NC","G4_SP_NC","G3_APERTURE_HOST_BR",
  "G4_GRAS_NC_BR","G3_APERTURE_HOST_BV","G4_GRAS_NC_BV","G3_APERTURE_HOST_DEFAULT"};
 for(unsigned i=0;i<8;i++) {
  seed();writeFail=i+1;
  assert(!fg_abi_init() && !fg_abi_live && writeCount==i+1);
  assert(strstr(lastLog,"init write") && strstr(lastLog,writes[i]) && strstr(lastLog,"failed"));
  assert(logLines==1 && dumpLines==11);
  assert(*(uint64_t *)G_PTR(FG_ABI_PT)==0x5a5a5a5a5a5a5a5aULL);
 }
 puts("PASS: actual init all register mismatches/read faults/eight write failures, no publication or tables, one-time bounded dump");
 /* Only address low words ignore offset bits; page-address corruption and
  * all high-word/control mismatches above still refuse publication. */
 for(unsigned i=0;i<8;i+=2) for(unsigned bit=0;bit<12;bit++) {
  seed();regs[verified[i]]^=1u<<bit;
  assert(fg_abi_init() && fuxi_gpu_abi_available() && writeCount==8 && !logLines);
 }
 seed();regs[0xe0d]=0x12fffff;assert(fg_abi_init()); /* unrounded model also valid */
 puts("PASS: observed A740 page-granular addresses, every ignored offset bit, inclusive GMEM end and exact high/control checks");
 reset();
 assert(writeCount==8 && !regs[G3_APERTURE_HOST] && !logLines);
 assert(*(uint64_t *)G_PTR(G3_PT)==0x5a5a5a5a5a5a5a5aULL);
 for(uint64_t pa=G5_META_PA;pa<FG_FW_BASE+3*FG_FW_SLOT;pa+=8)
  assert(*(uint64_t *)G_PTR(pa)==0x5a5a5a5a5a5a5a5aULL);
 volatile uint64_t *root=(volatile uint64_t *)G_PTR(FG_ABI_PT);
 assert((root[0]&3)==3 && (root[1]&3)==3 && !root[2]);
 volatile uint64_t *runtime=(volatile uint64_t *)G_PTR(FG_ABI_PT+12288);
 assert((runtime[0]&~4095ULL)==G3_SQE_PA && (runtime[0]&0xc0)==0xc0);
 /* Deliberately cross a 1 GiB root boundary with scattered 16K pages. */
 uint64_t va=0x3fffc000,bytes=32768;uintptr_t source=0x100000;
 assert(!fuxi_gpu_abi_map(va,source,bytes,3,0,2));
 for(unsigned off=0;off<bytes;off+=4096) {
  uint64_t want=((uint64_t)pmap_find_phys(0,source+off)<<14)|((source+off)&16383)|3|1u<<6|2u<<8|1u<<10;
  assert(*fg_abi_leaf(va+off)==want);
 }
 assert(fuxi_gpu_abi_map(va,source,bytes,3,0,2)==-1); /* no replacement */
 assert(!fuxi_gpu_abi_map(0x7ffff000,source,4096,1,1,3));
 assert((*fg_abi_leaf(0x7ffff000)&0xfff)==(3|3u<<6|1u<<2|3u<<8|1u<<10));
 assert(fuxi_gpu_abi_map(0x80000000,source,4096,3,0,2)==-1);
 assert(*(uint64_t *)G_PTR(G3_PT)==0x5a5a5a5a5a5a5a5aULL);
 uint64_t sizes[1]={4096},addresses[1]={va},fence=0;
 fg_busy=1;
 assert(fuxi_gpu_abi_submit(addresses,sizes,1,&fence)==1 && !doorbells);
 fg_busy=0;
 assert(!fuxi_gpu_abi_submit(addresses,sizes,1,&fence) && fence==1 && fg_client_busy && enqueued==1);
 assert(sregs[(g_cb(g3_bank)+0x20)/4]==(uint32_t)FG_ABI_PT);
 assert(fuxi_gpu_abi_submit(addresses,sizes,1,&fence)==1 && doorbells==1);
 assert(fuxi_gpu_abi_unmap(va,bytes)==1);
 assert(fuxi_gpu_abi_map(0x30000000,source,4096,3,0,2)==1);
 assert(fuxi_gpu_abi_poll(fence)==1);
 unsigned schedules=enqueued;fg_abi_watch(NULL,NULL);assert(enqueued==schedules+1);
 complete(); ticks=200000; /* completion wins despite a late watchdog */
 fg_abi_watch(NULL,NULL);
 assert(!fg_client_busy && !fuxi_gpu_abi_poll(fence) && !drains);
 assert(sregs[(g_cb(g3_bank)+0x20)/4]==(uint32_t)G3_PT);
 assert(!fuxi_gpu_abi_unmap(va,bytes));
 for(unsigned off=0;off<bytes;off+=4096) assert(!*fg_abi_leaf(va+off));
 assert(!fuxi_gpu_abi_map(va,source,bytes,3,0,2)); /* reuse after TLBI */
 /* Client never calls WaitFence: delayed callback must still stop DMA. */
 for(unsigned good=0;good<2;good++) {
  reset();drainGood=good;
  assert(!fuxi_gpu_abi_submit(addresses,sizes,1,&fence));
  regs[0x928]=0x23450000;regs[0x92a]=0x90;sregs[(g_cb(g3_bank)+0x58)/4]=0xc0000002;
  unsigned beforeWrites=writeCount;
  ticks=100001;fg_abi_watch(NULL,NULL);
  assert(state==2 && drains==1 && !fg_client_busy && !fuxi_gpu_abi_available());
  assert(fuxi_gpu_abi_poll(fence)==(good?-3:-4));
  assert(hangLines==16 && writeCount==beforeWrites);
  assert(strstr(hangLog[3],"CP_IB1_BASE_LO [0x928]=0x23450000"));
  assert(strstr(hangLog[5],"CP_IB1_REM_SIZE [0x92a]=0x90"));
  assert(strstr(hangLog[14],"FSR 0xc0000002"));
  fg_abi_watch(NULL,NULL);assert(hangLines==16 && drains==1);
  assert(fuxi_gpu_abi_unmap(va,bytes)==(good?0:-2));
 }
 reset();assert(!fuxi_gpu_abi_submit(addresses,sizes,1,&fence));fault=1;
 assert(fuxi_gpu_abi_poll(fence)==-1 && drains==1 && state==2);
 reset();failTlb=1;assert(fuxi_gpu_abi_submit(addresses,sizes,1,&fence)==-1 && !doorbells && drains==1);
 puts("PASS: actual backend root isolation, 16K/4K scattered leaves, VA reuse/TLBI, present exclusion, late retirement, independent watchdog/fault/drain/quarantine");
}
'''


def main():
    subprocess.run(['python3', str(ROOT/'tools/sync-fuxi-adreno-shared.py'), '--check'], check=True)
    with tempfile.TemporaryDirectory() as temp:
        out = Path(temp)
        wire = out/'wire.cpp'
        client=(HEADERS/'FuxiAdrenoUserClient.cpp').read_text()
        query=client[client.index(' IOReturn run('):client.index('  if (method==NativeAdrenoMethodWaitFence)')]
        query=query.replace(' IOReturn run(', 'static int deviceQuery(')+' (void)in;return -1;\n}\n'
        wire.write_text(WIRE.replace('int main() {', 'static constexpr int kIOReturnSuccess=0;\n'+query+'\nint main() {'))
        backend = (HEADERS/'fuxi_gpu_abi_backend.inc').read_text()
        backend = re.sub(r'__asm__ volatile\("dsb sy" ::: "memory"\);', '(void)0;', backend)
        model = out/'backend.c'
        model.write_text(BACKEND_PREFIX + backend + BACKEND_TEST)
        for compiler, source in [('clang++', wire), ('clang', model)]:
            binary = source.with_suffix('')
            subprocess.run([compiler, '-std=c++17' if compiler=='clang++' else '-std=gnu11',
                '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I', str(HEADERS), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        # Apply 0098 to baseline snapshots with a real patch engine, checking
        # exact native source bytes (no Apple source/binary fixture in git).
        import importlib.util
        spec = importlib.util.spec_from_file_location('metal_sync', ROOT/'tools/sync-fuxi-metal.py')
        sync = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sync)
        tree=out/'patchtree'
        for name in ('gpu.c','gpu-a740.h','gpu-zap.h','gpu-handoff.h','fuxi_adreno.c','fuxi_adreno.h'):
            path=tree/'bsd/dev'/name; path.parent.mkdir(parents=True,exist_ok=True);path.write_text(sync.baseline(name))
        conf=tree/'iokit/conf/files.arm64';conf.parent.mkdir(parents=True,exist_ok=True)
        conf.write_text(sync.CONFIG_BASE)
        subprocess.run(['patch','-s','-p1','--fuzz=0','-i', str(ROOT/'xnu/patches/0098-fuxi-native-adreno-abi.patch')],cwd=tree,check=True)
        delta=(ROOT/'xnu/patches/0100-sweet-execswap-gpu-abi-trace.patch').read_text()
        delta=delta[delta.index('--- a/bsd/kern/sweet_execswap.h'):]
        subprocess.run(['patch','-s','-p1','--fuzz=0'],input=delta,text=True,cwd=tree,check=True)
        for path, name in sync.FILES.items():
            assert (tree/path).read_bytes()==(HEADERS/name).read_bytes(),path
        for name in ('gpu.c','fuxi_adreno.c','fuxi_adreno.h'):
            folder=HEADERS if name.startswith('fuxi_') else ROOT/'shim/fuxi-probe'
            assert (tree/'bsd/dev'/name).read_bytes()==(folder/name).read_bytes(),name
        print('PASS: clean 0098 + 0100 ABI/helper patch application and canonical source bytes')


if __name__ == '__main__':
    main()
