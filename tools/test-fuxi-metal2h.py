#!/usr/bin/env python3
"""Execute the real managed-BO view, cpu-map lookup and drop with retained maps."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]


def main():
    client=(ROOT/'xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp').read_text()
    view=client[client.index('class FuxiAdrenoBoView'):client.index('\nstatic IOLock')]
    drop=client[client.index(' IOReturn drop('):client.index(' IOReturn allocate(')]
    lookup=client[client.index(' IOReturn clientMemoryForType('):client.index(' IOReturn clientClose(')].replace(' override','')
    assert 'kIOMemoryTypePhysical' not in client and 'IOAddressRange' not in client
    assert 'initSubRange(backing,0,backing->getLength(),kIODirectionInOut)' in view
    model=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x,y)
using IOReturn=int;using IOOptionBits=unsigned;using UInt32=unsigned;
constexpr int kIOReturnSuccess=0,kIOReturnBusy=1,kIOReturnIOError=2,kIOReturnBadArgument=3,kIOReturnNotFound=4;
constexpr unsigned kIODirectionInOut=3,kIOMapAnywhere=1,kIOMapWriteCombineCache=2;
static unsigned freed,completed,unmaps;static bool failBind,failDrain;
struct IOMemoryDescriptor {
 unsigned refs=1;virtual ~IOMemoryDescriptor()=default;
 void retain() {++refs;} void release() {assert(refs);if(!--refs){free();delete this;}}
 virtual void free() {} virtual uint64_t getLength() {return 65536;}
};
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {
 bool prepared=true;
 void complete() {assert(prepared);prepared=false;++completed;}
 void free() override {assert(!prepared);++freed;}
};
struct IOSubMemoryDescriptor: IOMemoryDescriptor {
 IOMemoryDescriptor *parent=nullptr;
 bool initSubRange(IOMemoryDescriptor *p,uint64_t off,uint64_t bytes,unsigned dir) {
  assert(!off && bytes==p->getLength() && dir==kIODirectionInOut);
  if(failBind)return false;parent=p;p->retain();return true;
 }
 void free() override {if(parent)parent->release();}
};
static void IOLockLock(int) {} static void IOLockUnlock(int) {}
static void abiTrace(const char *,int,int) {}
static int fuxi_gpu_abi_unmap(uint64_t,uint64_t) {++unmaps;return failDrain?1:0;}
'''+view+r'''
struct Client {
 struct Bo {IOBufferMemoryDescriptor *memory;FuxiAdrenoBoView *view;
 uint64_t handle,bytes,allocated,iova;unsigned token,leases;} bos_[1]={};
 bool live_=true;int lock_=0,pid_=78;uint64_t wired_=65536;
'''+drop+lookup+r'''
};
int main() {
 for(unsigned retained=0;retained<2;retained++) {
  Client c;auto *mem=new IOBufferMemoryDescriptor;auto *view=new FuxiAdrenoBoView;
  assert(view->bind(mem));assert(mem->refs==3);
  c.bos_[0]={mem,view,1,65536,65536,0x20000000,7,0};
  IOOptionBits options=0;IOMemoryDescriptor *map=nullptr;
  assert(c.clientMemoryForType(7,&options,&map)==kIOReturnNotFound);
  c.bos_[0].leases=1;
  assert(c.clientMemoryForType(7,&options,&map)==0 && map==view);
  assert(options==(kIOMapAnywhere|kIOMapWriteCombineCache));
  IOMemoryDescriptor *again=nullptr;
  assert(c.clientMemoryForType(7,&options,&again)==0 && again==map);again->release();
  assert(c.clientMemoryForType(8,&options,&again)==kIOReturnNotFound && !again);
  failDrain=true;assert(c.drop(c.bos_[0])==kIOReturnBusy && c.wired_==65536 && mem->prepared);failDrain=false;
  if(!retained)map->release();
  assert(c.drop(c.bos_[0])==0 && !c.wired_);
  assert(completed==retained+(!retained) && freed==completed);
  if(retained){assert(mem->prepared && mem->refs==2);map->release();}
  assert(completed==retained+1 && freed==completed);
 }
 failBind=true;auto *mem=new IOBufferMemoryDescriptor;auto *view=new FuxiAdrenoBoView;
 assert(!view->bind(mem));view->release();assert(mem->refs==1);mem->complete();mem->release();
 assert(freed==3 && completed==3 && unmaps==4);
 puts("PASS: real subdescriptor/cpu-map/drop; same pager and unmap identity, NC flags, retained mapping lifetime, failed bind/drain");
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src=Path(tmp)/'view.cpp';src.write_text(model);exe=Path(tmp)/'view'
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__=='__main__': main()
