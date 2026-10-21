/* Bounded Metal diagnostic. Original implementation; no Apple code. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mutex>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include "ClientGate.hpp"
#include "Translator.hpp"
#ifndef FUXI_MTL_BACKEND_HEADER
#define FUXI_MTL_BACKEND_HEADER "Backend.hpp"
#endif
#include FUXI_MTL_BACKEND_HEADER
extern "C" void _objc_msgForward(void);
#ifndef FUXI_MTL_CLIENTS_CONF
#define FUXI_MTL_CLIENTS_CONF "/usr/local/share/fuxi/metal-clients.conf"
#endif
static fuxi::ClientMode clientMode=fuxi::ClientMode::deny;
static char clientProcess[64]="unknown";
#ifndef FUXI_MTL_READ_CLIENTS
#define FUXI_MTL_READ_CLIENTS fuxi::readClients
#endif
static void checkClient() {
 static std::once_flag once;
 std::call_once(once,[]{
  // dyld's executable path, never argv[0] (exec-swap preserves daemon argv).
  bool identified=false;char path[4096];uint32_t size=sizeof(path);
  if(!_NSGetExecutablePath(path,&size)) {
   const char *name=strrchr(path,'/');name=name?name+1:path;
   if(fuxi::clientName(name)) {memcpy(clientProcess,name,strlen(name)+1);identified=true;}
  }
  clientMode=identified?FUXI_MTL_READ_CLIENTS(FUXI_MTL_CLIENTS_CONF,clientProcess):fuxi::ClientMode::deny;
  fprintf(stderr,"FUXI MTL: gate %s %s\n",clientMode==fuxi::ClientMode::deny?"deny":"allow",clientProcess);
  if(clientMode==fuxi::ClientMode::census)fprintf(stderr,"FUXI MTL: census enabled %s (no GPU)\n",clientProcess);
 });
}
// One bounded table per process, shared by every frontend object. No retained
// objects/NSStrings and no allocation once full. Repeated messages stay silent.
struct CensusEntry {Class cls;SEL selector;};
static CensusEntry censusEntries[256];static size_t censusCount;static std::mutex censusLock;
static void censusLog(id object,SEL selector,bool implemented) {
 if(clientMode!=fuxi::ClientMode::census)return;
 Class cls=object_getClass(object);std::lock_guard<std::mutex> lock(censusLock);
 for(size_t i=0;i<censusCount;++i)if(censusEntries[i].cls==cls && censusEntries[i].selector==selector)return;
 if(censusCount==256)return;
 censusEntries[censusCount++]={cls,selector};
 fprintf(stderr,"FUXI MTL: census %s %s %s %s\n",clientProcess,class_getName(cls),sel_getName(selector),implemented?"implemented":"unsupported");
 if(censusCount==256)fprintf(stderr,"FUXI MTL: census limit %s 256\n",clientProcess);
}
#define CENSUS() censusLog(self,_cmd,true)
static void unsupported(id object, SEL selector) {
 if(clientMode==fuxi::ClientMode::census) {censusLog(object,selector,false);return;}
 fprintf(stderr,"FUXI MTL: unsupported %s %s\n",class_getName(object_getClass(object)),sel_getName(selector));
}
static void invalid(NSString *why) {
 fprintf(stderr,"FUXI MTL: invalid %s\n",why.UTF8String);
 [NSException raise:NSInvalidArgumentException format:@"Fuxi Metal: %@",why];
}
@interface FuxiObject : NSObject
@end
@implementation FuxiObject
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s {
 Method method=class_getInstanceMethod(object_getClass(self),s);
 if(method)return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)];
 if(clientMode==fuxi::ClientMode::census) {
  unsigned count=0;Protocol *__unsafe_unretained *protocols=objc_copyProtocolList(&count);
  const char *types=nullptr;
  for(unsigned i=0;i<count && !types;++i)for(int required=0;required<2 && !types;++required)
   types=protocol_getMethodDescription(protocols[i],s,required,YES).types;
  free(protocols);
  if(types)return [NSMethodSignature signatureWithObjCTypes:types];
 }
 // A private selector absent from every loaded protocol/class has no type ABI.
 // Use the conventional object/word failure signature, without reading args.
 return clientMode==fuxi::ClientMode::census?[NSMethodSignature signatureWithObjCTypes:"@@:"]:nil;
}
- (void)doesNotRecognizeSelector:(SEL)s {unsupported(self,s);if(clientMode!=fuxi::ClientMode::census)[super doesNotRecognizeSelector:s];}
- (void)forwardInvocation:(NSInvocation *)inv {
 unsupported(self,inv.selector);
 if(clientMode==fuxi::ClientMode::census) {
  NSUInteger n=inv.methodSignature.methodReturnLength;
  if(n) {void *zero=calloc(1,n);[inv setReturnValue:zero];free(zero);}return;
 }
 const char *type=inv.methodSignature.methodReturnType;
 if(type[0]=='@') {id result=nil;[inv setReturnValue:&result];return;}
 [NSException raise:NSInvalidArgumentException format:@"Fuxi unsupported selector %@",NSStringFromSelector(inv.selector)];
}
@end
@interface FuxiOwner : NSObject { @public fuxi::Backend *backend; uint64_t registry; }
@end
@implementation FuxiOwner
- (void)dealloc {delete backend;[super dealloc];}
@end
static char ownerKey;
static FuxiOwner *owner(id obj) {return (FuxiOwner *)objc_getAssociatedObject(obj,&ownerKey);}
@interface FuxiBuffer : FuxiObject { @public id _device; fuxi::Buffer *_buffer; NSString *_label; }
@end
@implementation FuxiBuffer
- (void)dealloc {owner(_device)->backend->destroy(_buffer);[_device release];[_label release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (void *)contents {CENSUS();return _buffer->contents;}
- (NSUInteger)length {CENSUS();return _buffer->length;}
- (MTLStorageMode)storageMode {CENSUS();return MTLStorageModeShared;}
- (MTLCPUCacheMode)cpuCacheMode {CENSUS();return MTLCPUCacheModeDefaultCache;}
- (MTLHazardTrackingMode)hazardTrackingMode {CENSUS();return MTLHazardTrackingModeTracked;}
- (MTLResourceOptions)resourceOptions {CENSUS();return MTLResourceStorageModeShared;}
- (NSUInteger)allocatedSize {CENSUS();return _buffer->allocated;}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {CENSUS();[_label release];_label=[s copy];}
@end
static id metalError(NSError **error,const char *why) {
 fprintf(stderr,"FUXI MTL: pipeline/library FAIL %s\n",why);
 if(error)*error=[NSError errorWithDomain:@"org.fuxi.metal" code:1 userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:why]}];return nil;
}
@interface FuxiLibrary : FuxiObject { @public id _device; void *_library; }
@end
@interface FuxiFunction : FuxiObject { @public FuxiLibrary *_library; NSString *_name; int _stage; }
@end
@implementation FuxiFunction
- (void)dealloc {[_library release];[_name release];[super dealloc];}
- (id)device {CENSUS();return _library->_device;}
- (NSString *)name {CENSUS();return _name;}
- (MTLFunctionType)functionType {CENSUS();return _stage==1?MTLFunctionTypeVertex:MTLFunctionTypeFragment;}
@end
@implementation FuxiLibrary
- (void)dealloc {if(_library)fuxi::Translator::get().close(_library);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (id)newFunctionWithName:(NSString *)name {CENSUS();
 if(!name || !name.length || name.length>127 || strlen(name.UTF8String)!=[name lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) {unsupported(self,_cmd);return nil;}
 char error[2048]{};int stage=fuxi::Translator::get().function(_library,name.UTF8String,error,sizeof(error));
 if(!stage)return metalError(nullptr,error);
 FuxiFunction *f=[FuxiFunction new];f->_library=[self retain];f->_name=[name copy];f->_stage=stage;
 fprintf(stderr,"FUXI MTL: function %s class=%s stage=%d device=%u\n",name.UTF8String,object_getClassName(f),stage,[f device]==_device);return f;
}
@end
// Log the public value that failed, without inspecting Apple's object layout.
static bool stage4Value(const char *scope,const char *field,uint64_t value,bool accepted) {
 if(!accepted)fprintf(stderr,"FUXI MTL: %s reject %s %llu\n",scope,field,(unsigned long long)value);
 return accepted;
}
static bool stage4Class(const char *scope,const char *field,id object,Class expected) {
 if([object isKindOfClass:expected])return true;
 fprintf(stderr,"FUXI MTL: %s reject %s %s\n",scope,field,object?object_getClassName(object):"nil");return false;
}
static void stage4Require(const char *scope,const char *field,uint64_t value,bool accepted) {
 if(!stage4Value(scope,field,value,accepted))invalid([NSString stringWithFormat:@"%s %s",scope,field]);
}
static NSString *pipelineRejection(MTLRenderPipelineDescriptor *d,id device) {
 id v=d.vertexFunction,f=d.fragmentFunction;MTLVertexDescriptor *vd=d.vertexDescriptor;
 bool vertex=[v isKindOfClass:[FuxiFunction class]],fragment=[f isKindOfClass:[FuxiFunction class]];
 int vs=vertex?((FuxiFunction *)v)->_stage:0,fs=fragment?((FuxiFunction *)f)->_stage:0;
 auto vl=d.vertexLinkedFunctions,fl=d.fragmentLinkedFunctions;
 char colors[640]{};size_t used=0;
 for(unsigned i=0;i<8;++i) {auto a=d.colorAttachments[i];used+=(size_t)snprintf(colors+used,sizeof(colors)-used,"%s%u:%lu/%u/%lu",i?",":"",i,(unsigned long)a.pixelFormat,a.blendingEnabled,(unsigned long)a.writeMask);}
 fprintf(stderr,"FUXI MTL: pipeline descriptor present=%u vertex=%s/stage%d/device%u fragment=%s/stage%d/device%u vertexDescriptor=%u samples=%lu alphaCoverage=%u alphaOne=%u raster=%u depth=%lu stencil=%lu topology=%lu tessFormat=%lu tessMax=%lu indirect=%u amplification=%lu vertexLinked=%u/%lu/%lu/%lu/%lu fragmentLinked=%u/%lu/%lu/%lu/%lu colors(format/blend/mask)=%s\n",
  d!=nil,v?object_getClassName(v):"nil",vs,vertex && [v device]==device,f?object_getClassName(f):"nil",fs,fragment && [f device]==device,vd!=nil,
  (unsigned long)d.rasterSampleCount,d.alphaToCoverageEnabled,d.alphaToOneEnabled,d.rasterizationEnabled,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat,
  (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.tessellationFactorFormat,(unsigned long)d.maxTessellationFactor,d.supportIndirectCommandBuffers,(unsigned long)d.maxVertexAmplificationCount,
  vl!=nil,(unsigned long)vl.functions.count,(unsigned long)vl.binaryFunctions.count,(unsigned long)vl.privateFunctions.count,(unsigned long)vl.groups.count,
  fl!=nil,(unsigned long)fl.functions.count,(unsigned long)fl.binaryFunctions.count,(unsigned long)fl.privateFunctions.count,(unsigned long)fl.groups.count,colors);
#define CHECK(field,value,accepted) do {__typeof__(value) actual=(value);if(!stage4Value("pipeline",field,(uint64_t)actual,(accepted)))return [NSString stringWithUTF8String:field];}while(0)
 CHECK("descriptor",d,d!=nil);
 if(!stage4Class("pipeline","vertexFunction.class",v,[FuxiFunction class]))return @"vertexFunction.class";
 if(!stage4Class("pipeline","fragmentFunction.class",f,[FuxiFunction class]))return @"fragmentFunction.class";
 CHECK("vertexFunction.device",[v device],actual==device);
 CHECK("fragmentFunction.device",[f device],actual==device);
 CHECK("vertexFunction.stage",vs,actual==1);
 CHECK("fragmentFunction.stage",fs,actual==2);
 // 20H392's getter lazily creates an empty descriptor. Presence alone cannot
 // distinguish the untouched default from an explicitly supplied empty object.
 // Accept only that empty shape; any configured vertex input still fails closed.
 if(vd)for(unsigned i=0;i<31;++i) {
  auto a=vd.attributes[i];auto l=vd.layouts[i];char field[96];
#define VCHECK(member,property,value,expected) do {snprintf(field,sizeof(field),"vertexDescriptor.%s[%u].%s",member,i,property);if(!stage4Value("pipeline",field,(uint64_t)(value),(value)==(expected)))return [NSString stringWithUTF8String:field];}while(0)
  VCHECK("attributes","format",a.format,MTLVertexFormatInvalid);VCHECK("attributes","offset",a.offset,0);VCHECK("attributes","bufferIndex",a.bufferIndex,0);
  VCHECK("layouts","stride",l.stride,0);VCHECK("layouts","stepFunction",l.stepFunction,MTLVertexStepFunctionPerVertex);VCHECK("layouts","stepRate",l.stepRate,1);
#undef VCHECK
 }
 CHECK("rasterSampleCount",d.rasterSampleCount,actual==1);
 CHECK("alphaToCoverageEnabled",d.alphaToCoverageEnabled,!actual);
 CHECK("alphaToOneEnabled",d.alphaToOneEnabled,!actual);
 CHECK("rasterizationEnabled",d.rasterizationEnabled,actual);
 CHECK("depthAttachmentPixelFormat",d.depthAttachmentPixelFormat,actual==MTLPixelFormatInvalid);
 CHECK("stencilAttachmentPixelFormat",d.stencilAttachmentPixelFormat,actual==MTLPixelFormatInvalid);
 CHECK("inputPrimitiveTopology",d.inputPrimitiveTopology,actual==MTLPrimitiveTopologyClassUnspecified || actual==MTLPrimitiveTopologyClassTriangle);
 CHECK("maxVertexAmplificationCount",d.maxVertexAmplificationCount,actual==1);
 // Tessellation settings have no effect without a patch draw; indirect support
 // is a capability request, not state for our sole direct draw. Do not gate them.
#define LINKED(prefix,obj) CHECK(prefix ".functions",obj.functions.count,!actual);CHECK(prefix ".binaryFunctions",obj.binaryFunctions.count,!actual);CHECK(prefix ".privateFunctions",obj.privateFunctions.count,!actual);CHECK(prefix ".groups",obj.groups.count,!actual)
 LINKED("vertexLinkedFunctions",vl);LINKED("fragmentLinkedFunctions",fl);
#undef LINKED
 for(unsigned i=0;i<8;++i) {
  auto a=d.colorAttachments[i];char field[96];
  snprintf(field,sizeof(field),"colorAttachments[%u].pixelFormat",i);CHECK(field,a.pixelFormat,actual==(i?MTLPixelFormatInvalid:MTLPixelFormatBGRA8Unorm));
  // Blend and write-mask state on an absent target cannot affect this pipeline.
  if(!i) {CHECK("colorAttachments[0].blendingEnabled",a.blendingEnabled,!actual);CHECK("colorAttachments[0].writeMask",a.writeMask,actual==MTLColorWriteMaskAll);}
 }
#undef CHECK
 return nil;
}
@interface FuxiTexture : FuxiObject { @public id _device; fuxi::Texture *_texture; }
@end
@implementation FuxiTexture
- (void)dealloc {owner(_device)->backend->destroy(_texture);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (NSUInteger)width {CENSUS();return 256;}
- (NSUInteger)height {CENSUS();return 256;}
- (NSUInteger)depth {CENSUS();return 1;}
- (NSUInteger)arrayLength {CENSUS();return 1;}
- (NSUInteger)mipmapLevelCount {CENSUS();return 1;}
- (NSUInteger)sampleCount {CENSUS();return 1;}
- (MTLTextureType)textureType {CENSUS();return MTLTextureType2D;}
- (MTLPixelFormat)pixelFormat {CENSUS();return MTLPixelFormatBGRA8Unorm;}
- (MTLStorageMode)storageMode {CENSUS();return MTLStorageModePrivate;}
- (MTLTextureUsage)usage {CENSUS();return MTLTextureUsageRenderTarget;}
- (NSUInteger)allocatedSize {CENSUS();return _texture->allocated;}
@end
@interface FuxiPipeline : FuxiObject { @public id _device; fuxi::Pipeline *_pipeline; }
@end
@implementation FuxiPipeline
- (void)dealloc {owner(_device)->backend->destroy(_pipeline);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
@end
@class FuxiCommandBuffer;
@interface FuxiQueue : FuxiObject { @public id _device; NSString *_label; }
@end
@interface FuxiCommandBuffer : FuxiObject {
 @public FuxiQueue *_queue; NSMutableArray *_resources,*_handlers; NSString *_label; NSError *_error;
 std::vector<fuxi::Operation> _ops; MTLCommandBufferStatus _status; BOOL _encoding;
}
@end
@interface FuxiBlit : FuxiObject { @public FuxiCommandBuffer *_command; BOOL _ended; NSString *_label; }
@end
@interface FuxiRender : FuxiObject { @public FuxiCommandBuffer *_command; FuxiTexture *_target; FuxiPipeline *_pipeline; MTLClearColor _clear; BOOL _ended,_drawn; }
@end
@implementation FuxiRender
- (void)dealloc {[_command release];[_target release];[_pipeline release];[super dealloc];}
- (void)active {stage4Require("renderEncoder","ended",_ended,!_ended);stage4Require("renderEncoder","encoding",_command->_encoding,_command->_encoding);
 stage4Require("renderEncoder","commandStatus",_command->_status,_command->_status==MTLCommandBufferStatusNotEnqueued);}
- (void)setRenderPipelineState:(id)p {CENSUS();[self active];
 if(!stage4Class("renderEncoder","pipeline.class",p,[FuxiPipeline class]))invalid(@"renderEncoder pipeline.class");
 stage4Require("renderEncoder","pipeline.device",(uint64_t)[p device],[p device]==_command->_queue->_device);
 [_pipeline release];_pipeline=[p retain];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {CENSUS();[self active];
 stage4Require("draw","pipeline",(uint64_t)_pipeline,_pipeline!=nil);stage4Require("draw","alreadyDrawn",_drawn,!_drawn);
 stage4Require("draw","primitiveType",t,t==MTLPrimitiveTypeTriangle);stage4Require("draw","vertexStart",start,!start);stage4Require("draw","vertexCount",count,count==3);
 fuxi::Operation op{};op.texture=_target->_texture;op.pipeline=_pipeline->_pipeline;
 op.clear[0]=_clear.red;op.clear[1]=_clear.green;op.clear[2]=_clear.blue;op.clear[3]=_clear.alpha;
 [_command->_resources addObject:_target];[_command->_resources addObject:_pipeline];_command->_ops.push_back(op);_drawn=YES;
}
- (void)endEncoding {CENSUS();[self active];stage4Require("renderEncoder","drawn",_drawn,_drawn);_ended=YES;_command->_encoding=NO;}
@end
@implementation FuxiQueue
- (void)dealloc {[_device release];[_label release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {CENSUS();[_label release];_label=[s copy];}
- (id)commandBuffer {CENSUS();
 FuxiCommandBuffer *b=[FuxiCommandBuffer new];b->_queue=[self retain];
 b->_resources=[NSMutableArray new];b->_handlers=[NSMutableArray new];return [b autorelease];
}
@end
@implementation FuxiCommandBuffer
- (void)dealloc {[_queue release];[_resources release];[_handlers release];[_label release];[_error release];[super dealloc];}
- (id)device {CENSUS();return _queue->_device;}
- (id)commandQueue {CENSUS();return _queue;}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {CENSUS();[_label release];_label=[s copy];}
- (MTLCommandBufferStatus)status {CENSUS();return _status;}
- (NSError *)error {CENSUS();return _error;}
- (BOOL)retainedReferences {CENSUS();return YES;}
- (id)blitCommandEncoder {CENSUS();
 if(_status!=MTLCommandBufferStatusNotEnqueued || _encoding) {unsupported(self,_cmd);return nil;}
 FuxiBlit *e=[FuxiBlit new];e->_command=[self retain];_encoding=YES;return [e autorelease];
}
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {CENSUS();
 if(clientMode==fuxi::ClientMode::census)return nil;
 if(!stage4Value("renderEncoder","commandStatus",_status,_status==MTLCommandBufferStatusNotEnqueued) ||
    !stage4Value("renderEncoder","encoding",_encoding,!_encoding))return nil;
#define CHECK(field,value,accepted) do {__typeof__(value) actual=(value);stage4Require("renderPass",field,(uint64_t)actual,(accepted));}while(0)
 CHECK("descriptor",d,actual!=nil);
 auto a=d.colorAttachments[0];id t=a.texture;
 if(!stage4Class("renderPass","colorAttachments[0].texture.class",t,[FuxiTexture class]))invalid(@"renderPass texture.class");
 CHECK("colorAttachments[0].texture.device",[t device],actual==[self device]);
 CHECK("colorAttachments[0].level",a.level,!actual);CHECK("colorAttachments[0].slice",a.slice,!actual);CHECK("colorAttachments[0].depthPlane",a.depthPlane,!actual);
 CHECK("colorAttachments[0].resolveTexture",a.resolveTexture,!actual);
 CHECK("colorAttachments[0].loadAction",a.loadAction,actual==MTLLoadActionClear);CHECK("colorAttachments[0].storeAction",a.storeAction,actual==MTLStoreActionStore);
 // Attachment objects are lazily created on iOS too: gate their textures.
 CHECK("depthAttachment.texture",d.depthAttachment.texture,!actual);CHECK("stencilAttachment.texture",d.stencilAttachment.texture,!actual);
 CHECK("depthAttachment.resolveTexture",d.depthAttachment.resolveTexture,!actual);CHECK("stencilAttachment.resolveTexture",d.stencilAttachment.resolveTexture,!actual);
 CHECK("visibilityResultBuffer",d.visibilityResultBuffer,!actual);
 CHECK("renderTargetArrayLength",d.renderTargetArrayLength,actual<=1);
 CHECK("imageblockSampleLength",d.imageblockSampleLength,!actual);CHECK("tileWidth",d.tileWidth,!actual);CHECK("tileHeight",d.tileHeight,!actual);
 CHECK("threadgroupMemoryLength",d.threadgroupMemoryLength,!actual);CHECK("defaultRasterSampleCount",d.defaultRasterSampleCount,actual<=1);
 CHECK("rasterizationRateMap",d.rasterizationRateMap,!actual);
 CHECK("renderTargetWidth",d.renderTargetWidth,!actual || actual==256);CHECK("renderTargetHeight",d.renderTargetHeight,!actual || actual==256);
 CHECK("samplePositions.count",[d getSamplePositions:nullptr count:0],!actual);
 for(unsigned i=1;i<8;++i) {
  char field[80];snprintf(field,sizeof(field),"colorAttachments[%u].texture",i);CHECK(field,d.colorAttachments[i].texture,!actual);
  snprintf(field,sizeof(field),"colorAttachments[%u].resolveTexture",i);CHECK(field,d.colorAttachments[i].resolveTexture,!actual);
 }
 MTLClearColor c=a.clearColor;unsigned component=0;
 for(double x:{c.red,c.green,c.blue,c.alpha}) {
  if(!std::isfinite(x) || x<0 || x>1) {fprintf(stderr,"FUXI MTL: renderPass reject clearColor[%u] %.17g\n",component,x);invalid(@"clear colour outside 0..1");}++component;
 }
#undef CHECK
 FuxiRender *e=[FuxiRender new];e->_command=[self retain];e->_target=[t retain];e->_clear=c;_encoding=YES;return [e autorelease];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {CENSUS();
 if(!handler) invalid(@"nil completion handler");
 if(_status>=MTLCommandBufferStatusCompleted) {handler((id)self);return;}
 id copied=[handler copy];[_handlers addObject:copied];[copied release];
}
- (void)commit {CENSUS();
 if(_status!=MTLCommandBufferStatusNotEnqueued || _encoding) invalid(@"commit twice or active encoder");
 _status=MTLCommandBufferStatusCommitted;
 // Deliberately synchronous M4 queue: no dispatch worker or asynchronous promise.
 try {if(clientMode==fuxi::ClientMode::census)throw std::runtime_error("census: no GPU submission");owner(_queue->_device)->backend->submit(_ops);_status=MTLCommandBufferStatusCompleted;}
 catch(const std::exception &e) {
  fprintf(stderr,"FUXI MTL: submit FAIL %s\n",e.what());_status=MTLCommandBufferStatusError;
  _error=[[NSError alloc] initWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
      userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:e.what()]}];
 }
 NSArray *handlers=[_handlers copy];[_handlers removeAllObjects];
 for(MTLCommandBufferHandler handler in handlers) handler((id)self);
 [handlers release];[_resources removeAllObjects];_ops.clear();
}
- (void)waitUntilCompleted {CENSUS();if(_status<MTLCommandBufferStatusCompleted) invalid(@"wait before commit");}
@end
@implementation FuxiBlit
- (void)dealloc {[_command release];[_label release];[super dealloc];}
- (id)device {CENSUS();return _command->_queue->_device;}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {CENSUS();[_label release];_label=[s copy];}
- (FuxiBuffer *)validate:(id)b offset:(NSUInteger)o size:(NSUInteger)n {CENSUS();
 if(_ended || !_command->_encoding || _command->_status!=MTLCommandBufferStatusNotEnqueued) invalid(@"inactive encoder");
 if(![b isKindOfClass:[FuxiBuffer class]] || [b device]!=_command->_queue->_device) invalid(@"foreign buffer/device");
 FuxiBuffer *buffer=b;
 if(o>buffer->_buffer->length || n>buffer->_buffer->length-o || ((o|n)&3)) invalid(@"M4 transfers require in-bounds 4-byte aligned offset and length");
 return buffer;
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {CENSUS();
 FuxiBuffer *dst=[self validate:b offset:r.location size:r.length];if(!r.length)return;
 [_command->_resources addObject:dst];_command->_ops.push_back({nullptr,dst->_buffer,0,r.location,r.length,v});
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)to size:(NSUInteger)n {CENSUS();
 FuxiBuffer *src=[self validate:s offset:so size:n],*dst=[self validate:d offset:to size:n];
 if(src==dst && n && so<to+n && to<so+n) invalid(@"overlapping copy");
 if(!n)return;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];
 _command->_ops.push_back({src->_buffer,dst->_buffer,so,to,n,0});
}
- (void)copyFromTexture:(id)t sourceSlice:(NSUInteger)s sourceLevel:(NSUInteger)l sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z
 toBuffer:(id)b destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {CENSUS();
 stage4Require("readback","ended",_ended,!_ended);stage4Require("readback","encoding",_command->_encoding,_command->_encoding);
 stage4Require("readback","commandStatus",_command->_status,_command->_status==MTLCommandBufferStatusNotEnqueued);
 if(!stage4Class("readback","buffer.class",b,[FuxiBuffer class]))invalid(@"readback buffer.class");
 stage4Require("readback","buffer.device",(uint64_t)[b device],[b device]==_command->_queue->_device);
 FuxiBuffer *dst=b;
 stage4Require("readback","destinationOffset",offset,offset<=dst->_buffer->length);
 stage4Require("readback","buffer.length",dst->_buffer->length,dst->_buffer->length-offset>=256*256*4);
#define CHECK(field,value,accepted) do {__typeof__(value) actual=(value);stage4Require("readback",field,(uint64_t)actual,(accepted));}while(0)
 if(!stage4Class("readback","texture.class",t,[FuxiTexture class]))invalid(@"readback texture.class");
 CHECK("texture.device",[t device],actual==_command->_queue->_device);
 CHECK("sourceSlice",s,!actual);CHECK("sourceLevel",l,!actual);CHECK("sourceOrigin.x",o.x,!actual);CHECK("sourceOrigin.y",o.y,!actual);CHECK("sourceOrigin.z",o.z,!actual);
 CHECK("sourceSize.width",z.width,actual==256);CHECK("sourceSize.height",z.height,actual==256);CHECK("sourceSize.depth",z.depth,actual==1);
 CHECK("destinationBytesPerRow",row,actual==1024);CHECK("destinationBytesPerImage",image,actual==256*256*4);
 CHECK("destinationOffset",offset,!(actual&255));
#undef CHECK
 // Refuse reads without an earlier render in this command; no untracked image layout.
 bool rendered=false;for(const auto &op:_command->_ops)if(op.texture==((FuxiTexture *)t)->_texture && op.pipeline)rendered=true;
 stage4Require("readback","earlierRender",rendered,rendered);
 fuxi::Operation op{};op.dst=dst->_buffer;op.to=offset;op.texture=((FuxiTexture *)t)->_texture;
 [_command->_resources addObject:t];[_command->_resources addObject:dst];_command->_ops.push_back(op);
}
- (void)endEncoding {CENSUS();if(_ended)invalid(@"endEncoding twice");_ended=YES;_command->_encoding=NO;}
@end
/* Methods are copied onto a runtime subclass. No fixed private layout/ivars are
 * read, initialized, or freed through the private superclass implementation. */
