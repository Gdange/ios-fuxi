/* Native Mac tests with a synthetic 528-byte base and a CPU backend model.
 * They do not qualify iOS private ABI, NSBundle discovery or GPU execution. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
#include "LabelFrontendTests.hpp"
@interface FuxiTestBase : NSObject {uint8_t bytes[520];}
- (id)dangerousInheritedSelector;
@end
@implementation FuxiTestBase
- (id)dangerousInheritedSelector {abort();}
@end
// Private selectors QuartzCore sends on iOS 16 (observed in the M9 CA smoke).
@interface NSObject (FuxiCATest)
- (NSUInteger)protectionOptions;
- (void)setProtectionOptions:(NSUInteger)o;
- (BOOL)supportsYCBCRPackedFormats12;
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes;
- (BOOL)supportsNativeHardwareFP16;
- (BOOL)supportsTileShaders;
- (BOOL)supportsMemorylessRenderTargets;
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error;
@end
static void refuses(id<MTLCommandBuffer>command,void (^body)()) {
 FuxiCommandBuffer *c=(id)command;size_t n=c->_ops.size();body();assert(c->_rejected && c.error && c->_ops.size()==n);
 c->_rejected=NO;[c->_error release];c->_error=nil; // isolate modeled validation cases
}
int main() {
 @autoreleasepool {
  assert(!makeDeviceClass([NSObject class])); // Wrong-size base rejected before registration.
  assert(class_getInstanceSize([FuxiTestBase class])==528);
  Class cls=makeDeviceClass([FuxiTestBase class]);assert(cls);
  FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;state->registry=123;
  id<MTLDevice>device=(id)[cls alloc];
  objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
  assert([(id)device isKindOfClass:[FuxiTestBase class]] && [device conformsToProtocol:@protocol(MTLDevice)]);
  assert(![device supportsFamily:MTLGPUFamilyApple1] && device.registryID==123);
  { // Native half selects CA's lph names without claiming a feature family.
   uint64_t failures=fuxi_metal_failure_count();
   assert([(id)device supportsNativeHardwareFP16]);
   for(NSUInteger family:{0UL,1001UL,1002UL,1003UL,1005UL,2002UL,3001UL,3002UL,NSUIntegerMax})
    assert(![device supportsFamily:(MTLGPUFamily)family]);
   assert(![(FuxiDeviceMethods *)(id)device supportsFeatureSet:0]);
   assert(![(id)device supportsTileShaders] && ![(id)device supportsMemorylessRenderTargets]);
   assert(![(id)device supportsRasterOrderGroups] && ![(id)device supportsYCBCRPackedFormats12]);
   assert(fuxi_metal_failure_count()==failures);
  }
  assert(!device.currentAllocatedSize); // No live resources yet.
  MTLSizeAndAlign unsupportedSize=[device heapBufferSizeAndAlignWithLength:64 options:0];
  assert(!unsupportedSize.size && !unsupportedSize.align); // Missing struct ABI too.
  assert(![(FuxiTestBase *)device dangerousInheritedSelector]);
  assert(![device newBufferWithLength:0 options:0]);
  id<MTLBuffer> privateBuffer=[device newBufferWithLength:4 options:MTLResourceStorageModePrivate];
  assert(privateBuffer && privateBuffer.storageMode==MTLStorageModePrivate && !privateBuffer.contents);[privateBuffer release];
  assert(![device newLibraryWithSource:@"kernel void x(){}" options:nil error:nullptr]);
  id<MTLCommandQueue>q=[device newCommandQueue];
  checkLabel((id)q);
  assert([q commandBufferWithUnretainedReferences].retainedReferences);
  MTLCommandBufferDescriptor *commandDesc=[MTLCommandBufferDescriptor new];
  assert([q commandBufferWithDescriptor:commandDesc]);
  commandDesc.errorOptions=MTLCommandBufferErrorOptionEncoderExecutionStatus;
  assert(![q commandBufferWithDescriptor:commandDesc]);[commandDesc release];
  { // CA's optional queries and cache probes: answered without latching failures.
   uint64_t failures=fuxi_metal_failure_count();NSError *libraryError=nil;
   assert(![(id)device supportsYCBCRPackedFormats12]);
   assert([(id)device deviceLinearReadOnlyTextureAlignmentBytes]==16 && [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]==16);
   assert(![(id)device newPipelineLibraryWithFilePath:@"/nonexistent" error:&libraryError] && libraryError);
   id<MTLCommandBuffer>c=q.commandBuffer;assert(![(id)c protectionOptions]);
   [(id)c setProtectionOptions:0];[c enqueue];[c commit];
   assert(c.status==MTLCommandBufferStatusCompleted && !c.error && fuxi_metal_failure_count()==failures);
   id<MTLCommandBuffer>p=q.commandBuffer;refuses(p,^{[(id)p setProtectionOptions:1];});
  }
  for(unsigned i=0;i<16;++i) { @autoreleasepool {
   uint8_t initial[64];memset(initial,0x42,sizeof(initial));
   id<MTLBuffer>src=[device newBufferWithBytes:initial length:64 options:0];
   id<MTLBuffer>dst=[device newBufferWithLength:64 options:0];
   assert(src && dst && src.device==device && src.length==64 && src.contents);
   assert(!memcmp(src.contents,initial,64));
   id<MTLCommandBuffer>c=q.commandBuffer;
   id<MTLBlitCommandEncoder>e=c.blitCommandEncoder;
   checkLabel((id)src);checkLabel((id)dst);checkLabel((id)c);checkLabel((id)e);
   assert(!((FuxiCommandBuffer *)c)->_rejected && !c.error);
   assert(!c.blitCommandEncoder);
   refuses(c,^{[e fillBuffer:src range:NSMakeRange(1,4) value:1];});
   refuses(c,^{[e copyFromBuffer:src sourceOffset:0 toBuffer:src destinationOffset:4 size:16];});
   refuses(c,^{[e fillBuffer:dst range:NSMakeRange(NSUIntegerMax,4) value:1];});
   [e fillBuffer:src range:NSMakeRange(4,56) value:(uint8_t)i];
   [e copyFromBuffer:src sourceOffset:4 toBuffer:dst destinationOffset:8 size:48];[e endEncoding];
   checkLabel((id)e); // Metadata also works after the encoder ends.
   refuses(c,^{[e endEncoding];});
   __block unsigned calls=0;
   [c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusCompleted);++calls;}];
   [src release]; // Command buffer must retain resources through submission.
   [c commit];[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusCompleted && !c.error && calls==1);
   [c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusCompleted);++calls;}];assert(calls==2);
   for(unsigned j=0;j<64;j++)assert(((uint8_t *)dst.contents)[j]==(j>=8 && j<56?i:0));
   [c commit];assert(c.status==MTLCommandBufferStatusCompleted); // idempotent after completion
   assert(!c.blitCommandEncoder);[dst release];
  }}
  assert(fuxi::Backend::live==0 && fuxi::Backend::submissions==16 && fuxi::Backend::emptySubmits==1); // Enqueue probe completes without GPU work
  @autoreleasepool {
   id<MTLCommandBuffer>c=q.commandBuffer;id<MTLBlitCommandEncoder>e=c.blitCommandEncoder;
   // A missing encoder entry must name the selector, return without throwing,
   // retain prior resources, and never send even the valid prefix to the GPU.
   id<MTLBuffer>b=[device newBufferWithLength:64 options:0];[e fillBuffer:b range:NSMakeRange(0,64) value:7];
   unsigned before=fuxi::Backend::submissions;id<MTLTexture> none=nil;[e generateMipmapsForTexture:none];[e endEncoding];
   [c commit];assert(c.status==MTLCommandBufferStatusError && c.error && fuxi::Backend::submissions==(int)before);
   for(unsigned i=0;i<64;++i)assert(!((uint8_t *)b.contents)[i]);[b release];
  }
  @autoreleasepool {
   id<MTLCommandBuffer>c=q.commandBuffer;(void)c.blitCommandEncoder;[c commit];
   assert(c.status==MTLCommandBufferStatusError && c.error);
  }
  @autoreleasepool {
   id<MTLCommandBuffer>c=q.commandBuffer;
   setenv("FUXI_TEST_LOST","1",1);
   __block unsigned calls=0;[c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusError);++calls;}];
   [c commit];[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusError && c.error && calls==1);
   unsetenv("FUXI_TEST_LOST");
  }
  // CA must not submit a valid prefix after an earlier swallowed refusal.
  @autoreleasepool {
   strcpy(clientProcess,"fuxi-ca-smoke");assert(fuxi_metal_failure_count()>0);
   id<MTLBuffer>b=[device newBufferWithLength:64 options:0];
   id<MTLCommandBuffer>c=q.commandBuffer;id<MTLBlitCommandEncoder>e=c.blitCommandEncoder;
   [e fillBuffer:b range:NSMakeRange(0,64) value:9];[e endEncoding];
   int before=fuxi::Backend::submissions;[c commit];
   assert(c.status==MTLCommandBufferStatusError && fuxi::Backend::submissions==before);
   for(unsigned i=0;i<64;++i)assert(!((uint8_t *)b.contents)[i]);[b release];
   uint64_t count=fuxi_metal_failure_count();
   for(unsigned i=0;i<100;++i)failureLog("FUXI MTL: test refusal\n");
   assert(fuxi_metal_failure_count()==count+100); // Counter outlives log cap.
  }
  [q release];[device release];
 }
 puts("PASS: Objective-C size guard/subclass/protocols, capabilities, unsupported selectors, retained resources, fill/copy, 16 rounds, completion/error states, bounds/alignment/overlap rejection");
}
