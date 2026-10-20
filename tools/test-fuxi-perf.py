#!/usr/bin/env python3
"""Compile the governor's actual policy with boundary/LUT tests (offline)."""
from pathlib import Path
import argparse
import importlib.util
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xnu-tree', type=Path, help="also compile this tree's actual io_map with VM stubs")
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('submit', ROOT/'tools/test-fuxi-adreno.py')
submit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(submit)
actual = (ROOT/'xnu/fuxi-perf/fuxi_perf.c').read_text()
register_functions = ''.join(submit.function(actual, n) for n in ('fp_read','fp_vote','fp_temperature','fp_init_fail','fp_initialize'))
TEST = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "fuxi-epss-0097.h"
#include "fuxi_perf_logic.h"
#define R(l,n) ((1u<<30)|((n)<<16)|(l))
#define REGISTER_FUNCTIONS
int main(void) {
 uint32_t rows[40] = {R(26,4),R(73,4),R(60,4),R(146,1),R(140,4),R(140,4)};
 unsigned khz;
 assert(fp_level(rows,40,1401600,&khz)==1 && khz==1401600);
 assert(fp_level(rows,40,UINT32_MAX,&khz)==4 && khz==2688000);
 assert(fp_level(rows,40,100000,&khz)==-1 && !khz);
 rows[0]=0; assert(fp_level(rows,40,300000,&khz)==0 && khz==300000);
 rows[0]=R(26,1);rows[1]=R(26,1);
 assert(fp_level(rows,40,UINT32_MAX,&khz)==-1); /* turbo only */
 /* Exact temperature and inactivity boundaries, in arbitrary time units. */
 assert(!fp_want_boost(0,1,300,2000,0,1999,1500)); /* swaps cannot start boost */
 assert(fp_want_boost(0,1,600,2000,1999,0,1500));
 assert(!fp_want_boost(0,1,601,2000,1999,0,1500));
 assert(fp_want_boost(1,1,680,2000,1999,0,1500));
 assert(!fp_want_boost(1,1,681,2000,1999,0,1500));
 assert(!fp_want_boost(1,0,400,2000,1999,0,1500)); /* invalid fails cool */
 assert(fp_want_boost(0,1,400,2000,500,0,1500));
 assert(!fp_want_boost(0,1,400,2001,500,0,1500));
 assert(fp_want_boost(1,1,400,4000,500,3999,1500)); /* swaps extend */
 assert(!fp_want_boost(1,1,400,5500,500,3999,1500));
 assert(!fp_want_boost(0,1,400,2000,2001,0,1500));
 for(unsigned d=0;d<3;d++) {
  fp_percore[d]=1; assert(fp_vote(d,7)); assert(writes[d]==fp_cores[d]);
  fp_percore[d]=0; assert(fp_vote(d,2)); assert(writes[d]==fp_cores[d]+1);
 }
 for(unsigned d=0;d<2;d++) {
  regs[d+3][0xe4/4]=1;
  for(unsigned n=1;n<=(d?3:15);n++) regs[d+3][(0xa0+4*n)/4]=(1u<<21)|450;
 }
 int temp;
 regs[3][(0xa0+4*15)/4]=(1u<<21)|681;
 assert(fp_temperature(&temp) && temp==681);
 regs[4][(0xa0+4*3)/4]=450; assert(fp_temperature(&temp) && temp==681);
 regs[4][(0xa0+4*3)/4]=(1u<<21)|0xff6; /* -1 C, signed */
 assert(fp_temperature(&temp) && temp==681);
 regs[3][0xe4/4]=0;assert(!fp_temperature(&temp));
 /* Full init uses real frequency words/counts and stops after the first
  * repeated frequency. A fault at every later tail row must be irrelevant. */
 reset(); assert(fp_initialize() && log_count==1 && strstr(last_log,"ready"));
 for(unsigned d=0;d<3;d++) {
  assert(lut_reads[d]==observed_count[d]+1);
  assert(fp_cool[d]==(d==2?6:8));
  assert(fp_high[d]==(d==0?8:19));
  assert(fp_high_khz[d]==(d==0?1228800:d==1?2803200:2956800));
 }
 for(unsigned d=0;d<3;d++) for(unsigned i=observed_count[d]+1;i<40;i++) {
  reset(); fault_block=d;fault_off=0x100+4*i;
  assert(fp_initialize() && log_count==1 && strstr(last_log,"ready"));
 }
 /* Every init failure emits exactly one line with the failed domain,
  * offset, value and access status. No init path may raise a vote. */
 for(unsigned d=0;d<5;d++) {
  reset(); fail_map=d+1;
  assert(!fp_initialize() && log_count==1 && strstr(last_log,d<3?"step=map":"step=tsens map"));
  assert(strstr(last_log,"value=0 access=0"));
 }
 for(unsigned d=0;d<3;d++) {
  reset();regs[d][0]=2;
  assert(!fp_initialize() && log_count==1 && strstr(last_log,"step=enable") && strstr(last_log,"value=0x2 access=1"));
  reset();fault_block=d;fault_off=0;
  assert(!fp_initialize() && log_count==1 && strstr(last_log,"step=enable") && strstr(last_log,"value=0xffffffff access=0"));
  reset();fault_block=d;fault_off=0xb0;
  assert(!fp_initialize() && log_count==1 && strstr(last_log,"step=dcvs"));
  for(unsigned i=0;i<=observed_count[d];i++) {
   reset();fault_block=d;fault_off=0x100+4*i;
   assert(!fp_initialize() && log_count==1 && strstr(last_log,"step=LUT"));
   char expect[64];snprintf(expect,sizeof(expect),"row=%u",i);assert(strstr(last_log,expect));
   snprintf(expect,sizeof(expect),"offset=%#x",fault_off);assert(strstr(last_log,expect));
   snprintf(expect,sizeof(expect),"domain=%s(%u)",fp_domain[d],d);assert(strstr(last_log,expect));
  }
  reset();regs[d][0x100/4]=regs[d][0x104/4]=R(26,1);
  assert(!fp_initialize() && log_count==1 && strstr(last_log,"step=level selection"));
 }
 reset();for(unsigned d=0;d<3;d++) {
  /* A full 40-row table needs no duplicate or read of slot 40. */
  for(unsigned i=0;i<40;i++) regs[d][0x100/4+i]=R(16+i,3);
  observed_tail_fault=0;
 }
 assert(fp_initialize());
 unsigned khz2;assert(fp_level(rows,0,UINT32_MAX,&khz2)==-1 && !khz2);
 /* Quorum, read errors, signed/plausible limits and no stale hottest value. */
 for(unsigned invalid=0;invalid<=18;invalid++) {
  reset();unsigned index=0;
  for(unsigned d=0;d<2;d++) for(unsigned n=1;n<=(d?3:15);n++,index++)
   regs[d+3][(0xa0+4*n)/4]=(index<invalid?0:1u<<21)|550;
  int good=fp_temperature(&temp);
  assert(good==(invalid<=4));
  if(good) { assert(temp==550);assert(fp_want_boost(0,good,temp,2000,1999,0,1500)); }
  else assert(!fp_want_boost(1,good,temp,2000,1999,0,1500));
 }
 reset();regs[4][0xe4/4]=0;assert(fp_temperature(&temp) && temp==450); /* 15/18 */
 reset();regs[3][0xe4/4]=0;assert(!fp_temperature(&temp)); /* 3/18 */
 reset();fault_block=4;fault_off=0xe4;assert(fp_temperature(&temp));
 reset();fault_block=3;fault_off=0xe4;assert(!fp_temperature(&temp));
 reset();fault_block=3;fault_off=0xa4;assert(fp_temperature(&temp) && temp==450);
 for(unsigned d=0;d<2;d++) for(unsigned n=1;n<=(d?3:15);n++) {
  reset();regs[d+3][(0xa0+4*n)/4]=450;
  assert(fp_temperature(&temp) && temp==450);
  assert(fp_want_boost(0,1,temp,2000,1999,0,1500));
  assert(fp_want_boost(1,1,temp,2000,1999,0,1500));
  regs[d+3][(0xa0+4*n)/4]=(1u<<21)|681;
  assert(fp_temperature(&temp) && temp==681 && !fp_want_boost(1,1,temp,2000,1999,0,1500));
 }
 reset();for(unsigned d=0;d<2;d++) for(unsigned n=1;n<=(d?3:15);n++)
  regs[d+3][(0xa0+4*n)/4]=(1u<<21)|0xff6;
 assert(fp_temperature(&temp) && temp==-10); /* all sensors -1 C, signed */
 reset();regs[3][0xa4/4]=(1u<<21)|1501;assert(fp_temperature(&temp) && temp==450);
 regs[3][0xa4/4]=(1u<<21)|((-401)&0xfff);assert(fp_temperature(&temp) && temp==450);
 regs[3][0xa4/4]=(1u<<21)|1500;assert(fp_temperature(&temp) && temp==1500);
 regs[3][0xa4/4]=(1u<<21)|((-400)&0xfff);assert(fp_temperature(&temp) && temp==450);
#undef printf
 puts("PASS: real 0097 LUTs/init failures/16K mappings/14-of-18 sensor quorum; LUT decode/duplicates/turbo/nonmonotonic selection, thermal 60/68 boundaries, input-only entry, swap extension, 1500 ms expiry, per-core vote/readback and signed/invalid CPU sensors");
}
'''
REGISTERS = r'''
static uint32_t regs[5][1024], writes[3], lut_reads[3];
static uintptr_t fp_epss[3]={0x1000,0x2000,0x3000},fp_tsens[2]={0x4000,0x5000};
static const uint64_t fp_epss_pa[3]={0x17d91000,0x17d92000,0x17d93000};
static const uint64_t fp_tsens_pa[2]={0xc271000,0xc272000};
static const char * const fp_domain[3]={"silver","gold","prime"};
static unsigned fp_percore[3],fp_cool[3],fp_high[3],fp_cool_khz[3],fp_high_khz[3];
static const unsigned fp_cores[3]={3,4,1};
static const unsigned fp_cool_cap[3]={1228800,1401600,1363200};
static unsigned maps,fail_map,log_count,observed_tail_fault;
static int fault_block=-1;
static unsigned fault_off;
static char last_log[512];
/* Model exactly io_map's trunc_page / round_page / start+offset contract;
 * use separate virtual 16K pages, even for aliases of the same physical page. */
