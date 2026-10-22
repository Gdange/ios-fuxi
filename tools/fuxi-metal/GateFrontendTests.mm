/* Test policy injection only. Production always reads the protected file. */
#include "ClientGate.hpp"
#include "BackboardGate.hpp"
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <string>
static unsigned policyReads;
static fuxi::ClientMode testPolicy(const char *,const char *process) {
 ++policyReads;
 if(!strcmp(process,"gate-test"))return getenv("FUXI_TEST_GATE_ALLOW")?fuxi::ClientMode::allow:fuxi::ClientMode::deny;
 std::string data=getenv("FUXI_TEST_GATE_CONFIG")?getenv("FUXI_TEST_GATE_CONFIG"):"";size_t n=data.size();data.push_back(0);
 return fuxi::parseClients(data.data(),n,process,getenv("FUXI_TEST_BOOTARGS"));
}
static bool testAdmission(){const char *dir=getenv("FUXI_TEST_GATE_DIR");assert(dir);
 return fuxi::backboardGate.admit("aaaaaaaa-1111-2222-3333-444444444444",open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW));}
static unsigned preferenceWrites,setupEnvironments;
static void testPreference(const char *key){
 assert(!strcmp(key,"CAEnableFramebufferCompression") || !strcmp(key,"CAEnableIOSurfaceCompression"));++preferenceWrites;
}
static int testEnvironment(const char *key,const char *value,int overwrite){
 if(!strcmp(key,"TU_DEBUG")){assert(!strcmp(value,"sysmem,nocb") && !overwrite);}
 else {assert(!strcmp(key,"CA_DISABLE_FRAMEBUFFER_COMPRESSION") && !strcmp(value,"1") && overwrite);}
 ++setupEnvironments;return 0;
}
#define FUXI_MTL_SET_CA_PREFERENCE(key) testPreference(key)
#define FUXI_MTL_SET_ENV(key,value,overwrite) testEnvironment(key,value,overwrite)
#define FUXI_MTL_READ_CLIENTS testPolicy
#define FUXI_MTL_BACKBOARD_ADMIT() testAdmission()
#define FUXI_MTL_BACKBOARD_MODE() fuxi::backboardMode(getenv("FUXI_TEST_BOOTARGS"))
static std::atomic<double> testSurveyTime{100};
#define FUXI_MTL_SURVEY_NOW() testSurveyTime.load()
static std::atomic<unsigned> backboardExits{0};
static void testSurveyDeadline(void (^callback)());
#define FUXI_MTL_SURVEY_TIMER(callback) testSurveyDeadline(callback)
#define FUXI_MTL_BACKBOARD_EXIT() (void)++backboardExits
#define _NSGetExecutablePath testExecutablePath
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
static void (^surveyDeadline)();static unsigned deadlineArms;
static void testSurveyDeadline(void (^callback)()){assert(!surveyDeadline);surveyDeadline=[callback copy];++deadlineArms;}
extern "C" int testExecutablePath(char *path,uint32_t *size) {
 std::string s="/diagnostic/";s+=getenv("FUXI_TEST_PROCESS")?getenv("FUXI_TEST_PROCESS"):"gate-test";
 assert(*size>s.size());strcpy(path,s.c_str());return 0;
}
@interface FuxiGateBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiGateBase
@end
@interface FuxiSurveyDrawable : NSObject { @public unsigned presents; }
@end
@implementation FuxiSurveyDrawable
- (void)present {++presents;}
- (void)presentAtTime:(CFTimeInterval)time {(void)time;++presents;}
- (void)presentAfterMinimumDuration:(CFTimeInterval)time {(void)time;++presents;}
@end
int main() {
 @autoreleasepool {
  Class cls=makeDeviceClass([FuxiGateBase class]);assert(cls);
  for(unsigned i=0;i<64;++i) {
   // An invalid port would fail after policy evaluation as well; no IOKit or
   // backend constructor is reachable through the denied branch.
   assert(![[cls alloc] initWithAcceleratorPort:(getenv("FUXI_TEST_GATE_ALLOW")?0:1)]);
  }
  unsigned expected=clientMode==fuxi::ClientMode::allow?(!strcmp(clientProcess,"backboardd")?2u:1u):0u;
  assert(preferenceWrites==expected && setupEnvironments==expected);
  assert(policyReads==1 && fuxi::Backend::constructed==0 && fuxi::Backend::live==0 && fuxi::Backend::submissions==0);
  if(getenv("FUXI_TEST_GATE_EXPECT"))assert((clientMode==fuxi::ClientMode::allow)==!strcmp(getenv("FUXI_TEST_GATE_EXPECT"),"allow"));
  if(getenv("FUXI_TEST_GATE_SURVEY")){
   assert(backboardSurvey.active() && !fuxi::backboardGate.blocked());
   // A single refusal persists denial immediately, even beyond the normal log cap.
   failureCount=1000;failureLog("survey distinct first\n");assert(backboardExits==0 && fuxi::backboardGate.blocked());
   for(unsigned i=0;i<100;++i)failureLog("survey distinct first\n");assert(backboardSurvey.distinct()==1 && deadlineArms==1 && surveyDeadline);
   id device=[NSObject new];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
   objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
   FuxiQueue *queue=[FuxiQueue new];queue->_device=[device retain];
   FuxiEvent *event=[FuxiEvent new];event->_device=[device retain];
   FuxiSurveyDrawable *drawable=[FuxiSurveyDrawable new];__block unsigned completed=0,scheduled=0;
   int submissions=fuxi::Backend::submissions;
   for(unsigned i=0;i<3;++i){FuxiCommandBuffer *cb=(FuxiCommandBuffer *)[queue commandBuffer];
    [cb addScheduledHandler:^(id<MTLCommandBuffer> b){assert(b.status==MTLCommandBufferStatusCompleted);++scheduled;}];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b){assert(b.status==MTLCommandBufferStatusCompleted && !b.error);++completed;}];
    // No-op waits never stall, and signal/listener/present bookkeeping completes.
    [cb encodeWaitForEvent:(id)event value:999];[cb encodeSignalEvent:(id)event value:i+1];
    if(!i)[cb presentDrawable:(id)drawable];else if(i==1)[cb presentDrawable:(id)drawable atTime:0];else [cb presentDrawable:(id)drawable afterMinimumDuration:0];
    cb->_rejected=YES;[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted);
   }
   assert(completed==3 && scheduled==3 && drawable->presents==3 && [event signaledValue]==3 && fuxi::Backend::submissions==submissions);
   if(!strcmp(getenv("FUXI_TEST_GATE_SURVEY"),"cap")){
    for(unsigned i=1;i<64;++i)failureLog("survey distinct %u\n",i);
    assert(backboardSurvey.distinct()==64 && backboardExits==1);
    failureLog("survey overflow\n");finishSurvey();assert(backboardExits==1);
   }else{
    testSurveyTime=119.999;finishSurvey();assert(backboardExits==0);testSurveyTime=120;surveyDeadline();assert(backboardExits==1);surveyDeadline();assert(backboardExits==1);
   }
   testSurveyTime=200;surveyDeadline();assert(backboardExits==1);[surveyDeadline release];surveyDeadline=nil;
   [drawable release];[event release];[queue release];[device release];
  }
  if(getenv("FUXI_TEST_GATE_FAULT")) {
   // Exercise actual commit bookkeeping: only a defined, stored IOSurface render
   // completion qualifies; empty/event-only/ordinary-texture work cannot mark good.
   fuxi::Backend::modelTextures=true;
   id device=[NSObject new];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
   objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
   FuxiQueue *queue=[FuxiQueue new];queue->_device=[device retain];
   FuxiTexture *texture=[FuxiTexture new];texture->_device=[device retain];texture->_texture=owner(device)->backend->allocateTexture();
   auto draw=[&](uint32_t store){FuxiCommandBuffer *cb=(FuxiCommandBuffer *)[queue commandBuffer];
    fuxi::Operation op{};op.texture=texture->_texture;op.clearOnly=true;op.store=store;op.clear[3]=1;
    cb->_ops.push_back(op);[cb->_resources addObject:texture];[cb commit];assert(cb.status==MTLCommandBufferStatusCompleted);};
   auto good=[&]{char data[128]={};std::string path=getenv("FUXI_TEST_GATE_DIR");path+="/gate.state";
    int fd=open(path.c_str(),O_RDONLY);assert(fd>=0);assert(read(fd,data,sizeof(data)-1)>0);close(fd);return strstr(data," good\n")!=nullptr;};
   for(unsigned i=0;i<120;++i){[[queue commandBuffer] commit];draw(1);}assert(!good());
   texture->_surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@256,(id)kIOSurfaceHeight:@256,
    (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfacePixelFormat:@0x42475241});
   assert(texture->_surface && !IOSurfaceLock(texture->_surface,0,nullptr));IOSurfaceIncrementUseCount(texture->_surface);
   for(unsigned i=0;i<120;++i)draw(0);assert(!good());
   for(unsigned i=0;i<fuxi::BackboardGate::goodFrames-1;++i)draw(1);assert(!good());draw(1);assert(good());
   [texture release];[queue release];[device release];
   // Even a failure after qualification and beyond the ordinary log cap latches.
   failureCount=64;failureLog("test failure after good\n");assert(fuxi::backboardGate.blocked());
   // A latched render server exits so launchd respawns it on the CPU path.
   assert(backboardExits==1);
  }
 }
 puts("PASS: gated init, 64 attempts one policy read/log, executable identity independent of argv");
}
