#!/usr/bin/env python3
"""Compile the governor's actual policy with boundary/LUT tests (offline)."""
from pathlib import Path
import argparse
import ast
import struct
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
register_functions = actual[actual.index('static uint64_t fp_duty_used('):
                            actual.index('static int fp_thermal_guard(')]
register_functions += ''.join(submit.function(actual, n) for n in (
    'fp_read','fp_vote','fp_temperature','fp_init_fail','fp_init_report',
    'fp_initialize','fp_try_initialize','fp_update_votes','fp_thermal_guard',
    'fp_arg','fp_configure','fp_worker','fuxi_perf_input','fuxi_perf_swap'))
# Audit the loader's zero-based wire slots and execute the host decoder without
# importing its USB dependencies. The C model below records the governor reads.
probe = (ROOT/'shim/fuxi-probe/probe.c').read_text()
assert 'for (unsigned s=0;s<th_nsens[c];s++) w[4+s]=usb_rd(th_tm[c],0xa0+4*s);' in probe
host = ast.parse((ROOT/'tools/fuxi-xnu-monitor.py').read_text())
nodes = [n for n in host.body if
         (isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'TSENS_NAMES' for t in n.targets)) or
         (isinstance(n, ast.FunctionDef) and n.name == 'thermal_decode')]
namespace = {'struct': struct}
exec(compile(ast.Module(body=nodes, type_ignores=[]), '<host thermal decoder>', 'exec'), namespace)
names = namespace['TSENS_NAMES']
expected = ['cpuss-%d' % i for i in range(4)] + ['cpu-1-%d' % i for i in range(11)] + ['cpu-0-%d' % i for i in range(3)]
assert names[0][1:16] + names[1][1:4] == expected
wire = [0] * 90
wire[:2] = [0x31524854, 3]
for d in range(2):
    wire[7 + 20*d] = 8  # Observed TRDY, bit 0 clear.
    for n in range(16):
        wire[8 + 20*d + n] = (1 << 21) | (400 + 16*d + n)
decoded = namespace['thermal_decode'](struct.pack('<90I', *wire))
for d in range(2):
    for n, name in enumerate(names[d]):
        assert decoded['temps'][name] == (400 + 16*d + n) / 10
