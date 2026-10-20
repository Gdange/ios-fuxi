/* Host smoke adapter: synthetic private base, CPU backend, synthetic root/ID.
 * Compiled only by test-fuxi-metal-frontend.py. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
@interface FuxiTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiTestBase
@end
extern "C" id FuxiTestGetClass(const char *name) {
 return objc_getClass(!strcmp(name,"_MTLDevice")?"FuxiTestBase":name);
}
extern "C" uid_t FuxiTestGeteuid(void) {return getenv("FUXI_TEST_NOT_ROOT")?501:0;}
extern "C" unsigned FuxiTestAlarm(unsigned n) {return alarm(n?1:0);}
extern "C" id<MTLDevice> FuxiTestCreateDevice(void) {
 if(getenv("FUXI_TEST_NO_DEVICE"))return nil;
 Class cls=makeDeviceClass([FuxiTestBase class]);assert(cls);
 FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;state->registry=123;
 id device=[cls alloc];
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];return device;
}
