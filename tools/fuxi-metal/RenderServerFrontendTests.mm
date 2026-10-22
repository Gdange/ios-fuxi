/* Native entry-point/ordering tests, CPU model; no shader or device inputs. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include "IOS16Defaults.hpp"
#include <cassert>
@interface FuxiServerTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiServerTestBase
@end
@interface FuxiServerDrawable : NSObject { @public FuxiCommandBuffer *command;unsigned presents;double argument; }
@end
@implementation FuxiServerDrawable
- (void)present {assert(command.status==MTLCommandBufferStatusCompleted);++presents;}
- (void)presentAtTime:(CFTimeInterval)time {argument=time;[self present];}
- (void)presentAfterMinimumDuration:(CFTimeInterval)time {argument=time;[self present];}
@end
int main(){@autoreleasepool {
 Class cls=makeDeviceClass([FuxiServerTestBase class]);assert(cls);
 FuxiDeviceMethods *device=(FuxiDeviceMethods *)[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 assert(device.dedicatedMemorySize==0 && device.sharedMemorySize==NSProcessInfo.processInfo.physicalMemory);
 assert(device.recommendedMaxWorkingSetSize>0 && device.recommendedMaxWorkingSetSize<=device.sharedMemorySize);
 assert(device.maxBufferLength==fuxi::ResourceLimits::single && device.maxBufferLength<=device.recommendedMaxWorkingSetSize);
 NSUInteger baseline=device.currentAllocatedSize;FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithLength:4096 options:0];
 assert(buffer && device.currentAllocatedSize>=baseline+4096 && device.currentAllocatedSize<=device.recommendedMaxWorkingSetSize);
 [buffer release];assert(device.currentAllocatedSize==baseline);
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];
 assert([queue getGPUPriority]==1 && [queue getBackgroundGPUPriority]==3);
 assert(![queue setGPUPriority:2] && ![queue setBackgroundGPUPriority:2]);
 assert([queue getGPUPriority]==1 && [queue getBackgroundGPUPriority]==3);
 for(unsigned form=0;form<3;++form){
  FuxiCommandBuffer *command=(FuxiCommandBuffer *)[queue commandBuffer];FuxiServerDrawable *drawable=[FuxiServerDrawable new];drawable->command=command;
  if(!form)[command presentDrawable:(id<MTLDrawable>)drawable];
  else if(form==1)[command presentDrawable:(id<MTLDrawable>)drawable atTime:17.25];
  else [command presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:0.125];
  assert(!drawable->presents);[command commit];[command waitUntilCompleted];[command commit];
  assert(drawable->presents==1 && !command.error);if(form)assert(drawable->argument==(form==1?17.25:0.125));[drawable release];
 }
 NSError *error=nil;auto failures=failureCount.load();
 assert(![device newPipelineLibraryWithFilePath:@"default.pipelinelib" error:&error] && error && failureCount==failures);
 assert(![device newPipelineLibraryWithFilePath:@"default.pipelinelib" error:nullptr] && failureCount==failures);
 FuxiLibrary *library=[FuxiLibrary new];library->_device=[device retain];
 FuxiFunction *vertex=[FuxiFunction new],*fragment=[FuxiFunction new];
 vertex->_library=[library retain];vertex->_name=[@"server_vertex" copy];vertex->_stage=1;
 fragment->_library=[library retain];fragment->_name=[@"server_fragment" copy];fragment->_stage=2;
 auto d=ios16PipelineDefaults();d.vertexFunction=(id<MTLFunction>)vertex;d.fragmentFunction=(id<MTLFunction>)fragment;d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 NSThread *thread=NSThread.currentThread;__block unsigned calls=0;__block id retained=nil;
 fuxi::Backend::modelPipelines=true;
 [device newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> pipeline,NSError *e){
  assert(NSThread.currentThread==thread && !calls && pipeline && !e);retained=[pipeline retain];++calls;
 }];assert(calls==1 && retained);
 [device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone completionHandler:^(id<MTLRenderPipelineState> pipeline,MTLRenderPipelineReflection *reflection,NSError *e){
  assert(NSThread.currentThread==thread && calls==1 && pipeline && !reflection && !e);++calls;
 }];assert(calls==2);
 [device newRenderPipelineStateWithDescriptor:d options:(MTLPipelineOption)1 completionHandler:^(id<MTLRenderPipelineState> pipeline,MTLRenderPipelineReflection *reflection,NSError *e){
  assert(NSThread.currentThread==thread && calls==2 && !pipeline && !reflection && e);++calls;
 }];assert(calls==3);
 fuxi::Backend::modelPipelines=false;
 [device newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> pipeline,NSError *e){
  assert(NSThread.currentThread==thread && calls==3 && !pipeline && e);++calls;
 }];assert(calls==4);
 [device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone completionHandler:^(id<MTLRenderPipelineState> pipeline,MTLRenderPipelineReflection *reflection,NSError *e){
  assert(NSThread.currentThread==thread && calls==4 && !pipeline && !reflection && e);++calls;
 }];assert(calls==5);
 [device newComputePipelineStateWithFunction:(id<MTLFunction>)vertex completionHandler:^(id<MTLComputePipelineState> pipeline,NSError *e){
  assert(NSThread.currentThread==thread && calls==5 && !pipeline && e);++calls;
 }];assert(calls==6);
 [device newComputePipelineStateWithFunction:(id<MTLFunction>)vertex options:MTLPipelineOptionNone completionHandler:^(id<MTLComputePipelineState> pipeline,MTLComputePipelineReflection *reflection,NSError *e){
  assert(NSThread.currentThread==thread && calls==6 && !pipeline && !reflection && e);++calls;
 }];assert(calls==7);
 auto compute=[[MTLComputePipelineDescriptor new] autorelease];
 [device newComputePipelineStateWithDescriptor:compute options:MTLPipelineOptionNone completionHandler:^(id<MTLComputePipelineState> pipeline,MTLComputePipelineReflection *reflection,NSError *e){
  assert(NSThread.currentThread==thread && calls==7 && !pipeline && !reflection && e);++calls;
 }];assert(calls==8);
 // Missing bindings, a nil result replacing a previous good binding, and a
 // wrapper with no backend pipeline all drop without appending a draw.
 fuxi::Backend::modelTextures=true;auto td=ios16SmokeTexture();FuxiTexture *target=(FuxiTexture *)[device newTextureWithDescriptor:td];
 auto pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id<MTLTexture>)target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
 FuxiCommandBuffer *command=(FuxiCommandBuffer *)[queue commandBuffer];FuxiRender *render=(FuxiRender *)[command renderCommandEncoderWithDescriptor:pass];assert(render);
 auto dropped=fuxi_metal_dropped_draws();failures=failureCount.load();
 [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 [render setRenderPipelineState:retained];[render setRenderPipelineState:nil];
 [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 FuxiPipeline *empty=[FuxiPipeline new];empty->_device=[device retain];[render setRenderPipelineState:empty];
 [render drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:empty indexBufferOffset:0];
 assert(command->_ops.empty() && !command->_rejected && failureCount==failures && fuxi_metal_dropped_draws()==dropped+3);
 [render endEncoding];assert(command->_ops.size()==1 && command->_ops[0].clearOnly && !command->_ops[0].renderDraw);
 [command commit];assert(!command.error);[empty release];[retained release];[target release];
 [vertex release];[fragment release];[library release];[queue release];[device release];
 puts("PASS: M9m device memory/queue queries, presentation completion, inline pipeline handlers/errors/ownership, optional library fallback, null-pipeline draw drops");
}}
