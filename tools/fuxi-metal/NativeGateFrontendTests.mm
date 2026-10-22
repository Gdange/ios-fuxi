/* Mac CPU model: the real gate delegates only after admission and preflight. */
#import <Foundation/Foundation.h>
#include "FuxiNativeABI.h"
#include "ClientGate.hpp"
#include "BackboardGate.hpp"
#include <cassert>
#include <string>
#include <fstream>
static FuxiNativeHooks nativeHooks;
static unsigned nativeOpens,preflights,exits;
static id nativeObject;
static fuxi::ClientMode nativePolicy(const char *,const char *process){
 std::string config=getenv("N1_CONFIG")?:"";size_t n=config.size();config.push_back(0);
 return fuxi::parseClients(config.data(),n,process,getenv("N1_BOOTARGS"));
}
static bool nativeAdmission(){return fuxi::backboardGate.admit("aaaaaaaa-1111-2222-3333-444444444444",open(getenv("N1_GATE"),O_RDONLY|O_DIRECTORY|O_NOFOLLOW));}
static id nativeOpen(const char *process,uint64_t registry,const FuxiNativeHooks *hooks){
 assert(!strcmp(process,"backboardd") && registry==42);++nativeOpens;nativeHooks=*hooks;
 if(getenv("N1_EXCEPTION"))[NSException raise:NSInvalidArgumentException format:@"injected open exception"];
 if(getenv("N1_NIL"))return nil;
 return nativeObject;
}
static int registryID(unsigned port,uint64_t *value){assert(port==1);*value=42;return 0;}
static bool preflight(unsigned port){assert(port==1);++preflights;return !getenv("N1_PREFLIGHT_FAIL");}
#define FUXI_MTL_READ_CLIENTS nativePolicy
#define FUXI_MTL_BOOTARGS(out,size) (snprintf(out,size,"%s",getenv("N1_BOOTARGS")?:""),true)
#define FUXI_MTL_REGISTRY_ID(port,value) registryID(port,value)
#define FUXI_MTL_NATIVE_PREFLIGHT(port) preflight(port)
#define FUXI_MTL_NATIVE_OPEN(process,registry,hooks) nativeOpen(process,registry,hooks)
#define FUXI_MTL_BACKBOARD_ADMIT() nativeAdmission()
#define FUXI_MTL_BACKBOARD_MODE() fuxi::backboardMode(getenv("N1_BOOTARGS"))
#define FUXI_MTL_BACKBOARD_EXIT() (void)++exits
#define FUXI_MTL_SET_CA_PREFERENCE(key) (void)key
#define FUXI_MTL_SET_ENV(key,value,overwrite) ((void)key,(void)value,(void)overwrite,0)
#define _NSGetExecutablePath nativeExecutablePath
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
extern "C" int nativeExecutablePath(char *out,uint32_t *size){
 const char *path=getenv("N1_PROCESS")?:"/diagnostic/backboardd";
 assert(strlen(path)<*size);strcpy(out,path);return 0;
}
@interface NativeGateBase : NSObject {uint8_t bytes[520];}
@end
@implementation NativeGateBase
@end
int main(){@autoreleasepool{
 Class cls=makeDeviceClass([NativeGateBase class]);assert(cls);
 nativeObject=[NSObject new];id device=[[cls alloc] initWithAcceleratorPort:1];
 bool admitted=clientMode==fuxi::ClientMode::allow;
 if(clientMode==fuxi::ClientMode::census){assert(device && !nativeOpens && !preflights);}
 else if(!admitted){assert(!device && !nativeOpens && !preflights);}
 else if(getenv("N1_PREFLIGHT_FAIL")){assert(!device && !nativeOpens && preflights==1 && exits);}
 else if(getenv("N1_EXCEPTION") || getenv("N1_NIL")){assert(!device && nativeOpens==1 && exits);}
 else {
  assert(device==nativeObject && nativeOpens==1 && preflights==1);
  assert(!fuxi::Backend::constructed && !nativeHooks.blocked());
  for(unsigned i=0;i<120;++i)nativeHooks.completed(1000,1.0,1,false);
  {std::ifstream record(std::string(getenv("N1_GATE"))+"/gate.state");std::string text;std::getline(record,text);assert(text.find(" armed")!=std::string::npos);}
  for(unsigned i=0;i<120;++i)nativeHooks.completed(1000,1.0,1,true);
  {std::ifstream record(std::string(getenv("N1_GATE"))+"/gate.state");std::string text;std::getline(record,text);assert(text.find(" good")!=std::string::npos);}
  for(const char *why:{"pipeline creation failed","device lost","IOSurface refused","exception"})nativeHooks.failure(why);
  assert(nativeHooks.blocked());
  if(backboardSurvey.active())assert(!exits && backboardSurvey.distinct()==4);
  else assert(exits==4);
  assert(![[cls alloc] initWithAcceleratorPort:1] && nativeOpens==1);
 }
 assert(!fuxi::Backend::constructed && !fuxi::Backend::submissions);
 [device release];[nativeObject release];
 puts("PASS: native admission/delegation, census isolation, preflight, callbacks and exception/nil fallback");
}}