print('PASS: loader STATUS(s)=0xa0+4*s from s=0; host names/decoder zero-based CPU mapping')
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
 reset();
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
  regs[d+3][0xe4/4]=8;
  for(unsigned n=1;n<=(d?3:15);n++) regs[d+3][(0xa0+4*n)/4]=(1u<<21)|450;
 }
 int temp;
 regs[3][(0xa0+4*15)/4]=(1u<<21)|681;
 assert(fp_temperature(&temp) && temp==681);
 regs[4][(0xa0+4*3)/4]=450; assert(fp_temperature(&temp) && temp==681);
 regs[4][(0xa0+4*3)/4]=(1u<<21)|0xff6; /* -1 C, signed */
 assert(fp_temperature(&temp) && temp==681);
 regs[3][0xe4/4]=0;assert(fp_temperature(&temp) && temp==681);
 /* Full init uses real frequency words/counts and stops after the first
  * repeated frequency. A fault at every later tail row must be irrelevant. */
 reset_init(); assert(fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"ready"));
 for(unsigned d=0;d<3;d++) {
  assert(lut_reads[d]==observed_count[d]+1);
  assert(fp_cool[d]==(d==2?6:8));
  unsigned capped_khz;
  assert(fp_high[d]==(unsigned)fp_level(observed_rows[d],observed_count[d],fp_high_cap[d],&capped_khz));
  assert(fp_high_khz[d]==capped_khz && capped_khz<=fp_high_cap[d]);
 }
 for(unsigned d=0;d<3;d++) for(unsigned i=observed_count[d]+1;i<40;i++) {
  reset_init(); fault_block=d;fault_off=0x100+4*i;
  assert(fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"ready"));
 }
 /* Every init failure emits exactly one line with the failed domain,
  * offset, value and access status. No init path may raise a vote. */
 for(unsigned d=0;d<5;d++) {
  reset_init(); fail_map=d+1;
  assert(!fp_try_initialize(ticks) && log_count==1 && strstr(last_log,d<3?"step=map":"step=tsens map"));
  assert(strstr(last_log,"value=0 access=0"));
 }
 for(unsigned d=0;d<3;d++) {
  reset_init();regs[d][0]=2;
  assert(!fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"step=enable") && strstr(last_log,"value=0x2 access=1"));
  reset_init();fault_block=d;fault_off=0;
  assert(!fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"step=enable") && strstr(last_log,"value=0xffffffff access=0"));
  reset_init();fault_block=d;fault_off=0xb0;
  assert(!fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"step=dcvs"));
  for(unsigned i=0;i<=observed_count[d];i++) {
   reset_init();fault_block=d;fault_off=0x100+4*i;
   assert(!fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"step=LUT"));
   char expect[64];snprintf(expect,sizeof(expect),"row=%u",i);assert(strstr(last_log,expect));
   snprintf(expect,sizeof(expect),"offset=%#x",fault_off);assert(strstr(last_log,expect));
   snprintf(expect,sizeof(expect),"domain=%s(%u)",fp_domain[d],d);assert(strstr(last_log,expect));
  }
  reset_init();regs[d][0x100/4]=regs[d][0x104/4]=R(26,1);
  assert(!fp_try_initialize(ticks) && log_count==1 && strstr(last_log,"step=level selection"));
 }
 reset_init();for(unsigned d=0;d<3;d++) {
  /* A full 40-row table needs no duplicate or read of slot 40. */
  for(unsigned i=0;i<40;i++) regs[d][0x100/4+i]=R(16+i,3);
  observed_tail_fault=0;
 }
 assert(fp_try_initialize(ticks));
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
 reset();regs[4][0xe4/4]=0;assert(fp_temperature(&temp) && temp==450); /* TRDY is irrelevant */
 reset();regs[3][0xe4/4]=0;assert(fp_temperature(&temp) && temp==450);
 reset();fault_block=4;fault_off=0xe4;assert(fp_temperature(&temp));
 reset();fault_block=3;fault_off=0xe4;assert(fp_temperature(&temp));
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
 /* 0097c loader sample: TRDY=0x8 and decoded CPU temperatures (dC).
  * Reconstruct VALID+temperature; these are not captured full status words. */
 reset();
 const unsigned observed_temp[2][15]={
  {477,477,473,469,465,465,473,457,465,469,457,461,481,469,477},
  {475,475,479}
 };
 for(unsigned d=0;d<2;d++) {
  regs[d+3][0xe4/4]=8;
  for(unsigned n=1;n<=(d?3:15);n++)regs[d+3][(0xa0+4*n)/4]=(1u<<21)|observed_temp[d][n-1];
 }
 assert(fp_temperature(&temp) && temp==481);
 assert(tsens_reads[0]==0xfffe && tsens_reads[1]==0xe);
 assert(fp_want_boost(0,1,temp,2000,1999,0,1500));
 /* Exercise actual worker entry, extension, hot cutoff, quorum loss and stats
  * with TRDY bit 0 clear throughout. No input still cannot enter high. */
 fp_worker(0,0);assert(fp_ready && !fp_boosted);
 ticks=101;fuxi_perf_input();fp_worker(0,0);
 assert(fp_boosted && fp_boosts==1 && fp_max_temp==481 && scheduled==121);
 ticks=121;fuxi_perf_swap();fp_worker(0,0);assert(fp_boosted);
 regs[3][(0xa0+4*13)/4]=(1u<<21)|681;
 ticks=141;fp_worker(0,0);assert(!fp_boosted && fp_cuts==1 && fp_blocked && scheduled==161);
 regs[3][(0xa0+4*13)/4]=(1u<<21)|481;
 ticks=161;fuxi_perf_input();fp_worker(0,0);assert(!fp_boosted);
 ticks=660;fp_worker(0,0);assert(!fp_boosted);
 ticks=661;fp_worker(0,0);assert(fp_boosted);
 for(unsigned n=1;n<=5;n++)regs[3][(0xa0+4*n)/4]=480;
 ticks=681;fp_worker(0,0);assert(!fp_boosted && fp_blocked && fp_cuts==2);
 for(unsigned n=1;n<=5;n++)regs[3][(0xa0+4*n)/4]=(1u<<21)|480;
 ticks=10100;fp_worker(0,0);
 assert(strstr(last_log,"stats boosts 2") && strstr(last_log,"max temp 681 dC above 620 dC 40 ms cuts 2 valid 1 state cool"));
 /* Each included CPU sensor must independently control the hottest sample.
  * Every excluded AOSS/non-CPU sensor remains irrelevant, even at 150 C. */
 for(unsigned d=0;d<2;d++) for(unsigned n=0;n<16;n++) {
  reset();regs[d+3][(0xa0+4*n)/4]=(1u<<21)|1500;
  assert(fp_temperature(&temp));
  assert(temp==((n>=1 && n<=(d?3:15))?1500:450));
  assert(tsens_reads[0]==0xfffe && tsens_reads[1]==0xe);
 }
 /* Actual worker: reproduce 0097b's gold enable=2 during AP startup.
  * Frequent input/swaps must neither write votes nor accelerate retries. */
 reset_init();regs[1][0]=2;fp_worker(0,0);
 assert(!fp_ready && fp_enabled && log_count==1 && strstr(last_log,"step=enable"));
 assert(fp_retry_at==1100 && scheduled==1100 && maps==2);
 for(unsigned t=101;t<1100;t++) {
  ticks=t;fuxi_perf_input();fuxi_perf_swap();fp_worker(0,0);
  assert(!fp_ready && !fp_retries && log_count==1 && maps==2);
  assert(!writes[0] && !writes[1] && !writes[2]);
  for(unsigned d=0;d<3;d++) assert(!fp_cool_khz[d]);
 }
 ticks=1100;fp_worker(0,0);assert(fp_retries==1 && maps==2 && log_count==1);
 regs[1][0]=3;ticks=2100;fp_worker(0,0);
 assert(fp_ready && fp_enabled && fp_retries==2 && maps==5 && log_count==2);
 assert(strstr(last_log,"ready") && strstr(last_log,"after 2 retries"));
 for(unsigned d=0;d<3;d++) assert(writes[d]);
 /* Every init failure class recovers without a vote on the failed attempt. */
 for(unsigned kind=0;kind<6;kind++) {
  reset_init();
  if(kind==0)fail_map=1;
  if(kind==1)regs[2][0]=2;
  if(kind==2){fault_block=2;fault_off=0xb0;}
  if(kind==3){fault_block=1;fault_off=0x104;}
  if(kind==4)regs[1][0x100/4]=regs[1][0x104/4]=R(26,1);
  if(kind==5)fail_map=5;
  fp_worker(0,0);assert(!fp_ready && log_count==1);
  for(unsigned d=0;d<3;d++)assert(!writes[d] && !fp_cool_khz[d]);
  fail_map=0;fault_block=-1;regs[2][0]=3;
  memcpy(&regs[1][0x100/4],observed_rows[1],sizeof(observed_rows[1]));
  ticks=1100;fp_worker(0,0);
  assert(fp_ready && fp_retries==1 && log_count==2 && strstr(last_log,"after 1 retries"));
 }
 /* Exhaustion: initial + exactly 60 retries, two diagnostic lines total.
  * The final diagnostic describes the latest failure, not the first one. */
 reset_init();regs[1][0]=2;fp_worker(0,0);regs[1][0]=3;regs[2][0]=2;
 for(unsigned n=1;n<=60;n++) {
  ticks=100+1000*n;fp_worker(0,0);
  assert(fp_retries==n && !fp_ready && log_count==(n==60?2:1));
  assert(!writes[0] && !writes[1] && !writes[2]);
 }
 assert(!fp_enabled && maps==3 && strstr(last_log,"disabled after 60 retries"));
 assert(strstr(last_log,"domain=prime(2)") && strstr(last_log,"value=0x2 access=1"));
 regs[2][0]=3;ticks+=1000;fp_worker(0,0);fuxi_perf_input();
 assert(!fp_ready && log_count==2 && !writes[0]);
 /* Success on the last permitted retry; delayed calls cannot extend 60 s. */
 reset_init();regs[1][0]=2;fp_worker(0,0);
 for(unsigned n=1;n<60;n++){ticks=100+1000*n;fp_worker(0,0);}
 regs[1][0]=3;ticks=60100;fp_worker(0,0);
 assert(fp_ready && fp_retries==60 && log_count==2 && strstr(last_log,"after 60 retries"));
 reset_init();regs[1][0]=2;fp_worker(0,0);
 regs[1][0]=3;ticks=60101;fp_worker(0,0);
 assert(!fp_enabled && !fp_ready && !fp_retries && log_count==2 && !writes[0]);
 /* Runtime unchanged high policy must still notice a dropped enable.
  * Only that domain loses boost; recovered domains receive cool only. */
 reset_init();assert(fp_try_initialize(ticks));fp_update_votes(1);
 unsigned old_writes[3];memcpy(old_writes,writes,sizeof(writes));
 regs[1][0]=2;fp_update_votes(1);
 assert(fp_domain_failed[1] && !fp_domain_failed[0] && !fp_domain_failed[2]);
 assert(!memcmp(old_writes,writes,sizeof(writes)) && log_count==2);
 fp_update_votes(1);assert(log_count==2 && writes[1]==old_writes[1]);
 regs[1][0]=3;fp_update_votes(1);
 assert(regs[1][0x320/4]==fp_cool[1] && writes[1]>old_writes[1]);
 assert(regs[2][0x320/4]==fp_high[2] && log_count==2);
 /* Failed enable reads also skip all writes, independently for each domain. */
 for(unsigned d=0;d<3;d++) {
  reset_init();assert(fp_try_initialize(ticks));fp_update_votes(1);
  unsigned before=writes[d];fault_block=d;fault_off=0;fp_update_votes(1);
  assert(fp_domain_failed[d] && writes[d]==before);
  fault_block=-1;fp_update_votes(1);assert(regs[d][0x320/4]==fp_cool[d]);
 }
 /* Enable can fall between per-core writes; later writes must be skipped. */
 reset_init();assert(fp_try_initialize(ticks));fp_percore[1]=1;drop_block=1;drop_after=1;
 assert(!fp_vote(1,fp_high[1]) && writes[1]==1 && fp_domain_failed[1]);
 assert(!(regs[1][0]&1));
 /* A returned write/readback failure fails just this domain cool. */
 for(unsigned wr=0;wr<2;wr++) {
  reset_init();assert(fp_try_initialize(ticks));fault_block=1;fault_off=0x320;
  fault_write_only=wr;fp_update_votes(1);
  assert(fp_domain_failed[1] && !fp_domain_failed[0] && !fp_domain_failed[2]);
  fault_block=-1;fp_update_votes(1);assert(regs[1][0x320/4]==fp_cool[1]);
 }
 /* Boot arguments: invalid values preserve safety bounds, and lowering the
  * ceiling clamps dependent thresholds. Units match the actual timer API. */
 reset();arg_name="fuxiperflimit";arg_value=900;fp_configure();assert(fp_limit==680);
 arg_value=550;fp_configure();assert(fp_limit==550 && fp_hot==550 && fp_resume==540);
 reset();arg_name="fuxiperfhot";arg_value=610;fp_configure();assert(fp_hot==610);
 arg_name="fuxiperfresume";arg_value=570;fp_configure();assert(fp_resume==570);
 arg_name="fuxiperfrise";arg_value=200;fp_configure();assert(fp_rise==200);
 arg_name="fuxiperffast";arg_value=25;fp_configure();assert(fp_fast_ms==25);
 arg_value=100;fp_configure();assert(fp_fast_ms==25);
 arg_name="fuxiperfbudget";arg_value=123;fp_configure();assert(fp_budget==123);
 /* 77 C open-loop burst profile: force all 18 sensors to the trace. No
  * thermal model is assumed; assert votes, not an impossible lowering of an
  * externally supplied temperature. Every ceiling breach cuts in <=20 ms. */
 const int burst[]={480,490,520,550,620,680,770,776,650,590,580,570};
 reset_init();ticks=81;fp_worker(0,0);
 for(unsigned i=0;i<sizeof(burst)/sizeof(burst[0]);i++) {
  ticks=101+20*i;
  for(unsigned d=0;d<2;d++)for(unsigned n=1;n<=(d?3:15);n++)
   regs[d+3][(0xa0+4*n)/4]=(1u<<21)|burst[i];
  fuxi_perf_input();fp_worker(0,0);
  if(i==0)assert(fp_boosted && scheduled==ticks+20);
  if(i>=3)assert(!fp_boosted);
  if(burst[i]>=680)for(unsigned d=1;d<3;d++)assert(regs[d][0x320/4]==fp_cool[d]);
 }
 assert(fp_cuts==1 && fp_max_temp==776);
 /* Abrupt jump at each possible phase after a fast sample: the existing
  * timer must remove high at the first sample, at most one fast period. */
 for(unsigned phase=1;phase<=20;phase++) {
  reset_init();fp_worker(0,0);ticks=101;fuxi_perf_input();fp_worker(0,0);
  assert(fp_boosted && scheduled==121);
  unsigned crossing=101+phase;ticks=scheduled;
  regs[3][0xa4/4]=(1u<<21)|770;fp_worker(0,0);
  assert(!fp_boosted && ticks-crossing<=20 && fp_cuts==1);
  for(unsigned d=1;d<3;d++)assert(regs[d][0x320/4]==fp_cool[d]);
 }
 /* Sustained 48..70 C ramp: obey timer deadlines and keep touch/swaps live.
  * Every sample at/above 62 C stays cool, without cycling near the guard. */
 reset_init();fp_worker(0,0);unsigned ramp_high=0;
 for(unsigned ms=0;ms<=22000;) {
  ticks=101+ms;int t=480+(int)(ms/100);
  for(unsigned d=0;d<2;d++)for(unsigned n=1;n<=(d?3:15);n++)
   regs[d+3][(0xa0+4*n)/4]=(1u<<21)|t;
  fuxi_perf_input();fuxi_perf_swap();fp_worker(0,0);
  if(fp_boosted)ramp_high++;
  if(t>=620)assert(fp_blocked && !fp_boosted);
  if(t>=680)for(unsigned d=1;d<3;d++)assert(regs[d][0x320/4]==fp_cool[d]);
  ms+=(unsigned)(scheduled-ticks);
 }
 assert(ramp_high);
 /* At a sustained 51..52 C the rate/temperature guards do not latch. The
  * rolling budget still bounds high time in EVERY trailing second, even
  * across nominal window boundaries and with a fractional final sample. */
 for(unsigned budget=123;budget<=200;budget+=77) {
  reset_init();fp_budget_ms=budget;arg_name=0;fp_configure();
  for(unsigned d=0;d<2;d++)for(unsigned n=1;n<=(d?3:15);n++)
   regs[d+3][(0xa0+4*n)/4]=(1u<<21)|510;
  fp_worker(0,0);
  unsigned char high[6000]={0};unsigned rolling=0,on=0;
  for(unsigned ms=0;ms<6000;ms++) {
   ticks=101+ms;
   if(!ms || ticks>=scheduled) {
    for(unsigned d=0;d<2;d++)for(unsigned n=1;n<=(d?3:15);n++)
     regs[d+3][(0xa0+4*n)/4]=(1u<<21)|(510+(ms/1000)%2*10);
    fuxi_perf_input();fuxi_perf_swap();fp_worker(0,0);
    assert(!fp_blocked);
   }
   high[ms]=!!fp_boosted;rolling+=high[ms];on+=high[ms];
   if(ms>=1000)rolling-=high[ms-1000];
   assert(rolling<=budget);
  }
  assert(on>=5*budget && on<=6*budget && fp_cuts>=5);
 }
 /* Hysteresis requires 500 continuous ms <=58 C; jitter, invalid quorum and
  * renewals do not clear it. A fast rise alone and prediction each trip. */
 reset();fp_rate_valid=1;fp_rate_temp=580;
 assert(!fp_thermal_guard(1,582,100,20000000)); /* 10 C/s, below 62 C */
 reset();fp_rate_temp=500;fp_rate_valid=1;
 assert(!fp_thermal_guard(1,540,120,20000000)); /* predicted 74 C */
 assert(!fp_thermal_guard(1,580,140,20000000));
 assert(!fp_thermal_guard(1,581,639,499000000));
 assert(!fp_thermal_guard(1,580,640,1000000));
 assert(!fp_thermal_guard(0,-400,1000,360000000));
 assert(!fp_thermal_guard(1,580,1020,20000000));
 assert(!fp_thermal_guard(1,580,1519,499000000));
 assert(fp_thermal_guard(1,580,1520,1000000));
 reset();fp_hot=680;
 assert(fp_thermal_guard(1,679,100,0));
 assert(!fp_thermal_guard(1,680,120,20000000)); /* ceiling is inclusive */
 /* 1ms input callbacks must neither invent a slope from one quantized tick
  * nor hide a real rise by continually resetting the derivative history. */
 reset();assert(fp_thermal_guard(1,510,100,0));
 for(unsigned i=1;i<=20;i++)assert(fp_thermal_guard(1,514,100+i,1000000));
 assert(fp_rate==200 && !fp_blocked); /* 0.4 C / 20 ms, still below 55 C */
 reset();assert(fp_thermal_guard(1,550,100,0));
 for(unsigned i=1;i<20;i++)assert(fp_thermal_guard(1,550+i,100+i,1000000));
 assert(!fp_thermal_guard(1,570,120,1000000));