@interface FuxiDeviceMethods : FuxiObject
@end
@implementation FuxiDeviceMethods
- (id)init {CENSUS();unsupported(self,_cmd);[self release];return nil;}
- (id)initWithAcceleratorPort:(uint32_t)port {
 checkClient();
 if(clientMode==fuxi::ClientMode::deny) {[self release];return nil;}
 CENSUS();
 if((clientMode!=fuxi::ClientMode::census && geteuid()!=0) || !port || owner(self)) {[self release];return nil;}
 if(clientMode==fuxi::ClientMode::census) {
  FuxiOwner *state=[FuxiOwner new];
  objc_setAssociatedObject(self,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];return self;
 }
 void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
 auto getID=io?(int(*)(uint32_t,uint64_t *))dlsym(io,"IORegistryEntryGetRegistryEntryID"):nullptr;
 uint64_t registry=0;int rc=getID?getID(port,&registry):-1;
 if(io)dlclose(io);
 if(rc || !registry) {fprintf(stderr,"FUXI MTL: registryID FAIL\n");[self release];return nil;}
 FuxiOwner *state=[FuxiOwner new];state->registry=registry;
 try {if(clientMode!=fuxi::ClientMode::census)state->backend=new fuxi::Backend;}
 catch(const std::exception &e) {fprintf(stderr,"FUXI MTL: device FAIL %s\n",e.what());[state release];[self release];return nil;}
 objc_setAssociatedObject(self,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];return self;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc {
 objc_setAssociatedObject(self,&ownerKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 // Private base was never initialized. NSObject disposal only.
 object_dispose(self);
}
#pragma clang diagnostic pop
- (NSString *)name {CENSUS();return @"Fuxi Turnip Adreno 740 (M5b bounded offscreen)";}
- (uint64_t)registryID {CENSUS();return owner(self)->registry;}
- (NSUInteger)maxBufferLength {CENSUS();return fuxi::Backend::maxBufferLength;}
- (BOOL)hasUnifiedMemory {CENSUS();return YES;}
- (BOOL)isLowPower {CENSUS();return NO;}
- (BOOL)isHeadless {CENSUS();return YES;}
- (BOOL)isRemovable {CENSUS();return NO;}
- (BOOL)supportsFeatureSet:(NSUInteger)s {CENSUS();(void)s;return NO;}
- (BOOL)supportsFamily:(MTLGPUFamily)f {CENSUS();(void)f;return NO;}
- (BOOL)supportsTextureSampleCount:(NSUInteger)n {CENSUS();(void)n;return NO;}
- (BOOL)supportsRaytracing {CENSUS();return NO;}
- (BOOL)supportsFunctionPointers {CENSUS();return NO;}
- (BOOL)supportsDynamicLibraries {CENSUS();return NO;}
- (BOOL)supportsPullModelInterpolation {CENSUS();return NO;}
- (BOOL)supportsRasterOrderGroups {CENSUS();return NO;}
- (BOOL)areProgrammableSamplePositionsSupported {CENSUS();return NO;}
- (BOOL)areBarycentricCoordsSupported {CENSUS();return NO;}
- (BOOL)areRasterOrderGroupsSupported {CENSUS();return NO;}
- (MTLReadWriteTextureTier)readWriteTextureSupport {CENSUS();return MTLReadWriteTextureTierNone;}
- (MTLSize)maxThreadsPerThreadgroup {CENSUS();return MTLSizeMake(0,0,0);}
- (id)newCommandQueue {CENSUS();if(clientMode==fuxi::ClientMode::census)return nil;FuxiQueue *q=[FuxiQueue new];q->_device=[self retain];return q;}
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)options {CENSUS();
 // Reject write-combine/untracked/private/managed/sparse and unknown options.
 if(clientMode==fuxi::ClientMode::census)return nil;
 if(options!=MTLResourceStorageModeShared) {unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[FuxiBuffer new];b->_device=[self retain];
 try {b->_buffer=owner(self)->backend->allocate(n);}
 catch(const std::exception &e) {fprintf(stderr,"FUXI MTL: buffer FAIL %s\n",e.what());[b release];return nil;}
 return b;
}
- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)n options:(MTLResourceOptions)o {CENSUS();
 if(!bytes) {unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[self newBufferWithLength:n options:o];if(b)memcpy([b contents],bytes,n);return b;
}
- (id)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {CENSUS();
 if(error)*error=nil;if(clientMode==fuxi::ClientMode::census)return nil;
 if(!data || !dispatch_data_get_size(data) || dispatch_data_get_size(data)>4*1024*1024)return metalError(error,"library outside 1..4 MiB");
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 if(!mapped)return metalError(error,"map library failed");
 FuxiLibrary *lib=[FuxiLibrary new];lib->_device=[self retain];char why[2048]{};
 try {lib->_library=fuxi::Translator::get().open((const uint8_t *)bytes,size,why,sizeof(why));}
 catch(const std::exception &e) {dispatch_release(mapped);[lib release];return metalError(error,e.what());}
 dispatch_release(mapped);if(!lib->_library) {[lib release];return metalError(error,why);}return lib;
}
- (id)newLibraryWithURL:(NSURL *)url error:(NSError **)error {CENSUS();
 if(error)*error=nil;if(clientMode==fuxi::ClientMode::census)return nil;
 if(!url.isFileURL)return metalError(error,"library requires a file URL");
 int fd=open(url.fileSystemRepresentation,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st{};
 if(fd<0)return metalError(error,"open library failed");
 if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>4*1024*1024) {::close(fd);return metalError(error,"library file type/length");}
 NSMutableData *bytes=[NSMutableData dataWithLength:(NSUInteger)st.st_size];size_t done=0;
 while(done<bytes.length) {ssize_t n=read(fd,(uint8_t *)bytes.mutableBytes+done,bytes.length-done);
  if(n<0 && errno==EINTR)continue;if(n<=0) {::close(fd);return metalError(error,"read library failed");}done+=(size_t)n;}
 uint8_t extra;ssize_t tail;do {tail=read(fd,&extra,1);}while(tail<0 && errno==EINTR);::close(fd);
 if(tail!=0)return metalError(error,"library changed length during read");
 dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,dispatch_get_global_queue(0,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
 id result=[self newLibraryWithData:data error:error];dispatch_release(data);return result;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d {CENSUS();
 if(clientMode==fuxi::ClientMode::census)return nil;
#define CHECK(field,value,accepted) do {__typeof__(value) actual=(value);if(!stage4Value("texture",field,(uint64_t)actual,(accepted)))return nil;}while(0)
 CHECK("descriptor",d,actual!=nil);
 CHECK("textureType",d.textureType,actual==MTLTextureType2D);CHECK("pixelFormat",d.pixelFormat,actual==MTLPixelFormatBGRA8Unorm);
 CHECK("width",d.width,actual==256);CHECK("height",d.height,actual==256);CHECK("depth",d.depth,actual==1);
 CHECK("mipmapLevelCount",d.mipmapLevelCount,actual==1);CHECK("arrayLength",d.arrayLength,actual==1);CHECK("sampleCount",d.sampleCount,actual==1);
 CHECK("storageMode",d.storageMode,actual==MTLStorageModePrivate);CHECK("cpuCacheMode",d.cpuCacheMode,actual==MTLCPUCacheModeDefaultCache);
 CHECK("hazardTrackingMode",d.hazardTrackingMode,actual==MTLHazardTrackingModeDefault || actual==MTLHazardTrackingModeTracked);
 CHECK("usage",d.usage,actual==MTLTextureUsageRenderTarget);
 // allowGPUOptimizedContents is an implementation hint, not pixel semantics.
#undef CHECK
 FuxiTexture *t=[FuxiTexture new];t->_device=[self retain];
 try {t->_texture=owner(self)->backend->allocateTexture();}catch(const std::exception &e) {fprintf(stderr,"FUXI MTL: texture FAIL %s\n",e.what());[t release];return nil;}return t;
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {CENSUS();
 if(error)*error=nil;if(clientMode==fuxi::ClientMode::census)return nil;
 if(NSString *field=pipelineRejection(d,self))return metalError(error,field.UTF8String);
 id v=d.vertexFunction,f=d.fragmentFunction;
 FuxiPipeline *p=[FuxiPipeline new];p->_device=[self retain];
 try {auto a=(FuxiFunction *)v,b=(FuxiFunction *)f;p->_pipeline=owner(self)->backend->compilePipeline(a->_library->_library,a->_name.UTF8String,b->_library->_library,b->_name.UTF8String);}
 catch(const std::exception &e) {[p release];return metalError(error,e.what());}return p;
}
// MTLAddDevice on 16.7 calls these hooks. Capabilities come exclusively from
// our overrides; no Apple family/limit table is installed in the base object.
- (void)initLimits {CENSUS();}
- (void)initFeatureQueries {CENSUS();}
- (id)_deviceWrapper {CENSUS();return self;}
@end
static id allocateDevice(Class cls,SEL selector,NSZone *zone) {(void)selector;(void)zone;return class_createInstance(cls,0);}
static void initializeDevice(Class cls,SEL selector) {(void)cls;(void)selector;}
static void copyMethods(Class from,Class to) {
 unsigned n=0;Method *methods=class_copyMethodList(from,&n);
 for(unsigned i=0;i<n;++i)class_replaceMethod(to,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i]));
 free(methods);
}
static void rejectProtocol(Class cls,Protocol *proto) {
 if(!proto)return;
 for(int required=0;required<2;++required) {
  unsigned n=0;auto *methods=protocol_copyMethodDescriptionList(proto,required,YES,&n);
  for(unsigned i=0;i<n;++i) if(!class_getInstanceMethod(cls,methods[i].name))
   class_addMethod(cls,methods[i].name,(IMP)_objc_msgForward,methods[i].types);
  free(methods);
 }
 unsigned n=0;Protocol *__unsafe_unretained *parents=protocol_copyProtocolList(proto,&n);
 for(unsigned i=0;i<n;++i)rejectProtocol(cls,parents[i]);free(parents);
}
// A constructor runs during NSBundle load, before principalClass lookup.
static Class makeDeviceClass(Class base) {
 if(!base || class_getInstanceSize(base)!=528) {
  fprintf(stderr,"FUXI MTL: _MTLDevice size guard FAIL got %zu expected 528\n",base?class_getInstanceSize(base):0);return Nil;
 }
 Class cls=objc_allocateClassPair(base,"FuxiMTLDevice",0);if(!cls)return Nil;
 // Override inherited private implementations: unimplemented selectors must
 // reach forwarding, never execute against uninitialized private base ivars.
 for(Class c=base;c && c!=[NSObject class];c=class_getSuperclass(c)) {
  unsigned n=0;Method *m=class_copyMethodList(c,&n);
  for(unsigned i=0;i<n;++i)if(strncmp(sel_getName(method_getName(m[i])),".cxx_",5)) class_addMethod(cls,method_getName(m[i]),(IMP)_objc_msgForward,method_getTypeEncoding(m[i]));
  free(m);
 }
 copyMethods([NSObject class],cls);copyMethods([FuxiObject class],cls);copyMethods([FuxiDeviceMethods class],cls);
 for(const char *p:{"MTLDevice","MTLDeviceSPI"}) {Protocol *proto=objc_getProtocol(p);if(proto) {rejectProtocol(cls,proto);class_addProtocol(cls,proto);}}
 class_replaceMethod(object_getClass(cls),@selector(allocWithZone:),(IMP)allocateDevice,"@@:^v");
 class_replaceMethod(object_getClass(cls),@selector(initialize),(IMP)initializeDevice,"v@:");
 objc_registerClassPair(cls);
 fprintf(stderr,"FUXI MTL: registered FuxiMTLDevice base-size %zu instance-size %zu\n",class_getInstanceSize(base),class_getInstanceSize(cls));
 return cls;
}
#ifndef FUXI_MTL_FRONTEND_TEST
__attribute__((constructor)) static void registerDevice() {makeDeviceClass(objc_getClass("_MTLDevice"));}
#endif
// Public protocol selectors on resource classes also forward with real signatures.
__attribute__((constructor)) static void registerResources() {
 for(auto entry: {std::pair<Class,const char *>{[FuxiBuffer class],"MTLBuffer"},
      {[FuxiQueue class],"MTLCommandQueue"},{[FuxiCommandBuffer class],"MTLCommandBuffer"},{[FuxiBlit class],"MTLBlitCommandEncoder"},{[FuxiRender class],"MTLRenderCommandEncoder"},
      {[FuxiTexture class],"MTLTexture"},{[FuxiLibrary class],"MTLLibrary"},{[FuxiFunction class],"MTLFunction"},{[FuxiPipeline class],"MTLRenderPipelineState"}}) {
  Protocol *p=objc_getProtocol(entry.second);rejectProtocol(entry.first,p);if(p)class_addProtocol(entry.first,p);
 }
}
