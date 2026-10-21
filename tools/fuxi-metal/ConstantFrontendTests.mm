/* Native public-runtime + Foundation tests, using our own class cluster.
 * No Apple private layout or GPU execution is exercised. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cassert>
static unsigned baseSets,childSets;
static Class clusterClass;
@interface FuxiConstantModel : NSObject <NSCopying>
- (void)setConstantValue:(const void *)value type:(MTLDataType)type atIndex:(NSUInteger)index;
- (void)setConstantValue:(const void *)value type:(MTLDataType)type withName:(NSString *)name;
- (void)setConstantValues:(const void *)value type:(MTLDataType)type withRange:(NSRange)range;
- (void)reset;
@end
@implementation FuxiConstantModel
+ (id)alloc {return class_createInstance(self==[FuxiConstantModel class] && clusterClass?clusterClass:self,0);}
+ (id)allocWithZone:(NSZone *)zone {(void)zone;return [self alloc];}
- (id)init {return self;}
- (id)copyWithZone:(NSZone *)zone {(void)zone;return class_createInstance(object_getClass(self),0);}
- (void)setConstantValue:(const void *)value type:(MTLDataType)type atIndex:(NSUInteger)index {(void)value;(void)type;(void)index;++baseSets;}
- (void)setConstantValue:(const void *)value type:(MTLDataType)type withName:(NSString *)name {(void)value;(void)type;(void)name;}
- (void)setConstantValues:(const void *)value type:(MTLDataType)type withRange:(NSRange)range {(void)value;(void)type;(void)range;}
- (void)reset {}
@end
@interface FuxiConstantChild : FuxiConstantModel
@end
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-designated-initializers" // Deliberate missing-super regression.
@implementation FuxiConstantChild
- (id)init {return self;} // No super init: allocation, not init, grants provenance.
- (void)setConstantValue:(const void *)value type:(MTLDataType)type atIndex:(NSUInteger)index {
 ++childSets;[super setConstantValue:value type:type atIndex:index];
}
@end
#pragma clang diagnostic pop
#define FUXI_MTL_CONSTANT_CLASS FuxiConstantModel
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
static NSData *snapshot(id obj) {return constantSnapshot((MTLFunctionConstantValues *)obj);}
static bool constant(NSData *data,uint32_t index,uint8_t value) {
 for(size_t i=0;i<data.length/sizeof(fuxi::Constant);++i) {
  const auto &c=((const fuxi::Constant *)data.bytes)[i];
  if(c.index==index && !c.name[0])return c.type==MTLDataTypeBool && c.size==1 && c.bytes[0]==value;
 }return false;
}
static IMP lateSuper;
static void lateIndex(id self,SEL sel,const void *value,MTLDataType type,NSUInteger index) {
 ((void(*)(id,SEL,const void *,MTLDataType,NSUInteger))lateSuper)(self,sel,value,type,index);
}
static id escapeNew(id self,SEL) {return class_createInstance((Class)self,0);}
static void lateReset(id,SEL) {}
int main() {@autoreleasepool {
 BOOL value=YES;FuxiConstantModel *model=[FuxiConstantChild new];
 [model setConstantValue:&value type:MTLDataTypeBool atIndex:0];
 assert(baseSets==1 && childSets==1 && constant(snapshot(model),0,1));
 NSData *frozen=[snapshot(model) retain];value=NO;
 [model setConstantValue:&value type:MTLDataTypeBool atIndex:0];
 assert(constant(frozen,0,1) && constant(snapshot(model),0,0));[frozen release];
 id copied=[model copy];assert(constant(snapshot(copied),0,0));[copied release];
 [model reset];assert(snapshot(model).length==0);
 BOOL range[2]={YES,NO};[model setConstantValues:range type:MTLDataTypeBool withRange:NSMakeRange(2,2)];
 assert(constant(snapshot(model),2,1) && constant(snapshot(model),3,0));
 [model setConstantValue:&value type:MTLDataTypeBool withName:@"flag"];assert(snapshot(model).length==3*sizeof(fuxi::Constant));
 [model setConstantValue:&value type:MTLDataTypeFloat4 atIndex:0];assert(!snapshot(model));
 [model reset];assert(snapshot(model).length==0);[model release];
 // Public allocator returns an internal subclass; its init never calls super.
 clusterClass=[FuxiConstantChild class];model=[FuxiConstantModel new];
 assert(object_getClass(model)==clusterClass && snapshot(model));[model release];clusterClass=Nil;
 Class late=objc_allocateClassPair([FuxiConstantChild class],"FuxiLateConstants",0);assert(late);
 SEL index=@selector(setConstantValue:type:atIndex:);
 lateSuper=class_getMethodImplementation([FuxiConstantChild class],index);
 assert(class_addMethod(late,index,(IMP)lateIndex,method_getTypeEncoding(class_getInstanceMethod([FuxiConstantChild class],index))));
 objc_registerClassPair(late);
 model=[late new];value=YES;[model setConstantValue:&value type:MTLDataTypeBool atIndex:7];
 assert(constant(snapshot(model),7,1));[model release];
 Class escaped=objc_allocateClassPair([FuxiConstantModel class],"FuxiEscapedConstants",0);assert(escaped);
 assert(class_addMethod(object_getClass((id)escaped),@selector(new),(IMP)escapeNew,"@@:"));objc_registerClassPair(escaped);
 model=[escaped new];[model setConstantValue:&value type:MTLDataTypeBool atIndex:0];assert(!snapshot(model));
 [model reset];assert(!snapshot(model));id oldCopy=[model copy];assert(!snapshot(oldCopy));[oldCopy release];[model release];
 model=[escaped new];assert(!snapshot(model));[model release]; // Hooking +new cannot invent allocation history.
 model=[[escaped alloc] init];assert(snapshot(model));[model release];
 model=class_createInstance([FuxiConstantChild class],0); // Pre-hook allocation stand-in.
 [model init];[model reset];assert(!snapshot(model));[model release];
 Class changed=objc_allocateClassPair([FuxiConstantModel class],"FuxiChangedConstants",0);assert(changed);objc_registerClassPair(changed);
 model=[changed new];assert(snapshot(model));
 assert(class_addMethod(changed,@selector(reset),(IMP)lateReset,"v@:"));[model reset];assert(!snapshot(model));[model release];
 model=[changed new];assert(snapshot(model));[model release];
 // Failure diagnostics are bounded even if a caller repeats an unsupported object.
 model=class_createInstance([FuxiConstantModel class],0);for(unsigned i=0;i<32;++i)assert(!snapshot(model));[model release];
 }puts("PASS: native constant observation; subclass/super, class cluster, init bypass, late registration, old allocation/copy/reset refusal, immutable snapshots, setters and diagnostics");}