#undef printf
 puts("PASS: predictive/rise/62C cutoff, 68C ceiling votes within 20ms of 77C step at every phase, sustained ramp, 51-52C load, 500ms hysteresis, rolling 123/200ms duty, boot-arg bounds and actual timer deadlines");
 puts("PASS: observed TRDY=8/48.1 C worker boost/hot cutoff/quorum/stats; exact CPU sensor read masks; actual worker 60s retries/input gating/no-init-votes/domain enable loss and recovery; real 0097 LUTs/init failures/16K mappings/14-of-18 sensor quorum; LUT decode/duplicates/turbo/nonmonotonic selection, legacy eligibility 60/68 boundaries, input-only entry, swap extension, 1500 ms expiry, per-core vote/readback and signed/invalid CPU sensors");
}
'''
REGISTERS = r'''
static uint32_t regs[5][1024], writes[3], lut_reads[3], tsens_reads[2];
static uintptr_t fp_epss[3]={0x1000,0x2000,0x3000},fp_tsens[2]={0x4000,0x5000};
static const uint64_t fp_epss_pa[3]={0x17d91000,0x17d92000,0x17d93000};
static const uint64_t fp_tsens_pa[2]={0xc271000,0xc272000};
static const char * const fp_domain[3]={"silver","gold","prime"};
static unsigned fp_percore[3],fp_cool[3],fp_high[3],fp_cool_khz[3],fp_high_khz[3];
POLICY_CAPS
static unsigned maps,fail_map,log_count,observed_tail_fault;
static unsigned fp_domain_failed[3],fp_applied[3],fp_applied_valid[3];
static uint64_t ticks,fp_retry_interval,fp_retry_at,fp_retry_end;
static unsigned fp_attempted,fp_retries,fp_enabled,fp_ready;
static struct { const char *step; unsigned domain,offset; uint64_t pa; uint32_t value; int access,row; } fp_diagnostic;
static uint64_t fp_input,fp_swap,fp_hold,fp_last,fp_window,fp_window_start,fp_boost_time,fp_boosts;
static int fp_max_temp,fp_boosted;
THERMAL_GLOBALS
static const char *arg_name;
static unsigned arg_value;
static int PE_parse_boot_argn(const char *name,void *value,unsigned size) {
 assert(size==sizeof(unsigned));
 if(arg_name && !strcmp(name,arg_name)) { *(unsigned *)value=arg_value;return 1; }
 return 0;
}
typedef void *thread_call_param_t;
static int fp_call;
static uint64_t scheduled;
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL
static uint64_t mach_absolute_time(void) { return ticks; }
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) { *ns=t*NSEC_PER_MSEC; }
static void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *t) { *t=ns/NSEC_PER_MSEC; }
static void clock_interval_to_deadline(unsigned n,unsigned unit,uint64_t *t) { *t=ticks+(uint64_t)n*unit/NSEC_PER_MSEC; }
static int thread_call_enter_delayed(int *call,uint64_t t) { (void)call;scheduled=t;return 0; }
static int thread_call_enter(int *call) { (void)call;scheduled=ticks;return 0; }
static int fault_block=-1;
static unsigned fault_off,fault_write_only,drop_after;
static int drop_block=-1;
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
 if(block>=3) {
  assert(!wr && off>=0xa0 && off<0xe0); /* Never read TRDY, including on faults. */
  tsens_reads[block-3]|=1u<<((off-0xa0)/4);
 }
 if((int)block==fault_block && off==fault_off && (!fault_write_only || wr))return 1;
 if(block<3 && off>=0x100 && off<0x1a0) {
  unsigned row=(off-0x100)/4;lut_reads[block]++;
  if(observed_tail_fault && row>observed_count[block])return 1;
 }
 if(wr) { assert(block<3 && (regs[block][0]&1));writes[block]++;regs[block][off/4]=*v; if((int)block==drop_block && writes[block]==drop_after)regs[block][0]&=~1u; }
 else *v=regs[block][off/4];
 return 0;
}
static void fp_configure(void);
static void reset(void) {
 fp_hot=620;fp_limit=680;fp_resume=580;fp_rise=100;fp_fast_ms=20;fp_budget_ms=200;
 arg_name=0;fp_configure();
 fp_recover_since=fp_above_time=fp_cuts=fp_active_start=0;
 fp_duty_count=0;fp_prev_temp=fp_prev_valid=fp_blocked=0;
 fp_rate_elapsed=0;fp_rate_temp=fp_rate_valid=fp_rate=0;
 memset(tsens_reads,0,sizeof(tsens_reads));
 memset(regs,0,sizeof(regs));memset(writes,0,sizeof(writes));memset(lut_reads,0,sizeof(lut_reads));
 memset(fp_cool_khz,0,sizeof(fp_cool_khz));memset(fp_high_khz,0,sizeof(fp_high_khz));
 memset(fp_domain_failed,0,sizeof(fp_domain_failed));memset(fp_applied_valid,0,sizeof(fp_applied_valid));
 fp_attempted=fp_retries=fp_ready=0;fp_enabled=1;fp_retry_interval=1000;fp_retry_at=fp_retry_end=0;ticks=100;
 fp_input=fp_swap=fp_last=fp_window_start=fp_boost_time=fp_boosts=0;
 fp_hold=1500;fp_window=10000;fp_max_temp=-400;fp_boosted=0;scheduled=0;
 for(unsigned d=0;d<3;d++) fp_epss[d]=0x1000*(d+1);
 drop_block=-1;drop_after=fault_write_only=0;
 maps=fail_map=log_count=0;fault_block=-1;fault_off=0;observed_tail_fault=1;
 for(unsigned d=0;d<3;d++) {
  regs[d][0]=3;regs[d][0xb0/4]=0x9b30;
  memcpy(&regs[d][0x100/4],observed_rows[d],sizeof(observed_rows[d]));
 }
 for(unsigned d=0;d<2;d++) {
  fp_tsens[d]=0x100000+0x4000*(d+3)+(fp_tsens_pa[d]&0x3fff);
  regs[d+3][0xe4/4]=8;
  for(unsigned n=1;n<=(d?3:15);n++)regs[d+3][(0xa0+4*n)/4]=(1u<<21)|450;
 }
}
static void reset_init(void) {
 reset();memset(fp_epss,0,sizeof(fp_epss));memset(fp_tsens,0,sizeof(fp_tsens));
}
'''
thermal_globals = actual[actual.index('static unsigned fp_hot ='):actual.index('static unsigned fp_enabled, fp_started')]
REGISTERS = REGISTERS.replace('THERMAL_GLOBALS', thermal_globals)
policy_caps = '\n'.join(re.findall(r'^static const unsigned fp_(?:cores|cool_cap|high_cap)\[3\].*$', actual, re.M))
assert len(policy_caps.splitlines()) == 3
REGISTERS = REGISTERS.replace('POLICY_CAPS', policy_caps)
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
