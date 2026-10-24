#define FUXI_MTL_HOST_TEST
#include "CompileBoost.hpp"
#include <cassert>
#include <stdexcept>
#include <thread>
static thread_local unsigned enters=0,leaves=0;
static int refusal=0;
static int lease(int32_t command,fuxi_compile_reply *reply){
 if(command){++enters;if(refusal)return refusal;*reply={0,1,7,FC_ACTIVE,1};}
 else {++leaves;*reply={12000000,1,7,FC_DUTY,0};}
 return 0;
}
int main(){
 using namespace fuxi;
 TranslationPolicy off{},on{};on.compileBoost=true;
 {CompileBoostScope scope("off",off,lease);}assert(!enters && !leaves);
 {CompileBoostScope scope("pipeline",on,lease);
  {CompileBoostScope nested("stages",on,lease);}
  assert(enters==1 && !leaves);scope.finish();scope.finish();
 }assert(enters==1 && leaves==1);
 try {CompileBoostScope scope("exception",on,lease);throw std::runtime_error("compile");}
 catch(const std::runtime_error&){}assert(enters==2 && leaves==2);
 refusal=EAGAIN;
 {CompileBoostScope scope("thermal",on,lease);CompileBoostScope nested("stage",on,lease);}
 assert(enters==3 && leaves==2);refusal=0;
 std::thread worker([&]{CompileBoostScope scope("async",on,lease);assert(enters==1 && !leaves);});worker.join();
 assert(enters==3 && leaves==2);
 on.trace=true;{CompileBoostScope scope("trace",on,lease);}
 assert(enters==4 && leaves==3);
 fuxi_compile_reply reply{};assert(systemCompileBoost(1,&reply)==ENOTSUP);
 puts("PASS: compile scope disabled/denied, nested stages, balanced leave, exceptions, async thread isolation and voted-time trace");
}
