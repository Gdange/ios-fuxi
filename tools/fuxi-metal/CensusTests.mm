/* Synthetic runtime ABI and CPU backend only; no Apple implementation copied. */
#include "ClientGate.hpp"
#include <cassert>
static unsigned policyReads;
static fuxi::ClientMode testPolicy(const char *,const char *name) {
 ++policyReads;assert(!strcmp(name,"census-test"));return fuxi::ClientMode::census;
}
#define FUXI_MTL_READ_CLIENTS testPolicy
#define _NSGetExecutablePath testExecutablePath
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
extern "C" int testExecutablePath(char *path,uint32_t *size) {
 const char *s="/diagnostic/census-test";assert(*size>strlen(s));strcpy(path,s);return 0;
}
struct FuxiLargeResult {uint64_t words[8];};
@interface FuxiCensusBase : NSObject {uint8_t bytes[520];}
- (FuxiLargeResult)unimplementedLargeResult;
- (double)unimplementedDouble;
@end
@implementation FuxiCensusBase
- (FuxiLargeResult)unimplementedLargeResult {abort();}
- (double)unimplementedDouble {abort();}
@end
@protocol FuxiTestPrivateProtocol
- (MTLSize)privateSize;
@end
static void requireZero(id obj,SEL selector) {
 NSMethodSignature *sig=[obj methodSignatureForSelector:selector];assert(sig);
 NSInvocation *inv=[NSInvocation invocationWithMethodSignature:sig];inv.selector=selector;
 [inv invokeWithTarget:obj];
 std::vector<unsigned char> result(sig.methodReturnLength,255);
 if(!result.empty())[inv getReturnValue:result.data()];
 for(auto byte:result)assert(!byte);
}
int main() {
 @autoreleasepool {
  // Reference protocol so runtime metadata survives dead stripping.
  assert(@protocol(FuxiTestPrivateProtocol));
  Class cls=makeDeviceClass([FuxiCensusBase class]);assert(cls);
  id<MTLDevice>d=[[cls alloc] initWithAcceleratorPort:1];assert(d && policyReads==1);
  assert(!owner(d)->backend && d.registryID==0);
  assert(![d newCommandQueue]); // Cache-proven early CA stop, before library abort.
  assert(![d newBufferWithLength:4096 options:0]);
  assert(![d newDefaultLibrary]);
  requireZero(d,@selector(newLibraryWithData:error:));
  requireZero(d,@selector(newTextureWithDescriptor:iosurface:plane:));
  assert(![d newSharedEvent]);
  for(unsigned i=0;i<10;++i)assert(![d supportsFamily:MTLGPUFamilyApple1]);
  requireZero(d,@selector(heapTextureSizeAndAlignWithDescriptor:)); // structure
  requireZero(d,@selector(currentAllocatedSize)); // scalar
  requireZero(d,@selector(unimplementedLargeResult)); // arm64 indirect result
  requireZero(d,@selector(unimplementedDouble)); // floating point ABI
  requireZero(d,@selector(privateSize)); // typed private protocol without method
  requireZero(d,sel_registerName("untypedObjectProbe"));
  FuxiLargeResult large=[(FuxiCensusBase *)d unimplementedLargeResult];for(auto word:large.words)assert(!word);
  assert([(FuxiCensusBase *)d unimplementedDouble]==0.0);
  // The production census cannot create resources. Construct test objects to
  // cover forwarding on every current resource and future texture/render role.
  FuxiQueue *q=[FuxiQueue new];q->_device=[d retain];
  id<MTLCommandBuffer>c=[q commandBuffer];id<MTLBlitCommandEncoder>e=[c blitCommandEncoder];
  requireZero(q,@selector(insertDebugCaptureBoundary));
  requireZero(c,@selector(GPUEndTime));requireZero(e,@selector(pushDebugGroup:));
  [e endEncoding];__block unsigned calls=0;
  [c addCompletedHandler:^(id<MTLCommandBuffer>b){assert(b.status==MTLCommandBufferStatusError);++calls;}];
  [c commit];assert(calls==1 && c.error);
  FuxiObject *texture=[FuxiObject new];rejectProtocol([FuxiObject class],@protocol(MTLTexture));
  requireZero(texture,@selector(width));requireZero(texture,@selector(replaceRegion:mipmapLevel:withBytes:bytesPerRow:));
  [texture release];[q release];[d release];
  for(unsigned i=0;i<400;++i) {
   char name[64];snprintf(name,sizeof(name),"unimplementedProbe%u",i);
   requireZero(c,sel_registerName(name));
  }
  assert(censusCount==256 && fuxi::Backend::constructed==0 && fuxi::Backend::submissions==0 && fuxi::Backend::live==0);
 }
 puts("PASS: census init without GPU, recoverable CA queue stop, typed object/scalar/float/large-struct/void failures, all resource roles, command error, bounded 256 logs");
}
