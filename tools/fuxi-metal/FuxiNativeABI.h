/* Original MIT Fuxi safety callback ABI; shared with the external patched frontend. */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define FUXI_NATIVE_ABI 1
struct FuxiNativeHooks {
 uint32_t version;
 uint32_t bytes;
 void (*failure)(const char *);
 bool (*blocked)();
 void (*completed)(uint64_t gpuNanos,double wallMS,uint64_t draws,bool storedSurface);
};
#ifdef __OBJC__
typedef id (*FuxiNativeOpen)(const char *loader,const char *process,uint64_t registryID,const FuxiNativeHooks *hooks);
#endif
