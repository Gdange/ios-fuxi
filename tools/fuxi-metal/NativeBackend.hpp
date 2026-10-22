/* Original MIT adapter. No NativeMetal/translator source is linked here. */
#pragma once
#include "FuxiNativeABI.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <cstdlib>
#include <atomic>
static std::atomic<void *> &nativeLibraryHandle(){static std::atomic<void *> handle{nullptr};return handle;}
static uint64_t nativeMetric(unsigned index){
 void *library=nativeLibraryHandle().load();
 auto metric=library?(uint64_t(*)(unsigned))dlsym(library,"fuxi_native_metal_metric"):nullptr;
 return metric?metric(index):0;
}
#include "NativePreflight.hpp"
#ifndef FUXI_MTL_NATIVE_LIBRARY
#define FUXI_MTL_NATIVE_LIBRARY "/System/Library/Extensions/FuxiNativeMetal.bundle/FuxiNativeMetal"
#endif
#ifndef FUXI_MTL_ICD
#define FUXI_MTL_ICD "/usr/local/lib/libvulkan_freedreno.dylib"
#endif
// Query and CLOSE before Turnip opens: the current kernel admits one connection.
__attribute__((unused)) static bool nativeHostImportPreflight(uint32_t port) {
 void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
 if(!io)return false;
 auto openClient=(int(*)(uint32_t,uint32_t,uint32_t,uint32_t *))dlsym(io,"IOServiceOpen");
 auto closeClient=(int(*)(uint32_t))dlsym(io,"IOServiceClose");
 auto query=(int(*)(uint32_t,uint32_t,const void *,size_t,void *,size_t *))dlsym(io,"IOConnectCallStructMethod");
 bool ok=openClient && closeClient && query && fuxi::nativeImportPreflight(
  [&](uint32_t *connection){return !openClient(port,mach_task_self(),0x4147,connection);},
  [&](uint32_t connection,NativeAdrenoAbiParams *params,size_t *bytes){return !query(connection,NativeAdrenoMethodQuery,nullptr,0,params,bytes);},
  [&](uint32_t connection){return !closeClient(connection);});
 dlclose(io);return ok;
}
__attribute__((unused)) static id openNativeMetal(const char *process,uint64_t registry,const FuxiNativeHooks *hooks) {
 setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1);
 setenv("TU_DARWIN_OPEN_TYPE","0",1);
 // Keep the handle for process lifetime: objects and completion callbacks use it.
 static void *library=dlopen(FUXI_MTL_NATIVE_LIBRARY,RTLD_NOW|RTLD_LOCAL);
 nativeLibraryHandle().store(library);
 auto open=library?(FuxiNativeOpen)dlsym(library,"fuxi_native_metal_open"):nullptr;
 if(!open){hooks->failure("NativeMetal bundle unavailable or callback ABI missing");return nil;}
 return open(FUXI_MTL_ICD,process,registry,hooks);
}
