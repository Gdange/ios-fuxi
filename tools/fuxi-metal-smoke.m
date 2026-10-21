/* Root exec-swap diagnostic: all argv, including argv[0], is ignored. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <errno.h>
#include "fuxi-metal/M7Reference.h"
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef FUXI_MTL_CONF
#define FUXI_MTL_CONF "/usr/local/share/fuxi/metal-smoke.conf"
#endif
#ifndef FUXI_MTL_METALLIB
#define FUXI_MTL_METALLIB "/usr/local/share/fuxi/metal-smoke.metallib"
#endif
static unsigned stage;
static const char *name="discovery";
static char timeoutLine[192];static size_t timeoutLength;
static void expired(int sig) {(void)sig;(void)write(1,timeoutLine,timeoutLength);_exit(124);}
static void arm(unsigned seconds) {
 alarm(0);timeoutLength=(size_t)snprintf(timeoutLine,sizeof(timeoutLine),"FUXI MTL: stage %u %s FAIL exceeded %u s wall bound\n",stage,name,seconds);alarm(seconds);
}
__attribute__((noreturn)) static void fail(const char *fmt,...) {
 printf("FUXI MTL: stage %u %s FAIL ",stage,name);va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);putchar('\n');_exit(1);
}
static unsigned configuration(void) {
 FILE *f=fopen(FUXI_MTL_CONF,"rb");if(!f){if(errno==ENOENT)return 3;fail("config unreadable");}
 char raw[32];size_t n=fread(raw,1,sizeof(raw),f);BOOL bad=ferror(f);fclose(f);
 if(bad || n==sizeof(raw))fail("config read/length");
 size_t i=0;while(i<n && strchr(" \t\r\n",raw[i]) && raw[i])++i;
 if(i==n || raw[i]<'0' || raw[i]>'6')fail("config expected max stage 0..6");
 unsigned result=(unsigned)(raw[i++]-'0');while(i<n && raw[i] && strchr(" \t\r\n",raw[i]))++i;
 if(i!=n)fail("config trailing data");return result;
}
static void verify(id<MTLBuffer>b,NSUInteger begin,NSUInteger size,uint8_t value) {
 const uint8_t *p=b.contents;if(!p)fail("nil contents");
 for(NSUInteger i=begin;i<begin+size;++i)if(p[i]!=value)fail("byte %lu got 0x%02x want 0x%02x",(unsigned long)i,p[i],value);
}
static void finish(id<MTLCommandBuffer>command) {
 if(!command)fail("nil command buffer");
 __block unsigned completions=0;
 [command addCompletedHandler:^(id<MTLCommandBuffer>b) {if(b.status==MTLCommandBufferStatusCompleted)++completions;}];
 [command commit];[command waitUntilCompleted];
 if(command.status!=MTLCommandBufferStatusCompleted || command.error || completions!=1)
  fail("command status %lu completions %u error %s",(unsigned long)command.status,completions,command.error.description.UTF8String);
 [command addCompletedHandler:^(id<MTLCommandBuffer>b) {if(b.status==MTLCommandBufferStatusCompleted)++completions;}];
 if(completions!=2)fail("late completed handler count %u",completions);
}
static void fill(id<MTLCommandQueue>q,id<MTLBuffer>b,uint8_t value) {
 id<MTLCommandBuffer>c=q.commandBuffer;if(!c)fail("nil command buffer");
 id<MTLBlitCommandEncoder>e=c.blitCommandEncoder;if(!e)fail("nil blit encoder");
 [e fillBuffer:b range:NSMakeRange(0,b.length) value:value];[e endEncoding];finish(c);verify(b,0,b.length,value);
}
static void copy(id<MTLCommandQueue>q,id<MTLBuffer>src,id<MTLBuffer>dst,uint8_t value) {
 // CPU writes -> GPU copy -> CPU reads, with sentinel edges and nonzero offsets.
 memset(src.contents,value,src.length);memset(dst.contents,0x39,dst.length);
 id<MTLCommandBuffer>c=q.commandBuffer;if(!c)fail("nil command buffer");
 id<MTLBlitCommandEncoder>e=c.blitCommandEncoder;if(!e)fail("nil blit encoder");
 [e copyFromBuffer:src sourceOffset:4 toBuffer:dst destinationOffset:8 size:dst.length-16];
 [e endEncoding];finish(c);
 verify(dst,0,8,0x39);verify(dst,8,dst.length-16,value);verify(dst,dst.length-8,8,0x39);
}
static void pipeline(id<MTLDevice>device,id<MTLCommandQueue>q) {
 NSError *error=nil;NSURL *url=[NSURL fileURLWithPath:@FUXI_MTL_METALLIB];
 id<MTLLibrary>library=[device newLibraryWithURL:url error:&error];
 if(!library || error)fail("library URL %s",error.description.UTF8String);
 // Also exercise the public dispatch_data_t loading entry point with the same owned source.
 NSData *bytes=[NSData dataWithContentsOfURL:url];
 dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,dispatch_get_global_queue(0,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
 id<MTLLibrary>dataLibrary=[device newLibraryWithData:data error:&error];dispatch_release(data);
 if(!dataLibrary || error)fail("library data %s",error.description.UTF8String);
 id<MTLFunction>v=[library newFunctionWithName:@"fuxi_vertex"],f=[dataLibrary newFunctionWithName:@"fuxi_fragment"];
 if(!v || !f || v.functionType!=MTLFunctionTypeVertex || f.functionType!=MTLFunctionTypeFragment)fail("vertex/fragment functions");
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=v;pd.fragmentFunction=f;
 pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 id<MTLRenderPipelineState>p=[device newRenderPipelineStateWithDescriptor:pd error:&error];
 if(!p || error)fail("render pipeline %s",error.description.UTF8String);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
 td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture>t=[device newTextureWithDescriptor:td];
 id<MTLBuffer>b=[device newBufferWithLength:256*256*4+512 options:MTLResourceStorageModeShared];
 if(!t || !b)fail("texture/readback allocation");memset(b.contents,0x39,b.length);
 MTLRenderPassDescriptor *rd=[MTLRenderPassDescriptor renderPassDescriptor];rd.colorAttachments[0].texture=t;
 rd.colorAttachments[0].loadAction=MTLLoadActionClear;rd.colorAttachments[0].storeAction=MTLStoreActionStore;
 rd.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
 id<MTLCommandBuffer>c=q.commandBuffer;id<MTLRenderCommandEncoder>r=[c renderCommandEncoderWithDescriptor:rd];
 if(!r)fail("nil render encoder");[r setRenderPipelineState:p];
 [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[r endEncoding];
 id<MTLBlitCommandEncoder>blit=c.blitCommandEncoder;if(!blit)fail("nil readback encoder");
 [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
 toBuffer:b destinationOffset:256 destinationBytesPerRow:1024 destinationBytesPerImage:256*256*4];
 [blit endEncoding];finish(c);verify(b,0,256,0x39);verify(b,b.length-256,256,0x39);
 const uint8_t *pixels=(const uint8_t *)b.contents+256;unsigned red=0,black=0;
 for(unsigned i=0;i<65536;++i) {
  const uint8_t *x=pixels+4*i;
  if(x[0] || x[1] || x[3]!=255 || (x[2]!=0 && x[2]!=255))fail("pixel %u BGRA %u,%u,%u,%u",i,x[0],x[1],x[2],x[3]);
  if(x[2])++red;else ++black;
 }
 const unsigned samples[][3]={{128,128,255},{96,176,255},{160,176,255},{0,0,0},{128,26,0},{64,64,0}};
 for(unsigned i=0;i<6;++i)if(pixels[(samples[i][1]*256+samples[i][0])*4+2]!=samples[i][2])fail("inside/outside sample %u",i);
 if(red<8064 || red>8320 || red+black!=65536)fail("pixel counts red %u black %u",red,black);
 printf("FUXI MTL: stage 4 pipeline PASS BGRA8 256x256 inside/outside 6 red %u clear %u + sentinel edges\n",red,black);
 [b release];[t release];[p release];[pd release];[v release];[f release];[library release];[dataLibrary release];
}
static IOSurfaceRef surface(void) {
 NSDictionary *properties=@{(id)kIOSurfaceWidth:@256,(id)kIOSurfaceHeight:@256,
  (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@1024,
  (id)kIOSurfacePixelFormat:@(0x42475241),(id)kIOSurfaceAllocSize:@262144};
 IOSurfaceRef result=IOSurfaceCreate((CFDictionaryRef)properties);
 if(!result)fail("IOSurfaceCreate");return result;
}
static void texel(unsigned x,unsigned y,uint8_t *out) {
 // Four different opaque quadrants expose channel swaps, stale reads and flips.
 out[0]=y>=128?255:0;out[1]=x>=128?255:0;out[2]=y<128 && x<128?255:0;out[3]=255;
}
static void composition(id<MTLDevice>device,id<MTLCommandQueue>q) {
 IOSurfaceRef src=surface(),dst=surface();
 if(IOSurfaceLock(src,0,NULL))fail("IOSurface source lock");
 uint8_t *base=IOSurfaceGetBaseAddress(src);size_t row=IOSurfaceGetBytesPerRow(src);
 if(!base || row<1024)fail("IOSurface source mapping/stride");
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)texel(x,y,base+y*row+x*4);
 if(IOSurfaceUnlock(src,0,NULL))fail("IOSurface source unlock");
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 id<MTLTexture>input=[device newTextureWithDescriptor:td iosurface:src plane:0];
 if(!input)fail("IOSurface source import unavailable (same-memory required)");
 id<MTLTexture>target=[device newTextureWithDescriptor:td iosurface:dst plane:0];
 if(!target || input.iosurface!=src || target.iosurface!=dst || input.iosurfacePlane || target.iosurfacePlane)fail("IOSurface target/identity");
 NSError *error=nil;id<MTLLibrary>lib=[device newLibraryWithURL:[NSURL fileURLWithPath:@FUXI_MTL_METALLIB] error:&error];
 if(!lib || error)fail("quad library %s",error.description.UTF8String);
 id<MTLFunction>v=[lib newFunctionWithName:@"fuxi_quad"],f=[lib newFunctionWithName:@"fuxi_sample"];
 if(!v || !f)fail("quad functions");
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=v;pd.fragmentFunction=f;pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 id<MTLRenderPipelineState>p=[device newRenderPipelineStateWithDescriptor:pd error:&error];
 if(!p || error)fail("sampled pipeline %s",error.description.UTF8String);
 // The second creation must use the translator's process cache.
 id<MTLRenderPipelineState>cached=[device newRenderPipelineStateWithDescriptor:pd error:&error];
 if(!cached || error)fail("cached sampled pipeline");[cached release];
 MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];id<MTLSamplerState>sampler=[device newSamplerStateWithDescriptor:sd];
 id<MTLBuffer>readback=[device newBufferWithLength:262656 options:MTLResourceStorageModeShared];
 if(!sampler || !readback)fail("sampler/readback allocation");memset(readback.contents,0x39,readback.length);
 @autoreleasepool {
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;
  pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
  id<MTLCommandBuffer>command=q.commandBuffer;id<MTLRenderCommandEncoder>r=[command renderCommandEncoderWithDescriptor:pass];
  if(!r)fail("nil sampled render encoder");[r setRenderPipelineState:p];[r setFragmentTexture:input atIndex:0];[r setFragmentSamplerState:sampler atIndex:0];
  [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];[r endEncoding];
  id<MTLBlitCommandEncoder>b=command.blitCommandEncoder;if(!b)fail("nil sampled readback encoder");
  [b copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
   toBuffer:readback destinationOffset:256 destinationBytesPerRow:1024 destinationBytesPerImage:262144];[b endEncoding];finish(command);
 }
 verify(readback,0,256,0x39);verify(readback,262400,256,0x39);
 const uint8_t *pixels=(const uint8_t *)readback.contents+256;
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {uint8_t want[4];texel(x,y,want);
  if(memcmp(pixels+(y*256+x)*4,want,4))fail("sampled pixel %u,%u",x,y);
 }
 [input release];[target release]; // Release import locks before the CPU consumer.
 if(IOSurfaceLock(dst,kIOSurfaceLockReadOnly,NULL))fail("IOSurface target lock");
 base=IOSurfaceGetBaseAddress(dst);row=IOSurfaceGetBytesPerRow(dst);if(!base || row<1024)fail("IOSurface target mapping/stride");
 for(unsigned y=0;y<256;++y)if(memcmp(base+y*row,pixels+y*1024,1024))fail("IOSurface same-memory row %u",y);
 if(IOSurfaceUnlock(dst,kIOSurfaceLockReadOnly,NULL))fail("IOSurface target unlock");
 puts("FUXI MTL: stage 5 composition PASS IOSurface BGRA8 256x256 textured quad 65536 pixels + same-memory target + sentinel edges");
 [readback release];[sampler release];[sd release];[p release];[pd release];[v release];[f release];[lib release];CFRelease(src);CFRelease(dst);
}
static void layers(id<MTLDevice>device,id<MTLCommandQueue>q) {
 IOSurfaceRef surfaces[3]={surface(),surface(),surface()};id<MTLTexture>textures[3]={nil,nil,nil};
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 for(unsigned i=0;i<3;++i){if(IOSurfaceLock(surfaces[i],0,NULL))fail("layer surface lock");
  uint8_t *base=IOSurfaceGetBaseAddress(surfaces[i]);size_t row=IOSurfaceGetBytesPerRow(surfaces[i]);if(!base || row<1024)fail("layer mapping");
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){if(i<2)fuxiM7Texel(i,x,y,base+y*row+x*4);else memset(base+y*row+x*4,0x39,4);}
  if(IOSurfaceUnlock(surfaces[i],0,NULL))fail("layer surface unlock");
  textures[i]=[device newTextureWithDescriptor:td iosurface:surfaces[i] plane:0];if(!textures[i])fail("layer same-memory import");
 }
 NSError *error=nil;id<MTLLibrary>lib=[device newLibraryWithURL:[NSURL fileURLWithPath:@FUXI_MTL_METALLIB] error:&error];if(!lib || error)fail("layer library");
 MTLFunctionConstantValues *values=[MTLFunctionConstantValues new];BOOL enabled=YES;[values setConstantValue:&enabled type:MTLDataTypeBool atIndex:0];
 id<MTLFunction>v=[lib newFunctionWithName:@"fuxi_layer_vertex"],f=[lib newFunctionWithName:@"fuxi_layer_fragment" constantValues:values error:&error];
 if(!v || !f || error)fail("layer function constants %s",error.description.UTF8String);
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=v;pd.fragmentFunction=f;
 MTLVertexDescriptor *vd=[MTLVertexDescriptor vertexDescriptor];
 vd.attributes[0].format=MTLVertexFormatFloat2;vd.attributes[0].bufferIndex=0;
 vd.attributes[1].format=MTLVertexFormatFloat2;vd.attributes[1].offset=8;vd.attributes[1].bufferIndex=0;
 vd.attributes[2].format=MTLVertexFormatUChar4Normalized;vd.attributes[2].offset=16;vd.attributes[2].bufferIndex=0;vd.layouts[0].stride=sizeof(struct FuxiM7VertexData);pd.vertexDescriptor=vd;
 MTLRenderPipelineColorAttachmentDescriptor *color=pd.colorAttachments[0];color.pixelFormat=MTLPixelFormatBGRA8Unorm;color.blendingEnabled=YES;
 color.sourceRGBBlendFactor=color.sourceAlphaBlendFactor=MTLBlendFactorOne;
 color.destinationRGBBlendFactor=color.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
 id<MTLRenderPipelineState>p=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!p || error)fail("layer pipeline %s",error.description.UTF8String);
 id<MTLRenderPipelineState>cached=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!cached || error)fail("layer cache");[cached release];
 id<MTLBuffer>vertices=[device newBufferWithBytes:fuxiM7Vertices length:sizeof(fuxiM7Vertices) options:MTLResourceStorageModeShared];
 id<MTLBuffer>indices=[device newBufferWithBytes:fuxiM7Indices length:sizeof(fuxiM7Indices) options:MTLResourceStorageModeShared];
 id<MTLBuffer>readback=[device newBufferWithLength:262656 options:MTLResourceStorageModeShared];
 MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];id<MTLSamplerState>sampler=[device newSamplerStateWithDescriptor:sd];
 if(!vertices || !indices || !readback || !sampler)fail("layer buffers/sampler");memset(readback.contents,0x39,readback.length);
 @autoreleasepool {id<MTLCommandBuffer>command=q.commandBuffer;
  for(unsigned i=0;i<2;++i){MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=textures[2];
   pass.colorAttachments[0].loadAction=i?MTLLoadActionLoad:MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
   id<MTLRenderCommandEncoder>r=[command renderCommandEncoderWithDescriptor:pass];if(!r)fail("layer pass");
   [r setRenderPipelineState:p];[r setVertexBuffer:vertices offset:0 atIndex:0];[r setFragmentTexture:textures[i] atIndex:0];[r setFragmentSamplerState:sampler atIndex:0];
   float geometry[4]={1,1,1,1};[r setVertexBytes:geometry length:sizeof(geometry) atIndex:2];memset(geometry,0,sizeof(geometry));
   float tint[4]={i?0.5f:1.0f,i?0.5f:1.0f,i?0.5f:1.0f,i?0.5f:1.0f};[r setFragmentBytes:tint length:sizeof(tint) atIndex:1];memset(tint,0,sizeof(tint));
   [r setViewport:(MTLViewport){0,0,256,256,0,1}];if(i)[r setScissorRect:(MTLScissorRect){16,24,224,208}];
   [r drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:indices indexBufferOffset:0];[r endEncoding];
  }
  id<MTLBlitCommandEncoder>b=command.blitCommandEncoder;if(!b)fail("layer readback");
  [b copyFromTexture:textures[2] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
   toBuffer:readback destinationOffset:256 destinationBytesPerRow:1024 destinationBytesPerImage:262144];[b endEncoding];finish(command);
 }
 verify(readback,0,256,0x39);verify(readback,262400,256,0x39);const uint8_t *pixels=(const uint8_t *)readback.contents+256;
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t want[4];fuxiM7Reference(x,y,want);if(memcmp(pixels+4*(y*256+x),want,4))fail("layer pixel %u,%u",x,y);}
 for(unsigned i=0;i<3;++i)[textures[i] release];
 if(IOSurfaceLock(surfaces[2],kIOSurfaceLockReadOnly,NULL))fail("layer consumer lock");
 const uint8_t *base=IOSurfaceGetBaseAddress(surfaces[2]);size_t row=IOSurfaceGetBytesPerRow(surfaces[2]);
 for(unsigned y=0;y<256;++y)if(memcmp(base+y*row,pixels+y*1024,1024))fail("layer same-memory row %u",y);
 if(IOSurfaceUnlock(surfaces[2],kIOSurfaceLockReadOnly,NULL))fail("layer consumer unlock");
 puts("FUXI MTL: stage 6 layers PASS IOSurface BGRA8 source-over + vertex/index buffers + function constant + fragment bytes + two passes + scissor 65536 pixels + same-memory target + sentinel edges");
 [sampler release];[sd release];[readback release];[indices release];[vertices release];[p release];[pd release];[v release];[f release];[values release];[lib release];for(unsigned i=0;i<3;++i)CFRelease(surfaces[i]);
}
int main(int argc,char **argv) {
 (void)argc;(void)argv;setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);signal(SIGALRM,expired);arm(30);
 @autoreleasepool { @try {
  if(geteuid()!=0)fail("requires unsandboxed root");unsigned maximum=configuration();
  id<MTLDevice>device=MTLCreateSystemDefaultDevice();if(!device)fail("MTLCreateSystemDefaultDevice returned nil");
  Class base=objc_getClass("_MTLDevice");
  if(!base || class_getInstanceSize(base)!=528 || strcmp(class_getName(object_getClass(device)),"FuxiMTLDevice") || ![(id)device isKindOfClass:base] || !device.registryID)fail("class/size/registryID guard");
  printf("FUXI MTL: stage 0 discovery PASS name %s class %s base-size %zu instance-size %zu registryID %llu\n",device.name.UTF8String,class_getName(object_getClass(device)),class_getInstanceSize(base),class_getInstanceSize(object_getClass(device)),device.registryID);
  if(maximum) {
   stage=1;name="fill";arm(30);id<MTLCommandQueue>q=[device newCommandQueue];
   id<MTLBuffer>a=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
   if(!q || !a || a.length!=65536 || a.storageMode!=MTLStorageModeShared)fail("queue/shared buffer");
   @autoreleasepool {fill(q,a,0xa7);}printf("FUXI MTL: stage 1 fill PASS verified 65536 bytes + completion handlers\n");
   if(maximum>=2) {
    stage=2;name="copy";arm(30);uint8_t initial[65536];memset(initial,0x52,sizeof(initial));
    id<MTLBuffer>b=[device newBufferWithBytes:initial length:sizeof(initial) options:MTLResourceStorageModeShared];
    if(!b)fail("newBufferWithBytes");verify(b,0,b.length,0x52);
    @autoreleasepool {copy(q,a,b,0x63);}printf("FUXI MTL: stage 2 copy PASS verified CPU-write/GPU-copy/readback + sentinel edges\n");
    if(maximum>=3) {stage=3;name="reuse";arm(60);
     for(unsigned i=0;i<16;++i) {@autoreleasepool {fill(q,a,(uint8_t)(i+1));copy(q,a,b,(uint8_t)(0x80+i));}}
     printf("FUXI MTL: stage 3 reuse PASS verified 16 fill/copy rounds\n");
    }
    [b release];
   }
   if(maximum>=4) {stage=4;name="pipeline";arm(60);@autoreleasepool {pipeline(device,q);}}
   if(maximum>=5) {stage=5;name="composition";arm(60);@autoreleasepool {composition(device,q);}}
   if(maximum>=6) {stage=6;name="layers";arm(60);@autoreleasepool {layers(device,q);}}
   arm(10);[a release];[q release];
  }
  [device release];alarm(0);puts("FUXI MTL: done PASS");
 } @catch(NSException *e) {fail("exception %s %s",e.name.UTF8String,e.reason.UTF8String);} }
 return 0;
}
