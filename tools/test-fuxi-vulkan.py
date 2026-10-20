#!/usr/bin/env python3
"""Exercise probe lifetime/no-submit protocol against a fake ICD and offline staging.

Requires --headers pointing to an external Mesa include/ directory; no Mesa is
vendored. This does not execute Turnip or validate hardware.
"""
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]

FAKE=r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool instance,device,queue;
extern "C" uid_t __wrap_geteuid() {return 0;}
static VkResult create(const VkInstanceCreateInfo *i,const VkAllocationCallbacks *,VkInstance *out) {
 assert(!instance && i->pApplicationInfo->apiVersion==VK_API_VERSION_1_1);
 assert(!strcmp(getenv("TU_DARWIN_SERVICE"),"NativeAdrenoGpuPublished"));
 assert(!strcmp(getenv("TU_DARWIN_OPEN_TYPE"),"0"));
 instance=true;*out=(VkInstance)1;return VK_SUCCESS;
}
static void destroy(VkInstance,const VkAllocationCallbacks *) {assert(instance && !device);instance=false;}
static VkResult enumerate(VkInstance,uint32_t *n,VkPhysicalDevice *out) {
 if(getenv("FAIL_ENUM"))return VK_ERROR_INITIALIZATION_FAILED;
 *n=1;if(out)out[0]=(VkPhysicalDevice)2;return VK_SUCCESS;
}
static void properties(VkPhysicalDevice,VkPhysicalDeviceProperties2 *p) {
 p->properties.vendorID=0x5143;p->properties.deviceID=0x43050a01;strcpy(p->properties.deviceName,"Turnip Adreno (TM) 740");
 auto *d=(VkPhysicalDeviceDriverProperties *)p->pNext;strcpy(d->driverName,"turnip");strcpy(d->driverInfo,"fake");
}
static void memory(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties *p) {p->memoryHeapCount=1;p->memoryHeaps[0]={268435456,1};}
static void families(VkPhysicalDevice,uint32_t *n,VkQueueFamilyProperties *p) {*n=1;if(p){p->queueCount=1;p->queueFlags=VK_QUEUE_COMPUTE_BIT;}}
static VkResult createDevice(VkPhysicalDevice,const VkDeviceCreateInfo *p,const VkAllocationCallbacks *,VkDevice *d) {
 assert(!device && p->queueCreateInfoCount==1 && p->pQueueCreateInfos->queueCount==1);
 if(getenv("FAIL_DEVICE"))return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 device=true;*d=(VkDevice)3;return VK_SUCCESS;
}
static void getQueue(VkDevice,uint32_t family,uint32_t index,VkQueue *q) {assert(device && !family && !index);queue=true;*q=(VkQueue)4;}
static void destroyDevice(VkDevice,const VkAllocationCallbacks *) {assert(device && queue);device=false;}
static PFN_vkVoidFunction gdpa(VkDevice,const char *name) {
 if(!strcmp(name,"vkGetDeviceQueue"))return (PFN_vkVoidFunction)getQueue;
 if(!strcmp(name,"vkDestroyDevice"))return (PFN_vkVoidFunction)destroyDevice;
 assert(!"unexpected device call, including any submit");return nullptr;
}
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance,const char *name) {
#define SYM(n,f) if(!strcmp(name,n))return (PFN_vkVoidFunction)f
 SYM("vkCreateInstance",create);SYM("vkDestroyInstance",destroy);SYM("vkEnumeratePhysicalDevices",enumerate);
 SYM("vkGetPhysicalDeviceProperties2",properties);SYM("vkGetPhysicalDeviceMemoryProperties",memory);
 SYM("vkGetPhysicalDeviceQueueFamilyProperties",families);SYM("vkCreateDevice",createDevice);SYM("vkGetDeviceProcAddr",gdpa);
 assert(!"unexpected instance call");return nullptr;
}
__attribute__((destructor)) static void check() {assert(!instance && !device);}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--headers',type=Path,required=True);args=ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);fake=d/'fake.cpp';fake.write_text(FAKE)
        lib=d/'fake.so';probe=d/'fuxi-vk-probe';wrap=d/'wrap.cpp'
        wrap.write_text('#include <unistd.h>\nextern "C" uid_t __wrap_geteuid() {return 0;}\n')
        flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(args.headers)]
        subprocess.run(flags+['-shared','-fPIC',str(fake),'-o',str(lib)],check=True)
        subprocess.run(flags+['-fsanitize=address,undefined',str(ROOT/'tools/fuxi-vk-probe.cpp'),str(wrap),'-Wl,--wrap=geteuid','-ldl','-o',str(probe)],check=True)
        result=subprocess.check_output([str(probe),str(lib)],text=True)
        assert 'vendor 0x5143 device 0x43050a01' in result and 'done PASS' in result
        print(result,end='')
        import os
        for flag in ('FAIL_ENUM','FAIL_DEVICE'):
            r=subprocess.run([str(probe),str(lib)],env=dict(os.environ,**{flag:'1'}),capture_output=True,text=True)
            assert r.returncode==1 and 'done FAIL' in r.stdout
        tool=(ROOT/'tools/fuxi-vk-probe.cpp').read_text()
        parser=tool[tool.index('static const char *icd_path'):tool.index('\nstatic bool ok')]
        src=d/'parser.cpp';src.write_text('#include <assert.h>\n#include <string.h>\n'+parser+r'''
int main() {
 char host[]="/usr/libexec/peakpowermanagerd",self[]="/usr/local/bin/fuxi-vk-probe",arg[]="--launchd_boot",path[]="/fake/ICD";
 char *a[]={host,arg,path};assert(!strcmp(icd_path(3,a),"/usr/local/lib/libvulkan_freedreno.dylib"));
 a[0]=self;assert(!icd_path(2,a));a[1]=path;assert(icd_path(2,a)==path);
 assert(!icd_path(3,a));assert(!strcmp(icd_path(0,nullptr),"/usr/local/lib/libvulkan_freedreno.dylib"));
}
''')
        subprocess.run(flags+[str(src),'-o',str(d/'parser')],check=True);subprocess.run([str(d/'parser')],check=True)
        spec=importlib.util.spec_from_file_location('stage',ROOT/'tools/stage-fuxi-vulkan.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        root=d/'root';(root/'System/Library').mkdir(parents=True)
        library=d/'icd';binary=d/'probe'
        for p,kind in ((library,6),(binary,2)):
            p.write_bytes(struct.pack('<8I',0xfeedfacf,0x100000c,0,kind,1,16,0,0)+struct.pack('<4I',0x1d,16,48,0))
        with patch.object(m.os,'geteuid',return_value=0),patch.object(m.os,'chown') as chown:
            for _ in range(2):m.stage(root,library,binary)
            assert chown.call_count==4
        for source,(name,_) in zip((library,binary),m.ARTIFACTS):
            target=root/name.lstrip('/');assert target.read_bytes()==source.read_bytes()
            assert target.stat().st_mode&0o777==0o755
        with patch.object(m.os,'geteuid',return_value=1000):
            try:m.stage(root,library,binary);raise AssertionError('nonroot accepted')
            except PermissionError:pass
    print('PASS: ICD no-submit call sequence, cleanup after enumeration/device failure, host argv, idempotent mode/owner staging')


if __name__=='__main__':main()
