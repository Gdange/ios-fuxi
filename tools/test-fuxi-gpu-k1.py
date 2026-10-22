#!/usr/bin/env python3
"""K1 actual user-client dispatch/allocation tests with ASan/UBSan IOKit models.

The backend register/cache/containment model lives in test-fuxi-metal.py.
No Apple SDK or hardware is used here.
"""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hostimport', ROOT/'tools/test-fuxi-hostimport.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)

BUFFER = r'''
constexpr unsigned kIOMemoryKernelUserShared=64,kIOMapWriteCombineCache=128,kIOMemoryPhysicallyContiguous=256;
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {
 void *data=nullptr;
 ~IOBufferMemoryDescriptor() override {free(data);}
 static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t task,unsigned options,uint64_t n,uint64_t mask) {
  assert(task==kernel_task && mask && (options&kIOMapWriteCombineCache));
  if(fail==15 || (fail==16 && (options&kIOMemoryPhysicallyContiguous)))return nullptr;
  auto *m=new IOBufferMemoryDescriptor;m->length=n;m->data=calloc(1,n);assert(m->data);return m;
 }
 void *getBytesNoCopy() {return data;}
};
struct FuxiAdrenoBoView {
 IOBufferMemoryDescriptor *backing=nullptr;
 bool bind(IOBufferMemoryDescriptor *m) {backing=m;m->retain();return true;}
 void release() {if(backing){backing->complete();backing->release();}delete this;}
};
'''
EXTRA = r'''
constexpr int kIOReturnNotReady=6,kIOReturnTimeout=7,kIOReturnUnsupported=8,kIOReturnNotOpen=9;
static bool available=true;
static int timeRc,statusRc;
static uint64_t gpuTicks=0xabcdef0123456789ULL,gpuFaults=7,gpuSuspends=3;
static int fuxi_gpu_abi_available() {return available;}
static int fuxi_gpu_abi_timestamp(uint64_t *t) {*t=0;if(!timeRc)*t=gpuTicks;return timeRc;}
static int fuxi_gpu_abi_status(uint64_t *f,uint64_t *s) {*f=*s=0;if(!statusRc){*f=gpuFaults;*s=gpuSuspends;}return statusRc;}
static int fuxi_gpu_abi_poll(uint64_t) {return 0;}
static int fuxi_gpu_abi_map(uint64_t,uintptr_t,uint64_t,unsigned,unsigned,unsigned) {return 0;}
static int fuxi_gpu_abi_map_import(uint64_t,uintptr_t,uint64_t,unsigned) {return 0;}
static int fuxi_gpu_abi_submit(const uint64_t *,const uint64_t *,unsigned,uint64_t *) {return -1;}
static uint64_t mach_absolute_time() {return 0;}
static constexpr unsigned NSEC_PER_MSEC=1000000;
static void clock_interval_to_deadline(unsigned,unsigned,uint64_t *d) {*d=1;}
static void IOSleep(unsigned) {}
static const char *abiStep(unsigned) {return "test";}
using OSObject=int;using IOExternalMethodDispatch=int;
struct IOExternalMethodArguments {
 unsigned scalarInputCount=0,scalarOutputCount=0,asyncWakePort=0;
 void *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
 unsigned structureInputSize=0,structureOutputSize=0;
 const void *structureInput=nullptr;void *structureOutput=nullptr;
};
'''
TEST = r'''
};
static IOReturn call(Client &c,unsigned method,const void *in,void *out) {
 IOExternalMethodArguments a;
 a.structureInputSize=fg_abi_input_size(method);a.structureInput=in;
 a.structureOutputSize=fg_abi_output_size(method);a.structureOutput=out;
 return c.externalMethod(method,&a,nullptr,nullptr,nullptr);
}
int main() {
 Client c;
 NativeAdrenoAbiTimestamp ts{};NativeAdrenoAbiStatus status{};
 assert(!call(c,7,nullptr,&ts) && ts.ticks==gpuTicks);
 assert(!call(c,8,nullptr,&status) && status.faults==7 && status.suspends==3);
 available=false; // status bypasses availability so Turnip can see device loss
 assert(!call(c,8,nullptr,&status) && status.faults==7);
 for(timeRc=-1;timeRc>=-3;timeRc--) {
  ts.ticks=99;
  assert(call(c,7,nullptr,&ts)==(timeRc==-1?kIOReturnNotReady:timeRc==-2?kIOReturnIOError:kIOReturnTimeout));
  assert(!ts.ticks);
 }
 timeRc=0;statusRc=-1;status={99,99};
 assert(call(c,8,nullptr,&status)==kIOReturnNotReady && !status.faults && !status.suspends);
 statusRc=0;available=true;
 // Exact struct lengths, no scalar/async/descriptor ABI, no input on 7/8.
 for(unsigned m=7;m<=8;m++) {
  IOExternalMethodArguments a;a.structureOutput=&status;
  a.structureOutputSize=fg_abi_output_size(m)-1;
  assert(c.externalMethod(m,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
  a.structureOutputSize=fg_abi_output_size(m)+1;
  assert(c.externalMethod(m,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
  a.structureOutputSize=fg_abi_output_size(m);a.structureInputSize=1;a.structureInput=&ts;
  assert(c.externalMethod(m,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
  a.structureInputSize=0;a.scalarInputCount=1;
  assert(c.externalMethod(m,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
  a.scalarInputCount=0;a.structureOutput=nullptr;
  assert(c.externalMethod(m,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
 }
 c.live_=false;status={99,99};
 assert(call(c,8,nullptr,&status)==kIOReturnNotOpen && !status.faults && !status.suspends);
 c.live_=true;
 NativeAdrenoAbiBoAlloc alloc{4096,12,0};NativeAdrenoAbiBoAllocReply out{};
 // Allocate 1024 real model-backed objects (16 MiB after 16K rounding).
 for(unsigned i=0;i<FG_ABI_MAX_BOS;i++) {
  assert(!call(c,2,&alloc,&out) && out.handle);
  assert(c.bos_[i].handle==out.handle && c.bos_[i].allocated==16384);
 }
 assert(c.wired_==FG_ABI_MAX_BOS*16384 && descriptors==FG_ABI_MAX_BOS);
 assert(call(c,2,&alloc,&out)==kIOReturnNoResources && !out.handle);
 NativeAdrenoAbiBoFree release{c.bos_[700].handle};
 assert(!call(c,4,&release,nullptr));
 assert(!call(c,2,&alloc,&out) && out.handle==c.bos_[700].handle);
 NativeAdrenoAbiMetadata meta{};meta.handle=out.handle;meta.bytes=64;
 for(unsigned i=0;i<64;i++)meta.text[i]=(char)i;
 assert(!call(c,9,&meta,nullptr));
 NativeAdrenoAbiMetadata reply;
 for(unsigned n=0;n<64;n++) {
  meta.bytes=n;memset(&reply,0xff,sizeof(reply));
  assert(call(c,10,&meta,&reply)==kIOReturnBadArgument);
  NativeAdrenoAbiMetadata zero{};assert(!memcmp(&reply,&zero,sizeof(reply)));
 }
 meta.bytes=64;assert(!call(c,10,&meta,&reply));
 assert(reply.handle==meta.handle && reply.bytes==64 && !memcmp(meta.text,reply.text,64));
 meta.bytes=65;assert(call(c,10,&meta,&reply)==kIOReturnBadArgument);
 meta.bytes=64;meta.reserved=1;assert(call(c,10,&meta,&reply)==kIOReturnBadArgument);meta.reserved=0;
 meta.bytes=0;assert(!call(c,9,&meta,nullptr) && !call(c,10,&meta,&reply) && !reply.bytes);
 // Keep invalid-token and no-lease errors: Turnip HEAD only retries BoFree.
 NativeAdrenoAbiBoCpuMap cpu{meta.handle};NativeAdrenoAbiBoCpuMapReply view{};
 assert(!call(c,13,&cpu,&view));
 release.handle=meta.handle;assert(call(c,4,&release,nullptr)==kIOReturnBusy);
 NativeAdrenoAbiBoCpuUnmap undo{meta.handle,view.token,0};
 undo.token++;assert(call(c,14,&undo,nullptr)==kIOReturnBadArgument);undo.token--;
 assert(!call(c,14,&undo,nullptr));
 assert(call(c,14,&undo,nullptr)==kIOReturnBadArgument);
 assert(!c.clientClose() && !c.wired_ && !descriptors && !maps);
 // Rounded allocation shares the wired quota with imported BOs; refusal
 // occurs before allocating backing storage, including corrupt accounting.
 c.live_=true;c.wired_=FG_ABI_MAX_WIRED_BYTES-16384;
 assert(!call(c,2,&alloc,&out) && c.wired_==FG_ABI_MAX_WIRED_BYTES);
 assert(call(c,2,&alloc,&out)==kIOReturnNoResources);
 assert(!c.drop(c.bos_[0]));c.wired_=FG_ABI_MAX_WIRED_BYTES-4096;
 assert(call(c,2,&alloc,&out)==kIOReturnNoResources);
 c.wired_=FG_ABI_MAX_WIRED_BYTES+1;assert(call(c,2,&alloc,&out)==kIOReturnNoResources);c.wired_=0;
 c.nextToken_=UINT32_MAX;assert(call(c,2,&alloc,&out)==kIOReturnNoResources);c.nextToken_=1;
 c.nextHandle_=UINT64_MAX;assert(call(c,2,&alloc,&out)==kIOReturnNoResources);c.nextHandle_=1;
 fail=15;assert(call(c,2,&alloc,&out)==kIOReturnNoMemory && !c.wired_ && !descriptors);
 fail=16;assert(!call(c,2,&alloc,&out));assert(!c.drop(c.bos_[0]));
 alloc.contiguous=1;assert(call(c,2,&alloc,&out)==kIOReturnNoMemory && !descriptors);
 puts("PASS: actual methods 7/8 dispatch/errors/lengths, 1024 BO allocation/refusal/reuse/close, shared wired quota, metadata refusal and CPU leases");
}
'''


