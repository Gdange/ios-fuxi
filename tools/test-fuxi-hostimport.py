#!/usr/bin/env python3
"""ASan/UBSan tests of actual HostImport user-client methods, with VM/IOKit models.

No Apple SDK, source fixture or hardware. Cache instructions/SMMU are covered
separately by test-fuxi-metal.py's backend model, not executed on Linux.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index(' IOReturn '+name+'(')
    body = source.index('{', start)
    end, depth = body+1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]+'\n'


PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "fuxi_abi_wire.h"
using IOReturn=int;using IOByteCount=uint64_t;using ppnum_t=uint32_t;
using task_t=unsigned;using IOOptionBits=unsigned;
constexpr task_t kernel_task=99;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNoResources=2,
 kIOReturnNoMemory=3,kIOReturnBusy=4,kIOReturnIOError=5;
constexpr unsigned kIODirectionNone=0,kIODirectionInOut=3,kIOMemoryMapperNone=8,kIOMemoryPersistent=16,
 kIOMapAnywhere=32,kIOMapDefaultCache=0;
#define bzero(p,n) memset(p,0,n)
static task_t caller=7;
static task_t current_task() {return caller;}
static unsigned descriptors,maps,completes,prepares,fail,unmaps,waits;
static bool busy,undrained;
static bool connected=true;
static int openLock,clientLock;
static void IOLockLock(int *){}
static void IOLockUnlock(int *){}
static void IOLog(const char *,...){}
static void abiTrace(const char *,int,int){}
static bool pmap_valid_page(ppnum_t p) {return p!=0x12345;}
constexpr unsigned VM_WIMG_MASK=255,VM_WIMG_DEFAULT=2,VM_WIMG_WCOMB=6;
static unsigned hostCache=VM_WIMG_DEFAULT;
static unsigned pmap_cache_attributes(ppnum_t) {return fail==12?7:hostCache;}
struct IOMemoryMap;
struct IOMemoryDescriptor {
 unsigned refs=1,wired=0;uint64_t length=0;
 IOMemoryDescriptor() {descriptors++;}
 virtual ~IOMemoryDescriptor() {assert(!wired);descriptors--;}
 void retain() {refs++;}
 void release() {assert(refs);if(!--refs)delete this;}
 static IOMemoryDescriptor *withAddressRange(uint64_t a,uint64_t n,unsigned options,task_t task) {
  assert(task==7 && options==(kIODirectionInOut|kIOMemoryMapperNone|kIOMemoryPersistent));
  // Model holes, inaccessible/read-only mappings and entry allocation failure.
  if(fail==1 || a!=0x100000)return nullptr;
  auto *m=new IOMemoryDescriptor;m->length=n;return m;
 }
 IOReturn prepare() {prepares++;if(fail==2)return kIOReturnBadArgument;wired++;return 0;}
 IOReturn complete() {assert(wired);wired--;completes++;return 0;}
 uint64_t getLength() {return fail==3?length-1:length;}
 uint64_t getPhysicalSegment(uint64_t off,IOByteCount *n,unsigned flags) {
  assert(wired && flags==kIOMemoryMapperNone && !(off&16383));
  *n=fail==4?4096:16384;
  if(fail==5 && off==16384)return 0; // unwind a partially walked scatter list
  if(fail==6)return UINT64_C(1)<<36;
  if(fail==7)return 0x12345ULL<<14; // foreign/device page
  if(fail==8)return 0x10000001;
  return 0x10000000+off*3;
 }
 IOMemoryMap *createMappingInTask(task_t,uint64_t,unsigned);
};
struct IOMemoryMap {
 IOMemoryDescriptor *md;bool wired=false;
 IOMemoryMap(IOMemoryDescriptor *m):md(m) {maps++;md->retain();}
 int wireRange(unsigned options,uint64_t off,uint64_t n) {
  assert(!off && n==md->length);
  if(options==kIODirectionInOut) {assert(!wired);if(fail==13)return 1;wired=true;}
  else {assert(!options && wired);if(fail==14)return 1;wired=false;}
  return 0;
 }
 void release() {assert(!wired);maps--;md->release();delete this;}
 uint64_t getVirtualAddress() {return fail==10?0:0x400000;}
 uint64_t getLength() {return fail==11?md->length-1:md->length;}
};
IOMemoryMap *IOMemoryDescriptor::createMappingInTask(task_t task,uint64_t a,unsigned options) {
 assert(wired && task==kernel_task && !a && options==(kIOMapAnywhere|kIOMapDefaultCache));
 return fail==9?nullptr:new IOMemoryMap(this);
}
struct IOBufferMemoryDescriptor {void release(){assert(false);}};
struct FuxiAdrenoBoView {void release(){assert(false);}};
static int fuxi_gpu_abi_unmap(uint64_t va,uint64_t bytes) {
 assert(va==0x20000000 && bytes==32768);unmaps++;
 return undrained?-2:busy?1:0;
}
'''

TEST = r'''
};
static void empty() {assert(!maps && !descriptors);}
int main() {
 static_assert(NativeAdrenoAbiVersion==2);
 static_assert(NativeAdrenoAbiHostPageBytes==16384);
 static_assert(sizeof(NativeAdrenoAbiBoImport)==16);
 assert(!fuxi_adreno_host_page_ok(0x12345ULL<<14));
 for(hostCache=0;hostCache<512;hostCache++) {
  unsigned cache=hostCache&VM_WIMG_MASK;
  assert(fuxi_adreno_host_page_ok(0x10000000)==(cache==VM_WIMG_DEFAULT || cache==VM_WIMG_WCOMB));
 }
 hostCache=VM_WIMG_DEFAULT;
 assert(!fg_abi_import_valid(0,16384));
 assert(!fg_abi_import_valid(0x100000,0));
 assert(!fg_abi_import_valid(UINT64_MAX-16383,32768));
 assert(!fg_abi_import_valid(UINT64_C(1)<<47,16384));
 assert(!fg_abi_import_valid(0x100000,FG_ABI_MAX_BYTES+16384));
 assert(fg_abi_import_valid((UINT64_C(1)<<47)-16384,16384));
 for(unsigned i=1;i<16384;i++) {
  assert(!fg_abi_import_valid(0x100000+i,16384));
  assert(!fg_abi_import_valid(0x100000,16384+i));
 }
 NativeAdrenoAbiBoImport request={0x100000,32768};NativeAdrenoAbiBoAllocReply out={};
 Client c;
 for(fail=1;fail<=13;fail++) {
  unsigned before=completes;
  assert(c.importHost(request,out)!=0 && !out.handle && !c.wired_ && !c.bos_[0].handle);
  empty();assert(completes==before+(fail>=3));
 }
 fail=0;caller=8;assert(c.importHost(request,out)==kIOReturnBadArgument);empty();caller=7;
 c.task_=kernel_task;assert(c.importHost(request,out)==kIOReturnBadArgument);c.task_=7;
 c.wired_=FG_ABI_MAX_BYTES-16384;assert(c.importHost(request,out)==kIOReturnNoResources);c.wired_=0;
 for(auto &b:c.bos_)b.handle=1;
 assert(c.importHost(request,out)==kIOReturnNoResources);
 for(auto &b:c.bos_)b={};
 assert(!c.importHost(request,out) && out.handle && c.wired_==32768);
 auto &b=c.bos_[0];assert(!b.memory && !b.view && !b.token && b.importMemory->wired==1);
 assert(descriptors==1 && maps==1 && b.importMemory->refs==2);
 // An unmapped imported BO can be freed immediately.
 fail=14;unsigned prior=completes;
 assert(c.drop(b)==kIOReturnIOError && c.wired_==32768 && descriptors==1 && completes==prior);
 fail=0;assert(!c.drop(b) && !c.wired_ && !b.handle);empty();
 assert(!c.importHost(request,out));b.iova=0x20000000;busy=true;
 assert(c.drop(b)==kIOReturnBusy && maps==1 && descriptors==1 && c.wired_==32768);
 busy=false;assert(!c.drop(b));empty();
 // Death/close retires DMA before dropping the descriptor and its wired pages.
 assert(!c.importHost(request,out));b.iova=0x20000000;busy=true;connected=true;
 assert(!c.clientClose() && waits==1 && !c.live_ && !connected);empty();
 assert(!c.clientClose() && waits==1); // idempotent, no double complete
 // Unconfirmed hardware drain must preserve both references and pin. A reset
 // is modelled only to reclaim the intentional quarantine for LeakSanitizer.
 c.live_=true;assert(!c.importHost(request,out));b.iova=0x20000000;undrained=true;
 unsigned before=completes;
 assert(!c.clientClose() && !c.live_ && descriptors==1 && maps==1 && completes==before);
 undrained=false;assert(!c.drop(b));empty();
 assert(prepares>completes); // prepare failure itself never calls complete
 puts("PASS: ABI2/16K/overflow/ownership/quota, actual import failure unwind, pin/map refs, busy free, task close, quarantine and idempotent release");
}
'''


def main():
    source = (ROOT/'xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp').read_text()
    helper = (ROOT/'xnu/fuxi-adreno/fuxi_adreno_host.c').read_text()
    helper = helper[helper.index('bool fuxi_adreno_host_page_ok('):helper.index('#endif')]
    bo = source[source.index(' struct Bo {'):source.index(' task_t task_')]
    close = method(source, 'clientClose').replace(' override', '').replace('openLock);', '&openLock);')
    model = PREFIX + helper + 'struct Client {\n enum {MaxBos=64};\n' + bo + r'''
 task_t task_=7;uint64_t nextHandle_=1,wired_=0,lastFence_=1;
 int *lock_=&clientLock;bool live_=true;int pid_=0;
 void terminate(){}
 int wait(uint64_t,uint64_t) {waits++;busy=false;return undrained?kIOReturnIOError:0;}
''' + method(source, 'importHost') + method(source, 'drop') + close + TEST
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)/'import.cpp';path.write_text(model)
        exe = path.with_suffix('')
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I'+str(ROOT/'xnu/fuxi-adreno'), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
