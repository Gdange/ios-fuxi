/* Bounded Metal diagnostic. No Apple code. CA resource/queue/encoder adapters adapted
 * from UniXNU NativeMetal, MIT, Copyright (c) 2026 NativeMetal contributors.
 * See LICENSE.NativeMetal and docs-lanes/metal-m9e.md for source provenance. */
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
#include <cstdarg>
#include <chrono>
#include "ConstantHooks.hpp"
#include <cmath>
#include <cfloat>
#include <fcntl.h>
#include <sys/stat.h>
#include "ClientGate.hpp"
#include "Translator.hpp"
#include "CAState.hpp"
#include "Timeline.hpp"
#include "VertexProfile.hpp"
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
static bool caTrace() {return !strcmp(clientProcess,"fuxi-ca-smoke");}
static void censusLog(id object,SEL selector,bool implemented) {
 // The isolated CA smoke also traces each class/selector's first use (diagnostic only).
 if(clientMode!=fuxi::ClientMode::census && !caTrace())return;
 Class cls=object_getClass(object);std::lock_guard<std::mutex> lock(censusLock);
 for(size_t i=0;i<censusCount;++i)if(censusEntries[i].cls==cls && censusEntries[i].selector==selector)return;
 if(censusCount==256)return;
 censusEntries[censusCount++]={cls,selector};
 fprintf(stderr,"FUXI MTL: %s %s %s %s %s\n",clientMode==fuxi::ClientMode::census?"census":"trace",clientProcess,class_getName(cls),sel_getName(selector),implemented?"implemented":"unsupported");
 if(censusCount==256)fprintf(stderr,"FUXI MTL: census limit %s 256\n",clientProcess);
}
#define CENSUS() censusLog(self,_cmd,true)
// Diagnostic-only process counters. A client can refuse an entire candidate
// after CA swallowed a nil/error; these counters never grant client admission.
static std::atomic<uint64_t> failureCount{0},completedDraws{0};
extern "C" uint64_t fuxi_metal_failure_count() {return failureCount.load();}
extern "C" uint64_t fuxi_metal_completed_draws() {return completedDraws.load();}
extern "C" uint64_t fuxi_metal_pipeline_cache_hits() {return fuxi::Backend::pipelineHits.load();}
extern "C" uint64_t fuxi_metal_pipeline_cache_misses() {return fuxi::Backend::pipelineMisses.load();}
extern "C" uint64_t fuxi_metal_gpu_nanoseconds() {return fuxi::Backend::gpuNanos.load();}
extern "C" uint64_t fuxi_metal_gpu_samples() {return fuxi::Backend::gpuSamples.load();}
static void failureLog(const char *format,...) {
 uint64_t n=failureCount.fetch_add(1);if(n>=64)return;
 va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);
 if(n==63)fprintf(stderr,"FUXI MTL: failure log limit 64 (counter continues)\n");
}
static void unsupported(id object, SEL selector) {
 if(clientMode==fuxi::ClientMode::census) {censusLog(object,selector,false);return;}
 failureLog("FUXI MTL: unsupported %.96s %.160s\n",class_getName(object_getClass(object)),sel_getName(selector));
}
static void invalid(NSString *why) {
 failureLog("FUXI MTL: invalid %s\n",why.UTF8String);
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
// Resource/command metadata only. Keep ivars out of FuxiObject: its methods
// are also copied onto the runtime device subclass, whose layout is private.
@interface FuxiLabeledObject : FuxiObject { @public NSString *_label; }
- (NSString *)label;
- (void)setLabel:(NSString *)s;
@end
@implementation FuxiLabeledObject
- (void)dealloc {[_label release];[super dealloc];}
- (void)insertDebugSignpost:(NSString *)s {CENSUS();(void)s;}
- (void)pushDebugGroup:(NSString *)s {CENSUS();(void)s;}
- (void)popDebugGroup {CENSUS();}
- (NSString *)label {CENSUS();return _label;}
- (void)setLabel:(NSString *)s {CENSUS();NSString *next=[s copy];[_label release];_label=next;}
@end
@interface FuxiOwner : NSObject { @public fuxi::Backend *backend; uint64_t registry; }
@end
@implementation FuxiOwner
- (void)dealloc {delete backend;[super dealloc];}
@end
static char ownerKey;
static FuxiOwner *owner(id obj) {return (FuxiOwner *)objc_getAssociatedObject(obj,&ownerKey);}
// NativeMetal resource SPI: allocations remain resident; purgeability never
// discards live storage, and owner identity is explicitly not applied.
@protocol FuxiHeapBudget <NSObject>
- (void)fuxiRelinquishBytes:(NSUInteger)bytes;
@end
@interface FuxiResource : FuxiLabeledObject { @public id _device;id<FuxiHeapBudget> _parentHeap;NSUInteger _heapBytes;MTLPurgeableState _purgeable; }
@end
@implementation FuxiResource
- (void)dealloc {[_parentHeap fuxiRelinquishBytes:_heapBytes];[_parentHeap release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (NSUInteger)protectionOptions {CENSUS();return 0;}
- (id)heap {CENSUS();return _parentHeap;}
- (BOOL)isAliasable {CENSUS();return NO;}
- (int)setOwnerWithIdentity:(unsigned)identity {CENSUS();(void)identity;return 46;}
- (void)waitUntilComplete {CENSUS();} // Backend submissions complete on the caller.
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {CENSUS();
 if(state<MTLPurgeableStateKeepCurrent || state>MTLPurgeableStateEmpty){unsupported(self,_cmd);return (MTLPurgeableState)0;}
 if(_parentHeap || [(id)self allocatedSize]<16384)return MTLPurgeableStateNonVolatile;
 @synchronized(self){MTLPurgeableState before=_purgeable?(MTLPurgeableState)_purgeable:MTLPurgeableStateNonVolatile;
  if(state!=MTLPurgeableStateKeepCurrent)_purgeable=state;return before;}
}
@end
@interface FuxiBuffer : FuxiResource { @public fuxi::Buffer *_buffer;void *_wrapped;NSUInteger _wrappedLength;void (^_deallocator)(void *,NSUInteger);BOOL _shadow; }
@end
@implementation FuxiBuffer
- (void)dealloc {auto *backend=owner(_device)->backend;backend->destroy(_buffer);
 if(!backend->poisoned){if(_deallocator)_deallocator(_wrapped,_wrappedLength);[_deallocator release];}
 // On uncertain ownership retain the no-copy deallocator and its pages.
 [_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (void *)contents {CENSUS();return _shadow?_wrapped:_buffer->contents;}
- (NSUInteger)length {CENSUS();return _buffer->length;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {CENSUS();
 MTLPurgeableState before=[super setPurgeableState:state];
 if(state==MTLPurgeableStateEmpty && !_parentHeap && _buffer->allocated>=16384){
  if(owner(_device)->backend->poisoned)invalid(@"purge after uncertain GPU ownership");memset(_buffer->contents,0,_buffer->length);
 }return before;
}
- (void)didModifyRange:(NSRange)r {CENSUS();if(r.location>[self length] || r.length>[self length]-r.location)invalid(@"buffer modified range");}
- (IOSurfaceRef)iosurface {CENSUS();return nullptr;}
- (void)getBytes:(void *)bytes range:(NSRange)r {CENSUS();
 if(!bytes || r.location>[self length] || r.length>[self length]-r.location)invalid(@"buffer getBytes range");
 memcpy(bytes,(uint8_t *)_buffer->contents+r.location,r.length);
}
- (MTLStorageMode)storageMode {CENSUS();return MTLStorageModeShared;}
- (MTLCPUCacheMode)cpuCacheMode {CENSUS();return MTLCPUCacheModeDefaultCache;}
- (MTLHazardTrackingMode)hazardTrackingMode {CENSUS();return MTLHazardTrackingModeTracked;}
- (MTLResourceOptions)resourceOptions {CENSUS();return MTLResourceStorageModeShared;}
- (NSUInteger)allocatedSize {CENSUS();return _buffer->allocated;}
@end
static id metalError(NSError **error,const char *why) {
 failureLog("FUXI MTL: pipeline/library FAIL %.768s\n",why);
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
static size_t constantSize(MTLDataType t) {return fuxi::constantPayloadSize((uint32_t)t);}
static bool constantsObserved(id self) {
 uint64_t stamp=constantHooks().stamp(object_getClass(self));
 id allocation=objc_getAssociatedObject(self,&constantsAllocationKey);
 return stamp && [allocation isKindOfClass:[NSNumber class]] && [allocation unsignedLongLongValue]==stamp;
}
static void rememberConstant(id self,const void *value,MTLDataType type,id key) {
 id old=objc_getAssociatedObject(self,&constantsKey);size_t size=constantSize(type);
 if(!constantsObserved(self) || ![old isKindOfClass:[NSDictionary class]] || !value || !size || !key || ([old count]>=fuxi::maxFunctionConstants && !old[key])) {failureLog("FUXI MTL: constant reject type=%u bytes=%zu records=%lu\n",(unsigned)type,size,[old isKindOfClass:[NSDictionary class]]?(unsigned long)[old count]:0UL);objc_setAssociatedObject(self,&constantsKey,@NO,OBJC_ASSOCIATION_RETAIN_NONATOMIC);return;}
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
  if(!constantsObserved(self) || !size || range.length>fuxi::maxFunctionConstants || range.location>65535 || range.length>65536-range.location || !value){failureLog("FUXI MTL: constant range reject type=%u location=%lu length=%lu\n",(unsigned)type,(unsigned long)range.location,(unsigned long)range.length);objc_setAssociatedObject(self,&constantsKey,@NO,OBJC_ASSOCIATION_RETAIN_NONATOMIC);return;}
  for(NSUInteger i=0;i<range.length;++i)rememberConstant(self,(const uint8_t *)value+i*fuxi::constantElementStride((uint32_t)type),type,@(range.location+i));
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
 if(!strcmp(clientProcess,"fuxi-ca-smoke")) {
  failureLog("FUXI MTL: constants snapshot FAIL reason=%.96s class=%.96s\n",reason,values?object_getClassName(values):"nil");return nil;
 }
 failureCount.fetch_add(1);
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
 if(![records isKindOfClass:[NSDictionary class]] || [records count]>fuxi::maxFunctionConstants)return constantFailure(values,"unsupported-record");
 NSMutableData *data=[NSMutableData data];
 // Deterministic order makes equal constants share the translator cache.
 NSArray *keys=[[records allKeys] sortedArrayUsingComparator:^NSComparisonResult(id a,id b){return [[a description] compare:[b description]];}];
 for(id key in keys){NSArray *record=records[key];fuxi::Constant c{};c.type=[record[0] unsignedIntValue];NSData *bytes=record[1];c.size=(uint32_t)bytes.length;memcpy(c.bytes,bytes.bytes,bytes.length);
 if([key isKindOfClass:[NSNumber class]])c.index=[key unsignedIntValue];else {const char *name=[key UTF8String];if(!name || strlen(name)>=128)return constantFailure(values,"unsupported-name");strcpy(c.name,name);}
 [data appendBytes:&c length:sizeof(c)];}return data;}
}
@interface FuxiLibrary : FuxiLabeledObject { @public id _device; void *_library; }
- (id)fuxiNewFunctionWithName:(NSString *)name error:(NSError **)error;
@end
@interface FuxiFunction : FuxiLabeledObject { @public FuxiLibrary *_library; NSString *_name; NSData *_constants; int _stage; }
@end
@implementation FuxiFunction
- (void)dealloc {[_library release];[_name release];[_constants release];[super dealloc];}
- (id)device {CENSUS();return _library->_device;}
- (NSString *)name {CENSUS();return _name;}
- (MTLFunctionType)functionType {CENSUS();return _stage==1?MTLFunctionTypeVertex:MTLFunctionTypeFragment;}
- (MTLFunctionOptions)options {CENSUS();return MTLFunctionOptionNone;}
- (MTLFunctionDescriptor *)functionDescriptor {CENSUS();
 MTLFunctionDescriptor *d=[MTLFunctionDescriptor functionDescriptor];d.name=_name;
 if(_constants.length){MTLFunctionConstantValues *values=[MTLFunctionConstantValues new];
  const auto *constants=(const fuxi::Constant *)_constants.bytes;
  for(NSUInteger i=0;i<_constants.length/sizeof(fuxi::Constant);++i){const auto &c=constants[i];
   if(c.name[0])[values setConstantValue:c.bytes type:(MTLDataType)c.type withName:[NSString stringWithUTF8String:c.name]];
   else [values setConstantValue:c.bytes type:(MTLDataType)c.type atIndex:c.index];
  }d.constantValues=values;[values release];
 }return d;
}
@end
@implementation FuxiLibrary
- (MTLLibraryType)type {CENSUS();return MTLLibraryTypeExecutable;}
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
 FuxiFunction *f=[self fuxiNewFunctionWithName:name error:error];if(!f)return nil;
 f->_constants=[snapshot copy];return f;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {CENSUS();
 if(!handler){unsupported(self,_cmd);return;}NSError *error=nil;id function=[self newFunctionWithName:name constantValues:values error:&error];handler(function,error);[function release];
}
- (id)newFunctionWithName:(NSString *)name {CENSUS();
 return [self fuxiNewFunctionWithName:name error:nullptr];
}
- (id)fuxiNewFunctionWithName:(NSString *)name error:(NSError **)error {
 if(error)*error=nil;
 if(!name || !name.length || name.length>127 || !name.UTF8String || strlen(name.UTF8String)!=[name lengthOfBytesUsingEncoding:NSUTF8StringEncoding])return metalError(error,"invalid function name");
 // A lookup refusal returns nil/NSError, never a placeholder or a renamed
 // shader. CA itself retries then aborts on nil; see metal-m9c.md.
 char why[2048]{};int stage=0;
#ifndef FUXI_MTL_FUNCTION_LOOKUP
#define FUXI_MTL_FUNCTION_LOOKUP fuxi::Translator::get().function
#endif
 try {stage=FUXI_MTL_FUNCTION_LOOKUP(_library,name.UTF8String,why,sizeof(why));}
 catch(const std::exception &e) {return metalError(error,e.what());}
 if(!stage)return metalError(error,why[0]?why:"function lookup failed");
 FuxiFunction *f=[FuxiFunction new];f->_library=[self retain];f->_name=[name copy];f->_stage=stage;
 fprintf(stderr,"FUXI MTL: function %s class=%s stage=%d device=%u\n",name.UTF8String,object_getClassName(f),stage,[f device]==_device);return f;
}
@end
// Log the public value that failed, without inspecting Apple's object layout.
static bool stage4Value(const char *scope,const char *field,uint64_t value,bool accepted) {
 if(!accepted)failureLog("FUXI MTL: %s reject %s %llu\n",scope,field,(unsigned long long)value);
 return accepted;
}
static bool stage4Class(const char *scope,const char *field,id object,Class expected) {
 if([object isKindOfClass:expected])return true;
 failureLog("FUXI MTL: %s reject %s %s\n",scope,field,object?object_getClassName(object):"nil");return false;
}
static void stage4Require(const char *scope,const char *field,uint64_t value,bool accepted) {
 if(!stage4Value(scope,field,value,accepted))invalid([NSString stringWithFormat:@"%s %s",scope,field]);
}
static NSString *vertexAttributeText(MTLVertexDescriptor *vd,unsigned index) {
 auto a=vd.attributes[index];
 // Invalid buffer indices must never become an indexed SDK access.
 MTLVertexBufferLayoutDescriptor *l=a.bufferIndex<31?vd.layouts[a.bufferIndex]:nil;
 return [NSString stringWithFormat:@"attribute[%u] format=%lu(%s) bufferIndex=%lu offset=%lu stride=%lu stepFunction=%lu stepRate=%lu layoutPresent=%u",index,
  (unsigned long)a.format,fuxi::vertexFormatInfo(a.format).name,(unsigned long)a.bufferIndex,(unsigned long)a.offset,
  (unsigned long)l.stride,(unsigned long)l.stepFunction,(unsigned long)l.stepRate,l!=nil];
}
static NSString *vertexDescriptorText(MTLVertexDescriptor *vd) {
 // At most 31 public attributes/layouts; only configured entries, including
 // nondefault unused state, are emitted. No object descriptions or pointers.
 NSMutableString *text=[NSMutableString stringWithString:@"attributes={"];
 bool referenced[31]{};bool first=true;
 if(vd)for(unsigned i=0;i<31;++i){auto a=vd.attributes[i];
  if(a.format!=MTLVertexFormatInvalid && a.bufferIndex<31)referenced[a.bufferIndex]=true;
  if(a.format==MTLVertexFormatInvalid && !a.offset && !a.bufferIndex)continue;
  [text appendFormat:@"%s%@",first?"":";",vertexAttributeText(vd,i)];first=false;
 }
 [text appendString:@"} layouts={"];first=true;
 if(vd)for(unsigned i=0;i<31;++i){auto l=vd.layouts[i];
  if(!referenced[i] && !l.stride && l.stepFunction==MTLVertexStepFunctionPerVertex && l.stepRate==1)continue;
  [text appendFormat:@"%s%u:stride=%lu/stepFunction=%lu/stepRate=%lu",first?"":";",i,(unsigned long)l.stride,(unsigned long)l.stepFunction,(unsigned long)l.stepRate];first=false;
 }
 [text appendString:@"}"];return text;
}
static NSString *vertexDescriptorRejection(MTLVertexDescriptor *vd) {
 bool referenced[31]{};
 if(vd)for(unsigned i=0;i<31;++i){auto a=vd.attributes[i];
  if(a.format==MTLVertexFormatInvalid)continue;
  if(const char *why=fuxi::vertexAttributeRejection(i,a.format,a.bufferIndex,a.offset))
   return [NSString stringWithFormat:@"vertexDescriptor %@ reason=%s",vertexAttributeText(vd,i),why];
  referenced[a.bufferIndex]=true;
 }
 if(vd)for(unsigned i=0;i<31;++i){auto l=vd.layouts[i];
  if(!referenced[i] && !l.stride && l.stepFunction==MTLVertexStepFunctionPerVertex && l.stepRate==1)continue;
  if(const char *why=fuxi::vertexLayoutRejection(i,l.stride,l.stepFunction,l.stepRate,referenced[i])) {
   NSString *attribute=@"attribute=none";
   for(unsigned j=0;j<31;++j)if(vd.attributes[j].format!=MTLVertexFormatInvalid && vd.attributes[j].bufferIndex==i){attribute=vertexAttributeText(vd,j);break;}
   return [NSString stringWithFormat:@"vertexDescriptor layout[%u] stride=%lu stepFunction=%lu stepRate=%lu %@ reason=%s",i,(unsigned long)l.stride,(unsigned long)l.stepFunction,(unsigned long)l.stepRate,attribute,why];
  }
 }
 return nil;
}
static NSString *pipelineRejection(MTLRenderPipelineDescriptor *d,id device) {
 id v=d.vertexFunction,f=d.fragmentFunction;MTLVertexDescriptor *vd=d.vertexDescriptor;
 bool vertex=[v isKindOfClass:[FuxiFunction class]],fragment=[f isKindOfClass:[FuxiFunction class]];
 int vs=vertex?((FuxiFunction *)v)->_stage:0,fs=fragment?((FuxiFunction *)f)->_stage:0;
 auto vl=d.vertexLinkedFunctions,fl=d.fragmentLinkedFunctions;
 char colors[640]{};size_t used=0;
 for(unsigned i=0;i<8;++i) {auto a=d.colorAttachments[i];used+=(size_t)snprintf(colors+used,sizeof(colors)-used,"%s%u:%lu/%u/%lu",i?",":"",i,(unsigned long)a.pixelFormat,a.blendingEnabled,(unsigned long)a.writeMask);}
 static std::atomic<unsigned> descriptorLogs{0};
 if(descriptorLogs.fetch_add(1)<64)fprintf(stderr,"FUXI MTL: pipeline descriptor present=%u vertex=%s/stage%d/device%u fragment=%s/stage%d/device%u vertexDescriptor=%u samples=%lu alphaCoverage=%u alphaOne=%u raster=%u depth=%lu stencil=%lu topology=%lu tessFormat=%lu tessMax=%lu indirect=%u amplification=%lu vertexLinked=%u/%lu/%lu/%lu/%lu fragmentLinked=%u/%lu/%lu/%lu/%lu colors(format/blend/mask)=%s %s\n",
  d!=nil,v?object_getClassName(v):"nil",vs,vertex && [v device]==device,f?object_getClassName(f):"nil",fs,fragment && [f device]==device,vd!=nil,
  (unsigned long)d.rasterSampleCount,d.alphaToCoverageEnabled,d.alphaToOneEnabled,d.rasterizationEnabled,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat,
  (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.tessellationFactorFormat,(unsigned long)d.maxTessellationFactor,d.supportIndirectCommandBuffers,(unsigned long)d.maxVertexAmplificationCount,
  vl!=nil,(unsigned long)vl.functions.count,(unsigned long)vl.binaryFunctions.count,(unsigned long)vl.privateFunctions.count,(unsigned long)vl.groups.count,
  fl!=nil,(unsigned long)fl.functions.count,(unsigned long)fl.binaryFunctions.count,(unsigned long)fl.privateFunctions.count,(unsigned long)fl.groups.count,colors,vertexDescriptorText(vd).UTF8String);
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
 // ABI 6 transmits exact formats/offsets/buffer slots/strides, per-vertex only.
 if(NSString *why=vertexDescriptorRejection(vd))return why;
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
@interface FuxiTexture : FuxiResource { @public fuxi::Texture *_texture; MTLTextureUsage _usage; IOSurfaceRef _surface; BOOL _shared;FuxiTexture *_viewParent;FuxiBuffer *_backingBuffer;NSUInteger _backingOffset,_backingRow; }
@end
@implementation FuxiTexture
- (void)dealloc {
 bool safeToUnlock=_viewParent?true:owner(_device)->backend->destroy(_texture);
 // A timeout leaves ownership unknown. Preserve the locked surface/pages until
 // process teardown, just as Backend preserves Vulkan allocations on poison.
 if(_surface && safeToUnlock) {IOSurfaceUnlock(_surface,0,nullptr);IOSurfaceDecrementUseCount(_surface);CFRelease(_surface);}
 [_viewParent release];[_backingBuffer release];[_device release];[super dealloc];
}
- (id)device {CENSUS();return _device;}
- (NSUInteger)width {CENSUS();return _texture->width;}
- (NSUInteger)height {CENSUS();return _texture->height;}
- (NSUInteger)depth {CENSUS();return 1;}
- (NSUInteger)arrayLength {CENSUS();return 1;}
- (NSUInteger)protectionOptions {CENSUS();return 0;}
- (NSUInteger)mipmapLevelCount {CENSUS();return 1;}
- (NSUInteger)sampleCount {CENSUS();return 1;}
- (MTLTextureType)textureType {CENSUS();return MTLTextureType2D;}
- (MTLPixelFormat)pixelFormat {CENSUS();return (MTLPixelFormat)_texture->format;}
- (MTLStorageMode)storageMode {CENSUS();return (_surface || _shared)?MTLStorageModeShared:MTLStorageModePrivate;}
- (MTLTextureUsage)usage {CENSUS();return _usage;}
- (IOSurfaceRef)iosurface {CENSUS();return _viewParent?[_viewParent iosurface]:_surface;}
- (NSUInteger)iosurfacePlane {CENSUS();return 0;}
- (MTLTextureSwizzleChannels)swizzle {CENSUS();return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha);}
- (MTLCPUCacheMode)cpuCacheMode {CENSUS();return MTLCPUCacheModeDefaultCache;}
- (MTLHazardTrackingMode)hazardTrackingMode {CENSUS();return MTLHazardTrackingModeTracked;}
- (NSUInteger)allocatedSize {CENSUS();return _texture->allocated;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {CENSUS();
 if(state<MTLPurgeableStateKeepCurrent || state>MTLPurgeableStateEmpty){unsupported(self,_cmd);return (MTLPurgeableState)0;}
 if(_viewParent || _parentHeap)return MTLPurgeableStateNonVolatile;
 if(_backingBuffer)return [_backingBuffer setPurgeableState:state];return [super setPurgeableState:state];
}
- (BOOL)isFramebufferOnly {CENSUS();return NO;}
- (MTLResourceOptions)resourceOptions {CENSUS();return (MTLResourceOptions)([self storageMode]<<MTLResourceStorageModeShift);}
- (id)parentTexture {CENSUS();return _viewParent;}
- (id)buffer {CENSUS();return _viewParent?[_viewParent buffer]:_backingBuffer;}
- (NSUInteger)bufferOffset {CENSUS();return _viewParent?[_viewParent bufferOffset]:_backingOffset;}
- (NSUInteger)bufferBytesPerRow {CENSUS();return _viewParent?[_viewParent bufferBytesPerRow]:_backingRow;}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)format {CENSUS();
 if(format!=[self pixelFormat]){unsupported(self,_cmd);return nil;}
 FuxiTexture *view=[FuxiTexture new];view->_device=[_device retain];view->_texture=_texture;
 view->_usage=_usage;view->_shared=[self storageMode]==MTLStorageModeShared;view->_viewParent=[self retain];return view;
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {CENSUS();
 if(type!=MTLTextureType2D || levels.location || levels.length!=1 || slices.location || slices.length!=1){unsupported(self,_cmd);return nil;}
 return [self newTextureViewWithPixelFormat:format];
}
- (void)transfer:(void *)bytes row:(NSUInteger)row image:(NSUInteger)image region:(MTLRegion)r level:(NSUInteger)level slice:(NSUInteger)slice write:(BOOL)write {CENSUS();
 size_t pixel=fuxi::Backend::texelBytes((uint32_t)[self pixelFormat]),width=[self width],height=[self height];
 if(!bytes || level || slice || r.origin.z || r.size.depth!=1 || !r.size.width || !r.size.height ||
    r.origin.x>width || r.size.width>width-r.origin.x || r.origin.y>height || r.size.height>height-r.origin.y ||
    row<r.size.width*pixel || row>SIZE_MAX/r.size.height || (image && image<row*r.size.height))invalid(@"texture CPU region/stride");
 auto *backend=owner(_device)->backend;fuxi::Buffer *staging=nullptr;
 try {
  staging=backend->allocate(width*height*pixel);fuxi::Operation op{};op.texture=_texture;
  bool full=!r.origin.x && !r.origin.y && r.size.width==width && r.size.height==height;
  if(!write || (!full && _texture->defined)){op.dst=staging;backend->submit({op});}
  if(write){for(size_t y=0;y<r.size.height;++y)memcpy((uint8_t *)staging->contents+((r.origin.y+y)*width+r.origin.x)*pixel,(uint8_t *)bytes+y*row,r.size.width*pixel);
   op={};op.texture=_texture;op.src=staging;op.upload=true;backend->submit({op});
  }else for(size_t y=0;y<r.size.height;++y)memcpy((uint8_t *)bytes+y*row,(uint8_t *)staging->contents+((r.origin.y+y)*width+r.origin.x)*pixel,r.size.width*pixel);
  backend->destroy(staging);
 }catch(const std::exception &e){backend->destroy(staging);invalid([NSString stringWithUTF8String:e.what()]);}
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 if(caTrace())fprintf(stderr,"FUXI MTL: trace replaceRegion %lu,%lu %lux%lu row %lu format %lu\n",(unsigned long)r.origin.x,(unsigned long)r.origin.y,(unsigned long)r.size.width,(unsigned long)r.size.height,(unsigned long)row,(unsigned long)[self pixelFormat]);
 [self transfer:const_cast<void *>(bytes) row:row image:image region:r level:level slice:slice write:YES];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
 [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
 [self transfer:bytes row:row image:image region:r level:level slice:slice write:NO];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
 [self getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}
@end
@implementation FuxiBuffer (CATextures)
- (id)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {CENSUS();
 if(caTrace() && d)fprintf(stderr,"FUXI MTL: trace linearTexture %lux%lu offset %lu row %lu image %lu format %lu usage %lu length %lu\n",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)offset,(unsigned long)row,(unsigned long)image,(unsigned long)d.pixelFormat,(unsigned long)d.usage,(unsigned long)_wrappedLength);
 if(!d || d.textureType!=MTLTextureType2D || d.storageMode!=MTLStorageModeShared || d.cpuCacheMode!=MTLCPUCacheModeDefaultCache ||
    (d.hazardTrackingMode!=MTLHazardTrackingModeDefault && d.hazardTrackingMode!=MTLHazardTrackingModeTracked) ||
    d.depth!=1 || d.arrayLength!=1 || d.mipmapLevelCount!=1 || d.sampleCount!=1 || !identitySwizzle(d.swizzle) || !row || !d.width || !d.height || d.width>owner(_device)->backend->maxTextureDimension() || d.height>owner(_device)->backend->maxTextureDimension() || (image && (d.height>SIZE_MAX/row || image<row*d.height))){unsupported(self,_cmd);return nil;}
 FuxiTexture *t=[FuxiTexture new];t->_device=[_device retain];t->_usage=d.usage;t->_shared=YES;
 try {t->_texture=owner(_device)->backend->linearTexture(_buffer,(uint32_t)d.pixelFormat,(uint32_t)d.usage,(uint32_t)d.width,(uint32_t)d.height,offset,row);}
 catch(const std::exception &e){failureLog("FUXI MTL: linear texture FAIL %s\n",e.what());[t release];return nil;}
 t->_backingBuffer=[self retain];t->_backingOffset=offset;t->_backingRow=row;return t;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row {
 return [self newLinearTextureWithDescriptor:d offset:offset bytesPerRow:row bytesPerImage:0];
}
@end
@interface FuxiPipeline : FuxiLabeledObject { @public id _device; fuxi::Pipeline *_pipeline; }
@end
@implementation FuxiPipeline
- (void)dealloc {owner(_device)->backend->destroy(_pipeline);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
@end
@interface FuxiSampler : FuxiLabeledObject { @public id _device;fuxi::Sampler *_sampler; }
@end
@implementation FuxiSampler
- (MTLPixelFormat)pixelFormat {CENSUS();return MTLPixelFormatInvalid;}
- (void)dealloc {owner(_device)->backend->destroy(_sampler);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
@end
// Only the neutral state is representable by our attachment-free backend.
@interface FuxiDepthStencil : FuxiLabeledObject { @public id _device; }
@end
@implementation FuxiDepthStencil
- (void)dealloc {[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
@end
// Automatic shared heaps are a resource budget; Vulkan allocations remain
// independent and nonaliasable. Placement/sparse heaps are not advertised.
@interface FuxiHeap : FuxiResource <FuxiHeapBudget> { @public NSUInteger _size,_used; }
@end
@implementation FuxiHeap
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {CENSUS();if(state<MTLPurgeableStateKeepCurrent || state>MTLPurgeableStateEmpty){unsupported(self,_cmd);return (MTLPurgeableState)0;}return MTLPurgeableStateNonVolatile;}
- (void)dealloc {[_device release];[super dealloc];}
- (NSUInteger)size {CENSUS();return _size;}
- (NSUInteger)usedSize {CENSUS();@synchronized(self){return _used;}}
- (NSUInteger)currentAllocatedSize {CENSUS();return [self usedSize];}
- (MTLStorageMode)storageMode {CENSUS();return MTLStorageModeShared;}
- (MTLCPUCacheMode)cpuCacheMode {CENSUS();return MTLCPUCacheModeDefaultCache;}
- (MTLHazardTrackingMode)hazardTrackingMode {CENSUS();return MTLHazardTrackingModeTracked;}
- (MTLResourceOptions)resourceOptions {CENSUS();return MTLResourceStorageModeShared;}
- (MTLHeapType)type {CENSUS();return MTLHeapTypeAutomatic;}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {CENSUS();
 if(!alignment || (alignment&(alignment-1)))return 0;@synchronized(self){return (_size-_used)&~(alignment-1);}
}
- (void)fuxiRelinquishBytes:(NSUInteger)bytes {@synchronized(self){_used-=bytes;}}
- (id)adopt:(FuxiResource *)resource bytes:(NSUInteger)bytes {
 if(!resource)return nil;@synchronized(self){if(bytes>_size-_used){[resource release];return nil;}
 _used+=bytes;resource->_parentHeap=[self retain];resource->_heapBytes=bytes;}return resource;
}
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {CENSUS();
 if(options!=MTLResourceStorageModeShared || length>[self maxAvailableSizeWithAlignment:1])return nil;
 FuxiBuffer *b=[_device newBufferWithLength:length options:options];return [self adopt:b bytes:b?b->_buffer->allocated:0];
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {CENSUS();
 if(!descriptor || descriptor.storageMode!=MTLStorageModeShared)return nil;
 FuxiTexture *t=[_device newTextureWithDescriptor:descriptor];return [self adopt:t bytes:t?t->_texture->allocated:0];
}
@end
// MIT NativeMetal NBSharedEvent listener forwarding, adapted to MRC and our
// synchronous completion. No shared event handle is fabricated.
@interface FuxiEventNotice : NSObject { @public dispatch_queue_t _queue;MTLSharedEventNotificationBlock _block;uint64_t _value; }
@end
@implementation FuxiEventNotice
- (void)dealloc {if(_queue)dispatch_release(_queue);[_block release];[super dealloc];}
@end
@interface FuxiEvent : FuxiLabeledObject { @public id _device;fuxi::Timeline _timeline;NSMutableArray *_notices;BOOL _shared; }
@end
@implementation FuxiEvent
- (id)device {CENSUS();return _device;}
- (void)dealloc {[_device release];[_notices release];[super dealloc];}
- (uint64_t)signaledValue {CENSUS();return _timeline.value();}
- (void)setSignaledValue:(uint64_t)value {CENSUS();
 @synchronized(self){_timeline.signal(value);uint64_t actual=_timeline.value();NSArray *notices=[_notices copy];
  for(FuxiEventNotice *notice in notices)if(notice->_value<=actual){
   [_notices removeObjectIdenticalTo:notice];dispatch_async(notice->_queue,^{notice->_block((id)self,actual);});
  }
  [notices release];}
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {CENSUS();return _timeline.wait(value,milliseconds);}
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {CENSUS();
 if(!_shared || !listener || !block){unsupported(self,_cmd);return;}
 FuxiEventNotice *notice=[FuxiEventNotice new];notice->_queue=listener.dispatchQueue?:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0);dispatch_retain(notice->_queue);notice->_block=[block copy];notice->_value=value;
 @synchronized(self){if(!_notices)_notices=[NSMutableArray new];
  if(_notices.count>=256){[notice release];unsupported(self,_cmd);return;}
  [_notices addObject:notice];[self setSignaledValue:_timeline.value()];}
 [notice release];
}
- (id)newSharedEventHandle {CENSUS();unsupported(self,_cmd);return nil;}
@end
@interface FuxiSharedEvent : FuxiEvent
@end
@implementation FuxiSharedEvent
@end
@interface FuxiFence : FuxiLabeledObject { @public id _device; }
@end
@implementation FuxiFence
- (id)device {CENSUS();return _device;}
- (void)dealloc {[_device release];[super dealloc];}
@end
struct FuxiEventPoint {size_t operation;FuxiEvent *event;uint64_t value;bool signal;};
@class FuxiCommandBuffer;
static thread_local FuxiCommandBuffer *completionCommand;
@interface FuxiQueue : FuxiLabeledObject { @public id _device;dispatch_queue_t _completionQueue; }
@end
@interface FuxiCommandBuffer : FuxiLabeledObject {
 @public FuxiQueue *_queue; NSMutableArray *_resources,*_handlers,*_scheduledHandlers; NSError *_error;dispatch_group_t _completionGroup;
 std::vector<FuxiEventPoint> _events;std::vector<fuxi::Operation> _ops; MTLCommandBufferStatus _status; BOOL _encoding,_rejected,_enqueued,_scheduled;
}
@end
@interface FuxiBlit : FuxiLabeledObject { @public FuxiCommandBuffer *_command; BOOL _ended; }
@end
@interface FuxiRender : FuxiLabeledObject { @public FuxiCommandBuffer *_command; FuxiTexture *_target; FuxiPipeline *_pipeline; MTLClearColor _clear; BOOL _ended,_drawn;
 FuxiTexture *_samples[fuxi::resourceSlots];FuxiSampler *_samplers[fuxi::resourceSlots];fuxi::BufferBind _buffers[2][fuxi::resourceSlots];uint32_t _load,_store;size_t _lastOp;
 fuxi::Operation _state; }
@end
@implementation FuxiRender
- (void)rejectSelector:(SEL)s {unsupported(self,s);[_command rejectSelector:s];}
- (void)dealloc {[_command release];[_target release];[_pipeline release];for(unsigned i=0;i<fuxi::resourceSlots;++i){[_samples[i] release];[_samplers[i] release];}[super dealloc];}
- (void)active {stage4Require("renderEncoder","ended",_ended,!_ended);stage4Require("renderEncoder","encoding",_command->_encoding,_command->_encoding);
 stage4Require("renderEncoder","commandStatus",_command->_status,_command->_status==MTLCommandBufferStatusNotEnqueued);}
- (void)setRenderPipelineState:(id)p {@try {CENSUS();[self active];
 if(!stage4Class("renderEncoder","pipeline.class",p,[FuxiPipeline class]))invalid(@"renderEncoder pipeline.class");
 stage4Require("renderEncoder","pipeline.device",(uint64_t)[p device],[p device]==_command->_queue->_device);
 [p retain];[_pipeline release];_pipeline=p;
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i {@try {CENSUS();[self active];
 stage4Require("sampling","textureIndex",i,i<fuxi::resourceSlots);
 if(!t){[_samples[i] release];_samples[i]=nil;return;}
 if(!stage4Class("sampling","texture.class",t,[FuxiTexture class]))invalid(@"sampling texture.class");
 stage4Require("sampling","texture.device",(uint64_t)[t device],[t device]==_command->_queue->_device);
 stage4Require("sampling","texture.usage",((FuxiTexture *)t)->_usage,((FuxiTexture *)t)->_usage&MTLTextureUsageShaderRead);
 stage4Require("sampling","attachmentFeedback",t==_target,t!=_target && !(((FuxiTexture *)t)->_surface && _target->_surface && IOSurfaceGetID(((FuxiTexture *)t)->_surface)==IOSurfaceGetID(_target->_surface)));
 [t retain];[_samples[i] release];_samples[i]=t;
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i {@try {CENSUS();[self active];
 stage4Require("sampling","samplerIndex",i,i<fuxi::resourceSlots);
 if(!s){[_samplers[i] release];_samplers[i]=nil;return;}
 if(!stage4Class("sampling","sampler.class",s,[FuxiSampler class]))invalid(@"sampling sampler.class");
 stage4Require("sampling","sampler.device",(uint64_t)[s device],[s device]==_command->_queue->_device);
 [s retain];[_samplers[i] release];_samplers[i]=s;
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)setBuffer:(id)b offset:(NSUInteger)o index:(NSUInteger)i stage:(unsigned)stage bytes:(BOOL)bytes {
 [self active];if(i>=fuxi::resourceSlots || (b && (![b isKindOfClass:[FuxiBuffer class]] || [b device]!=_command->_queue->_device || o>=[b length])))invalid(@"buffer slot/device/offset");
 if(b)[_command->_resources addObject:b];_buffers[stage][i]={b?((FuxiBuffer *)b)->_buffer:nullptr,o,(bool)bytes};
}
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {@try {CENSUS();[self setBuffer:b offset:o index:i stage:0 bytes:NO];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {@try {CENSUS();[self setBuffer:b offset:o index:i stage:1 bytes:NO];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
// NativeMetal's stage adapters, retaining Fuxi's sixteen-slot/device/poison rules.
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {@try {CENSUS();[self active];
 if(i>=fuxi::resourceSlots)invalid(@"vertex buffer offset slot");auto &b=_buffers[0][i];
 if(!fuxi::bindingOffset(b.buffer!=nullptr,b.bytes,b.buffer?b.buffer->length:0,o))invalid(@"vertex buffer offset binding/bounds");b.offset=o;
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {@try {CENSUS();[self active];
 if(i>=fuxi::resourceSlots)invalid(@"fragment buffer offset slot");auto &b=_buffers[1][i];
 if(!fuxi::bindingOffset(b.buffer!=nullptr,b.bytes,b.buffer?b.buffer->length:0,o))invalid(@"fragment buffer offset binding/bounds");b.offset=o;
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {@try {CENSUS();[self active];
 if(!fuxi::bindingRange(r.location,r.length) || (r.length && (!b || !o)))invalid(@"vertex buffers range/pointers");
 for(NSUInteger k=0;k<r.length;++k)[self setBuffer:b[k] offset:o[k] index:r.location+k stage:0 bytes:NO];
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {@try {CENSUS();[self active];
 if(!fuxi::bindingRange(r.location,r.length) || (r.length && (!b || !o)))invalid(@"fragment buffers range/pointers");
 for(NSUInteger k=0;k<r.length;++k)[self setBuffer:b[k] offset:o[k] index:r.location+k stage:1 bytes:NO];
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {@try {CENSUS();[self active];
 if(!fuxi::bindingRange(r.location,r.length) || (r.length && !t))invalid(@"fragment textures range/pointer");
 for(NSUInteger k=0;k<r.length;++k)[self setFragmentTexture:t[k] atIndex:r.location+k];
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r {@try {CENSUS();[self active];
 if(!fuxi::bindingRange(r.location,r.length) || (r.length && !s))invalid(@"fragment samplers range/pointer");
 for(NSUInteger k=0;k<r.length;++k)[self setFragmentSamplerState:s[k] atIndex:r.location+k];
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si {
 CENSUS();[self setFragmentTexture:t atIndex:ti];[self setFragmentSamplerState:s atIndex:si];
}
- (void)fence:(id)f stages:(MTLRenderStages)stages {CENSUS();
 if(_ended || !_command->_encoding || ![f isKindOfClass:[FuxiFence class]] || [f device]!=[self device] || !stages || (stages&~(MTLRenderStageVertex|MTLRenderStageFragment))){[self rejectSelector:_cmd];return;}
 [_command->_resources addObject:f];
}
- (void)updateFence:(id<MTLFence>)f afterStages:(MTLRenderStages)stages {[self fence:f stages:stages];}
- (void)waitForFence:(id<MTLFence>)f beforeStages:(MTLRenderStages)stages {[self fence:f stages:stages];}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {CENSUS();(void)usage;(void)stages;
 if(_ended || (r && [r device]!=[self device])){[self rejectSelector:_cmd];return;}if(r)[_command->_resources addObject:r];
}
- (void)setDepthStencilState:(id)s {@try {CENSUS();[self active];
 if(s && (![s isKindOfClass:[FuxiDepthStencil class]] || [s device]!=[self device]))invalid(@"depth/stencil state device/class");
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
// CA's update_encoder_state restores these defaults when its dirty bits are
// set. With our sole color target and restricted blend factors they are inert.
// Refuse other raster/query state instead of silently accepting a real effect.
- (void)setFrontFacingWinding:(MTLWinding)w {@try {CENSUS();[self active];
 stage4Require("raster","winding",w,w==MTLWindingClockwise);
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setCullMode:(MTLCullMode)m {@try {CENSUS();[self active];
 stage4Require("raster","cullMode",m,m==MTLCullModeNone);
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setTriangleFillMode:(MTLTriangleFillMode)m {@try {CENSUS();[self active];
 stage4Require("raster","fillMode",m,m==MTLTriangleFillModeFill);
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {@try {CENSUS();[self active];
 if(bias!=0 || (slope!=0 && slope!=1) || clamp!=0)invalid(@"neutral depth bias only (0,0/1,0)");
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha {@try {CENSUS();[self active];
 for(float value:{red,green,blue,alpha})if(!std::isfinite(value) || value<0 || value>1)invalid(@"blend color outside 0..1");
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {@try {CENSUS();[self active];
 if(mode!=MTLVisibilityResultModeDisabled || offset)invalid(@"visibility queries deferred");
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (id)device {CENSUS();return _command->_queue->_device;}
- (void)setBytes:(const void *)bytes length:(NSUInteger)n index:(NSUInteger)i stage:(unsigned)stage {
 [self active];if(!bytes || !n || n>4096 || i>=fuxi::resourceSlots)invalid(@"inline bytes bound");
 FuxiBuffer *b=[FuxiBuffer new];b->_device=[_command->_queue->_device retain];
 try {b->_buffer=owner(b->_device)->backend->allocate(n);memcpy(b->_buffer->contents,bytes,n);}
 catch(const std::exception &e){[b release];invalid([NSString stringWithUTF8String:e.what()]);}
 [self setBuffer:b offset:0 index:i stage:stage bytes:YES];[b release];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)n atIndex:(NSUInteger)i {@try {CENSUS();[self setBytes:bytes length:n index:i stage:0];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)n atIndex:(NSUInteger)i {@try {CENSUS();[self setBytes:bytes length:n index:i stage:1];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setViewport:(MTLViewport)v {@try {CENSUS();[self active];
 if(!fuxi::viewportFits(v.originX,v.originY,v.width,v.height,v.znear,v.zfar,[_target width],[_target height]))invalid(@"viewport bounds");
 _state.viewport={(float)v.originX,(float)(v.originY+v.height),(float)v.width,(float)-v.height,(float)v.znear,(float)v.zfar};
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)setScissorRect:(MTLScissorRect)r {@try {CENSUS();[self active];if(!fuxi::scissorFits(r.x,r.y,r.width,r.height,[_target width],[_target height]))invalid(@"scissor bounds");
 _state.scissor={{(int32_t)r.x,(int32_t)r.y},{(uint32_t)r.width,(uint32_t)r.height}};
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)record:(MTLPrimitiveType)t start:(NSUInteger)start count:(NSUInteger)count indices:(id)indices offset:(NSUInteger)offset {
 [self active];if(!_pipeline || (t!=MTLPrimitiveTypeTriangle && t!=MTLPrimitiveTypeTriangleStrip) || !count || count>65536 || start>65536-count)invalid(@"draw bounds/pipeline/topology");
 if(indices && (![indices isKindOfClass:[FuxiBuffer class]] || [indices device]!=_command->_queue->_device || (offset&1) || offset>[indices length] || count*2>[indices length]-offset))invalid(@"index bounds/device");
 fuxi::Operation op=_state;op.texture=_target->_texture;op.pipeline=_pipeline->_pipeline;op.vertices=(uint32_t)count;op.first=(uint32_t)start;op.strip=t==MTLPrimitiveTypeTriangleStrip;
 op.indices=indices?((FuxiBuffer *)indices)->_buffer:nullptr;op.indexOffset=offset;
 // CA smoke diagnostic: for a textured draw, every vertex binding and its first vertices (stride 48 floats).
 bool textured=false;for(size_t i=0;i<fuxi::resourceSlots;++i)textured|=_samples[i]!=nil;
 static std::atomic<unsigned> texturedTraces{0};
 if(caTrace() && textured && texturedTraces.fetch_add(1)<8) {
  fprintf(stderr,"FUXI MTL: trace textured draw start %lu count %lu indexed %d\n",(unsigned long)start,(unsigned long)count,indices!=nil);
  for(size_t i=0;i<fuxi::resourceSlots;++i)if(_samples[i])fprintf(stderr,"FUXI MTL: trace textured slot %zu tex %lux%lu\n",i,(unsigned long)[_samples[i] width],(unsigned long)[_samples[i] height]);
  for(size_t i=0;i<fuxi::resourceSlots;++i) {const fuxi::BufferBind &v=_buffers[0][i];if(!v.buffer)continue;
   fprintf(stderr,"FUXI MTL: trace textured vbuf %zu offset %llu length %llu bytes %d\n",i,(unsigned long long)v.offset,(unsigned long long)v.buffer->length,v.bytes);
   for(unsigned k=0;k<4;++k) {uint64_t at=v.offset+k*48;if(at+32>v.buffer->length)break;const float *f=(const float *)((const uint8_t *)v.buffer->contents+at);
    fprintf(stderr,"FUXI MTL: trace textured vbuf %zu v%u %.3f %.3f %.3f %.3f | %.4f %.4f | %.4f %.4f\n",i,k,f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]);}
  }
  for(unsigned stage=0;stage<2;++stage)for(size_t i=0;i<fuxi::resourceSlots;++i) {const fuxi::BufferBind &v=_buffers[stage][i];
   if(!v.buffer || v.offset+64>v.buffer->length)continue;const float *f=(const float *)((const uint8_t *)v.buffer->contents+v.offset);
   fprintf(stderr,"FUXI MTL: trace textured %s buf %zu raw",stage?"frag":"vert",i);for(unsigned k=0;k<16;++k)fprintf(stderr," %.4g",f[k]);fputc('\n',stderr);
  }
 }
 op.load=_drawn?1:_load;op.store=1;
 for(unsigned i=0;i<fuxi::resourceSlots;++i){op.samples[i]=_samples[i]?_samples[i]->_texture:nullptr;op.samplers[i]=_samplers[i]?_samplers[i]->_sampler:nullptr;
 if(_samples[i])[_command->_resources addObject:_samples[i]];if(_samplers[i])[_command->_resources addObject:_samplers[i]];}
 memcpy(op.buffers,_buffers,sizeof(_buffers));if(indices)[_command->_resources addObject:indices];
 op.clear[0]=_clear.red;op.clear[1]=_clear.green;op.clear[2]=_clear.blue;op.clear[3]=_clear.alpha;
 [_command->_resources addObject:_target];[_command->_resources addObject:_pipeline];_lastOp=_command->_ops.size();_command->_ops.push_back(op);_drawn=YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {@try {CENSUS();[self record:t start:start count:count indices:nil offset:0];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances {@try {CENSUS();[self active];
 stage4Require("draw","instanceCount",instances,instances==1);[self record:t start:start count:count indices:nil offset:0];
} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)count indexType:(MTLIndexType)type indexBuffer:(id)b indexBufferOffset:(NSUInteger)offset {@try {CENSUS();if(type!=MTLIndexTypeUInt16 || !b)invalid(@"index type/buffer");[self record:t start:0 count:count indices:b offset:offset];} @catch(NSException *e){(void)e;[self rejectSelector:_cmd];}}
- (void)endEncoding {@try {CENSUS();[self active];
 if(!_drawn){fuxi::Operation op{};op.texture=_target->_texture;op.clearOnly=true;op.load=_load;op.store=_store;
 op.clear[0]=_clear.red;op.clear[1]=_clear.green;op.clear[2]=_clear.blue;op.clear[3]=_clear.alpha;
 [_command->_resources addObject:_target];_command->_ops.push_back(op);
 }else _command->_ops[_lastOp].store=_store;_ended=YES;_command->_encoding=NO;} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}

@end
@implementation FuxiQueue
- (void)dealloc {if(_completionQueue)dispatch_release(_completionQueue);[_device release];[super dealloc];}
- (id)device {CENSUS();return _device;}
- (id)commandBuffer {CENSUS();
 FuxiCommandBuffer *b=[FuxiCommandBuffer new];b->_queue=[self retain];
 b->_resources=[NSMutableArray new];b->_handlers=[NSMutableArray new];b->_scheduledHandlers=[NSMutableArray new];b->_completionGroup=dispatch_group_create();return [b autorelease];
}
// NativeMetal retains owners even for the unretained entry point. Resources
// remain alive through synchronous completion; this only strengthens lifetime.
- (id)commandBufferWithUnretainedReferences {CENSUS();return [self commandBuffer];}
- (id)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {CENSUS();
 if(!d || d.errorOptions!=MTLCommandBufferErrorOptionNone){unsupported(self,_cmd);return nil;}return [self commandBuffer];
}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {CENSUS();(void)queue;}
- (void)setCompletionQueue:(dispatch_queue_t)queue {CENSUS();if(queue)dispatch_retain(queue);if(_completionQueue)dispatch_release(_completionQueue);_completionQueue=queue;}
- (int)setOwnerWithIdentity:(unsigned)identity {CENSUS();(void)identity;return 46;}
- (void)setFineGrainedBackgroundVisibilityEnabled:(BOOL)enabled {CENSUS();(void)enabled;}
- (BOOL)setGPUPriority:(NSUInteger)priority {CENSUS();(void)priority;return NO;}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {CENSUS();(void)priority;return NO;}
@end
@implementation FuxiCommandBuffer
- (void)rejectSelector:(SEL)s {
 unsupported(self,s);_rejected=YES;
 if(!_error)_error=[[NSError alloc] initWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInvalidResource
  userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"unsupported %@",NSStringFromSelector(s)]}];
}
- (void)dealloc {[_queue release];[_resources release];[_handlers release];[_scheduledHandlers release];if(_completionGroup)dispatch_release(_completionGroup);[_error release];[super dealloc];}
- (id)device {CENSUS();return _queue->_device;}
- (id)commandQueue {CENSUS();return _queue;}
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
 CHECK("renderTargetWidth",d.renderTargetWidth,!actual || actual==[t width]);CHECK("renderTargetHeight",d.renderTargetHeight,!actual || actual==[t height]);
 CHECK("samplePositions.count",[d getSamplePositions:nullptr count:0],!actual);
 for(unsigned i=1;i<8;++i) {
  char field[80];snprintf(field,sizeof(field),"colorAttachments[%u].texture",i);CHECK(field,d.colorAttachments[i].texture,!actual);
  snprintf(field,sizeof(field),"colorAttachments[%u].resolveTexture",i);CHECK(field,d.colorAttachments[i].resolveTexture,!actual);
 }
 MTLClearColor c=a.clearColor;unsigned component=0;
 for(double x:{c.red,c.green,c.blue,c.alpha}) {
  if(!std::isfinite(x) || x<0 || x>1) {failureLog("FUXI MTL: renderPass reject clearColor[%u] %.17g\n",component,x);invalid(@"clear colour outside 0..1");}++component;
 }
#undef CHECK
 FuxiRender *e=[FuxiRender new];e->_state={};e->_command=[self retain];e->_target=[t retain];e->_clear=c;e->_load=(uint32_t)a.loadAction;e->_store=(uint32_t)a.storeAction;e->_state.viewport={0,(float)[t height],(float)[t width],-(float)[t height],0,1};e->_state.scissor={{0,0},{(uint32_t)[t width],(uint32_t)[t height]}};_encoding=YES;return [e autorelease];
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];return nil;}}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {CENSUS();
 if(!handler){[self rejectSelector:_cmd];return;}if(_scheduled){handler((id)self);return;}
 if(_status>=MTLCommandBufferStatusCompleted)return;
 id copied=[handler copy];[_scheduledHandlers addObject:copied];[copied release];
}
- (void)deliverCompletedHandlers:(NSArray *)handlers {
 if(!handlers.count)return;dispatch_group_enter(_completionGroup);
 void (^run)(void)=^{FuxiCommandBuffer *before=completionCommand;completionCommand=self;
  for(MTLCommandBufferHandler handler in handlers)handler((id)self);
  completionCommand=before;dispatch_group_leave(_completionGroup);};
 if(_queue->_completionQueue)dispatch_async(_queue->_completionQueue,run);else run();
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {CENSUS();
 if(!handler) {[self rejectSelector:_cmd];return;}
 if(_status>=MTLCommandBufferStatusCompleted) {id copied=[handler copy];[self deliverCompletedHandlers:@[copied]];[copied release];return;}
 id copied=[handler copy];[_handlers addObject:copied];[copied release];
}
- (void)event:(id)e value:(uint64_t)value signal:(BOOL)signal {CENSUS();
 if(_status!=MTLCommandBufferStatusNotEnqueued || _encoding || _events.size()>=256 || ![e isKindOfClass:[FuxiEvent class]] || [e device]!=[self device]){[self rejectSelector:_cmd];return;}
 [_resources addObject:e];_events.push_back({_ops.size(),e,value,(bool)signal});
}
- (void)encodeSignalEvent:(id<MTLEvent>)e value:(uint64_t)value {[self event:e value:value signal:YES];}
- (void)encodeWaitForEvent:(id<MTLEvent>)e value:(uint64_t)value {[self event:e value:value signal:NO];}
- (void)commit {CENSUS();
 if(_status!=MTLCommandBufferStatusNotEnqueued)return;
 if(_encoding)[self rejectSelector:_cmd];
 _status=MTLCommandBufferStatusCommitted;
 // Deliberately synchronous M4 queue: no dispatch worker or asynchronous promise.
 try {if(!strcmp(clientProcess,"fuxi-ca-smoke") && failureCount.load())throw std::runtime_error("CA diagnostic failure latched: GPU submission refused");if(_rejected)throw std::runtime_error("unsupported command: GPU submission refused");if(clientMode==fuxi::ClientMode::census)throw std::runtime_error("census: no GPU submission");auto *backend=owner(_queue->_device)->backend;size_t begin=0;
 for(id r in _resources){FuxiBuffer *s=nil;
  if([r isKindOfClass:[FuxiBuffer class]])s=r;else if([r isKindOfClass:[FuxiTexture class]])s=[(FuxiTexture *)r buffer];
  if(s && s->_shadow)memcpy(s->_buffer->contents,s->_wrapped,s->_wrappedLength);}
 for(const auto &point:_events){
  if(point.operation>begin){backend->submit(std::vector<fuxi::Operation>(_ops.begin()+begin,_ops.begin()+point.operation),[&]{_scheduled=YES;});begin=point.operation;}
  if(point.signal)[point.event setSignaledValue:point.value];
  else if(![point.event waitUntilSignaledValue:point.value timeoutMS:2000])throw std::runtime_error("process-local event wait timed out");
 }
 if(begin<_ops.size() || _events.empty())backend->submit(std::vector<fuxi::Operation>(_ops.begin()+begin,_ops.end()),[&]{_scheduled=YES;});
 else _scheduled=YES;
 for(const auto &op:_ops)if(op.pipeline)completedDraws.fetch_add(1);_status=MTLCommandBufferStatusCompleted;}
 catch(const std::exception &e) {
  failureLog("FUXI MTL: submit FAIL %.256s\n",e.what());_status=MTLCommandBufferStatusError;
  if(!_error)_error=[[NSError alloc] initWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
      userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:e.what()]}];
 }
 // Deliver outside Backend's mutex; handlers may create resources/commands.
 // On successful submission this callback may arrive after GPU completion.
 if(_scheduled){NSArray *scheduled=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
  for(MTLCommandBufferHandler handler in scheduled)handler((id)self);[scheduled release];}
 NSArray *handlers=[_handlers copy];[_handlers removeAllObjects];
 [self deliverCompletedHandlers:handlers];
 [handlers release];[_scheduledHandlers removeAllObjects];[_resources removeAllObjects];_ops.clear();_events.clear();
}
- (void)waitUntilCompleted {CENSUS();if(completionCommand!=self)dispatch_group_wait(_completionGroup,DISPATCH_TIME_FOREVER);}
- (void)waitUntilScheduled {CENSUS();}
- (BOOL)commitAndWaitUntilSubmitted {CENSUS();[self commit];return _scheduled && _status!=MTLCommandBufferStatusError;}
- (CFTimeInterval)kernelStartTime {CENSUS();return 0;}
- (CFTimeInterval)kernelEndTime {CENSUS();return 0;}
- (CFTimeInterval)GPUStartTime {CENSUS();return 0;}
- (CFTimeInterval)GPUEndTime {CENSUS();return 0;}
- (void)pushDebugGroup:(NSString *)name {CENSUS();(void)name;}
- (void)popDebugGroup {CENSUS();}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {CENSUS();(void)tag;
 if(count>256 || (count && !groups)){[self rejectSelector:_cmd];return;}
 for(NSUInteger i=0;i<count;++i)if(groups[i])[_resources addObject:groups[i]];
}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {
 [self encodeCacheHintTag:tag resourceGroups:groups count:count];
}
// The queue is synchronous, so enqueue only reserves nothing; commit order is
// submission order. A second enqueue or one after commit is a client error.
- (void)enqueue {CENSUS();if(_enqueued || _status!=MTLCommandBufferStatusNotEnqueued)[self rejectSelector:_cmd];_enqueued=YES;}
// Protected content is unsupported: only the "none" option is accepted.
- (NSUInteger)protectionOptions {CENSUS();return 0;}
- (void)setProtectionOptions:(NSUInteger)o {CENSUS();if(o)[self rejectSelector:_cmd];}
@end
@implementation FuxiBlit
- (void)rejectSelector:(SEL)s {unsupported(self,s);[_command rejectSelector:s];}
- (void)dealloc {[_command release];[super dealloc];}
- (id)device {CENSUS();return _command->_queue->_device;}
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
- (FuxiTexture *)texture:(id)t slice:(NSUInteger)s level:(NSUInteger)l origin:(MTLOrigin)o size:(MTLSize)z {
 if(_ended || !_command->_encoding || _command->_status!=MTLCommandBufferStatusNotEnqueued)invalid(@"inactive blit encoder");
 if(![t isKindOfClass:[FuxiTexture class]] || [t device]!=[self device])invalid(@"foreign texture/device");
 if(s || l || o.z || !z.width || !z.height || z.depth!=1 || o.x>[t width] || z.width>[t width]-o.x || o.y>[t height] || z.height>[t height]-o.y)invalid(@"texture copy region bounds");return t;
}
- (FuxiBuffer *)imageBuffer:(id)b offset:(NSUInteger)offset row:(NSUInteger)row image:(NSUInteger)image size:(MTLSize)size pixel:(NSUInteger)pixel {
 if(![b isKindOfClass:[FuxiBuffer class]] || [b device]!=[self device] || row<size.width*pixel || row%pixel || row>fuxi::Backend::maxBufferLength/size.height || (image && image<row*size.height) || offset%4 || offset%pixel)invalid(@"texture buffer stride/alignment");
 size_t bytes=(size.height-1)*row+size.width*pixel;
 if(offset>[b length] || bytes>[b length]-offset)invalid(@"texture buffer bounds");return b;
}
- (void)copyFromTexture:(id)t sourceSlice:(NSUInteger)s sourceLevel:(NSUInteger)l sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z
 toBuffer:(id)b destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {@try {CENSUS();
 FuxiTexture *src=[self texture:t slice:s level:l origin:o size:z];
 FuxiBuffer *dst=[self imageBuffer:b offset:offset row:row image:image size:z pixel:fuxi::Backend::texelBytes((uint32_t)[src pixelFormat])];
 fuxi::Operation op{};op.texture=src->_texture;op.dst=dst->_buffer;op.to=offset;op.srcX=o.x;op.srcY=o.y;op.width=z.width;op.height=z.height;op.rowBytes=row;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];_command->_ops.push_back(op);
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)copyFromBuffer:(id)b sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
 sourceSize:(MTLSize)z toTexture:(id)t destinationSlice:(NSUInteger)s destinationLevel:(NSUInteger)l destinationOrigin:(MTLOrigin)o {@try {CENSUS();
 if(caTrace())fprintf(stderr,"FUXI MTL: trace bufferToTexture offset %lu row %lu %lux%lu to %lu,%lu format %lu\n",(unsigned long)offset,(unsigned long)row,(unsigned long)z.width,(unsigned long)z.height,(unsigned long)o.x,(unsigned long)o.y,(unsigned long)[t pixelFormat]);
 FuxiTexture *dst=[self texture:t slice:s level:l origin:o size:z];
 FuxiBuffer *src=[self imageBuffer:b offset:offset row:row image:image size:z pixel:fuxi::Backend::texelBytes((uint32_t)[dst pixelFormat])];
 fuxi::Operation op{};op.src=src->_buffer;op.from=offset;op.texture=dst->_texture;op.upload=true;op.dstX=o.x;op.dstY=o.y;op.width=z.width;op.height=z.height;op.rowBytes=row;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];_command->_ops.push_back(op);
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)copyFromTexture:(id)t sourceSlice:(NSUInteger)s sourceLevel:(NSUInteger)l sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z
 toTexture:(id)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dest {@try {CENSUS();
 FuxiTexture *src=[self texture:t slice:s level:l origin:o size:z],*dst=[self texture:d slice:ds level:dl origin:dest size:z];
 if([src pixelFormat]!=[dst pixelFormat] || src->_texture==dst->_texture || (src->_surface && dst->_surface && IOSurfaceGetID(src->_surface)==IOSurfaceGetID(dst->_surface)))invalid(@"aliased texture copy");
 fuxi::Operation op{};op.sourceTexture=src->_texture;op.texture=dst->_texture;op.srcX=o.x;op.srcY=o.y;op.dstX=dest.x;op.dstY=dest.y;op.width=z.width;op.height=z.height;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];_command->_ops.push_back(op);
} @catch(NSException *e) {(void)e;[self rejectSelector:_cmd];}}
- (void)copyFromTexture:(id<MTLTexture>)src toTexture:(id<MTLTexture>)dst {
 [self copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(src.width,src.height,1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
}
- (void)fence:(id)f {CENSUS();
 if(_ended || !_command->_encoding || ![f isKindOfClass:[FuxiFence class]] || [f device]!=[self device]){[self rejectSelector:_cmd];return;}[_command->_resources addObject:f];
}
- (void)updateFence:(id<MTLFence>)f {[self fence:f];}
- (void)waitForFence:(id<MTLFence>)f {[self fence:f];}
- (void)synchronizeResource:(id<MTLResource>)r {CENSUS();
 if(_ended || (r && [r device]!=[self device])){[self rejectSelector:_cmd];return;}if(r)[_command->_resources addObject:r];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t {CENSUS();
 if(_ended || ![t isKindOfClass:[FuxiTexture class]] || [t device]!=[self device] || [t mipmapLevelCount]!=1){[self rejectSelector:_cmd];return;}[_command->_resources addObject:t];
}
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
 if(rc || !registry) {failureLog("FUXI MTL: registryID FAIL\n");[self release];return nil;}
 FuxiOwner *state=[FuxiOwner new];state->registry=registry;
 try {if(clientMode!=fuxi::ClientMode::census)state->backend=new fuxi::Backend;}
 catch(const std::exception &e) {failureLog("FUXI MTL: device FAIL %s\n",e.what());[state release];[self release];return nil;}
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
- (BOOL)supportsYCBCRPackedFormats12 {CENSUS();return NO;}
- (NSUInteger)minConstantBufferAlignmentBytes {CENSUS();return 256;}
// QuartzCore sizes linear image uploads by these (M9 CA smoke stage 3). 16 bytes is
// Apple's linear texture alignment and metal2vulkan's DeviceLinearTextureAlignmentBytes.
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes {CENSUS();return 16;}
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format {CENSUS();(void)format;return 16;}
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format {CENSUS();(void)format;return 16;}
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes {CENSUS();return 256;}
- (NSUInteger)maxFragmentTextures {CENSUS();return fuxi::resourceSlots;}
- (NSUInteger)maxFragmentSamplers {CENSUS();return fuxi::resourceSlots;}
- (NSUInteger)maxTextureWidth2D {CENSUS();return owner(self)->backend?owner(self)->backend->maxTextureDimension():0;}
- (NSUInteger)maxTextureHeight2D {CENSUS();return owner(self)->backend?owner(self)->backend->maxTextureDimension():0;}
- (NSUInteger)maxColorAttachments {CENSUS();return 1;}
- (NSString *)vendorName {CENSUS();return @"Qualcomm";}
- (BOOL)supportsYCBCRFormats {CENSUS();return NO;}
- (BOOL)supportsYCBCRFormats12 {CENSUS();return NO;}
// iOS 16.7 CA uses this scalar to select lph shader names. A740 has native
// FP16; this does not grant an Apple family or any tile/fetch/format feature.
- (BOOL)supportsNativeHardwareFP16 {CENSUS();return YES;}
- (BOOL)supportsTileShaders {CENSUS();return NO;}
- (BOOL)supportsSIMDGroup {CENSUS();return NO;}
- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n {CENSUS();
 if(!n || n>64) {unsupported(self,_cmd);return nil;}return [self newCommandQueue];
}
- (id)newCommandQueue {CENSUS();if(clientMode==fuxi::ClientMode::census)return nil;FuxiQueue *q=[FuxiQueue new];q->_device=[self retain];return q;}
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)d {CENSUS();
 if(!d || d.type!=MTLHeapTypeAutomatic || !d.size || d.size>fuxi::Backend::maxBufferLength || d.storageMode!=MTLStorageModeShared || d.cpuCacheMode!=MTLCPUCacheModeDefaultCache ||
    (d.hazardTrackingMode!=MTLHazardTrackingModeDefault && d.hazardTrackingMode!=MTLHazardTrackingModeTracked)){unsupported(self,_cmd);return nil;}
 FuxiHeap *heap=[FuxiHeap new];heap->_device=[self retain];heap->_size=d.size;return heap;
}
- (id)newEvent {CENSUS();FuxiEvent *e=[FuxiEvent new];e->_device=[self retain];return e;}
- (id)newSharedEvent {CENSUS();FuxiEvent *e=[FuxiSharedEvent new];e->_device=[self retain];e->_shared=YES;return e;}
- (id)newFence {CENSUS();FuxiFence *f=[FuxiFence new];f->_device=[self retain];return f;}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count {CENSUS();
 if(count>256 || (count && !resources)){unsupported(self,_cmd);return nil;}
 NSMutableArray *group=[NSMutableArray new];for(NSUInteger i=0;i<count;++i)if(resources[i]){
  if(![resources[i] isKindOfClass:[FuxiResource class]] || [resources[i] device]!=self){[group release];unsupported(self,_cmd);return nil;}
  [group addObject:resources[i]];
 }
 return group;
}
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)options {CENSUS();
 // Reject write-combine/untracked/private/managed/sparse and unknown options.
 if(clientMode==fuxi::ClientMode::census)return nil;
 if(options!=MTLResourceStorageModeShared) {unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[FuxiBuffer new];b->_device=[self retain];
 try {b->_buffer=owner(self)->backend->allocate(n);}
 catch(const std::exception &e) {failureLog("FUXI MTL: buffer FAIL %s\n",e.what());[b release];return nil;}
 return b;
}
- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)n options:(MTLResourceOptions)o {CENSUS();
 if(!bytes) {unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[self newBufferWithLength:n options:o];if(b)memcpy([b contents],bytes,n);return b;
}
- (id)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *,NSUInteger))deallocator {CENSUS();
 if(options!=MTLResourceStorageModeShared || !pointer || clientMode==fuxi::ClientMode::census){unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[FuxiBuffer new];b->_device=[self retain];
 try {b->_buffer=owner(self)->backend->importBuffer(pointer,length);}
 catch(const std::exception &e){
  // QuartzCore wraps CGImage bytes, which are often unaligned or read-only
  // copy-on-write pages the kernel will not wire for GPU access (M9 CA smoke
  // stage 3). Shadow them: contents stays the client's pointer and every
  // commit refreshes the GPU copy. GPU writes do not reach the client's pages.
  static std::atomic<bool> logged{false};
  if(!logged.exchange(true))fprintf(stderr,"FUXI MTL: no-copy buffer shadowed (%s)\n",e.what());
  try {b->_buffer=owner(self)->backend->allocate(length);}
  catch(const std::exception &a){failureLog("FUXI MTL: no-copy buffer FAIL %s\n",a.what());[b release];return nil;}
  memcpy(b->_buffer->contents,pointer,length);b->_shadow=YES;
 }
 b->_wrapped=pointer;b->_wrappedLength=length;b->_deallocator=[deallocator copy];return b;
}
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {CENSUS();
 auto neutral=[](MTLStencilDescriptor *s){return !s || fuxi::neutralStencil(s.stencilCompareFunction,s.stencilFailureOperation,s.depthFailureOperation,s.depthStencilPassOperation);};
 if(!d || !fuxi::neutralDepth(d.depthCompareFunction,d.depthWriteEnabled) || !neutral(d.frontFaceStencil) || !neutral(d.backFaceStencil)) {
  unsupported(self,_cmd);return nil;
 }
 FuxiDepthStencil *s=[FuxiDepthStencil new];s->_device=[self retain];s->_label=[d.label copy];return s;
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
- (id)newLibraryWithFile:(NSString *)path error:(NSError **)error {CENSUS();return [self newLibraryWithURL:path?[NSURL fileURLWithPath:path]:nil error:error];}
- (id)newDefaultLibrary {CENSUS();return [self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nullptr];}
- (id)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {CENSUS();
 return [self newLibraryWithURL:[bundle URLForResource:@"default" withExtension:@"metallib"] error:error];
}
// Pipeline libraries are an optional precompiled cache; callers fall back to
// building pipelines, so report absence without counting a failure.
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {CENSUS();(void)path;
 if(error)*error=[NSError errorWithDomain:@"org.fuxi.metal" code:2 userInfo:@{NSLocalizedDescriptionKey:@"no pipeline library support"}];return nil;
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
 CHECK("width",d.width,actual && actual<=owner(self)->backend->maxTextureDimension());CHECK("height",d.height,actual && actual<=owner(self)->backend->maxTextureDimension());CHECK("depth",d.depth,actual==1);
 CHECK("mipmapLevelCount",d.mipmapLevelCount,actual==1);CHECK("arrayLength",d.arrayLength,actual==1);CHECK("sampleCount",d.sampleCount,actual==1);CHECK("swizzle.identity",identitySwizzle(d.swizzle),actual);
 CHECK("storageMode",d.storageMode,actual==MTLStorageModePrivate || actual==MTLStorageModeShared);CHECK("cpuCacheMode",d.cpuCacheMode,actual==MTLCPUCacheModeDefaultCache);
 CHECK("hazardTrackingMode",d.hazardTrackingMode,actual==MTLHazardTrackingModeDefault || actual==MTLHazardTrackingModeTracked);
 CHECK("usage",d.usage,actual && !(actual&~(MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead)));
 // allowGPUOptimizedContents is an implementation hint, not pixel semantics.
#undef CHECK
 FuxiTexture *t=[FuxiTexture new];t->_device=[self retain];t->_usage=d.usage;t->_shared=d.storageMode==MTLStorageModeShared;
 try {t->_texture=owner(self)->backend->allocateTexture((uint32_t)d.pixelFormat,(uint32_t)d.usage,(uint32_t)d.width,(uint32_t)d.height);}catch(const std::exception &e) {failureLog("FUXI MTL: texture FAIL %s\n",e.what());[t release];return nil;}return t;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {CENSUS();
 if(clientMode==fuxi::ClientMode::census)return nil;
 // One BGRA plane within device bounds; no external producer/fence.
 if(!owner(self)->backend->supportsHostImport()) {
  unsupported(self,_cmd);failureLog("FUXI MTL: IOSurface import FAIL NativeAdreno HostImport / VK_EXT_external_memory_host unavailable\n");return nil;
 }
 if(!d || !surface || plane || IOSurfaceGetPlaneCount(surface)!=0 || IOSurfaceGetPixelFormat(surface)!=0x42475241 ||
    !d.width || !d.height || d.width>owner(self)->backend->maxTextureDimension() || d.height>owner(self)->backend->maxTextureDimension() ||
    IOSurfaceGetWidth(surface)!=d.width || IOSurfaceGetHeight(surface)!=d.height || IOSurfaceGetBytesPerElement(surface)!=4 || d.depth!=1 || d.textureType!=MTLTextureType2D || d.pixelFormat!=MTLPixelFormatBGRA8Unorm ||
    d.mipmapLevelCount!=1 || d.arrayLength!=1 || d.sampleCount!=1 || !identitySwizzle(d.swizzle) || d.storageMode!=MTLStorageModeShared ||
    d.cpuCacheMode!=MTLCPUCacheModeDefaultCache || (d.hazardTrackingMode!=MTLHazardTrackingModeDefault && d.hazardTrackingMode!=MTLHazardTrackingModeTracked) ||
    !d.usage || (d.usage&~(MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget))) {unsupported(self,_cmd);return nil;}
 if(IOSurfaceLock(surface,kIOSurfaceLockAvoidSync,nullptr)!=0) {unsupported(self,_cmd);return nil;}
 FuxiTexture *t=[FuxiTexture new];t->_device=[self retain];t->_usage=d.usage;
 t->_surface=(IOSurfaceRef)CFRetain(surface);IOSurfaceIncrementUseCount(surface);
 try {t->_texture=owner(self)->backend->importTexture(IOSurfaceGetBaseAddress(surface),IOSurfaceGetAllocSize(surface),IOSurfaceGetBytesPerRow(surface),(uint32_t)d.width,(uint32_t)d.height);}
 catch(const std::exception &e) {unsupported(self,_cmd);failureLog("FUXI MTL: IOSurface import FAIL %s\n",e.what());[t release];return nil;}
 fprintf(stderr,"FUXI MTL: IOSurface import PASS BGRA8 %lux%lu same-memory\n",(unsigned long)d.width,(unsigned long)d.height);return t;
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
 catch(const std::exception &e) {unsupported(self,_cmd);failureLog("FUXI MTL: sampler FAIL %s\n",e.what());[state release];return nil;}return state;
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {CENSUS();
 if(error)*error=nil;if(clientMode==fuxi::ClientMode::census)return nil;
 if(NSString *field=pipelineRejection(d,self))return metalError(error,field.UTF8String);
 id v=d.vertexFunction,f=d.fragmentFunction;
 FuxiPipeline *p=[FuxiPipeline new];p->_device=[self retain];p->_label=[d.label copy];
 try {auto a=(FuxiFunction *)v,b=(FuxiFunction *)f;fuxi::CompileOptions o{};auto color=d.colorAttachments[0];
  o.format=(uint32_t)color.pixelFormat;o.blend=color.blendingEnabled;o.mask=(uint32_t)color.writeMask;
  o.srcRGB=(uint32_t)color.sourceRGBBlendFactor;o.dstRGB=(uint32_t)color.destinationRGBBlendFactor;o.rgbOp=(uint32_t)color.rgbBlendOperation;
  o.srcAlpha=(uint32_t)color.sourceAlphaBlendFactor;o.dstAlpha=(uint32_t)color.destinationAlphaBlendFactor;o.alphaOp=(uint32_t)color.alphaBlendOperation;
  for(unsigned i=0;i<8;++i){auto attr=d.vertexDescriptor.attributes[i];o.attributes[i]={(uint32_t)attr.format,(uint32_t)attr.offset,(uint32_t)attr.bufferIndex};}
  for(unsigned i=0;i<4;++i)o.strides[i]=(uint32_t)d.vertexDescriptor.layouts[i].stride;
  NSData *constants[2]={a->_constants,b->_constants};for(unsigned i=0;i<2;++i){o.constantCount[i]=(uint32_t)(constants[i].length/sizeof(fuxi::Constant));if(o.constantCount[i]>fuxi::maxFunctionConstants)throw std::runtime_error("constants bound");if(constants[i].length)memcpy(o.constants[i],constants[i].bytes,constants[i].length);}
  p->_pipeline=owner(self)->backend->compilePipeline(a->_library->_library,a->_name.UTF8String,b->_library->_library,b->_name.UTF8String,o);}
 catch(const std::exception &e) {[p release];return metalError(error,e.what());}return p;
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options reflection:(MTLRenderPipelineReflection **)reflection error:(NSError **)error {CENSUS();
 if(reflection)*reflection=nil;
 if(options!=MTLPipelineOptionNone)return metalError(error,"pipeline reflection/archive options deferred");
 return [self newRenderPipelineStateWithDescriptor:d error:error];
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
__attribute__((unused)) static Class makeDeviceClass(Class base) {
 if(!base || class_getInstanceSize(base)!=528) {
  failureLog("FUXI MTL: _MTLDevice size guard FAIL got %zu expected 528\n",base?class_getInstanceSize(base):0);return Nil;
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
      {[FuxiHeap class],"MTLHeap"},{[FuxiEvent class],"MTLEvent"},{[FuxiSharedEvent class],"MTLSharedEvent"},{[FuxiFence class],"MTLFence"},{[FuxiDepthStencil class],"MTLDepthStencilState"},
      {[FuxiQueue class],"MTLCommandQueue"},{[FuxiCommandBuffer class],"MTLCommandBuffer"},{[FuxiBlit class],"MTLBlitCommandEncoder"},{[FuxiRender class],"MTLRenderCommandEncoder"},
      {[FuxiSampler class],"MTLSamplerState"},{[FuxiTexture class],"MTLTexture"},{[FuxiLibrary class],"MTLLibrary"},{[FuxiFunction class],"MTLFunction"},{[FuxiPipeline class],"MTLRenderPipelineState"}}) {
  Protocol *p=objc_getProtocol(entry.second);rejectProtocol(entry.first,p);if(p)class_addProtocol(entry.first,p);
 }
}