def main():
    source = (ROOT/'xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp').read_text()
    prefix = host.PREFIX.replace('#include <cstdio>', '#include <cstdio>\n#include <cstdlib>')
    start = prefix.index('struct IOBufferMemoryDescriptor {')
    end = prefix.index('static int fuxi_gpu_abi_unmap', start)
    prefix = prefix[:start]+BUFFER+prefix[end:]
    helper = (ROOT/'xnu/fuxi-adreno/fuxi_adreno_host.c').read_text()
    helper = helper[helper.index('bool fuxi_adreno_host_page_ok('):helper.index('#endif')]
    bo = source[source.index(' enum {MaxBos='):source.index(' task_t task_')]
    close = host.method(source, 'clientClose').replace(' override', '').replace('openLock);', '&openLock);')
    external = host.method(source, 'externalMethod').replace(' override', '')
    # find() is pointer-returning, so use its adjacent source boundaries.
    find = source[source.index(' Bo *find('):source.index(' IOReturn wait(')]
    model = prefix+EXTRA+helper+'struct Client {\n'+bo+r'''
 /* The kernel allocates the table in initWithTask; the model owns it inline. */
 Bo storage_[MaxBos]={};
 Client(){bos_={storage_,storage_+MaxBos};}
 task_t task_=7;uint64_t nextHandle_=1,wired_=0,lastFence_=0,firstFence_=0;
 uint32_t nextToken_=1;bool queues_[8]={true};
 int *lock_=&clientLock;bool live_=true;int pid_=0;
 void terminate(){} void result(){}
 int wait(uint64_t,uint64_t) {waits++;busy=false;return 0;}
'''+find+''.join(host.method(source, n) for n in ('allocate', 'importHost', 'drop', 'run'))+close+external+TEST
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)/'k1.cpp';path.write_text(model)
        exe = path.with_suffix('')
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I'+str(ROOT/'xnu/fuxi-adreno'), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        # Parse the complete device helper on Linux, without pretending these
        # minimal Mach declarations constitute an iOS SDK or linked build.
        mach = Path(tmp)/'mach';mach.mkdir()
        (mach/'mach.h').write_text('''#include <stdint.h>
typedef uint32_t mach_port_t;
typedef mach_port_t task_t;
typedef int kern_return_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
#define MACH_PORT_NULL 0
extern "C" task_t mach_task_self(void);
''')
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsyntax-only', '-I'+tmp, str(ROOT/'tools/fuxi-gpu-abi-test.cpp')], check=True)
        print('PASS: complete device helper C++ syntax with minimal Mach declarations (no SDK/link)')


if __name__ == '__main__':
    main()
