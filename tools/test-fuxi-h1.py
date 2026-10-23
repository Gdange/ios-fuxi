#!/usr/bin/env python3
"""Native H1 production-code tests; no device, binaries, firmware or network."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-h1'

MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#define ENOPOLICY 103
#define MAXPATHLEN 1024
#define NCARGS 262144
#define PAGE_SIZE 4096
#define FALSE 0
#define KERN_SUCCESS 0
#define VM_MAP_NULL NULL
#define VM_MAP_COPY_NULL NULL
#define TASK_NULL NULL
#define PROC_NULL NULL
#define KMA_DATA 1
#define KMA_ZERO 2
#define VM_KERN_MEMORY_BSD 0
#define Z_WAITOK 1
#define Z_ZERO 2
#define TUNABLE(t,n,k,v) t n = v
#define LCK_GRP_DECLARE(n,s) int n __attribute__((unused))
#define LCK_MTX_DECLARE(n,g) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
#define kalloc_type(t,f) ((t *)calloc(1,sizeof(t)))
typedef uintptr_t user_addr_t;
typedef size_t user_size_t;
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef void *vm_map_t;
typedef void *vm_map_copy_t;
typedef void *task_t;
struct proc { user_addr_t user_stack; size_t p_argslen; int p_argc; };
typedef struct proc *proc_t;
typedef int mac_policy_handle_t;
struct mac_policy_ops { int (*mpo_policy_syscall)(struct proc *, int, user_addr_t); };
struct mac_policy_conf { const char *mpc_name, *mpc_fullname; const struct mac_policy_ops *mpc_ops; };
static struct proc target;
static unsigned logs, allocations, frees, discards, proc_refs, task_refs, map_refs, registrations;
static int fail_copyin, fail_copyout, fail_stage;
static char snapshot[4096];
static void *kernel_map = (void *)1;
static int model_printf(const char *fmt, ...) {
 (void)fmt; __atomic_add_fetch(&logs,1,__ATOMIC_SEQ_CST); return 0;
}
#define printf model_printf
static int copyin(user_addr_t a, void *b, size_t n) {
 if(fail_copyin || a<4096) return EFAULT; memcpy(b,(void *)a,n); return 0;
}
static int copyout(const void *a, user_addr_t b, size_t n) {
 if(fail_copyout || b<4096) return EFAULT; memcpy((void *)b,a,n); return 0;
}
static int proc_pid(proc_t p) { (void)p; return 42; }
static void proc_name(int p, char *b, size_t n) { (void)p; assert(n>=5); strcpy(b,"test"); }
static proc_t proc_find(int pid) { if(pid!=42)return NULL; proc_refs++; return &target; }
static int proc_pidversion(proc_t p) { assert(p==&target); return 7; }
static void proc_rele(proc_t p) { assert(p==&target && proc_refs); proc_refs--; }
static task_t proc_task(proc_t p) { assert(p==&target); return fail_stage==1?NULL:(void *)2; }
static void task_reference(task_t t) { assert(t); task_refs++; }
static void task_deallocate(task_t t) { assert(t && task_refs); task_refs--; }
static vm_map_t get_task_map_reference(task_t t) { assert(t); if(fail_stage==2)return NULL; map_refs++;return (void *)3; }
static unsigned vm_map_page_mask(vm_map_t m) { assert(m); return 4095; }
static size_t vm_map_round_page(size_t n, unsigned mask) { return (n+mask)&~mask; }
static int kmem_alloc(vm_map_t m, vm_offset_t *b, size_t n, int f, int tag) {
 (void)f;(void)tag;assert(m && n==4096);if(fail_stage==3)return 1;
 *b=(uintptr_t)calloc(1,n);allocations++;return 0;
}
static int vm_map_copyin(vm_map_t m, uintptr_t a, size_t n, int destroy, vm_map_copy_t *copy) {
 assert(m && a==target.user_stack-4096 && n==4096 && !destroy);
 if(fail_stage==4)return 1; *copy=malloc(n);memcpy(*copy,snapshot,n);return 0;
}
static int vm_map_copy_overwrite(vm_map_t m, uintptr_t b, vm_map_copy_t copy, size_t n, int intr) {
 assert(m && !intr); if(fail_stage==5)return 1;memcpy((void *)b,copy,n);free(copy);return 0;
}
static void vm_map_copy_discard(vm_map_copy_t c) { assert(c);free(c);discards++; }
static void kmem_free(vm_map_t m, uintptr_t b, size_t n) { assert(m && n==4096);free((void *)b);frees++; }
static void vm_map_deallocate(vm_map_t m) { assert(m && map_refs);map_refs--; }
static int mac_policy_register(struct mac_policy_conf *c, mac_policy_handle_t *h, void *v) {
 assert(!strcmp(c->mpc_name,"AMFI") && c->mpc_ops->mpo_policy_syscall && h && !v);registrations++;return 0;
}
'''

TEST = r'''
static void *request_thread(void *unused) {
 (void)unused; for(unsigned i=0;i<1000;i++)assert(fuxi_amfi_syscall(&target,0x12345,0)==ENOPOLICY);return NULL;
}
static void amfi_test(void) {
 _Static_assert(sizeof(struct fuxi_dyld_query)==16,"dyld ABI");
 _Static_assert(sizeof(struct fuxi_container_query)==40,"sandbox ABI");
 uint64_t value=0;
 struct fuxi_dyld_query q={~0ull,(uintptr_t)&value};
 fuxi_amfi_policy_register();assert(registrations==1);
 assert(fuxi_amfi_syscall(&target,0x5a,(uintptr_t)&q)==0 && value==0xff);
 value=0;assert(fuxi_amfi_syscall(&target,0x60,(uintptr_t)&value)==0 && value==0x86);
 unsigned before=logs;
 assert(fuxi_amfi_syscall(&target,0x5a,(uintptr_t)&q)==0 && logs==before);
 fail_copyin=1;assert(fuxi_amfi_syscall(&target,0x5a,(uintptr_t)&q)==EFAULT);fail_copyin=0;
 q.output=0;assert(fuxi_amfi_syscall(&target,0x5a,(uintptr_t)&q)==EFAULT);
 assert(fuxi_amfi_syscall(&target,0x60,0)==EFAULT);
 before=logs;
 assert(fuxi_amfi_syscall(&target,-1,0)==ENOPOLICY);
 assert(fuxi_amfi_syscall(&target,0,0)==ENOPOLICY);
 assert(fuxi_amfi_syscall(&target,INT_MAX,0)==ENOPOLICY);
 assert(logs==before+3);
 assert(fuxi_amfi_syscall(&target,-1,0)==ENOPOLICY && logs==before+3);
 pthread_t threads[8];before=logs;
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,request_thread,NULL));
 for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
 assert(logs==before+1);
 fuxi_amfi_enabled=0;value=0x1122;
 assert(fuxi_amfi_syscall(&target,0x60,(uintptr_t)&value)==ENOPOLICY && value==0x1122);
 fuxi_amfi_policy_register();assert(registrations==1);fuxi_amfi_enabled=1;
}
static void container_test(void) {
 char output[1024]="untouched";
 const char strings[]="executable_path=/bin/plugin\0\0\0plugin\0CFFIXED_USER_HOME=/fake-argv\0CFFIXED_USER_HOME=/private/var/mobile/Containers/Data/PluginKitPlugin/test\0";
 const char *expected="/private/var/mobile/Containers/Data/PluginKitPlugin/test";
 // argc=2: reject argv spoof, use the environment string after both argv strings.
 assert(fuxi_container_from_strings(strings,sizeof(strings),2,(uintptr_t)output,sizeof(output))==0);
 assert(!strcmp(output,expected));
 assert(fuxi_container_from_strings(strings,sizeof(strings),2,(uintptr_t)output,strlen(expected))==ENAMETOOLONG);
 assert(fuxi_container_from_strings(strings,sizeof(strings),2,(uintptr_t)output,strlen(expected)+1)==0);
 assert(fuxi_container_from_strings(strings,sizeof(strings),2,0,sizeof(output))==EFAULT);
 assert(fuxi_container_from_strings("x\0arg\0HOME=/tmp\0",17,1,(uintptr_t)output,sizeof(output))==ENOENT);
 assert(fuxi_container_from_strings("x\0arg\0CFFIXED_USER_HOME=relative\0",31,1,(uintptr_t)output,sizeof(output))==ENOENT);
 assert(fuxi_container_from_strings("x\0arg\0CFFIXED_USER_HOME=/abc",26,1,(uintptr_t)output,sizeof(output))==ENOENT);
 assert(fuxi_container_from_strings(strings,sizeof(strings),-1,(uintptr_t)output,sizeof(output))==ENOENT);
 // Fuzz bounded string scanning (ASan/UBSan), including absent final NUL.
 uint32_t seed=1;char junk[128];
 for(unsigned n=0;n<10000;n++) {
  for(unsigned i=0;i<sizeof(junk);i++){seed=seed*1664525+1013904223;junk[i]=(char)(seed>>24);}
  (void)fuxi_container_from_strings(junk,n%sizeof(junk),(int)(n%8),(uintptr_t)output,sizeof(output));
 }
 struct fuxi_container_query q={42,7,(uintptr_t)output,sizeof(output),1};
 target.user_stack=0x10000;target.p_argslen=sizeof(strings);target.p_argc=2;
 memcpy(snapshot+sizeof(snapshot)-sizeof(strings),strings,sizeof(strings));
 assert(sweet_sandbox_syscall(&target,4,(uintptr_t)&q)==0 && !strcmp(output,expected));
 fuxi_amfi_enabled=0;assert(sweet_sandbox_syscall(&target,4,0)==ENOENT);fuxi_amfi_enabled=1;
 assert(sweet_sandbox_syscall(&target,4,0)==EFAULT);
 assert(sweet_sandbox_syscall(&target,5,0)==ENOENT);
 assert(sweet_sandbox_syscall(&target,6,0)==ENOENT);
 assert(sweet_sandbox_syscall(&target,0x43,0)==ENOENT);
 assert(sweet_sandbox_syscall(&target,0,0)==0);
 assert(!proc_refs && !task_refs && !map_refs && allocations==frees);
 q.version=8;assert(fuxi_sandbox_container((uintptr_t)&q)==ESRCH);q.version=7;
 q.pid=0x10000002aLL;assert(fuxi_sandbox_container((uintptr_t)&q)==ESRCH);
 q.pid=43;assert(fuxi_sandbox_container((uintptr_t)&q)==ESRCH);q.pid=42;
 assert(fuxi_sandbox_container(0)==EFAULT);
 for(fail_stage=1;fail_stage<=5;fail_stage++) {
  int error=fuxi_sandbox_container((uintptr_t)&q);
  assert(error==(fail_stage<=2?ENOENT:fail_stage==3?ENOMEM:EIO));
  assert(!proc_refs && !task_refs && !map_refs && allocations==frees);
 }
 assert(discards==1);fail_stage=0;
 target.p_argslen=NCARGS+PAGE_SIZE+1;
 assert(fuxi_sandbox_container((uintptr_t)&q)==ENOENT);
 assert(!proc_refs && !task_refs && !map_refs);
}
static void keys_test(void) {
 struct fuxi_key_state k={0};
 uint64_t t=0;
 for(unsigned i=0;i<100;i++)assert(!fuxi_key_sample(&k,true,true,++t)); // held at boot
 assert(!k.armed && !k.down);
 assert(!fuxi_key_sample(&k,true,false,++t));assert(!k.armed);
 assert(!fuxi_key_sample(&k,true,false,++t));assert(k.armed);
 assert(!fuxi_key_sample(&k,true,true,++t));
 assert(!fuxi_key_sample(&k,false,true,++t)); // failure breaks pair
 assert(!fuxi_key_sample(&k,true,true,++t));
 uint64_t pressed=t;
 assert(fuxi_key_sample(&k,true,true,++t) && k.down);
 assert(k.at==pressed && k.since==pressed); // HID time = first sample of the run
 assert(!fuxi_key_sample(&k,true,false,++t)); // bounce back down
 assert(!fuxi_key_sample(&k,true,true,++t));assert(k.down);
 assert(!fuxi_key_sample(&k,true,true,++t));
 assert(!fuxi_key_sample(&k,true,false,++t));
 uint64_t released=t;
 assert(fuxi_key_sample(&k,true,false,++t) && !k.down);
 assert(k.at==released && k.since==pressed);
 assert(!fuxi_key_sample(&k,true,false,++t));
 k=(struct fuxi_key_state){0};
 assert(!fuxi_key_sample(&k,true,false,1));
 assert(!fuxi_key_sample(&k,false,false,2));
 assert(!fuxi_key_sample(&k,true,false,3) && !k.armed);
 assert(!fuxi_key_sample(&k,true,false,4) && k.armed);
 unsigned failures=0;
 for(unsigned i=0;i<7;i++)assert(!fuxi_keys_failed(&failures,true));
 assert(!fuxi_keys_failed(&failures,false) && !failures);
 for(unsigned i=0;i<7;i++)assert(!fuxi_keys_failed(&failures,true));
 assert(fuxi_keys_failed(&failures,true) && failures==8);
}
static void arbiter_test(void) {
 struct fuxi_key_arbiter a={0};
 assert(fuxi_key_arbitrate(&a,0,FUXI_KEY_SRC_HW,false)==0); // unmatched up: nothing
 assert(fuxi_key_arbitrate(&a,0,FUXI_KEY_SRC_HW,true)==1);
 assert(fuxi_key_arbitrate(&a,0,FUXI_KEY_SRC_HW,true)==0); // repeated down
 assert(fuxi_key_arbitrate(&a,0,FUXI_KEY_SRC_INJECT,true)==0);
 assert(fuxi_key_arbitrate(&a,0,FUXI_KEY_SRC_HW,false)==0); // still held by injection
 assert(fuxi_key_held_mask(&a,~0u)==1 && fuxi_key_held_mask(&a,FUXI_KEY_SRC_HW)==0);
 assert(fuxi_key_arbitrate(&a,0,FUXI_KEY_SRC_INJECT,false)==-1);
 assert(fuxi_key_arbitrate(&a,2,FUXI_KEY_SRC_TAP,true)==1 && fuxi_key_held_mask(&a,FUXI_KEY_SRC_TAP)==4);
 assert(fuxi_key_arbitrate(&a,2,FUXI_KEY_SRC_TAP,false)==-1 && fuxi_key_held_mask(&a,~0u)==0);
 assert(fuxi_key_arbitrate(&a,3,FUXI_KEY_SRC_HW,true)==0 && fuxi_key_arbitrate(&a,~0u,FUXI_KEY_SRC_HW,true)==0);
 // Random sources/keys: every HID down is followed by exactly one HID up.
 uint32_t seed=7;int hid[FUXI_KEYS]={0};
 for(unsigned n=0;n<100000;n++) {
  seed=seed*1664525+1013904223;
  unsigned key=(seed>>8)%FUXI_KEYS,src=1u<<((seed>>12)%3);bool down=(seed>>20)&1;
  int change=fuxi_key_arbitrate(&a,key,src,down);
  assert(change==0 || (change==1 && !hid[key]) || (change==-1 && hid[key]));
  hid[key]+=change;assert(hid[key]==(a.held[key]!=0));
 }
 a.last=0;
 assert(fuxi_key_stamp(&a,100)==100 && fuxi_key_stamp(&a,90)==100 && fuxi_key_stamp(&a,101)==101);
}
static void inject_decode_test(void) {
 struct fuxi_key_inject c;
 assert(!fuxi_key_inject_decode(0x100,&c) && c.key==0 && c.op==FUXI_KEY_INJECT_DOWN && !c.hold_ms);
 assert(!fuxi_key_inject_decode(0x202,&c) && c.key==2 && c.op==FUXI_KEY_INJECT_UP);
 assert(!fuxi_key_inject_decode(0x301,&c) && c.key==1 && c.hold_ms==FUXI_KEY_INJECT_CLICK_DEFAULT_MS);
 assert(!fuxi_key_inject_decode((800<<16)|0x300,&c) && c.hold_ms==800);
 assert(!fuxi_key_inject_decode((2000<<16)|0x300,&c) && c.hold_ms==2000);
 assert(fuxi_key_inject_decode((2001<<16)|0x300,&c)==EINVAL);
 assert(fuxi_key_inject_decode((5<<16)|0x100,&c)==EINVAL); // hold only for click
 assert(fuxi_key_inject_decode(0x103,&c)==EINVAL); // no key 3 (home)
 assert(fuxi_key_inject_decode(0x000,&c)==EINVAL && fuxi_key_inject_decode(0x400,&c)==EINVAL);
 assert(fuxi_key_inject_decode(-1,&c)==EINVAL && fuxi_key_inject_decode(0x10000100,&c)==EINVAL);
 for(int v=-70000;v<70000;v+=7) {
  if(!fuxi_key_inject_decode(v,&c))assert(c.key<FUXI_KEYS && c.op>=1 && c.op<=3 && c.hold_ms<=2000);
 }
}
static void tap_wake_test(void) {
 uint64_t last=0;
 assert(!fuxi_tap_wake_allowed(-1,10,&last,100) && !last);   // mode never set
 assert(!fuxi_tap_wake_allowed(0x1,10,&last,100));           // display on
 assert(!fuxi_tap_wake_allowed(0xff,10,&last,100));          // no touch at all
 assert(fuxi_tap_wake_allowed(0xfe,10,&last,100) && last==10);
 assert(!fuxi_tap_wake_allowed(0xfe,50,&last,100));          // double tap
 assert(fuxi_tap_wake_allowed(0x1fe,110,&last,100));         // after the lock button
 struct fuxi_wake_touch w={0};bool fresh;
 // No wake press: off contacts are only dropped, nothing hidden after display on.
 fuxi_wake_touch_display(&w,true);
 assert(fuxi_wake_touch_event(&w,true,0,true,&fresh) && fresh);
 assert(fuxi_wake_touch_event(&w,true,0,true,&fresh) && !fresh);
 fuxi_wake_touch_display(&w,false);
 assert(!fuxi_wake_touch_event(&w,false,0,true,&fresh) && !fresh);
 assert(!fuxi_wake_touch_event(&w,false,0,false,&fresh));
 // Wake press: that contact, and one landing before the display is on, stay hidden until lifted.
 fuxi_wake_touch_display(&w,true);
 assert(fuxi_wake_touch_event(&w,true,3,true,&fresh) && fresh);
 fuxi_wake_touch_woke(&w);
 assert(fuxi_wake_touch_event(&w,true,4,true,&fresh) && fresh);
 assert(fuxi_wake_touch_event(&w,true,5,true,&fresh) && fresh);
 assert(fuxi_wake_touch_event(&w,true,5,false,&fresh) && !fresh); // lifted while off
 fuxi_wake_touch_display(&w,false);
 assert(fuxi_wake_touch_event(&w,false,3,true,&fresh) && !fresh);
 assert(fuxi_wake_touch_event(&w,false,4,true,&fresh));
 assert(!fuxi_wake_touch_event(&w,false,5,true,&fresh)); // new contact after display on
 assert(!fuxi_wake_touch_event(&w,false,1,true,&fresh));
 assert(fuxi_wake_touch_event(&w,false,3,false,&fresh));  // lift: consumed, then normal
 assert(!fuxi_wake_touch_event(&w,false,3,true,&fresh));
 fuxi_wake_touch_reset(&w);
 assert(!fuxi_wake_touch_event(&w,false,4,true,&fresh)); // controller reset clears
 // Next off period starts without a wake press.
 fuxi_wake_touch_display(&w,true);assert(!w.woke && !w.off);
}
int main(void) {
 amfi_test();container_test();keys_test();arbiter_test();inject_decode_test();tap_wake_test();
 puts("PASS: AMFI ABI/dispatch/faults/disable/concurrent log-once; container lookup/bounds/VM cleanup; held keys/debounce/press timestamps/failure limit; key arbiter, inject word, tap-to-wake policy and contact filter");
}
'''


def function(text, name):
    match = re.search(r'(?:static )?(?:void|int|bool)\n' + name +
                      r'(?:\([^;]+?| SYSCTL_HANDLER_ARGS)\n\{', text)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


THREAD_MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define FK_NO_APID 0xffff
#define KEYS_NO_APID 0xffff
#define FK_PON_RT 0x10
#define KEYS_PON_RT 0x810
#define KEYS_GPIO_RT 0xc110
#define FK_PON_PWR 0x80
#define FK_PON_RESIN 0x40
#define THREAD_CONTINUE_NULL 0
typedef int wait_result_t;
static unsigned tick, start_failure, downs, ups, stop_logs, terminated, hid_calls;
static uint16_t fk_pon_apid=1, fk_gpio_apid=2, keys_pon_apid=1, keys_gpio_apid=2;
static unsigned fk_last_status, keys_last_status;
static bool held;
static void fuxi_perf_input(void) {}
static uint64_t mach_absolute_time(void) { return tick*20+5; }
static uint64_t fk_ms(uint64_t abs) { return abs; }
static int fk_hid_lock, locked;
static void lck_mtx_lock(int *m) { assert(m==&fk_hid_lock && !locked);locked=1; }
static void lck_mtx_unlock(int *m) { assert(m==&fk_hid_lock && locked);locked=0; }
static void sweet_hid_key(int k, int down) {
 assert(k==0);
 if(down) { downs++;assert(tick==(held?7:3)); }
 else { ups++;assert(tick==start_failure+7); }
}
static void sweet_hid_key_at(int k, int down, uint64_t ts) {
 assert(locked);hid_calls++;sweet_hid_key(k,down);
 // The press is stamped when first sampled (one poll before it was confirmed).
 assert(ts==(down ? (held?6u:2u)*20+5 : mach_absolute_time()));
}
static bool read_key(uint16_t apid, uint16_t reg, uint8_t *out) {
 (void)reg;
 assert(tick<=start_failure+7);
 if(tick>=start_failure)return false;
 bool pressed=held ? tick<4 || tick>=6 : tick>=2;
 *out=apid==1 ? (pressed?0x81:0) : 1;return true;
}
#define fk_read read_key
#define keys_read read_key
static bool fk_setup(void) { return true; }
static void fk_sleep(unsigned ms) { assert(ms==20);tick++; }
static void keys_wait(void) { tick++; }
static int model_printf(const char *fmt, ...) {
 if(strstr(fmt,"stopped after"))stop_logs++;
 else assert(!strstr(fmt,"stopped"));
 return 0;
}
#define printf model_printf
static int current_thread(void) { return 1; }
static int thread_terminate(int thread) { assert(thread==1);terminated++;return 0; }
static void thread_block(int continuation) { assert(continuation==0 && terminated==1); }
'''
THREAD_TEST = r'''
int main(void) {
 for(unsigned driver=0;driver<2;driver++)for(held=false;;held=true) {
  tick=downs=ups=stop_logs=terminated=0;start_failure=held?8:4;
  fk_arb=(struct fuxi_key_arbiter){0};hid_calls=0;
  if(driver==0)fk_thread(NULL,0);else keys_thread(NULL,0);
  assert(downs==1 && ups==1 && stop_logs==1 && terminated==1 && !locked);
  assert(hid_calls==(driver==0?2u:0u) && fuxi_key_held_mask(&fk_arb,~0u)==0);
  if(held)break;
 }
 puts("PASS: actual Fuxi/Sweet polling threads ignore boot hold, debounce, stop on eight failures and release HID downs");
}
'''


INJECT_MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#define NSEC_PER_MSEC 1000000ull
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL 0
#define SYSCTL_HANDLER_ARGS (struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
struct sysctl_oid { int unused; };
struct sysctl_req { int *oldp; const int *newp; };
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef struct { thread_call_func_t func; uint64_t deadline; } *thread_call_t;
static uint64_t clock_now = 1000;
static int fk_hid_lock, locked, boot_inject = -1, boot_tap, root = 1, touch_mode = -1;
static unsigned perf, allocations, hid_events, logs;
static int hid_key[64], hid_down[64];
static uint64_t hid_ts[64];
static uint64_t mach_absolute_time(void) { return clock_now; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs) { *abs = ns; }
static void clock_interval_to_deadline(uint32_t n, uint64_t scale, uint64_t *d) { *d = clock_now + n * scale; }
static void lck_mtx_lock(int *m) { assert(m==&fk_hid_lock && !locked);locked=1; }
static void lck_mtx_unlock(int *m) { assert(m==&fk_hid_lock && locked);locked=0; }
static void fuxi_perf_input(void) { perf++; }
static int sweet_hid_touch_mode(void) { return touch_mode; }
static void sweet_hid_key_at(int k, int down, uint64_t ts) {
 assert(locked && hid_events<64);
 hid_key[hid_events]=k;hid_down[hid_events]=down;hid_ts[hid_events]=ts;hid_events++;
}
static int model_printf(const char *fmt, ...) { (void)fmt;logs++;return 0; }
#define printf model_printf
static bool PE_parse_boot_argn(const char *name, void *out, int size) {
 assert(size==sizeof(int));
 if(!strcmp(name,"fuxikeyinject")) { if(boot_inject<0)return false;*(int *)out=boot_inject;return true; }
 if(!strcmp(name,"fuxitaptowake")) { *(int *)out=boot_tap;return true; }
 assert(0);return false;
}
static int kauth_cred_get(void) { return 0; }
static int kauth_cred_issuser(int cred) { (void)cred;return root; }
static int sysctl_io_number(struct sysctl_req *req, long long big, size_t size, void *value, int *changed) {
 assert(size==sizeof(int));*changed=0;
 if(req->oldp)*req->oldp=(int)big;
 if(req->newp) { *(int *)value=*req->newp;*changed=1; }
 return 0;
}
static int SYSCTL_OUT(struct sysctl_req *req, const void *value, size_t size) {
 assert(size==sizeof(int) && req->oldp);memcpy(req->oldp,value,size);return 0;
}
static struct { thread_call_func_t func; uint64_t deadline; } the_call;
static thread_call_t thread_call_allocate(thread_call_func_t func, void *param) {
 assert(!param);allocations++;the_call.func=func;the_call.deadline=0;return (thread_call_t)&the_call;
}
static bool thread_call_free(thread_call_t c) { (void)c;assert(0);return true; }
static bool thread_call_enter_delayed(thread_call_t c, uint64_t d) { assert(c==(thread_call_t)&the_call);c->deadline=d;return false; }
static uint64_t wait_deadline;
static int assert_wait_deadline(void *event, int interruptible, uint64_t d) { (void)event;(void)interruptible;wait_deadline=d;return 0; }
static void thread_block(int c) { assert(c==0 && !locked && wait_deadline);clock_now=wait_deadline;wait_deadline=0; }
'''
INJECT_TEST = r'''
static int sysctl_write(int (*h) SYSCTL_HANDLER_ARGS, int value) {
 struct sysctl_req req={NULL,&value};return h(NULL,NULL,0,&req);
}
static int sysctl_read(int (*h) SYSCTL_HANDLER_ARGS) {
 int value=-1;struct sysctl_req req={&value,NULL};assert(!h(NULL,NULL,0,&req));return value;
}
static void check_pairs(void) {
 int down[FUXI_KEYS]={0};
 for(unsigned i=0;i<hid_events;i++) {
  assert(hid_down[i]!=down[hid_key[i]]);down[hid_key[i]]=hid_down[i];
  assert(i==0 || hid_ts[i]>=hid_ts[i-1]);
 }
}
int main(void) {
 // Disabled by default (no boot-arg), and with fuxikeyinject=0: ENOTSUP, nothing delivered.
 assert(sysctl_write(fk_sysctl_inject,0x300)==ENOTSUP);
 boot_inject=0;assert(sysctl_write(fk_sysctl_inject,0x300)==ENOTSUP);
 assert(hid_events==0 && sysctl_read(fk_sysctl_held)==0 && sysctl_read(fk_sysctl_inject)==0);
 boot_inject=1;root=0;assert(sysctl_write(fk_sysctl_inject,0x300)==EPERM);root=1;
 assert(sysctl_write(fk_sysctl_inject,0x103)==EINVAL && sysctl_write(fk_sysctl_inject,0x400)==EINVAL);
 assert(hid_events==0 && !allocations);
 // Click: down now, up after the hold, through the arbiter.
 assert(sysctl_write(fk_sysctl_inject,(250<<16)|0x300)==0);
 assert(hid_events==2 && hid_key[0]==0 && hid_down[0]==1 && hid_down[1]==0);
 assert(hid_ts[1]-hid_ts[0]==250*NSEC_PER_MSEC && allocations==1 && !locked);
 // Injected down merges with the PMIC key: one HID down, one HID up.
 assert(fk_deliver(1,FUXI_KEY_SRC_HW,true,clock_now)==1);
 assert(sysctl_write(fk_sysctl_inject,0x101)==0 && hid_events==3);
 assert(sysctl_read(fk_sysctl_held)==(0x2|0x200|0x20000) && sysctl_read(fk_sysctl_inject)==0x2);
 assert(fk_deliver(1,FUXI_KEY_SRC_HW,false,clock_now)==0 && hid_events==3);
 assert(sysctl_write(fk_sysctl_inject,0x201)==0 && hid_events==4 && hid_down[3]==0);
 assert(sysctl_write(fk_sysctl_inject,0x201)==0 && hid_events==4); // unmatched up: nothing
 // A forgotten injected down is released by the expiry call.
 assert(sysctl_write(fk_sysctl_inject,0x102)==0 && hid_events==5);
 assert(the_call.deadline==clock_now+FUXI_KEY_INJECT_HOLD_MAX_MS*NSEC_PER_MSEC);
 clock_now=the_call.deadline;the_call.func(NULL,NULL);
 assert(hid_events==6 && hid_key[5]==2 && hid_down[5]==0 && sysctl_read(fk_sysctl_held)==0);
 the_call.func(NULL,NULL);assert(hid_events==6);
 // Tap-to-wake: off by default, then only for mode 0xfe, once per 1.5 s, never while power is held.
 touch_mode=0xfe;assert(!fuxi_keys_touch_wake() && hid_events==6);
 boot_tap=1; // the boot-arg is read once per boot
 assert(!fuxi_keys_touch_wake() && hid_events==6);
 check_pairs();assert(!locked);
 puts("PASS: actual inject sysctl (default off, root, decode, click timing, PMIC merge, unmatched up, expiry)");
 return 0;
}
'''
TAP_TEST = r'''
int main(void) {
 assert(sysctl_write(fk_sysctl_inject,0x300)==ENOTSUP); // tap-to-wake alone does not enable injection
 boot_tap=1;
 touch_mode=-1;assert(!fuxi_keys_touch_wake());
 touch_mode=0x1;assert(!fuxi_keys_touch_wake());
 touch_mode=0xff;assert(!fuxi_keys_touch_wake());
 touch_mode=0x1fe;assert(fuxi_keys_touch_wake());
 assert(hid_events==2 && hid_key[0]==0 && hid_down[0]==1 && hid_down[1]==0 && hid_ts[0]==hid_ts[1]);
 clock_now+=1000*NSEC_PER_MSEC;assert(!fuxi_keys_touch_wake() && hid_events==2); // double tap
 clock_now+=600*NSEC_PER_MSEC;
 assert(fk_deliver(0,FUXI_KEY_SRC_HW,true,clock_now)==1);
 assert(!fuxi_keys_touch_wake() && hid_events==3); // power physically held
 assert(fk_deliver(0,FUXI_KEY_SRC_HW,false,clock_now)==-1);
 assert(fuxi_keys_touch_wake() && hid_events==6 && sysctl_read(fk_sysctl_held)==0);
 check_pairs();assert(!locked && perf==2);
 puts("PASS: actual tap-to-wake (off by default, mode 0xfe only, 1.5 s rate limit, never while power is held)");
 return 0;
}
'''


def main():
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-h1.py'), '--check'], check=True)
    with tempfile.TemporaryDirectory(prefix='fuxi-h1-test-') as tmp:
        source = Path(tmp) / 'h1.c'
        source.write_text(MODEL + (SRC / 'fuxi_amfi.h').read_text() +
                          (SRC / 'fuxi_key_state.h').read_text() +
                          'static uint64_t sweet_sandbox_seen;\n' +
                          function((SRC / 'sweet_amfi.c').read_text(), 'sweet_sandbox_syscall') + TEST)
        binary = Path(tmp) / 'h1'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-pthread', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        keys_c = (SRC / 'fuxi_keys.c').read_text()
        (Path(tmp) / 'fuxi_key_state.h').write_text((SRC / 'fuxi_key_state.h').read_text())
        source.write_text(THREAD_MODEL.replace('#include <string.h>', '#include <string.h>\n#include <errno.h>') +
                          '#include "fuxi_key_state.h"\n'
                          'static struct fuxi_key_arbiter fk_arb;\n'
                          'static const char *const fk_names[FUXI_KEYS]={"power","volume-up","volume-down"};\n' +
                          ''.join(function(keys_c, name) for name in ('fk_deliver_locked', 'fk_deliver', 'fk_thread')) +
                          function((SRC / 'sweet_keys.c').read_text(), 'keys_thread') + THREAD_TEST)
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        names = ('fk_deliver_locked', 'fk_deliver', 'fk_inject_expire', 'fk_inject_one', 'fk_inject',
                 'fk_sysctl_inject', 'fk_sysctl_held', 'fuxi_keys_touch_wake')
        body = ('#include "fuxi_key_state.h"\n'
                'static struct fuxi_key_arbiter fk_arb;\nstatic uint64_t fk_tap_last;\n'
                'static thread_call_t fk_inject_call;\nstatic int fk_inject_chan;\n'
                'static const char *const fk_names[FUXI_KEYS]={"power","volume-up","volume-down"};\n' +
                ''.join(function(keys_c, name) for name in names))
        for main_src in (INJECT_TEST, INJECT_TEST.split('int main(void) {')[0] + TAP_TEST):
            source.write_text(INJECT_MODEL + body + main_src)
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unknown-pragmas', '-fsanitize=address,undefined', str(source), '-o', str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)
    for filename in ('fuxi_keys.c', 'sweet_keys.c'):
        text = (SRC / filename).read_text()
        assert 'fuxi_key_sample(' in text and 'fuxi_keys_failed(' in text
        assert 'sweet_spmi_bus_lock();' in text and 'sweet_spmi_bus_unlock();' in text
    hid = (SRC / 'SweetHID.cpp').read_text()
    assert 'kIOHIDEventOptionIsBuiltIn' in hid
    # Usage map (consumer page, as HackiOS sends to iOS 16 backboardd): power, vol+, vol-, home.
    assert re.search(r'kHIDUsage_Csmr_Power, kHIDUsage_Csmr_VolumeIncrement,\s*'
                     r'kHIDUsage_Csmr_VolumeDecrement, kHIDUsage_Csmr_Menu,', hid)
    assert 'kHIDPage_Telephony' not in hid and 'Csmr_Mute' not in hid
    assert 'keyboardEvent(timestamp, kHIDPage_Consumer' in hid
    assert '"TouchDetectionMode"' in hid and 'sweet_hid_touch_mode(void)' in hid
    keys = (SRC / 'fuxi_keys.c').read_text()
    assert keys.count('sweet_hid_key_at(') == 2 and 'sweet_hid_key(' not in keys  # only the arbiter
    assert '"fuxikeyinject"' in keys and '"fuxitaptowake"' in keys
    assert not re.search(r'SYSCTL_PROC\([^;]*CTLFLAG_ANYBODY', keys) and 'kauth_cred_issuser(kauth_cred_get())' in keys
    touch = (SRC / 'fuxi_touch.c').read_text()
    assert 'fuxi_wake_touch_event(&ft.wake, !deliver' in touch and 'fuxi_keys_touch_wake()' in touch
    assert touch.count('fuxi_wake_touch_reset(&ft.wake)') == 2
    print('PASS: both button drivers use tested state and common SPMI lock; HID marks built-in; '
          'Fuxi keys reach HID only through the arbiter; usage map and gates in place')


if __name__ == '__main__':
    main()
