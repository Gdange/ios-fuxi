/* Native Mac CPU-model checks of NativeMetal-derived CA adapters. No GPU. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include "IOS16Defaults.hpp"
#include <cassert>
@interface FuxiCATestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiCATestBase
@end
int main(){@autoreleasepool {
 // Persistence diagnostics preserve enum/byte values through the C exports.
 fuxi::Backend::pipelineCacheLoadResult=1;fuxi::Backend::pipelineCacheLoadBytes=4096;
 fuxi::Backend::pipelineCacheSaveResult=3;fuxi::Backend::pipelineCacheSaveBytes=2048;
 fuxi::Backend::pipelineCacheSaveCount=2;fuxi::Backend::pipelineCacheSaveFailures=1;
 assert(fuxi_metal_pipeline_cache_load_result()==1 && fuxi_metal_pipeline_cache_load_bytes()==4096);
 assert(fuxi_metal_pipeline_cache_save_result()==3 && fuxi_metal_pipeline_cache_save_bytes()==2048);
 assert(fuxi_metal_pipeline_cache_save_count()==2 && fuxi_metal_pipeline_cache_save_failures()==1);
 Class cls=makeDeviceClass([FuxiCATestBase class]);assert(cls && class_getInstanceSize(cls)==528);
 id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];FuxiCommandBuffer *command=(FuxiCommandBuffer *)[queue commandBuffer];
 __block unsigned sequence=0;[command addScheduledHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusCompleted);assert(++sequence==1);
  id b=[device newBufferWithLength:16 options:0];assert(b);[b release]; // No backend lock in callbacks.
 }];
 [command addCompletedHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusCompleted);assert(++sequence==2);}];
 assert(fuxi::Backend::submissions==0);auto emptyBefore=fuxi::Backend::emptySubmits.load();
 assert([command commitAndWaitUntilSubmitted] && sequence==2);[command waitUntilScheduled];[command waitUntilCompleted];
 assert(fuxi::Backend::submissions==0 && fuxi::Backend::emptySubmits==emptyBefore+1);
 [command addScheduledHandler:^(id<MTLCommandBuffer> c){(void)c;++sequence;}];assert(sequence==3);
 FuxiEvent *event=[device newSharedEvent];id<MTLFence> fence=[device newFence];
 dispatch_queue_t callbacks=dispatch_queue_create("fuxi.ca.test",DISPATCH_QUEUE_SERIAL);
 dispatch_semaphore_t delivered=dispatch_semaphore_create(0);
 MTLSharedEventListener *listener=[[MTLSharedEventListener alloc] initWithDispatchQueue:callbacks];
 [event notifyListener:listener atValue:4 block:^(id<MTLSharedEvent> e,uint64_t value){assert((id)e==event && value>=4);dispatch_semaphore_signal(delivered);}];
 command=(FuxiCommandBuffer *)[queue commandBuffer];[command encodeSignalEvent:(id)event value:4];[command commit];
 assert(command.status==MTLCommandBufferStatusCompleted && [event signaledValue]==4 && [event conformsToProtocol:@protocol(MTLSharedEvent)]);
 assert(!dispatch_semaphore_wait(delivered,dispatch_time(DISPATCH_TIME_NOW,1000000000)));
 assert([event waitUntilSignaledValue:4 timeoutMS:0] && ![event waitUntilSignaledValue:5 timeoutMS:0]);
 command=(FuxiCommandBuffer *)[queue commandBuffer];[command encodeWaitForEvent:(id)event value:4];
 FuxiBlit *blit=(FuxiBlit *)[command blitCommandEncoder];[blit updateFence:fence];[blit waitForFence:fence];[blit endEncoding];
 [command commit];assert(!command.error);
 // A real modeled write, signal/wait, then an empty command preserve queue order
 // and callback reentrancy. Valid SDK enums only: never provoke Mac Metal asserts.
 FuxiBuffer *ordered=(FuxiBuffer *)[device newBufferWithLength:16 options:MTLResourceStorageModeShared];
 command=(FuxiCommandBuffer *)[queue commandBuffer];blit=(FuxiBlit *)[command blitCommandEncoder];[blit fillBuffer:(id)ordered range:NSMakeRange(0,16) value:0x6d];[blit endEncoding];
 [command encodeSignalEvent:(id)event value:5];[command commit];assert(!command.error);
 auto submittedBefore=fuxi::Backend::submissions;emptyBefore=fuxi::Backend::emptySubmits.load();sequence=0;
 command=(FuxiCommandBuffer *)[queue commandBuffer];[command encodeWaitForEvent:(id)event value:5];[command encodeSignalEvent:(id)event value:6];
 [command addScheduledHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusCompleted && ++sequence==1);assert([event signaledValue]==6);
  for(unsigned i=0;i<16;++i)assert(((uint8_t *)[ordered contents])[i]==0x6d);
 }];
 [command addCompletedHandler:^(id<MTLCommandBuffer> c){assert(!c.error && ++sequence==2);
  id<MTLCommandBuffer> nested=[queue commandBuffer];[nested commit];assert(nested.status==MTLCommandBufferStatusCompleted);
 }];[command commit];[command waitUntilCompleted];assert(sequence==2);
 // Event-only work never calls submit; the nested empty command completes once.
 assert(fuxi::Backend::submissions==submittedBefore && fuxi::Backend::emptySubmits==emptyBefore+1);
 [ordered release];
 [queue setCompletionQueue:callbacks];command=(FuxiCommandBuffer *)[queue commandBuffer];
 [command addCompletedHandler:^(id<MTLCommandBuffer> c){[c waitUntilCompleted];assert(!c.error);dispatch_semaphore_signal(delivered);}];[command commit];
 assert(!dispatch_semaphore_wait(delivered,dispatch_time(DISPATCH_TIME_NOW,1000000000)));
 [command waitUntilCompleted];[queue setCompletionQueue:nullptr];
 MTLHeapDescriptor *hd=[MTLHeapDescriptor new];hd.storageMode=MTLStorageModeShared;hd.size=128;
 FuxiHeap *heap=(FuxiHeap *)[device newHeapWithDescriptor:hd];assert(heap && [heap usedSize]==0);
 id<MTLBuffer> buffer=[heap newBufferWithLength:64 options:0];assert(buffer && buffer.heap==(id)heap && [heap usedSize]==64);
 assert(![heap newBufferWithLength:80 options:0]);
 assert([(id)buffer setPurgeableState:MTLPurgeableStateVolatile]==MTLPurgeableStateNonVolatile);
 uint8_t bytes[64];memset(bytes,0x42,64);memcpy(buffer.contents,bytes,64);uint8_t copied[16]{};
 [(FuxiBuffer *)buffer getBytes:copied range:NSMakeRange(8,16)];assert(!memcmp(copied,bytes,16));
 [(FuxiBuffer *)buffer didModifyRange:NSMakeRange(0,64)];
 id objects[]={buffer};id group=[device newResourceGroupFromResources:objects count:1];assert(group);
 command=(FuxiCommandBuffer *)[queue commandBuffer];[command encodeCacheHintTag:7 resourceGroups:&group count:1];[group release];
 [command encodeCacheHintFinalize:7 resourceGroups:nullptr count:0];[buffer release];
 assert([heap usedSize]==64);[command commit];assert([heap usedSize]==0);
 [heap release];[hd release];
 FuxiBuffer *large=(FuxiBuffer *)[device newBufferWithLength:16384 options:0];memset([large contents],0x42,16384);
 assert([large setPurgeableState:MTLPurgeableStateVolatile]==MTLPurgeableStateNonVolatile);
 assert([large setPurgeableState:MTLPurgeableStateKeepCurrent]==MTLPurgeableStateVolatile);
 assert([large setPurgeableState:MTLPurgeableStateEmpty]==MTLPurgeableStateVolatile);
 for(unsigned i=0;i<16384;++i)assert(!((uint8_t *)[large contents])[i]);
 assert([large setPurgeableState:MTLPurgeableStateNonVolatile]==MTLPurgeableStateEmpty);[large release];
 __block bool deallocated=false;uint8_t callerBytes[16]{};
 // Unaligned (unimportable) bytes are shadowed: contents stays the caller's pointer.
 id<MTLBuffer> wrapped=[device newBufferWithBytesNoCopy:callerBytes length:16 options:0 deallocator:^(void *bytes,NSUInteger length){(void)bytes;(void)length;deallocated=true;}];
 assert(wrapped && [wrapped contents]==callerBytes && [wrapped length]==16 && !deallocated);[wrapped release];assert(deallocated);
 fuxi::Backend::modelTextures=true;
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:4 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 FuxiTexture *texture=(FuxiTexture *)[device newTextureWithDescriptor:td];assert(texture && texture.width==8 && texture.height==4);
 uint8_t input[160];for(unsigned i=0;i<160;++i)input[i]=(uint8_t)i;
 [texture replaceRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0 withBytes:input bytesPerRow:40];
 uint8_t output[160];memset(output,0xa5,sizeof(output));[texture getBytes:output bytesPerRow:40 fromRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0];
 for(unsigned y=0;y<4;++y){assert(!memcmp(output+y*40,input+y*40,32));for(unsigned x=32;x<40;++x)assert(output[y*40+x]==0xa5);}
 uint8_t patch[8]={1,2,3,4,5,6,7,8};[texture replaceRegion:MTLRegionMake2D(2,1,2,1) mipmapLevel:0 withBytes:patch bytesPerRow:8];
 FuxiTexture *view=(FuxiTexture *)[texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];assert(view && view.parentTexture==texture && view->_texture==texture->_texture);
 [view getBytes:output bytesPerRow:40 fromRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0];assert(!memcmp(output+48,patch,8));
 @try{[view getBytes:output bytesPerRow:40 fromRegion:MTLRegionMake2D(8,0,1,1) mipmapLevel:0];assert(false);}@catch(NSException *e){(void)e;}
 MTLRenderPassDescriptor *pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id)texture;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
 command=(FuxiCommandBuffer *)[queue commandBuffer];FuxiRender *render=(FuxiRender *)[command renderCommandEncoderWithDescriptor:pass];assert(render && render->_state.scissor.extent.width==8);
 [render updateFence:fence afterStages:MTLRenderStageFragment];[render endEncoding];assert(command->_ops.size()==1 && command->_ops[0].clearOnly);[command commit];
 assert(!command.error);[texture getBytes:output bytesPerRow:40 fromRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0];
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x){const uint8_t red[]={0,0,255,255};assert(!memcmp(output+y*40+x*4,red,4));}
 [texture release];assert(view.width==8);[view release];fuxi::Backend::modelTextures=false;
 // M9g: real wrappers accept display-size descriptors and target-relative state.
 fuxi::Backend::modelTextures=true;
 td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1080 height:2400 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 texture=[device newTextureWithDescriptor:td];assert(texture && texture.width==1080 && texture.height==2400);
 assert([(FuxiDeviceMethods *)device maxTextureWidth2D]==4096 && [(FuxiDeviceMethods *)device maxTextureHeight2D]==4096);
 pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id)texture;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 command=(FuxiCommandBuffer *)[queue commandBuffer];render=(FuxiRender *)[command renderCommandEncoderWithDescriptor:pass];assert(render);
 [render setViewport:(MTLViewport){0,0,1080,2400,0,1}];[render setScissorRect:(MTLScissorRect){1000,2300,80,100}];assert(!command->_rejected);
 [render setScissorRect:(MTLScissorRect){1000,2300,81,100}];assert(command->_rejected);[render endEncoding];
 command=(FuxiCommandBuffer *)[queue commandBuffer];render=(FuxiRender *)[command renderCommandEncoderWithDescriptor:pass];
 [render setViewport:(MTLViewport){0,0,1080,2401,0,1}];assert(command->_rejected);[render endEncoding];
 [texture release];
 // Metal SDK setters accept 4097 and sampleCount=2; do not install invalid
 // enum ordinals or out-of-range descriptor-array indices (Mac Metal asserts).
 td.width=4097;assert(![device newTextureWithDescriptor:td]);td.width=1080;td.sampleCount=2;assert(![device newTextureWithDescriptor:td]);
 fuxi::Backend::modelTextures=false;
 // Shared backend identity survives releasing either independently labeled wrapper.
 fuxi::Backend::modelPipelines=true;
 FuxiLibrary *library=[FuxiLibrary new];library->_device=[device retain];
 FuxiFunction *vertex=[FuxiFunction new],*fragment=[FuxiFunction new];
 vertex->_library=[library retain];vertex->_name=[@"cache_vertex" copy];vertex->_stage=1;
 fragment->_library=[library retain];fragment->_name=[@"cache_fragment" copy];fragment->_stage=2;
 MTLRenderPipelineDescriptor *pd=ios16PipelineDefaults();pd.vertexFunction=(id)vertex;pd.fragmentFunction=(id)fragment;
 pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;NSError *error=nil;
 FuxiPipeline *one=[device newRenderPipelineStateWithDescriptor:pd error:&error];assert(one && !error);
 pd.label=@"other wrapper";FuxiPipeline *two=[device newRenderPipelineStateWithDescriptor:pd error:&error];
 assert(two && one!=two && one->_pipeline==two->_pipeline && two->_pipeline->references==3);
 [one release];assert(two->_pipeline->references==2 && [two.label isEqualToString:@"other wrapper"]);
 pd.colorAttachments[0].writeMask=MTLColorWriteMaskRed;
 FuxiPipeline *different=[device newRenderPipelineStateWithDescriptor:pd error:&error];assert(different && different->_pipeline!=two->_pipeline);
 [different release];[two release];[vertex release];[fragment release];[library release];fuxi::Backend::modelPipelines=false;
 [listener release];[event release];[fence release];dispatch_release(delivered);dispatch_release(callbacks);
 [queue release];[device release];assert(fuxi::Backend::live==0);
 puts("PASS: CA scheduling/completion routing, local events/listeners/fences, automatic heap lifetime/cache hints, texture regions/views/empty pass");
}}
