/* Native selector, buffer-row ownership and line recording controls. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include "IOS16Defaults.hpp"
#include <cassert>
@interface FuxiLinearTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiLinearTestBase
@end
static void refuses(FuxiCommandBuffer *command,void (^body)()){
 size_t count=command->_ops.size();body();assert(command->_rejected && command.error && command->_ops.size()==count);
 command->_rejected=NO;[command->_error release];command->_error=nil;
}
int main(){@autoreleasepool {
 Class cls=makeDeviceClass([FuxiLinearTestBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];fuxi::Backend::modelTextures=true;
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];auto failures=failureCount.load();
 for(MTLPixelFormat format:{MTLPixelFormatA8Unorm,MTLPixelFormatRG8Unorm}){
  size_t pixel=fuxi::Backend::texelBytes((uint32_t)format),row=16*pixel,offset=3*pixel,length=offset+4*row+7*pixel;
  FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithLength:length options:MTLResourceStorageModeShared];assert(buffer);
  memset(buffer.contents,0xa7,length);
  MTLTextureDescriptor *d=[MTLTextureDescriptor new];d.textureType=MTLTextureType2D;d.pixelFormat=format;d.width=7;d.height=5;d.depth=1;d.usage=MTLTextureUsageShaderRead;d.storageMode=MTLStorageModeShared;
  FuxiTexture *texture=(FuxiTexture *)[buffer newTextureWithDescriptor:d offset:offset bytesPerRow:row];assert(texture && texture.buffer==buffer && texture.bufferOffset==offset && texture.bufferBytesPerRow==row);
  FuxiTexture *view=(FuxiTexture *)[texture newTextureViewWithPixelFormat:format];assert(view && view.buffer==buffer);
  uint8_t input[128],output[128];memset(input,0x43,sizeof(input));memset(output,0,sizeof(output));
  [view replaceRegion:MTLRegionMake2D(1,1,3,2) mipmapLevel:0 withBytes:input bytesPerRow:4*pixel];
  auto *contents=(uint8_t *)buffer.contents;for(unsigned y=0;y<5;++y)for(size_t x=0;x<7*pixel;++x)assert(contents[offset+y*row+x]==(y>=1 && y<3 && x>=pixel && x<4*pixel?0x43:0xa7));
  contents[offset]=0x62;[buffer release];[texture release]; // view keeps both owners alive.
  [view getBytes:output bytesPerRow:7*pixel fromRegion:MTLRegionMake2D(0,0,7,5) mipmapLevel:0];assert(output[0]==0x62 && output[7*pixel+pixel]==0x43);
  [view release];[d release];
 }
 assert(failureCount==failures);
 FuxiTexture *target=(FuxiTexture *)[device newTextureWithDescriptor:ios16SmokeTexture()];
 FuxiPipeline *pipeline=[FuxiPipeline new];pipeline->_device=[device retain];pipeline->_pipeline=new fuxi::Pipeline{};
 FuxiBuffer *indices=(FuxiBuffer *)[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
 FuxiCommandBuffer *command=(FuxiCommandBuffer *)[queue commandBuffer];auto pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id<MTLTexture>)target;
 FuxiRender *render=(FuxiRender *)[command renderCommandEncoderWithDescriptor:pass];assert(render);[render setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline];
 [render drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:65];assert(command->_ops.size()==1 && command->_ops.back().vertices==64 && command->_ops.back().topology==fuxi::Topology::Line);
 [render drawPrimitives:MTLPrimitiveTypeLineStrip vertexStart:1 vertexCount:2];assert(command->_ops.size()==2 && command->_ops.back().first==1 && command->_ops.back().topology==fuxi::Topology::LineStrip);
 [render drawIndexedPrimitives:MTLPrimitiveTypeLine indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:(id<MTLBuffer>)indices indexBufferOffset:0];assert(command->_ops.size()==3 && command->_ops.back().vertices==2 && command->_ops.back().indices==indices->_buffer);
 [render drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:1];[render drawPrimitives:MTLPrimitiveTypeLineStrip vertexStart:0 vertexCount:1];assert(command->_ops.size()==3 && !command->_rejected);
 refuses(command,^{[render drawPrimitives:MTLPrimitiveTypeLine vertexStart:65535 vertexCount:2];});
 refuses(command,^{[render drawIndexedPrimitives:MTLPrimitiveTypeLine indexCount:4 indexType:MTLIndexTypeUInt16 indexBuffer:(id<MTLBuffer>)indices indexBufferOffset:0];});
 refuses(command,^{[render drawPrimitives:(MTLPrimitiveType)99 vertexStart:0 vertexCount:2];});
 [render endEncoding];[indices release];[pipeline release];[target release];[queue release];[device release];
 puts("PASS: M9o buffer textures/CPU rows/views/retention, line/strip/indexed selectors, trailing/empty counts and bounds");
}}
