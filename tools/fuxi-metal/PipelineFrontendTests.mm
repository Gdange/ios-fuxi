/* Objective-C encoder/ownership tests only. CPU model; no translation/GPU claim. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
#include "IOS16Defaults.hpp"
@interface FuxiPipelineTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiPipelineTestBase
@end
static void refuses(FuxiCommandBuffer *c,void (^body)()) {
 size_t count=c->_ops.size();body();assert(c->_rejected && c.error && c->_ops.size()==count);
 // Isolate each validation case; production never clears a rejected command.
 c->_rejected=NO;[c->_error release];c->_error=nil;
}
int main() {@autoreleasepool {
 Class cls=makeDeviceClass([FuxiPipelineTestBase class]);assert(cls);
 FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;state->registry=123;
 id device=[cls alloc];objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 id<MTLCommandQueue>queue=[device newCommandQueue];
 MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];id<MTLSamplerState>sampler=[device newSamplerStateWithDescriptor:sd];
 assert(sampler && sampler.device==device);[sampler release];sd.maxAnisotropy=2;
 assert(![device newSamplerStateWithDescriptor:sd]);[sd release];
 FuxiTexture *t=[FuxiTexture new];t->_device=[device retain];t->_texture=new fuxi::Texture{262144};t->_usage=MTLTextureUsageRenderTarget;
 FuxiPipeline *p=[FuxiPipeline new];p->_device=[device retain];p->_pipeline=new fuxi::Pipeline{};
 NSError *error=nil;assert(![device newLibraryWithData:dispatch_data_empty error:&error] && error);
 error=nil;assert(![device newLibraryWithURL:[NSURL URLWithString:@"https://example.invalid/test"] error:&error] && error);
 error=nil;assert(![device newRenderPipelineStateWithDescriptor:[[MTLRenderPipelineDescriptor new] autorelease] error:&error] && error);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
 assert(![device newTextureWithDescriptor:td]);
 MTLRenderPassDescriptor *pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id)t;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 FuxiCommandBuffer *c=(id)queue.commandBuffer;
 pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
 refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.colorAttachments[0].loadAction=MTLLoadActionClear;
 pass.defaultRasterSampleCount=4;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.defaultRasterSampleCount=0;
 pass.renderTargetArrayLength=2;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.renderTargetArrayLength=0;
 pass.renderTargetWidth=128;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.renderTargetWidth=0;
 pass.depthAttachment.texture=(id)t;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.depthAttachment.texture=nil;
 pass.colorAttachments[7].resolveTexture=(id)t;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.colorAttachments[7].resolveTexture=nil;
 MTLRenderPassDescriptor *samplePass=[[pass copy] autorelease];
 MTLSamplePosition positions[4]={{0.25f,0.25f},{0.75f,0.25f},{0.25f,0.75f},{0.75f,0.75f}};[samplePass setSamplePositions:positions count:4];
 refuses(c,^{[c renderCommandEncoderWithDescriptor:samplePass];});
 FuxiRender *r=[c renderCommandEncoderWithDescriptor:pass];assert(r);
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];});
 refuses(c,^{[r setRenderPipelineState:(id)t];});[r setRenderPipelineState:(id)p];
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:3];});
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:1 vertexCount:3];});
 [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];});
 [r endEncoding];refuses(c,^{[r endEncoding];});
 FuxiBuffer *b=[device newBufferWithLength:262656 options:0];FuxiBlit *blit=[c blitCommandEncoder];
 refuses(c,^{[blit copyFromTexture:(id)t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
 toBuffer:(id)b destinationOffset:0 destinationBytesPerRow:1000 destinationBytesPerImage:262144];});
 [blit copyFromTexture:(id)t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
 toBuffer:(id)b destinationOffset:256 destinationBytesPerRow:1024 destinationBytesPerImage:262144];[blit endEncoding];
 assert(c->_ops.size()==2 && c->_ops[0].texture==t->_texture && c->_ops[0].pipeline==p->_pipeline && c->_ops[1].dst==b->_buffer);
 assert(c->_resources.count==4);
 [t release];[p release];[b release]; // Encoder and command references keep resources alive.
 assert([c->_resources[0] device]==device && [c->_resources[1] device]==device);
 // Never submit modeled render operations; translation/pixels are tested on real Vulkan.
 [queue release];[device release];
 }assert(fuxi::Backend::live==0);
 puts("PASS: pipeline frontend CPU-model descriptor refusals, one-draw state, readback bounds, retained resource ownership (no submit)");}
