/* Original Fuxi current-thread compile scope. Stages compile synchronously
 * inside the bridge call; the outer lease includes both stages and Turnip.
 */
#pragma once
#include <chrono>
#include <cstdint>
#include <cerrno>
#include "TranslationPolicy.hpp"
#include "../../xnu/fuxi-perf/fuxi_compile_abi.h"
namespace fuxi {
static_assert(sizeof(fuxi_compile_reply)==24,"compile lease wire ABI");
inline int systemCompileBoost(int32_t command,fuxi_compile_reply *reply) {
#if defined(__APPLE__) && !defined(FUXI_MTL_HOST_TEST)
 size_t size=sizeof(*reply);
 if(sysctlbyname("kern.fuxicompileboost",reply,&size,&command,sizeof(command)))return errno;
 return size==sizeof(*reply) && reply->version==1?0:EPROTO;
#else
 (void)command;(void)reply;return ENOTSUP;
#endif
}
using CompileBoostCall=int (*)(int32_t,fuxi_compile_reply *);
class CompileBoostScope {
 inline static thread_local unsigned depth=0;
 bool enabled,trace,outer=false,entered=false,finished=false;
 const char *phase;
 CompileBoostCall call;
 std::chrono::steady_clock::time_point start{};
 fuxi_compile_reply entry{};
 int enterError=0;
public:
 explicit CompileBoostScope(const char *label,TranslationPolicy policy=systemTranslationPolicy(),
     CompileBoostCall fn=systemCompileBoost):enabled(policy.compileBoost),trace(policy.trace),phase(label),call(fn) {
  if(!enabled)return;
  outer=depth++==0;
  if(outer){start=std::chrono::steady_clock::now();enterError=call(1,&entry);entered=enterError==0;}
 }
 void finish() noexcept {
  if(finished)return;finished=true;
  if(!enabled)return;
  --depth;if(!outer)return;
  auto end=std::chrono::steady_clock::now();fuxi_compile_reply reply{};
  int leaveError=entered?call(0,&reply):0;
  // A timeout can discard the reply; -1 means unknown, never zero boost.
  double voted=entered && !leaveError && reply.state!=FC_NONE?reply.boosted_ns/1000000.0:-1;
  if(trace)fprintf(stderr,"FUXI MTL: compile boost phase=%s enter_errno=%d leave_errno=%d cpu=%d state=%u scope_ms=%.3f voted_ms=%.3f\n",
      phase,enterError,leaveError,entered?entry.cpu:-1,reply.state,
      std::chrono::duration<double,std::milli>(end-start).count(),voted);
 }
 ~CompileBoostScope(){finish();}
 CompileBoostScope(const CompileBoostScope&)=delete;
 CompileBoostScope& operator=(const CompileBoostScope&)=delete;
};
}
