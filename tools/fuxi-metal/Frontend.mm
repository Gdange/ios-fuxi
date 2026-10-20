/* Transfer-only Metal diagnostic. Original implementation; no Apple code. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>
#include <dlfcn.h>
#ifndef FUXI_MTL_BACKEND_HEADER
#define FUXI_MTL_BACKEND_HEADER "Backend.hpp"
#endif
#include FUXI_MTL_BACKEND_HEADER
extern "C" void _objc_msgForward(void);
static void unsupported(id object, SEL selector) {
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
 return method?[NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)]:nil;
}
- (void)doesNotRecognizeSelector:(SEL)s {unsupported(self,s);[super doesNotRecognizeSelector:s];}
- (void)forwardInvocation:(NSInvocation *)inv {
 unsupported(self,inv.selector);
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
- (id)device {return _device;}
- (void *)contents {return _buffer->contents;}
- (NSUInteger)length {return _buffer->length;}
- (MTLStorageMode)storageMode {return MTLStorageModeShared;}
- (MTLCPUCacheMode)cpuCacheMode {return MTLCPUCacheModeDefaultCache;}
- (MTLHazardTrackingMode)hazardTrackingMode {return MTLHazardTrackingModeTracked;}
- (MTLResourceOptions)resourceOptions {return MTLResourceStorageModeShared;}
- (NSUInteger)allocatedSize {return _buffer->allocated;}
- (NSString *)label {return _label;}
- (void)setLabel:(NSString *)s {[_label release];_label=[s copy];}
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
@implementation FuxiQueue
- (void)dealloc {[_device release];[_label release];[super dealloc];}
- (id)device {return _device;}
- (NSString *)label {return _label;}
- (void)setLabel:(NSString *)s {[_label release];_label=[s copy];}
- (id)commandBuffer {
 FuxiCommandBuffer *b=[FuxiCommandBuffer new];b->_queue=[self retain];
 b->_resources=[NSMutableArray new];b->_handlers=[NSMutableArray new];return [b autorelease];
}
@end
@implementation FuxiCommandBuffer
- (void)dealloc {[_queue release];[_resources release];[_handlers release];[_label release];[_error release];[super dealloc];}
- (id)device {return _queue->_device;}
- (id)commandQueue {return _queue;}
- (NSString *)label {return _label;}
- (void)setLabel:(NSString *)s {[_label release];_label=[s copy];}
- (MTLCommandBufferStatus)status {return _status;}
- (NSError *)error {return _error;}
- (BOOL)retainedReferences {return YES;}
- (id)blitCommandEncoder {
 if(_status!=MTLCommandBufferStatusNotEnqueued || _encoding) {unsupported(self,_cmd);return nil;}
 FuxiBlit *e=[FuxiBlit new];e->_command=[self retain];_encoding=YES;return [e autorelease];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
 if(!handler) invalid(@"nil completion handler");
 if(_status>=MTLCommandBufferStatusCompleted) {handler((id)self);return;}
 id copied=[handler copy];[_handlers addObject:copied];[copied release];
}
- (void)commit {
 if(_status!=MTLCommandBufferStatusNotEnqueued || _encoding) invalid(@"commit twice or active encoder");
 _status=MTLCommandBufferStatusCommitted;
 // Deliberately synchronous M4 queue: no dispatch worker or asynchronous promise.
 try {owner(_queue->_device)->backend->submit(_ops);_status=MTLCommandBufferStatusCompleted;}
 catch(const std::exception &e) {
  fprintf(stderr,"FUXI MTL: submit FAIL %s\n",e.what());_status=MTLCommandBufferStatusError;
  _error=[[NSError alloc] initWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
      userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:e.what()]}];
 }
 NSArray *handlers=[_handlers copy];[_handlers removeAllObjects];
 for(MTLCommandBufferHandler handler in handlers) handler((id)self);
 [handlers release];[_resources removeAllObjects];_ops.clear();
}
- (void)waitUntilCompleted {if(_status<MTLCommandBufferStatusCompleted) invalid(@"wait before commit");}
@end
@implementation FuxiBlit
- (void)dealloc {[_command release];[_label release];[super dealloc];}
- (id)device {return [_command device];}
- (NSString *)label {return _label;}
- (void)setLabel:(NSString *)s {[_label release];_label=[s copy];}
- (FuxiBuffer *)validate:(id)b offset:(NSUInteger)o size:(NSUInteger)n {
 if(_ended || !_command->_encoding || _command->_status!=MTLCommandBufferStatusNotEnqueued) invalid(@"inactive encoder");
 if(![b isKindOfClass:[FuxiBuffer class]] || [b device]!=[_command device]) invalid(@"foreign buffer/device");
 FuxiBuffer *buffer=b;
 if(o>buffer->_buffer->length || n>buffer->_buffer->length-o || ((o|n)&3)) invalid(@"M4 transfers require in-bounds 4-byte aligned offset and length");
 return buffer;
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {
 FuxiBuffer *dst=[self validate:b offset:r.location size:r.length];if(!r.length)return;
 [_command->_resources addObject:dst];_command->_ops.push_back({nullptr,dst->_buffer,0,r.location,r.length,v});
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)to size:(NSUInteger)n {
 FuxiBuffer *src=[self validate:s offset:so size:n],*dst=[self validate:d offset:to size:n];
 if(src==dst && n && so<to+n && to<so+n) invalid(@"overlapping copy");
 if(!n)return;
 [_command->_resources addObject:src];[_command->_resources addObject:dst];
 _command->_ops.push_back({src->_buffer,dst->_buffer,so,to,n,0});
}
- (void)endEncoding {if(_ended)invalid(@"endEncoding twice");_ended=YES;_command->_encoding=NO;}
@end
/* Methods are copied onto a runtime subclass. No fixed private layout/ivars are
 * read, initialized, or freed through the private superclass implementation. */
