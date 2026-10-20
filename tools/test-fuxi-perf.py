#!/usr/bin/env python3
"""Compile the governor's actual policy with boundary/LUT tests (offline)."""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('submit', ROOT/'tools/test-fuxi-adreno.py')
submit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(submit)
actual = (ROOT/'xnu/fuxi-perf/fuxi_perf.c').read_text()
register_functions = ''.join(submit.function(actual, n) for n in ('fp_read','fp_vote','fp_temperature'))
TEST = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "fuxi_perf_logic.h"
#define R(l,n) ((1u<<30)|((n)<<16)|(l))
#define REGISTER_FUNCTIONS
int main(void) {
 uint32_t rows[40] = {R(26,4),R(73,4),R(60,4),R(146,1),R(140,4),R(140,4)};
 unsigned khz;
 assert(fp_level(rows,1401600,&khz)==1 && khz==1401600);
 assert(fp_level(rows,UINT32_MAX,&khz)==4 && khz==2688000);
 assert(fp_level(rows,100000,&khz)==-1 && !khz);
 rows[0]=0; assert(fp_level(rows,300000,&khz)==0 && khz==300000);
 rows[0]=R(26,1);rows[1]=R(26,1);
 assert(fp_level(rows,UINT32_MAX,&khz)==-1); /* turbo only */
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
 regs[4][(0xa0+4*3)/4]=450; assert(!fp_temperature(&temp));
 regs[4][(0xa0+4*3)/4]=(1u<<21)|0xff6; /* -1 C, signed */
 assert(fp_temperature(&temp) && temp==681);
 regs[3][0xe4/4]=0;assert(!fp_temperature(&temp));
 puts("PASS: LUT decode/duplicates/turbo/nonmonotonic selection, thermal 60/68 boundaries, input-only entry, swap extension, 1500 ms expiry, per-core vote/readback and signed/invalid CPU sensors");
}
'''
REGISTERS = r'''
static uint32_t regs[5][1024], writes[3];
static uintptr_t fp_epss[3]={0x1000,0x2000,0x3000},fp_tsens[2]={0x4000,0x5000};
static unsigned fp_percore[3];
static const unsigned fp_cores[3]={3,4,1};
static unsigned fuxi_gpu_access(uintptr_t address,uint64_t *v,unsigned wr,unsigned wide) {
 assert(!wide);unsigned block=address>>12,off=(address&0xfff)/4;
 assert(block>=1 && block<=5 && off<1024);block--;
 if(wr) { assert(block<3);writes[block]++;regs[block][off]=*v; }
 else *v=regs[block][off];
 return 0;
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
                    '-I',str(ROOT/'xnu/fuxi-perf'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
