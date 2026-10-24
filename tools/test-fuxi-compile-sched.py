#!/usr/bin/env python3
"""Offline exercise of the actual scheduler helper with locked runqueue stubs."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
MODEL = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define FUXI 1
#define PROCESSOR_IDLE 4
#define BASEPRI_RTQUEUES 96
#define SCHED_PREEMPT 1
#define SCHED_TAILQ 2
#define THREAD_CONTINUE_NULL 0
typedef int spl_t,boolean_t;
struct processor {int state,cpu,processor_set;};
typedef struct processor *processor_t;
#define PROCESSOR_NULL ((processor_t)0)
struct thread {processor_t bound_processor;int sched_pri,active,queued;};
typedef struct thread *thread_t;
static struct processor cpus[7];
static struct thread self;
static unsigned spl,thread_locked,pset_locked,mtx_locked,removed,reinserted,blocks;
static spl_t splsched(void){assert(!spl);spl=1;return 0;}
static void splx(spl_t s){assert(!s && spl && !thread_locked && !pset_locked);spl=0;}
static void thread_lock(thread_t t){assert(t==&self && spl && !thread_locked);thread_locked=1;}
static void thread_unlock(thread_t t){assert(t==&self && thread_locked);thread_locked=0;}
static void thread_mtx_lock(thread_t t){assert(t==&self && !spl && !mtx_locked);mtx_locked=1;}
static void thread_mtx_unlock(thread_t t){assert(t==&self && mtx_locked);mtx_locked=0;}
static void pset_lock(int p){assert(p==1 && spl && !pset_locked);pset_locked=1;}
static void pset_unlock(int p){assert(p==1 && pset_locked);pset_locked=0;}
static int ml_get_cpu_number(uint32_t phys){assert(!(phys&255));unsigned n=phys>>8;return n==1 || n>7?-1:n?((int)n-1):0;}
static processor_t cpu_to_processor(int cpu){assert(cpu>=0 && cpu<7);return &cpus[cpu];}
static thread_t current_thread(void){return &self;}
static processor_t thread_bind_internal(thread_t t,processor_t p){
 assert(thread_locked && spl && t==&self && !t->queued && t->sched_pri<BASEPRI_RTQUEUES);
 processor_t previous=t->bound_processor;t->bound_processor=p;return previous;
}
static void thread_block(int continuation){assert(!continuation && !spl && !thread_locked);blocks++;}
static boolean_t thread_run_queue_remove(thread_t t){assert(thread_locked);removed++;int q=t->queued;t->queued=0;return q;}
static void thread_run_queue_reinsert(thread_t t,int options){assert(thread_locked && !t->queued && options==3);t->queued=1;reinserted++;}
#include "fuxi_compile_sched.h"
int main(void){
 for(int n=0;n<7;n++)cpus[n]=(struct processor){6,n,1};
 self=(struct thread){0,50,1,0};
 assert(!fuxi_compile_online(1) && !fuxi_compile_online(8));
 assert(fuxi_compile_bind(6)==7 && self.bound_processor==&cpus[6] && !blocks);
 fuxi_compile_migrate();assert(blocks==1); /* migration is separate, unlocked */
 assert(fuxi_compile_bind(6)==-2); /* never replace another binding */
 fuxi_compile_unbind(&self,6);assert(self.bound_processor==&cpus[6]);
 self.queued=1;fuxi_compile_unbind(&self,7);
 assert(!self.bound_processor && self.queued && removed==1 && reinserted==1);self.queued=0;
 cpus[6].state=0;assert(fuxi_compile_bind(6)==6 && self.bound_processor==&cpus[5]);
 fuxi_compile_unbind(&self,6);
 for(int n=5;n>=2;n--){cpus[n].state=0;int want=n==2?-1:n;assert(fuxi_compile_bind(6)==want);if(want!=-1)fuxi_compile_unbind(&self,want);}
 assert(!self.bound_processor); /* never falls to silver */
 for(int n=2;n<7;n++)cpus[n].state=PROCESSOR_IDLE;
 assert(fuxi_compile_bind(2)==6);fuxi_compile_unbind(&self,6);
 assert(fuxi_compile_bind(4)==7);fuxi_compile_unbind(&self,7);
 assert(fuxi_compile_bind(0)==-1);
 self.sched_pri=96;assert(fuxi_compile_bind(6)==-2);self.sched_pri=50;
 assert(fuxi_compile_bind(6)==7);self.sched_pri=96;self.queued=1;
 fuxi_compile_unbind(&self,7);assert(!self.bound_processor && self.queued);self.queued=0;
 assert(fuxi_compile_alive(&self));self.active=0;assert(!fuxi_compile_alive(&self));
 assert(!spl && !thread_locked && !pset_locked && !mtx_locked);
 puts("PASS: actual scheduler helper physical/logical mapping, prime/gold online fallback, no silver/monitor, deferred block, prebound/realtime denial, locked runqueue removal/reinsert and dead owner");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source, exe = Path(tmp)/'sched.c', Path(tmp)/'sched'
    source.write_text(MODEL)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I',str(ROOT/'xnu/fuxi-perf'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
