/* Ownership/snapshot/callback tests against the actual frontend and CPU model. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
@interface FuxiAsyncTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiAsyncTestBase
@end
int main(){@autoreleasepool {
 assert(fuxi::systemTranslationPolicy().async);fuxi::Backend::modelPipelines=true;
 Class cls=makeDeviceClass([FuxiAsyncTestBase class]);assert(cls);
 FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 id device=[cls alloc];objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 FuxiLibrary *library=[FuxiLibrary new];library->_device=[device retain];
 FuxiFunction *v=[FuxiFunction new],*f=[FuxiFunction new];
 v->_library=[library retain];v->_name=[@"vertex" copy];v->_stage=1;
 f->_library=[library retain];f->_name=[@"fragment" copy];f->_stage=2;
 MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=(id)v;d.fragmentFunction=(id)f;d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 dispatch_queue_t q=pipelineCompilationQueue();dispatch_suspend(q);
 dispatch_semaphore_t done=dispatch_semaphore_create(0);__block unsigned called=0;__block id retained=nil;
 [device newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> p,NSError *error){
  assert(p && !error && p.device==device);retained=[p retain];++called;dispatch_semaphore_signal(done);
 }];
 // Mutations and releases immediately after return must not change the request.
 d.vertexFunction=nil;assert(!called);[d release];[v release];[f release];[library release];
 [device newRenderPipelineStateWithDescriptor:nil options:MTLPipelineOptionArgumentInfo completionHandler:^(id<MTLRenderPipelineState> p,MTLRenderPipelineReflection *r,NSError *error){
  assert(!p && !r && error);++called;dispatch_semaphore_signal(done);
 }];
 [device release];dispatch_resume(q);
 assert(!dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)));
 assert(!dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)));
 // Drain past worker autorelease pools before checking retained-state lifetime.
 dispatch_sync(q,^{});assert(called==2 && [retained device]);[retained release];dispatch_release(done);
 puts("PASS: async render returns before work, copies descriptor, retains inputs/device/handler, delivers success/error once, balances new ownership");
}}
