/* Native Mac selector/lease checks with the CPU backend, no GPU or device. */
#import <IOSurface/IOSurfaceRef.h>
#include <cassert>
static unsigned surfaceLocks,surfaceUnlocks,heldSurfaceLocks;
static bool failSurfaceLock;
static kern_return_t scanoutTestLock(IOSurfaceRef,IOSurfaceLockOptions,uint32_t *) {
 ++surfaceLocks;if(failSurfaceLock)return 1;++heldSurfaceLocks;return 0;
}
static kern_return_t scanoutTestUnlock(IOSurfaceRef,IOSurfaceLockOptions,uint32_t *) {
 assert(heldSurfaceLocks);--heldSurfaceLocks;++surfaceUnlocks;return 0;
}
#define IOSurfaceLock scanoutTestLock
#define IOSurfaceUnlock scanoutTestUnlock
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#undef IOSurfaceLock
#undef IOSurfaceUnlock
@interface FuxiScanoutTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiScanoutTestBase
@end
int main(){@autoreleasepool {
 Class cls=makeDeviceClass([FuxiScanoutTestBase class]);id device=[cls alloc];
 FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 fuxi::Backend::modelHostImport=true;
 IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@8,(id)kIOSurfaceHeight:@6,
  (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@64,(id)kIOSurfaceAllocSize:@16384,
  (id)kIOSurfacePixelFormat:@(0x42475241)});assert(surface);
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:6 mipmapped:NO];
 d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 gpuScanout=false;
 FuxiTexture *legacy=(FuxiTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
 assert(legacy && legacy->_surfaceLockHeld && heldSurfaceLocks==1);[legacy release];assert(!heldSurfaceLocks);
 gpuScanout=true;
 FuxiTexture *t=(FuxiTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
 assert(t && !t->_surfaceLockHeld && !heldSurfaceLocks);
 FuxiTexture *view=(FuxiTexture *)[t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];assert([view iosurface]==surface);
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];
 unsigned before=surfaceLocks;
 FuxiCommandBuffer *c=(FuxiCommandBuffer *)[queue commandBuffer];[c->_resources addObject:t];[c->_resources addObject:view];
 [c addCompletedHandler:^(id<MTLCommandBuffer> buffer){(void)buffer;assert(!heldSurfaceLocks);}];
 [c commit];assert(!c.error && surfaceLocks==before+1 && !heldSurfaceLocks);
 // CPU transfers, including through a view, participate in the same writer lease.
 t->_texture->pixels.resize(8*6*4);uint8_t input[192]={},output[192]={};
 before=surfaceLocks;
 [view replaceRegion:MTLRegionMake2D(0,0,8,6) mipmapLevel:0 withBytes:input bytesPerRow:32];
 [view getBytes:output bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,6) mipmapLevel:0];
 assert(surfaceLocks==before+2 && !heldSurfaceLocks && !memcmp(input,output,sizeof(input)));
 failSurfaceLock=true;c=(FuxiCommandBuffer *)[queue commandBuffer];[c->_resources addObject:t];
 int submits=fuxi::Backend::submissions;[c commit];assert(c.error && !heldSurfaceLocks && fuxi::Backend::submissions==submits);
 failSurfaceLock=false;
 setenv("FUXI_TEST_LOST","1",1);c=(FuxiCommandBuffer *)[queue commandBuffer];[c->_resources addObject:t];[c commit];
 assert(c.error && owner(device)->backend->poisoned && heldSurfaceLocks==1);unsetenv("FUXI_TEST_LOST");
 // Test-only teardown after the modeled loss; production never releases this lease.
 owner(device)->backend->poisoned=false;scanoutTestUnlock(surface,0,nullptr);
 [view release];[t release];[queue release];[device release];CFRelease(surface);
 assert(!heldSurfaceLocks && surfaceUnlocks);
 puts("PASS: actual frontend lifetime/per-submit locks, view deduplication, CPU transfers, completion-before-callback, lock refusal and poison retention");
}}
