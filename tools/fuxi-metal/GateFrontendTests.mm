/* Test policy injection only. Production always reads the protected file. */
#include "ClientGate.hpp"
#include <cassert>
#include <cstdlib>
static unsigned policyReads;
static fuxi::ClientMode testPolicy(const char *,const char *process) {
 ++policyReads;assert(!strcmp(process,"gate-test"));return getenv("FUXI_TEST_GATE_ALLOW")?fuxi::ClientMode::allow:fuxi::ClientMode::deny;
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
   assert(![[cls alloc] initWithAcceleratorPort:(getenv("FUXI_TEST_GATE_ALLOW")?0:1)]);
  }
  assert(policyReads==1 && fuxi::Backend::constructed==0 && fuxi::Backend::live==0 && fuxi::Backend::submissions==0);
 }
 puts("PASS: gated init, 64 attempts one policy read/log, executable identity independent of argv");
}
