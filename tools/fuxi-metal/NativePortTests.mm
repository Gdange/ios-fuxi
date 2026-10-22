/* Mac tests against the actual externally patched NativeMetal frontend.
 * No Vulkan device is opened; null handles and post-latch CPU bookkeeping only. */
#include <cassert>
#include <string>
#include <vector>
#ifndef FUXI_NATIVE_FRONTEND
#error Supply the prepared external NativeBridgeMetalFrontend.mm
#endif
#include FUXI_NATIVE_FRONTEND
static bool latched;
static unsigned completions;
static std::vector<std::string> failures;
static void failed(const char *why){failures.emplace_back(why);latched=true;}
int main(){@autoreleasepool{
 FuxiNativeHooks hooks={FUXI_NATIVE_ABI,sizeof(FuxiNativeHooks),failed,[](){return latched;},[](uint64_t,double,uint64_t,bool){++completions;}};
 fuxi_native_metal_set_hooks(&hooks);
 NBDeviceMethods *methods=[NBDeviceMethods new];id<MTLDevice> dev=(id)methods;
 NSError *error=nil;
 assert(![(id)dev newPipelineLibraryWithFilePath:@"optional.pipelinelib" error:&error] && error && failures.empty());
 (void)nbError(@"descriptor refused");assert(latched && failures.back()=="descriptor refused");
 refuseSurface(1,nullptr,nil,1,0,0);assert(failures.back().find("IOSurface refused")!=std::string::npos);
 error=nil;id<MTLRenderPipelineState> render=[dev newRenderPipelineStateWithDescriptor:[MTLRenderPipelineDescriptor new] error:&error];
 assert(render && !error && ![(NBRenderPipeline *)render handle]);
 id<MTLComputePipelineState> compute=[dev newComputePipelineStateWithFunction:(id)[NSObject new] error:&error];
 assert(compute && !error && ![(NBComputePipeline *)compute handle]);
 NBCommandBuffer *command=[NBCommandBuffer new];__block unsigned scheduledCount=0,completedCount=0;
 [command addScheduledHandler:^(id<MTLCommandBuffer> cb){assert(cb.status==MTLCommandBufferStatusCompleted);++scheduledCount;}];
 [command addCompletedHandler:^(id<MTLCommandBuffer> cb){assert(cb.status==MTLCommandBufferStatusCompleted && !cb.error);++completedCount;}];
 [command commit];[command waitUntilScheduled];[command waitUntilCompleted];
 assert(command.fuxiNoop && scheduledCount==1 && completedCount==1 && !completions);
 size_t before=failures.size();
 @try {[[NBObject new] doesNotRecognizeSelector:sel_registerName("n1Unsupported")];assert(false);}
 @catch(NSException *e){assert(e && failures.size()>before);}
 puts("PASS: actual native refusal/surface/exception hooks, optional fallback, survey placeholders and no-op completion");
}}
