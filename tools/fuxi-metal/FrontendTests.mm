/* Native Mac tests with a synthetic 528-byte base and a CPU backend model.
 * They do not qualify iOS private ABI, NSBundle discovery or GPU execution. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
@interface FuxiTestBase : NSObject {uint8_t bytes[520];}
- (id)dangerousInheritedSelector;
@end
@implementation FuxiTestBase
- (id)dangerousInheritedSelector {abort();}
@end
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
  assert(![(FuxiTestBase *)device dangerousInheritedSelector]);
  assert(![device newBufferWithLength:0 options:0]);
  assert(![device newBufferWithLength:4 options:MTLResourceStorageModePrivate]);
  assert(![device newLibraryWithSource:@"kernel void x(){}" options:nil error:nil]);
  id<MTLCommandQueue>q=[device newCommandQueue];
  for(unsigned i=0;i<16;++i) { @autoreleasepool {
   uint8_t initial[64];memset(initial,0x42,sizeof(initial));
   id<MTLBuffer>src=[device newBufferWithBytes:initial length:64 options:0];
   id<MTLBuffer>dst=[device newBufferWithLength:64 options:0];
   assert(src && dst && src.device==device && src.length==64 && src.contents);
   assert(!memcmp(src.contents,initial,64));
   id<MTLCommandBuffer>c=q.commandBuffer;
   id<MTLBlitCommandEncoder>e=c.blitCommandEncoder;
   assert(!c.blitCommandEncoder);
   @try {[c commit];assert(false);}@catch(NSException *error) {(void)error;}
   @try {[e fillBuffer:src range:NSMakeRange(1,4) value:1];assert(false);}@catch(NSException *error) {(void)error;}
   @try {[e copyFromBuffer:src sourceOffset:0 toBuffer:src destinationOffset:4 size:16];assert(false);}@catch(NSException *error) {(void)error;}
   @try {[e fillBuffer:dst range:NSMakeRange(NSUIntegerMax,4) value:1];assert(false);}@catch(NSException *error) {(void)error;}
   [e fillBuffer:src range:NSMakeRange(4,56) value:(uint8_t)i];
   [e copyFromBuffer:src sourceOffset:4 toBuffer:dst destinationOffset:8 size:48];[e endEncoding];
   @try {[e endEncoding];assert(false);}@catch(NSException *error) {(void)error;}
   __block unsigned calls=0;
   [c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusCompleted);++calls;}];
   [src release]; // Command buffer must retain resources through submission.
   [c commit];[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusCompleted && !c.error && calls==1);
   [c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusCompleted);++calls;}];assert(calls==2);
   for(unsigned j=0;j<64;j++)assert(((uint8_t *)dst.contents)[j]==(j>=8 && j<56?i:0));
   @try {[c commit];assert(false);}@catch(NSException *error) {(void)error;}
   assert(!c.blitCommandEncoder);[dst release];
  }}
  assert(fuxi::Backend::live==0 && fuxi::Backend::submissions==16);
  @autoreleasepool {
   id<MTLCommandBuffer>c=q.commandBuffer;
   setenv("FUXI_TEST_LOST","1",1);
   __block unsigned calls=0;[c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusError);++calls;}];
   [c commit];[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusError && c.error && calls==1);
   unsetenv("FUXI_TEST_LOST");
  }
  [q release];[device release];
 }
 puts("PASS: Objective-C size guard/subclass/protocols, capabilities, unsupported selectors, retained resources, fill/copy, 16 rounds, completion/error states, bounds/alignment/overlap rejection");
}
