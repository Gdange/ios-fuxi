/* Test policy injection only. Production always reads the protected file. */
#include "ClientGate.hpp"
#include <cassert>
static unsigned policyReads;
static fuxi::ClientMode testPolicy(const char *,const char *process) {
 ++policyReads;assert(!strcmp(process,"gate-test"));return fuxi::ClientMode::deny;
}
#define FUXI_MTL_READ_CLIENTS testPolicy
#define _NSGetExecutablePath testExecutablePath
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
extern "C" int testExecutablePath(char *path,uint32_t *size) {
 const char *s="/diagnostic/gate-test";assert(*size>strlen(s));strcpy(path,s);return 0;
}
@interface FuxiGateBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiGateBase
@end
int main() {
 @autoreleasepool {
  Class cls=makeDeviceClass([FuxiGateBase class]);assert(cls);
  for(unsigned i=0;i<64;++i) {
   // An invalid port would fail after policy evaluation as well; no IOKit or
   // backend constructor is reachable through the denied branch.
   assert(![[cls alloc] initWithAcceleratorPort:1]);
  }
  assert(policyReads==1 && fuxi::Backend::live==0 && fuxi::Backend::submissions==0);
 }
 puts("PASS: denied init nil, 64 attempts one policy read/log, executable identity independent of argv");
}
