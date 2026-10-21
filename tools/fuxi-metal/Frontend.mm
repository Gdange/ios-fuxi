/* Bounded Metal diagnostic. Original implementation; no Apple code. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mutex>
#include <atomic>
#include <chrono>
#include "ConstantHooks.hpp"
#include <cmath>
#include <cfloat>
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
static uint64_t observationMicros() {return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static std::atomic<uint64_t> clientCheckUs{0};
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
  clientCheckUs=observationMicros();
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
- (void)rejectSelector:(SEL)s;
@end
@implementation FuxiObject
- (void)rejectSelector:(SEL)s {unsupported(self,s);}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s {
 Method method=class_getInstanceMethod(object_getClass(self),s);
 if(method)return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)];
 {
  unsigned count=0;Protocol *__unsafe_unretained *protocols=objc_copyProtocolList(&count);
  const char *types=nullptr;
  for(unsigned i=0;i<count && !types;++i)for(int required=0;required<2 && !types;++required)
   types=protocol_getMethodDescription(protocols[i],s,required,YES).types;
  free(protocols);
  if(types)return [NSMethodSignature signatureWithObjCTypes:types];
 }
 // A private selector absent from every loaded protocol/class has no type ABI.
 // Use the conventional object/word failure signature, without reading args.
 return [NSMethodSignature signatureWithObjCTypes:"@@:"];
}
- (void)doesNotRecognizeSelector:(SEL)s {[self rejectSelector:s];}
- (void)forwardInvocation:(NSInvocation *)inv {
 [self rejectSelector:inv.selector];
 // Public protocols / inherited methods supply the exact return ABI, including
 // structs. Missing encoder methods poison their command; commit never submits.
 NSUInteger n=inv.methodSignature.methodReturnLength;
 if(n) {void *zero=calloc(1,n);[inv setReturnValue:zero];free(zero);}
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
// Track only public calls. Allocation provenance and method-coverage epochs
// prevent pre-hook objects or missed overrides from acquiring a trusted snapshot.
static char constantsKey,constantsAllocationKey;
static std::atomic<uint64_t> constantsConstructorUs{0},constantsInstallUs{0};
static fuxi::ClientMode constantsConstructorMode=fuxi::ClientMode::deny;
static IMP makeConstantHook(fuxi::ConstantMethod,IMP,SEL);
#ifndef FUXI_MTL_CONSTANT_CLASS
#define FUXI_MTL_CONSTANT_CLASS MTLFunctionConstantValues
#endif
static fuxi::ConstantHooks &constantHooks() {
 // Function-local state is safe even when the dyld constructor precedes C++
 // dynamic initializers. Each method gets its own captured original IMP, so
 // subclass overrides calling super cannot recurse through a global original.
 // The installed IMPs can run during process teardown; keep their registry alive.
 static auto *hooks=new fuxi::ConstantHooks([FUXI_MTL_CONSTANT_CLASS class],makeConstantHook);return *hooks;
}
static size_t constantSize(MTLDataType t) {
 switch(t){case MTLDataTypeBool:case MTLDataTypeChar:case MTLDataTypeUChar:return 1;
 case MTLDataTypeHalf:case MTLDataTypeShort:case MTLDataTypeUShort:return 2;
 case MTLDataTypeFloat:case MTLDataTypeInt:case MTLDataTypeUInt:return 4;default:return 0;}
}
static bool constantsObserved(id self) {
 uint64_t stamp=constantHooks().stamp(object_getClass(self));
 id allocation=objc_getAssociatedObject(self,&constantsAllocationKey);
 return stamp && [allocation isKindOfClass:[NSNumber class]] && [allocation unsignedLongLongValue]==stamp;
}
static void rememberConstant(id self,const void *value,MTLDataType type,id key) {
 id old=objc_getAssociatedObject(self,&constantsKey);size_t size=constantSize(type);
 if(!constantsObserved(self) || ![old isKindOfClass:[NSDictionary class]] || !value || !size || !key || ([old count]>=32 && !old[key])) {objc_setAssociatedObject(self,&constantsKey,@NO,OBJC_ASSOCIATION_RETAIN_NONATOMIC);return;}
 NSMutableDictionary *next=[old mutableCopy];next[key]=@[@(type),[NSData dataWithBytes:value length:size]];
 objc_setAssociatedObject(self,&constantsKey,next,OBJC_ASSOCIATION_COPY_NONATOMIC);[next release];
}
static id allocatedConstants(id result) {
 if(result) {constantHooks().install(object_getClass(result));@synchronized(result){
  if(!objc_getAssociatedObject(result,&constantsAllocationKey)) {
   uint64_t stamp=constantHooks().stamp(object_getClass(result));
   if(stamp) {objc_setAssociatedObject(result,&constantsAllocationKey,@(stamp),OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(result,&constantsKey,@{},OBJC_ASSOCIATION_COPY_NONATOMIC);}
  }
 }}return result;
}
static IMP makeConstantHook(fuxi::ConstantMethod kind,IMP original,SEL sel) {
 switch(kind) {
 case fuxi::ConstantInit:return imp_implementationWithBlock(^id(id self){return ((id(*)(id,SEL))original)(self,sel);});
 case fuxi::ConstantCopy:return imp_implementationWithBlock(^id(id self,NSZone *zone){@synchronized(self){
  bool observed=constantsObserved(self);id records=[objc_getAssociatedObject(self,&constantsKey) retain];
  id result=((id(*)(id,SEL,NSZone *))original)(self,sel,zone);
  if(result && result!=self) {constantHooks().install(object_getClass(result));
   uint64_t stamp=constantHooks().stamp(object_getClass(result));
   objc_setAssociatedObject(result,&constantsAllocationKey,observed && stamp?@(stamp):nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   objc_setAssociatedObject(result,&constantsKey,observed?records:nil,OBJC_ASSOCIATION_COPY_NONATOMIC);}
  [records release];return result;
 }});
 case fuxi::ConstantIndex:return imp_implementationWithBlock(^(id self,const void *value,MTLDataType type,NSUInteger index){@synchronized(self){
  ((void(*)(id,SEL,const void *,MTLDataType,NSUInteger))original)(self,sel,value,type,index);
  rememberConstant(self,value,type,index<=65535?@(index):nil);
 }});
 case fuxi::ConstantName:return imp_implementationWithBlock(^(id self,const void *value,MTLDataType type,NSString *name){@synchronized(self){
  ((void(*)(id,SEL,const void *,MTLDataType,NSString *))original)(self,sel,value,type,name);
  const char *utf8=name.UTF8String;size_t n=[name lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
  rememberConstant(self,value,type,utf8 && n && n<128 && strlen(utf8)==n?name:nil);
 }});
 case fuxi::ConstantRange:return imp_implementationWithBlock(^(id self,const void *value,MTLDataType type,NSRange range){@synchronized(self){
  ((void(*)(id,SEL,const void *,MTLDataType,NSRange))original)(self,sel,value,type,range);size_t size=constantSize(type);
  if(!constantsObserved(self) || !size || range.length>32 || range.location>65535 || range.length>65536-range.location || !value){objc_setAssociatedObject(self,&constantsKey,@NO,OBJC_ASSOCIATION_RETAIN_NONATOMIC);return;}
  for(NSUInteger i=0;i<range.length;++i)rememberConstant(self,(const uint8_t *)value+i*size,type,@(range.location+i));
 }});
 case fuxi::ConstantReset:return imp_implementationWithBlock(^(id self){@synchronized(self){
  ((void(*)(id,SEL))original)(self,sel);
  objc_setAssociatedObject(self,&constantsKey,constantsObserved(self)?@{}:nil,OBJC_ASSOCIATION_COPY_NONATOMIC);
 }});
 case fuxi::ConstantAlloc:return imp_implementationWithBlock(^id(id self,NSZone *zone){
  // Inherited factories cover late-registered subclasses before init/setters.
  constantHooks().install((Class)self);
  return allocatedConstants(((id(*)(id,SEL,NSZone *))original)(self,sel,zone));
 });
 case fuxi::ConstantNew:return imp_implementationWithBlock(^id(id self){
  constantHooks().install((Class)self);return ((id(*)(id,SEL))original)(self,sel);
 });
 case fuxi::ConstantPlainAlloc:return imp_implementationWithBlock(^id(id self){
  constantHooks().install((Class)self);return allocatedConstants(((id(*)(id,SEL))original)(self,sel));
 });
 case fuxi::ConstantMethodCount:break;
 }return nullptr;
}
static void refreshConstantHooks() {
#ifndef FUXI_MTL_FRONTEND_TEST
 if(clientMode!=fuxi::ClientMode::allow)return;
#endif
 constantHooks().refresh();uint64_t unset=0;
 constantsInstallUs.compare_exchange_strong(unset,observationMicros());
}
__attribute__((constructor)) static void observeConstants() {
 constantsConstructorUs=observationMicros();
#ifndef FUXI_MTL_FRONTEND_TEST
 checkClient();
#endif
 constantsConstructorMode=clientMode;refreshConstantHooks();
 fprintf(stderr,"FUXI MTL: constants observe mode=%s ctor_us=%llu gate_us=%llu install_us=%llu hooks=%zu\n",
  clientMode==fuxi::ClientMode::allow?"allow":clientMode==fuxi::ClientMode::census?"census":"deny",
  (unsigned long long)constantsConstructorUs.load(),(unsigned long long)clientCheckUs.load(),(unsigned long long)constantsInstallUs.load(),constantsInstallUs.load()?constantHooks().count():0);
}
static NSData *constantFailure(id values,const char *reason) {
 // Eight lines/process maximum; no private layout, constant bytes or paths.
 static std::atomic<unsigned> failures{0};if(failures.fetch_add(1)>=8)return nil;
 fuxi::ConstantDiagnostic diagnostic;
 Class cls=object_getClass(values);id record=objc_getAssociatedObject(values,&constantsKey);
 diagnostic.append("FUXI MTL: constants snapshot FAIL reason=%s class=%.96s chain=",reason,cls?class_getName(cls):"nil");
 Class c=cls;unsigned depth=0;for(;c && depth<8;c=class_getSuperclass(c),++depth)diagnostic.append("%s%.80s",depth?">":"",class_getName(c));if(c)diagnostic.append(">...");
 diagnostic.append(" ctor_us=%llu gate_us=%llu install_us=%llu ctor_mode=%u mode=%u hooks=%zu record=%.80s/count=%ld allocation=%llu coverage=%llu",
  (unsigned long long)constantsConstructorUs.load(),(unsigned long long)clientCheckUs.load(),(unsigned long long)constantsInstallUs.load(),
  (unsigned)constantsConstructorMode,(unsigned)clientMode,constantHooks().count(),record?object_getClassName(record):"nil",
  [record isKindOfClass:[NSDictionary class]]?(long)[record count]:-1L,
  (unsigned long long)[objc_getAssociatedObject(values,&constantsAllocationKey) unsignedLongLongValue],(unsigned long long)constantHooks().stamp(cls));
 for(unsigned i=0;i<fuxi::ConstantMethodCount;++i) {
  auto h=constantHooks().resolved(cls,(fuxi::ConstantMethod)i);Class target=i>=fuxi::ConstantAlloc?object_getClass((id)cls):cls;
  IMP actual=class_getMethodImplementation(target,sel_registerName(fuxi::constantSelectors[i]));
  diagnostic.append(" %s=%p/%p/%p",fuxi::constantSelectors[i],(void *)actual,(void *)h.hook,(void *)h.original);
 }
 diagnostic.append(" imp=actual/hook/original");fprintf(stderr,"%s\n",diagnostic.line);return nil;
}
static NSData *constantSnapshot(MTLFunctionConstantValues *values) {
 if(!values)return [NSData data];refreshConstantHooks();@synchronized(values){
 if(!objc_getAssociatedObject(values,&constantsAllocationKey))return constantFailure(values,"unobserved-allocation");
 if(!constantsObserved(values))return constantFailure(values,"changed-or-unhooked-method");
 id records=objc_getAssociatedObject(values,&constantsKey);
 if(![records isKindOfClass:[NSDictionary class]] || [records count]>32)return constantFailure(values,"unsupported-record");
 NSMutableData *data=[NSMutableData data];
 // Deterministic order makes equal constants share the translator cache.
 NSArray *keys=[[records allKeys] sortedArrayUsingComparator:^NSComparisonResult(id a,id b){return [[a description] compare:[b description]];}];
 for(id key in keys){NSArray *record=records[key];fuxi::Constant c{};c.type=[record[0] unsignedIntValue];NSData *bytes=record[1];c.size=(uint32_t)bytes.length;memcpy(c.bytes,bytes.bytes,bytes.length);
 if([key isKindOfClass:[NSNumber class]])c.index=[key unsignedIntValue];else {const char *name=[key UTF8String];if(!name || strlen(name)>=128)return constantFailure(values,"unsupported-name");strcpy(c.name,name);}
 [data appendBytes:&c length:sizeof(c)];}return data;}
}
@interface FuxiLibrary : FuxiObject { @public id _device; void *_library; }
@end
@interface FuxiFunction : FuxiObject { @public FuxiLibrary *_library; NSString *_name; NSData *_constants; int _stage; }
@end
@implementation FuxiFunction
- (void)dealloc {[_library release];[_name release];[_constants release];[super dealloc];}
- (id)device {CENSUS();return _library->_device;}
- (NSString *)name {CENSUS();return _name;}
- (MTLFunctionType)functionType {CENSUS();return _stage==1?MTLFunctionTypeVertex:MTLFunctionTypeFragment;}
@end
@implementation FuxiLibrary
- (void)dealloc {if(_library)fuxi::Translator::get().close(_library);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (id)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error {CENSUS();
 if(error)*error=nil;
 // Constants are immutable snapshots of observed public setter calls.
 if(!d || d.specializedName || d.options!=MTLFunctionOptionNone) {
  unsupported(self,_cmd);return metalError(error,"unsupported function specialization/options");
 }
 return [self newFunctionWithName:d.name constantValues:d.constantValues error:error];
}
- (id)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {CENSUS();
 if(error)*error=nil;NSData *snapshot=constantSnapshot(values);
 if(!snapshot)return metalError(error,"unobserved/unsupported function constants");
 FuxiFunction *f=[self newFunctionWithName:name];if(!f)return metalError(error,"function lookup failed");
 f->_constants=[snapshot copy];return f;
}
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
 // Validate configured attributes and per-vertex layouts against our small profile.
 if(vd)for(unsigned i=0;i<31;++i) {
  auto a=vd.attributes[i];auto l=vd.layouts[i];
  if(a.format!=MTLVertexFormatInvalid && (i>=8 || (a.format!=MTLVertexFormatFloat2 && a.format!=MTLVertexFormatFloat4 && a.format!=MTLVertexFormatUChar4Normalized) || a.bufferIndex>=4 || a.offset>252))return @"vertexDescriptor attribute";
  if(l.stride && (i>=4 || l.stride>256 || l.stepFunction!=MTLVertexStepFunctionPerVertex || l.stepRate!=1))return @"vertexDescriptor layout";
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
  snprintf(field,sizeof(field),"colorAttachments[%u].pixelFormat",i);CHECK(field,a.pixelFormat,i?actual==MTLPixelFormatInvalid:(actual==80 || actual==70 || actual==115 || actual==10 || actual==30));
  // Blend and write-mask state on an absent target cannot affect this pipeline.
  if(!i) {CHECK("colorAttachments[0].writeMask",a.writeMask,!(actual&~15u));
   if(a.blendingEnabled && (a.sourceRGBBlendFactor>MTLBlendFactorOneMinusDestinationAlpha || a.destinationRGBBlendFactor>MTLBlendFactorOneMinusDestinationAlpha || a.sourceAlphaBlendFactor>MTLBlendFactorOneMinusDestinationAlpha || a.destinationAlphaBlendFactor>MTLBlendFactorOneMinusDestinationAlpha || a.rgbBlendOperation>MTLBlendOperationMax || a.alphaBlendOperation>MTLBlendOperationMax))return @"blend factor/operation";}
 }
#undef CHECK
 return nil;
}
static bool identitySwizzle(MTLTextureSwizzleChannels s) {
 return s.red==MTLTextureSwizzleRed && s.green==MTLTextureSwizzleGreen && s.blue==MTLTextureSwizzleBlue && s.alpha==MTLTextureSwizzleAlpha;
}
@interface FuxiTexture : FuxiObject { @public id _device; fuxi::Texture *_texture; MTLTextureUsage _usage; IOSurfaceRef _surface; }
@end
@implementation FuxiTexture
- (void)dealloc {
 bool safeToUnlock=owner(_device)->backend->destroy(_texture);
 // A timeout leaves ownership unknown. Preserve the locked surface/pages until
 // process teardown, just as Backend preserves Vulkan allocations on poison.
 if(_surface && safeToUnlock) {IOSurfaceUnlock(_surface,0,nullptr);IOSurfaceDecrementUseCount(_surface);CFRelease(_surface);}
 [_device release];[super dealloc];
}
- (id)device {CENSUS();return _device;}
- (NSUInteger)width {CENSUS();return 256;}
- (NSUInteger)height {CENSUS();return 256;}
- (NSUInteger)depth {CENSUS();return 1;}
- (NSUInteger)arrayLength {CENSUS();return 1;}
- (NSUInteger)mipmapLevelCount {CENSUS();return 1;}
- (NSUInteger)sampleCount {CENSUS();return 1;}
- (MTLTextureType)textureType {CENSUS();return MTLTextureType2D;}
- (MTLPixelFormat)pixelFormat {CENSUS();return (MTLPixelFormat)_texture->format;}
- (MTLStorageMode)storageMode {CENSUS();return _surface?MTLStorageModeShared:MTLStorageModePrivate;}
- (MTLTextureUsage)usage {CENSUS();return _usage;}
- (IOSurfaceRef)iosurface {CENSUS();return _surface;}
- (NSUInteger)iosurfacePlane {CENSUS();return 0;}
- (MTLTextureSwizzleChannels)swizzle {CENSUS();return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha);}
- (MTLCPUCacheMode)cpuCacheMode {CENSUS();return MTLCPUCacheModeDefaultCache;}
- (MTLHazardTrackingMode)hazardTrackingMode {CENSUS();return MTLHazardTrackingModeTracked;}
- (NSUInteger)allocatedSize {CENSUS();return _texture->allocated;}
@end
@interface FuxiPipeline : FuxiObject { @public id _device; fuxi::Pipeline *_pipeline; }
@end
@implementation FuxiPipeline
- (void)dealloc {owner(_device)->backend->destroy(_pipeline);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
@end
@interface FuxiSampler : FuxiObject { @public id _device;fuxi::Sampler *_sampler;NSString *_label; }
@end
@implementation FuxiSampler
- (void)dealloc {owner(_device)->backend->destroy(_sampler);[_device release];[_label release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (NSString *)label {CENSUS();return _label;}
@end
@class FuxiCommandBuffer;
@interface FuxiQueue : FuxiObject { @public id _device; NSString *_label; }
@end
@interface FuxiCommandBuffer : FuxiObject {
 @public FuxiQueue *_queue; NSMutableArray *_resources,*_handlers; NSString *_label; NSError *_error;
 std::vector<fuxi::Operation> _ops; MTLCommandBufferStatus _status; BOOL _encoding,_rejected;
}
@end
@interface FuxiBlit : FuxiObject { @public FuxiCommandBuffer *_command; BOOL _ended; NSString *_label; }
@end
@interface FuxiRender : FuxiObject { @public FuxiCommandBuffer *_command; FuxiTexture *_target; FuxiPipeline *_pipeline; MTLClearColor _clear; BOOL _ended,_drawn;
 FuxiTexture *_samples[4];FuxiSampler *_samplers[4];fuxi::BufferBind _buffers[2][4];uint32_t _load,_store;size_t _lastOp;
 fuxi::Operation _state; }
@end
@implementation FuxiRender
- (void)rejectSelector:(SEL)s {unsupported(self,s);[_command rejectSelector:s];}
- (void)dealloc {[_command release];[_target release];[_pipeline release];for(unsigned i=0;i<4;++i){[_samples[i] release];[_samplers[i] release];}[super dealloc];}
- (void)active {stage4Require("renderEncoder","ended",_ended,!_ended);stage4Require("renderEncoder","encoding",_command->_encoding,_command->_encoding);
 stage4Require("renderEncoder","commandStatus",_command->_status,_command->_status==MTLCommandBufferStatusNotEnqueued);}
- (void)setRenderPipelineState:(id)p {@try {CENSUS();[self active];
 if(!stage4Class("renderEncoder","pipeline.class",p,[FuxiPipeline class]))invalid(@"renderEncoder pipeline.class");
 stage4Require("renderEncoder","pipeline.device",(uint64_t)[p device],[p device]==_command->_queue->_device);
 [p retain];[_pipeline release];_pipeline=p;
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i {@try {CENSUS();[self active];
 stage4Require("sampling","textureIndex",i,i<4);
 if(!stage4Class("sampling","texture.class",t,[FuxiTexture class]))invalid(@"sampling texture.class");
 stage4Require("sampling","texture.device",(uint64_t)[t device],[t device]==_command->_queue->_device);
 stage4Require("sampling","texture.usage",((FuxiTexture *)t)->_usage,((FuxiTexture *)t)->_usage&MTLTextureUsageShaderRead);
 stage4Require("sampling","attachmentFeedback",t==_target,t!=_target && !(((FuxiTexture *)t)->_surface && _target->_surface && IOSurfaceGetID(((FuxiTexture *)t)->_surface)==IOSurfaceGetID(_target->_surface)));
 [t retain];[_samples[i] release];_samples[i]=t;
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i {@try {CENSUS();[self active];
 stage4Require("sampling","samplerIndex",i,i<4);
 if(!stage4Class("sampling","sampler.class",s,[FuxiSampler class]))invalid(@"sampling sampler.class");
 stage4Require("sampling","sampler.device",(uint64_t)[s device],[s device]==_command->_queue->_device);
 [s retain];[_samplers[i] release];_samplers[i]=s;
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)setBuffer:(id)b offset:(NSUInteger)o index:(NSUInteger)i stage:(unsigned)stage bytes:(BOOL)bytes {
 [self active];if(i>=4 || (b && (![b isKindOfClass:[FuxiBuffer class]] || [b device]!=_command->_queue->_device || o>=[b length])))invalid(@"buffer slot/device/offset");
 if(b)[_command->_resources addObject:b];_buffers[stage][i]={b?((FuxiBuffer *)b)->_buffer:nullptr,o,(bool)bytes};
}
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {@try {CENSUS();[self setBuffer:b offset:o index:i stage:0 bytes:NO];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {@try {CENSUS();[self setBuffer:b offset:o index:i stage:1 bytes:NO];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setBytes:(const void *)bytes length:(NSUInteger)n index:(NSUInteger)i stage:(unsigned)stage {
 [self active];if(!bytes || !n || n>4096 || i>=4)invalid(@"inline bytes bound");
 FuxiBuffer *b=[FuxiBuffer new];b->_device=[_command->_queue->_device retain];
 try {b->_buffer=owner(b->_device)->backend->allocate(n);memcpy(b->_buffer->contents,bytes,n);}
 catch(const std::exception &e){[b release];invalid([NSString stringWithUTF8String:e.what()]);}
 [self setBuffer:b offset:0 index:i stage:stage bytes:YES];[b release];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)n atIndex:(NSUInteger)i {@try {CENSUS();[self setBytes:bytes length:n index:i stage:0];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)n atIndex:(NSUInteger)i {@try {CENSUS();[self setBytes:bytes length:n index:i stage:1];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setViewport:(MTLViewport)v {@try {CENSUS();[self active];
 if(!std::isfinite(v.originX) || !std::isfinite(v.originY) || !std::isfinite(v.width) || !std::isfinite(v.height) || !std::isfinite(v.znear) || !std::isfinite(v.zfar) || v.originX<0 || v.originY<0 || v.width<=0 || v.height<=0 || v.originX+v.width>256 || v.originY+v.height>256 || v.znear<0 || v.zfar>1 || v.znear>v.zfar)invalid(@"viewport bounds");
 _state.viewport={(float)v.originX,(float)(v.originY+v.height),(float)v.width,(float)-v.height,(float)v.znear,(float)v.zfar};
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setScissorRect:(MTLScissorRect)r {@try {CENSUS();[self active];if(r.x>256 || r.y>256 || r.width>256-r.x || r.height>256-r.y)invalid(@"scissor bounds");
 _state.scissor={{(int32_t)r.x,(int32_t)r.y},{(uint32_t)r.width,(uint32_t)r.height}};
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)record:(MTLPrimitiveType)t start:(NSUInteger)start count:(NSUInteger)count indices:(id)indices offset:(NSUInteger)offset {
 [self active];if(!_pipeline || (t!=MTLPrimitiveTypeTriangle && t!=MTLPrimitiveTypeTriangleStrip) || !count || count>65536 || start>65536-count)invalid(@"draw bounds/pipeline/topology");
 if(indices && (![indices isKindOfClass:[FuxiBuffer class]] || [indices device]!=_command->_queue->_device || (offset&1) || offset>[indices length] || count*2>[indices length]-offset))invalid(@"index bounds/device");
 fuxi::Operation op=_state;op.texture=_target->_texture;op.pipeline=_pipeline->_pipeline;op.vertices=(uint32_t)count;op.first=(uint32_t)start;op.strip=t==MTLPrimitiveTypeTriangleStrip;
 op.indices=indices?((FuxiBuffer *)indices)->_buffer:nullptr;op.indexOffset=offset;
 op.load=_drawn?1:_load;op.store=1;
 for(unsigned i=0;i<4;++i){op.samples[i]=_samples[i]?_samples[i]->_texture:nullptr;op.samplers[i]=_samplers[i]?_samplers[i]->_sampler:nullptr;
 if(_samples[i])[_command->_resources addObject:_samples[i]];if(_samplers[i])[_command->_resources addObject:_samplers[i]];}
 memcpy(op.buffers,_buffers,sizeof(_buffers));if(indices)[_command->_resources addObject:indices];
 op.clear[0]=_clear.red;op.clear[1]=_clear.green;op.clear[2]=_clear.blue;op.clear[3]=_clear.alpha;
 [_command->_resources addObject:_target];[_command->_resources addObject:_pipeline];_lastOp=_command->_ops.size();_command->_ops.push_back(op);_drawn=YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {@try {CENSUS();[self record:t start:start count:count indices:nil offset:0];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)count indexType:(MTLIndexType)type indexBuffer:(id)b indexBufferOffset:(NSUInteger)offset {@try {CENSUS();if(type!=MTLIndexTypeUInt16 || !b)invalid(@"index type/buffer");[self record:t start:0 count:count indices:b offset:offset];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)endEncoding {@try {CENSUS();[self active];stage4Require("renderEncoder","drawn",_drawn,_drawn);_command->_ops[_lastOp].store=_store;_ended=YES;_command->_encoding=NO;} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}

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
- (void)rejectSelector:(SEL)s {
 unsupported(self,s);_rejected=YES;
 if(!_error)_error=[[NSError alloc] initWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInvalidResource
  userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"unsupported %@",NSStringFromSelector(s)]}];
}
- (void)dealloc {[_queue release];[_resources release];[_handlers release];[_label release];[_error release];[super dealloc];}
- (id)device {CENSUS();return _queue->_device;}
- (id)commandQueue {CENSUS();return _queue;}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {CENSUS();[_label release];_label=[s copy];}
- (MTLCommandBufferStatus)status {CENSUS();return _status;}
- (NSError *)error {CENSUS();return _error;}
- (BOOL)retainedReferences {CENSUS();return YES;}
- (id)blitCommandEncoder {CENSUS();
 if(_rejected || _status!=MTLCommandBufferStatusNotEnqueued || _encoding) {unsupported(self,_cmd);return nil;}
 FuxiBlit *e=[FuxiBlit new];e->_command=[self retain];_encoding=YES;return [e autorelease];
}
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {@try {CENSUS();
 if(clientMode==fuxi::ClientMode::census || _rejected)return nil;
 if(!stage4Value("renderEncoder","commandStatus",_status,_status==MTLCommandBufferStatusNotEnqueued) ||
    !stage4Value("renderEncoder","encoding",_encoding,!_encoding))return nil;
#define CHECK(field,value,accepted) do {__typeof__(value) actual=(value);stage4Require("renderPass",field,(uint64_t)actual,(accepted));}while(0)
 CHECK("descriptor",d,actual!=nil);
 auto a=d.colorAttachments[0];id t=a.texture;
 if(!stage4Class("renderPass","colorAttachments[0].texture.class",t,[FuxiTexture class]))invalid(@"renderPass texture.class");
 CHECK("colorAttachments[0].texture.device",[t device],actual==[self device]);
 CHECK("colorAttachments[0].texture.usage",((FuxiTexture *)t)->_usage,actual&MTLTextureUsageRenderTarget);
 CHECK("colorAttachments[0].level",a.level,!actual);CHECK("colorAttachments[0].slice",a.slice,!actual);CHECK("colorAttachments[0].depthPlane",a.depthPlane,!actual);
 CHECK("colorAttachments[0].resolveTexture",a.resolveTexture,!actual);
 CHECK("colorAttachments[0].loadAction",a.loadAction,actual<=MTLLoadActionClear);CHECK("colorAttachments[0].storeAction",a.storeAction,actual<=MTLStoreActionStore);
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
 FuxiRender *e=[FuxiRender new];e->_state={};e->_command=[self retain];e->_target=[t retain];e->_clear=c;e->_load=(uint32_t)a.loadAction;e->_store=(uint32_t)a.storeAction;_encoding=YES;return [e autorelease];
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];return nil;}}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {CENSUS();
 if(!handler) {[self rejectSelector:_cmd];return;}
 if(_status>=MTLCommandBufferStatusCompleted) {handler((id)self);return;}
 id copied=[handler copy];[_handlers addObject:copied];[copied release];
}
- (void)commit {CENSUS();
 if(_status!=MTLCommandBufferStatusNotEnqueued)return;
 if(_encoding)[self rejectSelector:_cmd];
 _status=MTLCommandBufferStatusCommitted;
 // Deliberately synchronous M4 queue: no dispatch worker or asynchronous promise.
 try {if(_rejected)throw std::runtime_error("unsupported command: GPU submission refused");if(clientMode==fuxi::ClientMode::census)throw std::runtime_error("census: no GPU submission");owner(_queue->_device)->backend->submit(_ops);_status=MTLCommandBufferStatusCompleted;}
 catch(const std::exception &e) {
  fprintf(stderr,"FUXI MTL: submit FAIL %s\n",e.what());_status=MTLCommandBufferStatusError;
  if(!_error)_error=[[NSError alloc] initWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
      userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:e.what()]}];
 }
 NSArray *handlers=[_handlers copy];[_handlers removeAllObjects];
 for(MTLCommandBufferHandler handler in handlers) handler((id)self);
 [handlers release];[_resources removeAllObjects];_ops.clear();
}
- (void)waitUntilCompleted {CENSUS();if(_status<MTLCommandBufferStatusCompleted){[self rejectSelector:_cmd];[self commit];}}
@end
@implementation FuxiBlit
- (void)rejectSelector:(SEL)s {unsupported(self,s);[_command rejectSelector:s];}
- (void)dealloc {[_command release];[_label release];[super dealloc];}
- (id)device {CENSUS();return _command->_queue->_device;}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {@try {CENSUS();[_label release];_label=[s copy];} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (FuxiBuffer *)validate:(id)b offset:(NSUInteger)o size:(NSUInteger)n {CENSUS();
 if(_ended || !_command->_encoding || _command->_status!=MTLCommandBufferStatusNotEnqueued) invalid(@"inactive encoder");
 if(![b isKindOfClass:[FuxiBuffer class]] || [b device]!=_command->_queue->_device) invalid(@"foreign buffer/device");
 FuxiBuffer *buffer=b;
 if(o>buffer->_buffer->length || n>buffer->_buffer->length-o || ((o|n)&3)) invalid(@"M4 transfers require in-bounds 4-byte aligned offset and length");
 return buffer;
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {@try {CENSUS();
 FuxiBuffer *dst=[self validate:b offset:r.location size:r.length];if(!r.length)return;
 [_command->_resources addObject:dst];_command->_ops.push_back({nullptr,dst->_buffer,0,r.location,r.length,v});
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)to size:(NSUInteger)n {@try {CENSUS();
 FuxiBuffer *src=[self validate:s offset:so size:n],*dst=[self validate:d offset:to size:n];
 if(src==dst && n && so<to+n && to<so+n) invalid(@"overlapping copy");
 if(!n)return;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];
 _command->_ops.push_back({src->_buffer,dst->_buffer,so,to,n,0});
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)copyFromTexture:(id)t sourceSlice:(NSUInteger)s sourceLevel:(NSUInteger)l sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z
 toBuffer:(id)b destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {@try {CENSUS();
 stage4Require("readback","ended",_ended,!_ended);stage4Require("readback","encoding",_command->_encoding,_command->_encoding);
 stage4Require("readback","commandStatus",_command->_status,_command->_status==MTLCommandBufferStatusNotEnqueued);
 if(!stage4Class("readback","buffer.class",b,[FuxiBuffer class]))invalid(@"readback buffer.class");
 stage4Require("readback","buffer.device",(uint64_t)[b device],[b device]==_command->_queue->_device);
 FuxiBuffer *dst=b;
 stage4Require("readback","destinationOffset",offset,offset<=dst->_buffer->length);
 stage4Require("readback","buffer.length",dst->_buffer->length,[t isKindOfClass:[FuxiTexture class]] && dst->_buffer->length-offset>=65536*fuxi::Backend::texelBytes((uint32_t)[t pixelFormat]));
#define CHECK(field,value,accepted) do {__typeof__(value) actual=(value);stage4Require("readback",field,(uint64_t)actual,(accepted));}while(0)
 if(!stage4Class("readback","texture.class",t,[FuxiTexture class]))invalid(@"readback texture.class");
 CHECK("texture.device",[t device],actual==_command->_queue->_device);
 CHECK("sourceSlice",s,!actual);CHECK("sourceLevel",l,!actual);CHECK("sourceOrigin.x",o.x,!actual);CHECK("sourceOrigin.y",o.y,!actual);CHECK("sourceOrigin.z",o.z,!actual);
 CHECK("sourceSize.width",z.width,actual==256);CHECK("sourceSize.height",z.height,actual==256);CHECK("sourceSize.depth",z.depth,actual==1);
 CHECK("destinationBytesPerRow",row,actual==256*fuxi::Backend::texelBytes((uint32_t)[t pixelFormat]));CHECK("destinationBytesPerImage",image,actual==65536*fuxi::Backend::texelBytes((uint32_t)[t pixelFormat]));
 CHECK("destinationOffset",offset,!(actual&255));
#undef CHECK

 fuxi::Operation op{};op.dst=dst->_buffer;op.to=offset;op.texture=((FuxiTexture *)t)->_texture;
 [_command->_resources addObject:t];[_command->_resources addObject:dst];_command->_ops.push_back(op);
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (FuxiTexture *)texture:(id)t slice:(NSUInteger)s level:(NSUInteger)l origin:(MTLOrigin)o size:(MTLSize)z {
 if(_ended || !_command->_encoding || _command->_status!=MTLCommandBufferStatusNotEnqueued)invalid(@"inactive blit encoder");
 if(![t isKindOfClass:[FuxiTexture class]] || [t device]!=[self device])invalid(@"foreign texture/device");
 if(s || l || o.x || o.y || o.z || z.width!=256 || z.height!=256 || z.depth!=1)invalid(@"unsupported texture copy region");return t;
}
- (void)copyFromBuffer:(id)b sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
 sourceSize:(MTLSize)z toTexture:(id)t destinationSlice:(NSUInteger)s destinationLevel:(NSUInteger)l destinationOrigin:(MTLOrigin)o {@try {CENSUS();
 FuxiTexture *dst=[self texture:t slice:s level:l origin:o size:z];size_t pixelBytes=fuxi::Backend::texelBytes((uint32_t)[dst pixelFormat]);
 FuxiBuffer *src=[self validate:b offset:offset size:65536*pixelBytes];
 if(row!=256*pixelBytes || image!=65536*pixelBytes || (offset&255))invalid(@"unsupported upload stride/alignment");
 fuxi::Operation op{};op.src=src->_buffer;op.from=offset;op.texture=dst->_texture;op.upload=true;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];_command->_ops.push_back(op);
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)copyFromTexture:(id)t sourceSlice:(NSUInteger)s sourceLevel:(NSUInteger)l sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z
 toTexture:(id)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dest {@try {CENSUS();
 FuxiTexture *src=[self texture:t slice:s level:l origin:o size:z],*dst=[self texture:d slice:ds level:dl origin:dest size:z];
 if([src pixelFormat]!=[dst pixelFormat] || src==dst || (src->_surface && dst->_surface && IOSurfaceGetID(src->_surface)==IOSurfaceGetID(dst->_surface)))invalid(@"aliased texture copy");
 fuxi::Operation op{};op.sourceTexture=src->_texture;op.texture=dst->_texture;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];_command->_ops.push_back(op);
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)endEncoding {@try {CENSUS();if(_ended)invalid(@"endEncoding twice");_ended=YES;_command->_encoding=NO;} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
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
 // CA's documented late library/pipeline failures can abort, rather than fall
 // back. Refuse discovery on the current no-import kernel even after a root
 // allow-list edit; no partially usable CA device enters Metal's device cache.
 if((!strcmp(clientProcess,"backboardd") || !strcmp(clientProcess,"SpringBoard")) && !state->backend->supportsHostImport()) {
  unsupported(self,@selector(newTextureWithDescriptor:iosurface:plane:));
  fprintf(stderr,"FUXI MTL: CA preflight deny IOSurface HostImport unavailable\n");[state release];[self release];return nil;
 }
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
- (NSString *)name {CENSUS();return @"Fuxi Turnip Adreno 740 (M7 bounded offscreen)";}
- (uint64_t)registryID {CENSUS();return owner(self)->registry;}
- (NSUInteger)maxBufferLength {CENSUS();return fuxi::Backend::maxBufferLength;}
- (BOOL)hasUnifiedMemory {CENSUS();return YES;}
- (BOOL)isLowPower {CENSUS();return NO;}
- (BOOL)isHeadless {CENSUS();return YES;}
- (BOOL)isRemovable {CENSUS();return NO;}
- (BOOL)supportsFeatureSet:(NSUInteger)s {CENSUS();(void)s;return NO;}
- (BOOL)supportsFamily:(MTLGPUFamily)f {CENSUS();(void)f;return NO;}
- (BOOL)supportsTextureSampleCount:(NSUInteger)n {CENSUS();return n==1;}
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
// Private scalar queries established by the repo's QuartzCore call-site notes.
// These are limits of this bounded path, never an Apple family emulation table.
- (BOOL)supportsMemorylessRenderTargets {CENSUS();return NO;}
- (NSUInteger)minConstantBufferAlignmentBytes {CENSUS();return 256;}
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes {CENSUS();return 256;}
- (NSUInteger)maxFragmentTextures {CENSUS();return 4;}
- (NSUInteger)maxFragmentSamplers {CENSUS();return 4;}
- (NSUInteger)maxTextureWidth2D {CENSUS();return 256;}
- (NSUInteger)maxTextureHeight2D {CENSUS();return 256;}
- (NSUInteger)maxColorAttachments {CENSUS();return 1;}
- (NSString *)vendorName {CENSUS();return @"Qualcomm";}
- (BOOL)supportsYCBCRFormats {CENSUS();return NO;}
- (BOOL)supportsYCBCRFormats12 {CENSUS();return NO;}
- (BOOL)supportsNativeHardwareFP16 {CENSUS();return NO;}
- (BOOL)supportsTileShaders {CENSUS();return NO;}
- (BOOL)supportsSIMDGroup {CENSUS();return NO;}
- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n {CENSUS();
 if(!n || n>64) {unsupported(self,_cmd);return nil;}return [self newCommandQueue];
}
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
 CHECK("textureType",d.textureType,actual==MTLTextureType2D);CHECK("pixelFormat",d.pixelFormat,actual==80 || actual==70 || actual==115 || actual==10 || actual==1 || actual==30);
 CHECK("width",d.width,actual==256);CHECK("height",d.height,actual==256);CHECK("depth",d.depth,actual==1);
 CHECK("mipmapLevelCount",d.mipmapLevelCount,actual==1);CHECK("arrayLength",d.arrayLength,actual==1);CHECK("sampleCount",d.sampleCount,actual==1);CHECK("swizzle.identity",identitySwizzle(d.swizzle),actual);
 CHECK("storageMode",d.storageMode,actual==MTLStorageModePrivate);CHECK("cpuCacheMode",d.cpuCacheMode,actual==MTLCPUCacheModeDefaultCache);
 CHECK("hazardTrackingMode",d.hazardTrackingMode,actual==MTLHazardTrackingModeDefault || actual==MTLHazardTrackingModeTracked);
 CHECK("usage",d.usage,actual && !(actual&~(MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead)));
 // allowGPUOptimizedContents is an implementation hint, not pixel semantics.
#undef CHECK
 FuxiTexture *t=[FuxiTexture new];t->_device=[self retain];t->_usage=d.usage;
 try {t->_texture=owner(self)->backend->allocateTexture((uint32_t)d.pixelFormat,(uint32_t)d.usage);}catch(const std::exception &e) {fprintf(stderr,"FUXI MTL: texture FAIL %s\n",e.what());[t release];return nil;}return t;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {CENSUS();
 if(clientMode==fuxi::ClientMode::census)return nil;
 // Scope is deliberately one BGRA plane, 256 square, no external producer/fence.
 if(!owner(self)->backend->supportsHostImport()) {
  unsupported(self,_cmd);fprintf(stderr,"FUXI MTL: IOSurface import FAIL NativeAdreno HostImport / VK_EXT_external_memory_host unavailable\n");return nil;
 }
 if(!d || !surface || plane || IOSurfaceGetPlaneCount(surface)!=0 || IOSurfaceGetPixelFormat(surface)!=0x42475241 ||
    IOSurfaceGetWidth(surface)!=256 || IOSurfaceGetHeight(surface)!=256 || IOSurfaceGetBytesPerElement(surface)!=4 ||
    d.width!=256 || d.height!=256 || d.depth!=1 || d.textureType!=MTLTextureType2D || d.pixelFormat!=MTLPixelFormatBGRA8Unorm ||
    d.mipmapLevelCount!=1 || d.arrayLength!=1 || d.sampleCount!=1 || !identitySwizzle(d.swizzle) || d.storageMode!=MTLStorageModeShared ||
    d.cpuCacheMode!=MTLCPUCacheModeDefaultCache || (d.hazardTrackingMode!=MTLHazardTrackingModeDefault && d.hazardTrackingMode!=MTLHazardTrackingModeTracked) ||
    !d.usage || (d.usage&~(MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget))) {unsupported(self,_cmd);return nil;}
 if(IOSurfaceLock(surface,kIOSurfaceLockAvoidSync,nullptr)!=0) {unsupported(self,_cmd);return nil;}
 FuxiTexture *t=[FuxiTexture new];t->_device=[self retain];t->_usage=d.usage;
 t->_surface=(IOSurfaceRef)CFRetain(surface);IOSurfaceIncrementUseCount(surface);
 try {t->_texture=owner(self)->backend->importTexture(IOSurfaceGetBaseAddress(surface),IOSurfaceGetAllocSize(surface),IOSurfaceGetBytesPerRow(surface));}
 catch(const std::exception &e) {unsupported(self,_cmd);fprintf(stderr,"FUXI MTL: IOSurface import FAIL %s\n",e.what());[t release];return nil;}
 fprintf(stderr,"FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory\n");return t;
}
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {CENSUS();
 if(clientMode==fuxi::ClientMode::census)return nil;
 if(!d || d.minFilter>MTLSamplerMinMagFilterLinear || d.magFilter>MTLSamplerMinMagFilterLinear ||
    d.mipFilter!=MTLSamplerMipFilterNotMipmapped || d.sAddressMode!=MTLSamplerAddressModeClampToEdge ||
    d.tAddressMode!=MTLSamplerAddressModeClampToEdge || d.rAddressMode!=MTLSamplerAddressModeClampToEdge ||
    !d.normalizedCoordinates || d.maxAnisotropy!=1 || d.compareFunction!=MTLCompareFunctionNever ||
    d.lodMinClamp!=0 || d.lodMaxClamp!=FLT_MAX || d.lodAverage || d.supportArgumentBuffers) {unsupported(self,_cmd);return nil;}
 FuxiSampler *state=[FuxiSampler new];state->_device=[self retain];state->_label=[d.label copy];
 try {state->_sampler=owner(self)->backend->allocateSampler(d.minFilter==MTLSamplerMinMagFilterLinear,d.magFilter==MTLSamplerMinMagFilterLinear);}
 catch(const std::exception &e) {unsupported(self,_cmd);fprintf(stderr,"FUXI MTL: sampler FAIL %s\n",e.what());[state release];return nil;}return state;
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {CENSUS();
 if(error)*error=nil;if(clientMode==fuxi::ClientMode::census)return nil;
 if(NSString *field=pipelineRejection(d,self))return metalError(error,field.UTF8String);
 id v=d.vertexFunction,f=d.fragmentFunction;
 FuxiPipeline *p=[FuxiPipeline new];p->_device=[self retain];
 try {auto a=(FuxiFunction *)v,b=(FuxiFunction *)f;fuxi::CompileOptions o{};auto color=d.colorAttachments[0];
  o.format=(uint32_t)color.pixelFormat;o.blend=color.blendingEnabled;o.mask=(uint32_t)color.writeMask;
  o.srcRGB=(uint32_t)color.sourceRGBBlendFactor;o.dstRGB=(uint32_t)color.destinationRGBBlendFactor;o.rgbOp=(uint32_t)color.rgbBlendOperation;
  o.srcAlpha=(uint32_t)color.sourceAlphaBlendFactor;o.dstAlpha=(uint32_t)color.destinationAlphaBlendFactor;o.alphaOp=(uint32_t)color.alphaBlendOperation;
  for(unsigned i=0;i<8;++i){auto attr=d.vertexDescriptor.attributes[i];o.attributes[i]={(uint32_t)attr.format,(uint32_t)attr.offset,(uint32_t)attr.bufferIndex};}
  for(unsigned i=0;i<4;++i)o.strides[i]=(uint32_t)d.vertexDescriptor.layouts[i].stride;
  NSData *constants[2]={a->_constants,b->_constants};for(unsigned i=0;i<2;++i){o.constantCount[i]=(uint32_t)(constants[i].length/sizeof(fuxi::Constant));if(o.constantCount[i]>32)throw std::runtime_error("constants bound");if(constants[i].length)memcpy(o.constants[i],constants[i].bytes,constants[i].length);}
  p->_pipeline=owner(self)->backend->compilePipeline(a->_library->_library,a->_name.UTF8String,b->_library->_library,b->_name.UTF8String,o);}
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
      {[FuxiSampler class],"MTLSamplerState"},{[FuxiTexture class],"MTLTexture"},{[FuxiLibrary class],"MTLLibrary"},{[FuxiFunction class],"MTLFunction"},{[FuxiPipeline class],"MTLRenderPipelineState"}}) {
  Protocol *p=objc_getProtocol(entry.second);rejectProtocol(entry.first,p);if(p)class_addProtocol(entry.first,p);
 }
}
