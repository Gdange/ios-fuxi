#!/usr/bin/env python3
"""Compile actual pinned/patched Turnip import/query/free paths on Linux.

Supply external Mesa and hash-verified upstream text patches. The build recipe
checks/applies the exact known patch prefix; no Mesa sources are vendored.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    at = source.index('\n'+name+'(')
    start = source.rfind('\n\n', 0, at)+2
    body = source.index('{', at)
    end, depth = body+1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]+'\n'


PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vulkan/vulkan_core.h>
#include "NativeAdrenoABI.h"
#define VK_FROM_HANDLE(type,var,h) type *var=reinterpret_cast<type *>(h)
#define BITMASK_ENUM(type) unsigned
#define TU_RMV(...) ((void)0)
#define mesa_loge(...) ((void)0)
#define vk_error(p,r) (r)
#define vk_errorf(p,r,...) (r)
using kern_return_t=int;using mach_vm_address_t=uint64_t;
constexpr int KERN_SUCCESS=0,kIOReturnBusy=1;
constexpr int TU_BO_ALLOC_NO_FLAGS=0;
struct tu_physical_device {bool has_host_import;};
struct tu_device {tu_physical_device *physical_device;int dma_bo_lock;};
struct tu_bo {
 uint32_t gem_handle;uint64_t size,iova;const char *name;int refcnt;
 uint64_t unique_id,darwin_handle;uint32_t darwin_token;void *map;bool never_unmap;
};
static tu_bo storage;
static struct {NativeAdrenoAbiParams params;int conn;} dw;
static unsigned allocations,iova_frees,imports,bo_frees,cpu_unmaps,waits,fail;
static bool iova_live,bo_live;
static VkResult darwin_allocate_iova(tu_device *,uint64_t n,uint64_t requested,int flags,uint64_t *out) {
 assert(n && !requested && flags==TU_BO_ALLOC_NO_FLAGS && !iova_live);allocations++;
 if(fail==1)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 iova_live=true;*out=0x20000000;return VK_SUCCESS;
}
static void darwin_free_iova(tu_device *,uint64_t address,uint64_t size) {
 assert(iova_live && address==0x20000000 && size==32768);iova_live=false;iova_frees++;
}
static int dw_call(unsigned method,const void *input,size_t n,void *out,size_t m) {
 if(method==NativeAdrenoMethodBoImport) {
  assert(n==sizeof(NativeAdrenoAbiBoImport) && m==sizeof(NativeAdrenoAbiBoAllocReply));
  auto &r=*(const NativeAdrenoAbiBoImport *)input;
  assert(r.address==0x100000 && r.bytes==32768 && iova_live);imports++;
  if(fail==2)return 123;
  bo_live=true;((NativeAdrenoAbiBoAllocReply *)out)->handle=1;return 0;
 }
 if(method==NativeAdrenoMethodBoMap) {
  assert(n==sizeof(NativeAdrenoAbiBoMap) && m==sizeof(NativeAdrenoAbiBoMapReply));
  auto &r=*(const NativeAdrenoAbiBoMap *)input;
  assert(bo_live && r.handle==1 && r.iova==0x20000000 && r.permissions==3 && !r.writeback && r.shareability==2 && !r.reserved);
  if(fail==3)return 123;
  ((NativeAdrenoAbiBoMapReply *)out)->iova=r.iova;return 0;
 }
 assert(method==NativeAdrenoMethodBoFree && n==8 && !out && !m && bo_live);
 bo_frees++;
 if(fail==4)return 123; // unknown DMA ownership: preserve IOVA and kernel pin
 if(fail==5 && bo_frees%2==1)return kIOReturnBusy;
 bo_live=false;return 0;
}
static tu_bo *tu_device_lookup_bo(tu_device *,uint32_t h) {assert(h==1);return &storage;}
static const char *tu_debug_bos_add(tu_device *,uint64_t,const char *s) {return s;}
static void tu_debug_bos_del(tu_device *,tu_bo *){}
static void tu_dump_bo_init(tu_device *,tu_bo *){}
static void tu_dump_bo_del(tu_device *,tu_bo *){}
static void darwin_wait_idle(tu_device *) {waits++;}
static void u_rwlock_rdlock(int *){}
static void u_rwlock_rdunlock(int *){}
static bool p_atomic_dec_zero(int *p) {assert(*p>0);return --*p==0;}
static int mach_task_self() {return 1;}
static void IOConnectUnmapMemory64(int,uint32_t,int,uint64_t) {cpu_unmaps++;}
static void vk_android_get_ahb_buffer_properties(VkPhysicalDevice,const VkPhysicalDeviceExternalBufferInfo *,VkExternalBufferProperties *) {assert(false);}
'''

TEST = r'''
int main() {
 static_assert(NativeAdrenoAbiVersion==2 && NativeAdrenoAbiHostPageBytes==16384);
 tu_physical_device physical={false};tu_device dev={&physical,0};
 VkDevice device=reinterpret_cast<VkDevice>(&dev);
 VkMemoryHostPointerPropertiesEXT hp={};hp.memoryTypeBits=99;
 assert(tu_GetMemoryHostPointerPropertiesEXT(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,(void *)0x100000,&hp)==VK_ERROR_INVALID_EXTERNAL_HANDLE && !hp.memoryTypeBits);
 physical.has_host_import=true;
 for(auto p:{uintptr_t(0),uintptr_t(0x100001),uintptr_t(0x101000)})
  assert(tu_GetMemoryHostPointerPropertiesEXT(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,(void *)p,&hp)==VK_ERROR_INVALID_EXTERNAL_HANDLE && !hp.memoryTypeBits);
 assert(tu_GetMemoryHostPointerPropertiesEXT(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT,(void *)0x100000,&hp)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 assert(!tu_GetMemoryHostPointerPropertiesEXT(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,(void *)0x100000,&hp) && hp.memoryTypeBits==1);
 VkPhysicalDeviceExternalBufferInfo bi={};bi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
 VkExternalBufferProperties bp={};
 VkPhysicalDeviceImageFormatInfo2 ii={};ii.type=VK_IMAGE_TYPE_2D;ii.tiling=VK_IMAGE_TILING_LINEAR;
 VkExternalImageFormatProperties ip={};
 for(unsigned enabled=0;enabled<2;enabled++) {
  physical.has_host_import=enabled;
  tu_GetPhysicalDeviceExternalBufferProperties(reinterpret_cast<VkPhysicalDevice>(&physical),&bi,&bp);
  assert(bp.externalMemoryProperties.externalMemoryFeatures==(enabled?VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT:0));
  assert(!bp.externalMemoryProperties.exportFromImportedHandleTypes);
  assert(tu_get_external_image_format_properties(&physical,&ii,bi.handleType,&ip)==(enabled?VK_SUCCESS:VK_ERROR_FORMAT_NOT_SUPPORTED));
 }
 assert(ip.externalMemoryProperties.externalMemoryFeatures==VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
 assert(ip.externalMemoryProperties.compatibleHandleTypes==bi.handleType && !ip.externalMemoryProperties.exportFromImportedHandleTypes);
 ii.tiling=VK_IMAGE_TILING_OPTIMAL;
 assert(tu_get_external_image_format_properties(&physical,&ii,bi.handleType,&ip)==VK_ERROR_FORMAT_NOT_SUPPORTED);
 ii.tiling=VK_IMAGE_TILING_LINEAR;ii.type=VK_IMAGE_TYPE_3D;
 assert(tu_get_external_image_format_properties(&physical,&ii,bi.handleType,&ip)==VK_ERROR_FORMAT_NOT_SUPPORTED);
 tu_bo *out=(tu_bo *)1;
 assert(darwin_bo_init_host(&dev,&out,(void *)0x100000,32768)==VK_ERROR_INVALID_EXTERNAL_HANDLE && !out && !allocations);
 dw.params.flags=NativeAdrenoParamsHostImport;
 for(auto n:{uint64_t(0),uint64_t(4096),uint64_t(32769),uint64_t(1)<<29})
  assert(darwin_bo_init_host(&dev,&out,(void *)0x100000,n)==VK_ERROR_INVALID_EXTERNAL_HANDLE && !out && !allocations);
 assert(darwin_bo_init_host(&dev,&out,(void *)0x101000,32768)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 assert(darwin_bo_init_host(&dev,&out,(void *)(UINTPTR_MAX-16383),32768)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 for(fail=1;fail<=3;fail++) {
  assert(darwin_bo_init_host(&dev,&out,(void *)0x100000,32768)!=VK_SUCCESS && !out && !bo_live && !iova_live);
 }
 fail=0;assert(!darwin_bo_init_host(&dev,&out,(void *)0x100000,32768));
 assert(out==&storage && out->map==(void *)0x100000 && out->never_unmap && !out->darwin_token && out->darwin_handle==1 && out->refcnt==1);
 out->refcnt=2;unsigned before=bo_frees;
 darwin_bo_finish(&dev,out);assert(out->refcnt==1 && bo_frees==before && bo_live && iova_live);
 darwin_bo_finish(&dev,out);assert(!bo_live && !iova_live && !out->gem_handle && !cpu_unmaps);
 assert(!darwin_bo_init_host(&dev,&out,(void *)0x100000,32768));fail=4;
 darwin_bo_finish(&dev,out);assert(bo_live && iova_live && !out->gem_handle && !cpu_unmaps);
 // The Vulkan allocation branch rejects incompatible memory type, duplicate
 // import handles, foreign handles and wrong host alignment before allocating.
 bo_live=iova_live=false;fail=0;
 VkMemoryAllocateInfo ai={};ai.allocationSize=32768;
 VkImportMemoryHostPointerInfoEXT hi={};hi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;hi.pHostPointer=(void *)0x100000;
 VkImportMemoryFdInfoKHR fi={};fi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
 Memory mem={};unsigned prior_allocs=allocations;
 ai.memoryTypeIndex=1;assert(allocate_host(&dev,&mem,&ai,&hi)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 ai.memoryTypeIndex=0;assert(allocate_host(&dev,&mem,&ai,&hi,&fi)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 hi.pHostPointer=nullptr;assert(allocate_host(&dev,&mem,&ai,&hi)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 hi.pHostPointer=(void *)0x101000;assert(allocate_host(&dev,&mem,&ai,&hi)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 hi.pHostPointer=(void *)0x100000;ai.allocationSize=4096;assert(allocate_host(&dev,&mem,&ai,&hi)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 ai.allocationSize=32768;physical.has_host_import=false;assert(allocate_host(&dev,&mem,&ai,&hi)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 physical.has_host_import=true;hi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT;
 assert(allocate_host(&dev,&mem,&ai,&hi)==VK_ERROR_INVALID_EXTERNAL_HANDLE && allocations==prior_allocs);
 hi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
 assert(!allocate_host(&dev,&mem,&ai,&hi) && mem.bo==&storage);
 darwin_bo_finish(&dev,mem.bo);assert(!bo_live && !iova_live);
 // KMD leaves failed-free ownership in the kernel; model reset for the next case.
 bo_live=iova_live=false;fail=0;
 assert(!darwin_bo_init_host(&dev,&out,(void *)0x100000,32768));fail=5;bo_frees=0;
 darwin_bo_finish(&dev,out);assert(bo_frees==2 && !iova_live && !bo_live && waits>=4);
 puts("PASS: actual Turnip host pointer/image/buffer queries, ABI2/16K validation, import/map unwind, same pointer, BO refcount/free/retry and failed-free IOVA retention");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mesa', type=Path, required=True)
    ap.add_argument('--patches', type=Path, required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        subprocess.run(['python3', str(ROOT/'tools/build-fuxi-turnip.py'), '--mesa', str(args.mesa.resolve()),
                        '--patches', str(args.patches.resolve()), '--build', str(out/'unused-build'), '--verify-inputs-only'], check=True)
        folder = args.mesa/'src/freedreno/vulkan'
        model = PREFIX
        for file, names in [('tu_device.cc', ('tu_GetMemoryHostPointerPropertiesEXT',)),
                            ('tu_formats.cc', ('tu_get_external_image_format_properties',)),
                            ('tu_buffer.cc', ('tu_GetPhysicalDeviceExternalBufferProperties',)),
                            ('tu_knl_darwin.cc', ('darwin_bo_init_host', 'darwin_bo_finish'))]:
            source = (folder/file).read_text()
            model += ''.join(function(source, name) for name in names)
        # Exercise the exact vkAllocateMemory host branch as well, with only
        # Vulkan object allocation/accounting outside the branch modelled.
        source = (folder/'tu_device.cc').read_text()
        start = source.index('   if (host_info && host_info->handleType) {')
        body = source.index('{', start)
        end, depth = body+1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        model += r'''
static VkResult tu_bo_init_host(tu_device *d,tu_bo **b,void *p,uint64_t n) {
 return darwin_bo_init_host(d,b,p,n);
}
struct Memory {tu_bo *bo;};
static VkResult allocate_host(tu_device *device,Memory *mem,const VkMemoryAllocateInfo *pAllocateInfo,
 const VkImportMemoryHostPointerInfoEXT *host_info,const VkImportMemoryFdInfoKHR *fd_info=nullptr) {
 VkResult result=VK_ERROR_INVALID_EXTERNAL_HANDLE;
''' + source[start:end] + '\n return result;\n}\n'
        model += TEST
        model = model.replace('#include <cstring>', '#include <cstring>\n#include <initializer_list>')
        path = out/'import.cpp';path.write_text(model)
        exe = path.with_suffix('')
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-missing-field-initializers', '-fsanitize=address,undefined', '-I'+str(ROOT/'xnu/fuxi-adreno'),
                        '-I'+str(args.mesa/'include'), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