static uintptr_t ml_io_map(uint64_t pa,unsigned size) {
 assert(size==0x1000);unsigned d;
 for(d=0;d<5;d++) if(pa==(d<3?fp_epss_pa[d]:fp_tsens_pa[d-3])) break;
 assert(d<5);
 unsigned offset=pa&0x3fff;
 assert(((size+offset+0x3fff)&~0x3fff)==0x4000);
 if(++maps==fail_map)return 0;
 return 0x100000+0x4000*d+offset;
}
static int model_printf(const char *fmt,...) {
 va_list ap;va_start(ap,fmt);vsnprintf(last_log,sizeof(last_log),fmt,ap);va_end(ap);log_count++;return 0;
}
#define printf model_printf
static unsigned fuxi_gpu_access(uintptr_t address,uint64_t *v,unsigned wr,unsigned wide) {
 assert(!wide);unsigned block,off;
 if(address<0x100000) {block=(address>>12)-1;off=address&0xfff;}
 else {
  block=(address-0x100000)/0x4000;
  assert(block<5);
  unsigned offset=(block<3?fp_epss_pa[block]:fp_tsens_pa[block-3])&0x3fff;
  assert((address&0x3fff)>=offset);
  off=(address&0x3fff)-offset;
 }
 assert(block<5 && off<0x1000 && !(off&3));
 if((int)block==fault_block && off==fault_off)return 1;
 if(block<3 && off>=0x100 && off<0x1a0) {
  unsigned row=(off-0x100)/4;lut_reads[block]++;
  if(observed_tail_fault && row>observed_count[block])return 1;
 }
 if(wr) { assert(!maps && block<3);writes[block]++;regs[block][off/4]=*v; }
 else *v=regs[block][off/4];
 return 0;
}
static void reset(void) {
 memset(regs,0,sizeof(regs));memset(writes,0,sizeof(writes));memset(lut_reads,0,sizeof(lut_reads));
 memset(fp_cool_khz,0,sizeof(fp_cool_khz));memset(fp_high_khz,0,sizeof(fp_high_khz));
 maps=fail_map=log_count=0;fault_block=-1;fault_off=0;observed_tail_fault=1;
 for(unsigned d=0;d<3;d++) {
  regs[d][0]=3;regs[d][0xb0/4]=0x9b30;
  memcpy(&regs[d][0x100/4],observed_rows[d],sizeof(observed_rows[d]));
 }
 for(unsigned d=0;d<2;d++) {
  fp_tsens[d]=0x100000+0x4000*(d+3)+(fp_tsens_pa[d]&0x3fff);
  regs[d+3][0xe4/4]=1;
  for(unsigned n=1;n<=(d?3:15);n++)regs[d+3][(0xa0+4*n)/4]=(1u<<21)|450;
 }
}
'''
TEST = TEST.replace('#define REGISTER_FUNCTIONS', REGISTERS + register_functions)
# Verify patch snapshots are exact and hooks are present in the final patch.
patch = (ROOT/'xnu/patches/0097-fuxi-touch-boost.patch').read_text()
for name in ('fuxi_perf.c','fuxi_perf_logic.h'):
    block = re.search(r'^--- a/bsd/dev/'+name+r'\n.*?(?=^--- a/|\Z)',patch,re.M|re.S)[0]
    added = ''.join(l[1:] for l in block.splitlines(True) if l.startswith('+') and not l.startswith('+++'))
    assert added == (ROOT/'xnu/fuxi-perf'/name).read_text()
assert patch.count('+\tfuxi_perf_input();') == 1 and '+\t\t\t\tfuxi_perf_input();' in patch
with tempfile.TemporaryDirectory() as d:
    source, exe = Path(d)/'perf.c', Path(d)/'perf'
    source.write_text(TEST)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I',str(ROOT/'xnu/fuxi-perf'),'-I',str(ROOT/'tools/fixtures'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

if args.xnu_tree:
    io_source = (args.xnu_tree/'osfmk/arm/io_map.c').read_text()
    # Keep the entire function including both early and VM-running branches.
    start = io_source.index('vm_offset_t\nio_map(')
    io_function = io_source[start:]
    assert io_function.rstrip().endswith('}'), 'io_map must be the final function'
    io_model = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t, vm_map_offset_t, vm_size_t;
typedef int vm_prot_t, kma_flags_t, bool;
#define STARTUP_SUB_KMEM 1
#define VM_WIMG_WCOMB 2
#define VM_WIMG_IO 3
#define PMAP_MAP_BD_WCOMB 0
#define KMA_NOFAIL 1
#define KMA_PAGEABLE 2
#define KMA_DATA 4
#define KMA_PERMANENT 8
#define VM_KERN_MEMORY_IOKIT 1
#define round_page(x) (((x)+16383)&~(vm_offset_t)16383)
#define trunc_page(x) ((x)&~(vm_offset_t)16383)
static unsigned startup_phase, maps;
static vm_offset_t virtual_space_start, last_va, last_pa, last_end, allocation;
static int kernel_map=1,io_submap=2,kasan_notify_address;
static void pmap_map(intptr_t va,uintptr_t pa,uintptr_t end,int prot,unsigned flags) {
 assert(prot==3 && flags==VM_WIMG_IO);last_va=va;last_pa=pa;last_end=end;maps++;
}
static void pmap_map_bd(uintptr_t va,uintptr_t pa,uintptr_t end,int prot) {pmap_map(va,pa,end,prot,VM_WIMG_IO);}
static void pmap_map_bd_with_options(uintptr_t va,uintptr_t pa,uintptr_t end,int prot,int opts) {(void)opts;pmap_map_bd(va,pa,end,prot);}
static void kmem_alloc(int map,uintptr_t *va,uintptr_t size,int flags,int tag) {
 assert(map==io_submap && flags==(KMA_NOFAIL|KMA_PAGEABLE|KMA_PERMANENT) && tag==VM_KERN_MEMORY_IOKIT);
 allocation=size;*va=0x800000;
}
'''
    io_cases = r'''
int main(void) {
 uintptr_t addresses[]={0x17d91000,0x17d92000,0x17d93000,0xc271000,0xc272000,0xae36000};
 for(startup_phase=0;startup_phase<=1;startup_phase++) for(unsigned i=0;i<6;i++) {
  uintptr_t pa=addresses[i];virtual_space_start=0x800000;
  uintptr_t va=io_map(pa,0x1000,VM_WIMG_IO,3,0);
  assert(last_pa==(pa&~(uintptr_t)16383) && last_end-last_pa==16384);
  assert(va==last_va+(pa&16383));
  if(startup_phase)assert(allocation==16384);
  else assert(virtual_space_start==0x804000);
 }
 assert(maps==12);(void)kasan_notify_address;
 puts("PASS: actual XNU io_map, early/VM paths, all EPSS/TSENS/known display offsets on 16K pages");
}
'''
    with tempfile.TemporaryDirectory() as d:
        source, exe = Path(d)/'io.c', Path(d)/'io'
        source.write_text(io_model + io_function + io_cases)
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