@interface FuxiDeviceMethods : FuxiObject
@end
@implementation FuxiDeviceMethods
- (id)init {unsupported(self,_cmd);[self release];return nil;}
- (id)initWithAcceleratorPort:(uint32_t)port {
 if(geteuid()!=0 || !port || owner(self)) {[self release];return nil;}
 void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
 auto getID=io?(int(*)(uint32_t,uint64_t *))dlsym(io,"IORegistryEntryGetRegistryEntryID"):nullptr;
 uint64_t registry=0;int rc=getID?getID(port,&registry):-1;
 if(io)dlclose(io);
 if(rc || !registry) {fprintf(stderr,"FUXI MTL: registryID FAIL\n");[self release];return nil;}
 FuxiOwner *state=[FuxiOwner new];state->registry=registry;
 try {state->backend=new fuxi::Backend;}
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
- (NSString *)name {return @"Fuxi Turnip Adreno 740 (M4 transfers only)";}
- (uint64_t)registryID {return owner(self)->registry;}
- (NSUInteger)maxBufferLength {return fuxi::Backend::maxBufferLength;}
- (BOOL)hasUnifiedMemory {return YES;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isHeadless {return YES;}
- (BOOL)isRemovable {return NO;}
- (BOOL)supportsFeatureSet:(NSUInteger)s {(void)s;return NO;}
- (BOOL)supportsFamily:(MTLGPUFamily)f {(void)f;return NO;}
- (BOOL)supportsTextureSampleCount:(NSUInteger)n {(void)n;return NO;}
- (BOOL)supportsRaytracing {return NO;}
- (BOOL)supportsFunctionPointers {return NO;}
- (BOOL)supportsDynamicLibraries {return NO;}
- (BOOL)supportsPullModelInterpolation {return NO;}
- (BOOL)supportsRasterOrderGroups {return NO;}
- (BOOL)areProgrammableSamplePositionsSupported {return NO;}
- (BOOL)areBarycentricCoordsSupported {return NO;}
- (BOOL)areRasterOrderGroupsSupported {return NO;}
- (MTLReadWriteTextureTier)readWriteTextureSupport {return MTLReadWriteTextureTierNone;}
- (MTLSize)maxThreadsPerThreadgroup {return MTLSizeMake(0,0,0);}
- (id)newCommandQueue {FuxiQueue *q=[FuxiQueue new];q->_device=[self retain];return q;}
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)options {
 // Reject write-combine/untracked/private/managed/sparse and unknown options.
 if(options!=MTLResourceStorageModeShared) {unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[FuxiBuffer new];b->_device=[self retain];
 try {b->_buffer=owner(self)->backend->allocate(n);}
 catch(const std::exception &e) {fprintf(stderr,"FUXI MTL: buffer FAIL %s\n",e.what());[b release];return nil;}
 return b;
}
- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)n options:(MTLResourceOptions)o {
 if(!bytes) {unsupported(self,_cmd);return nil;}
 FuxiBuffer *b=[self newBufferWithLength:n options:o];if(b)memcpy([b contents],bytes,n);return b;
}
// MTLAddDevice on 16.7 calls these hooks. Capabilities come exclusively from
// our overrides; no Apple family/limit table is installed in the base object.
- (void)initLimits {}
- (void)initFeatureQueries {}
- (id)_deviceWrapper {return self;}
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
      {[FuxiQueue class],"MTLCommandQueue"},{[FuxiCommandBuffer class],"MTLCommandBuffer"},{[FuxiBlit class],"MTLBlitCommandEncoder"}}) {
  Protocol *p=objc_getProtocol(entry.second);rejectProtocol(entry.first,p);if(p)class_addProtocol(entry.first,p);
 }
}
