#!/usr/bin/env python3
"""Run the real smoke on an external software ICD; inject timeout/loss/bad data.

No Turnip/GPU qualification is implied. Requires Linux clang++, an external
Vulkan include directory and the generated build-time SPIR-V header.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
PROXY=r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static PFN_vkInvalidateMappedMemoryRanges invalidate;
static PFN_vkMapMemory map;
static PFN_vkCmdDispatch dispatch;
static PFN_vkWaitForFences wait;
static PFN_vkDeviceWaitIdle idle;
static void *pointer;
static bool compute;
static VkResult mapped(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **p) {
 VkResult r=map(d,m,o,n,f,p);pointer=*p;return r;
}
static void dispatched(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z) {compute=true;dispatch(c,x,y,z);}
static VkResult invalidated(VkDevice d,uint32_t n,const VkMappedMemoryRange *r) {
 VkResult result=invalidate(d,n,r);
 if(getenv("BAD_FILL") || (compute && getenv("BAD_COMPUTE")))static_cast<uint32_t *>(pointer)[16383]^=1;
 return result;
}
static VkResult waited(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t ns) {
 assert(ns==2000000000ULL);
 if(getenv("TIMEOUT"))return VK_TIMEOUT;
 if(getenv("LOST"))return VK_ERROR_DEVICE_LOST;
 return wait(d,n,f,all,ns);
}
static VkResult idled(VkDevice d) {if(getenv("HANG_IDLE"))for(;;)pause();return idle(d);}
static PFN_vkVoidFunction getDevice(VkDevice d,const char *name) {
 PFN_vkVoidFunction f=gdpa(d,name);assert(f);
#define WRAP(n,var,fn) if(!strcmp(name,"vk" #n)){var=reinterpret_cast<PFN_vk##n>(f);return (PFN_vkVoidFunction)fn;}
 WRAP(MapMemory,map,mapped);WRAP(InvalidateMappedMemoryRanges,invalidate,invalidated);
 WRAP(CmdDispatch,dispatch,dispatched);WRAP(WaitForFences,wait,waited);WRAP(DeviceWaitIdle,idle,idled);
 return f;
}
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance i,const char *name) {
 if(!gipa) {
  assert(!strcmp(getenv("TU_DEBUG"),"nocb"));
  assert(!strcmp(getenv("TU_DARWIN_SERVICE"),"NativeAdrenoGpuPublished"));
  assert(!strcmp(getenv("TU_DARWIN_OPEN_TYPE"),"0"));
  void *lib=dlopen(getenv("SOFTWARE_ICD"),RTLD_NOW|RTLD_LOCAL);assert(lib);
  gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vk_icdGetInstanceProcAddr"));assert(gipa);
 }
 PFN_vkVoidFunction f=gipa(i,name);
 if(!strcmp(name,"vkGetDeviceProcAddr")){gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(f);return (PFN_vkVoidFunction)getDevice;}
 return f;
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--headers',type=Path,required=True);ap.add_argument('--generated',type=Path,required=True)
    ap.add_argument('--icd',type=Path,default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    args=ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);conf=d/'smoke.conf';src=d/'proxy.cpp';src.write_text(PROXY)
        proxy=d/'proxy.so';binary=d/'smoke';wrap=d/'wrap.cpp'
        wrap.write_text('#include <unistd.h>\nextern "C" uid_t __wrap_geteuid() {return 0;}\n')
        flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(args.headers)]
        subprocess.run(flags+['-fPIC','-shared',str(src),'-ldl','-o',str(proxy)],check=True)
        subprocess.run(flags+['-fsanitize=address,undefined','-I'+str(args.generated),
                             '-DFUXI_SMOKE_CONF="'+str(conf)+'"','-DFUXI_SMOKE_ICD="'+str(proxy)+'"',
                             str(ROOT/'tools/fuxi-vk-smoke.cpp'),str(wrap),'-Wl,--wrap=geteuid','-ldl','-o',str(binary)],check=True)
        env=dict(os.environ,SOFTWARE_ICD=str(args.icd),ASAN_OPTIONS='detect_leaks=0')
        def run(data,rc=0,**extra):
            if data is None:conf.unlink(missing_ok=True)
            else:conf.write_bytes(data)
            p=subprocess.run([str(binary),'--host-argv','ignored'],env=dict(env,**extra),text=True,capture_output=True,timeout=15)
            assert p.returncode==rc,(p.returncode,p.stdout,p.stderr)
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr,p.stderr
            return p.stdout
        result=run(None)
        assert 'stage 4 reuse-reopen PASS' in result and result.count('verify fill iteration')==18 and result.count('verify compute words')==2
        assert 'verify triangle inside/outside 6 red 8192 black 57344 PASS' in result
        print(result,end='')
        for n in (1,2,3):
            result=run(str(n).encode())
            assert 'done PASS' in result and ('stage %d '%(n+1)) not in result
        for data in (b'',b'0',b'5',b'2 junk',b'1\0',b'1'*32):
            assert 'stage 0 setup FAIL' in run(data,1)
        for flag,word in (('BAD_FILL','fill'),('BAD_COMPUTE','compute')):
            result=run(b'4',1,**{flag:'1'})
            assert word+' word 16383' in result or 'fill iteration 0 word 16383' in result
            assert 'stage 3 triangle BEGIN' not in result
        result=run(b'4',1,LOST='1');assert 'vkWaitForFences rc -4' in result and 'stage 2 compute BEGIN' not in result
        result=run(b'4',1,TIMEOUT='1');assert 'elapsed-ms' in result and 'bounded-idle rc 0' in result
        result=run(b'4',124,TIMEOUT='1',HANG_IDLE='1');assert 'vkDeviceWaitIdle after fence timeout exceeded 1 s' in result
        spec=importlib.util.spec_from_file_location('stage',ROOT/'tools/stage-fuxi-vulkan.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        root=d/'root';(root/'System/Library').mkdir(parents=True)
        library=d/'icd';probe=d/'probe';smoke=d/'signed-smoke'
        for p,kind in ((library,6),(probe,2),(smoke,2)):
            p.write_bytes(struct.pack('<8I',0xfeedfacf,0x100000c,0,kind,1,16,0,0)+struct.pack('<4I',0x1d,16,48,0))
        conf.write_bytes(b'4\n')
        with patch.object(m.os,'geteuid',return_value=0),patch.object(m.os,'chown') as chown:
            for _ in range(2):m.stage(root,library,probe,smoke,conf)
            assert chown.call_count==8
            target=root/'usr/local/bin/fuxi-vk-smoke';assert target.read_bytes()==smoke.read_bytes() and target.stat().st_mode&0o777==0o755
            target=root/'usr/local/share/fuxi/vk-smoke.conf';assert target.read_bytes()==b'4\n' and target.stat().st_mode&0o777==0o644
            target.unlink();target.symlink_to(d/'escape')
            try:m.stage(root,library,probe,smoke,conf);raise AssertionError('escaping config accepted')
            except ValueError:pass
    print('PASS: software Vulkan fill/compute/render/reuse/reopen; stages/host argv; last-word corruption; loss/timeout/bounded idle; signed staging modes/ownership/symlink rejection')


if __name__=='__main__':main()
