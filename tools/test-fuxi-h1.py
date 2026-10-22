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
 for(unsigned i=0;i<100;i++)assert(!fuxi_key_sample(&k,true,true)); // held at boot
 assert(!k.armed && !k.down);
 assert(!fuxi_key_sample(&k,true,false));assert(!k.armed);
 assert(!fuxi_key_sample(&k,true,false));assert(k.armed);
 assert(!fuxi_key_sample(&k,true,true));
 assert(!fuxi_key_sample(&k,false,true)); // failure breaks pair
 assert(!fuxi_key_sample(&k,true,true));
 assert(fuxi_key_sample(&k,true,true) && k.down);
 assert(!fuxi_key_sample(&k,true,false)); // bounce back down
 assert(!fuxi_key_sample(&k,true,true));assert(k.down);
 assert(!fuxi_key_sample(&k,true,false));
 assert(fuxi_key_sample(&k,true,false) && !k.down);
 assert(!fuxi_key_sample(&k,true,false));
 k=(struct fuxi_key_state){0};
 assert(!fuxi_key_sample(&k,true,false));
 assert(!fuxi_key_sample(&k,false,false));
 assert(!fuxi_key_sample(&k,true,false) && !k.armed);
 assert(!fuxi_key_sample(&k,true,false) && k.armed);
 unsigned failures=0;
 for(unsigned i=0;i<7;i++)assert(!fuxi_keys_failed(&failures,true));
 assert(!fuxi_keys_failed(&failures,false) && !failures);
 for(unsigned i=0;i<7;i++)assert(!fuxi_keys_failed(&failures,true));
 assert(fuxi_keys_failed(&failures,true) && failures==8);
}
int main(void) {
 amfi_test();container_test();keys_test();
 puts("PASS: AMFI ABI/dispatch/faults/disable/concurrent log-once; container lookup/bounds/VM cleanup; held keys/debounce/failure limit");
}
'''


def function(text, name):
    match = re.search(r'(?:static )?(?:void|int)\n' + name + r'\([^;]+?\n\{', text)
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
static unsigned tick, start_failure, downs, ups, stop_logs, terminated;
static uint16_t fk_pon_apid=1, fk_gpio_apid=2, keys_pon_apid=1, keys_gpio_apid=2;
static unsigned fk_last_status, keys_last_status;
static bool held;
static void fuxi_perf_input(void) {}
static void sweet_hid_key(int k, int down) {
 assert(k==0);
 if(down) { downs++;assert(tick==(held?7:3)); }
 else { ups++;assert(tick==start_failure+7); }
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
  if(driver==0)fk_thread(NULL,0);else keys_thread(NULL,0);
  assert(downs==1 && ups==1 && stop_logs==1 && terminated==1);
  if(held)break;
 }
 puts("PASS: actual Fuxi/Sweet polling threads ignore boot hold, debounce, stop on eight failures and release HID downs");
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
        source.write_text(THREAD_MODEL + (SRC / 'fuxi_key_state.h').read_text() +
                          function((SRC / 'fuxi_keys.c').read_text(), 'fk_thread') +
                          function((SRC / 'sweet_keys.c').read_text(), 'keys_thread') + THREAD_TEST)
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    for filename in ('fuxi_keys.c', 'sweet_keys.c'):
        text = (SRC / filename).read_text()
        assert 'fuxi_key_sample(' in text and 'fuxi_keys_failed(' in text
        assert 'sweet_spmi_bus_lock();' in text and 'sweet_spmi_bus_unlock();' in text
    assert 'kIOHIDEventOptionIsBuiltIn' in (SRC / 'SweetHID.cpp').read_text()
    print('PASS: both button drivers use tested state and common SPMI lock; HID marks built-in')


if __name__ == '__main__':
    main()
