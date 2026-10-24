/* Pause a real bridge artifact read; object and Rust product hits must proceed. */
#include "Backend.hpp"
#include <cassert>
#include <fstream>
#include <future>
#include <iterator>
static decltype(fuxi::Translator::get().compile) original;
static std::mutex pauseMutex;static std::condition_variable pauseWake;
static bool entered=false,released=false;static std::atomic<bool> intercept{true};
static size_t pauseRead(void *,const uint8_t *,const uint8_t **out){
 *out=nullptr;std::unique_lock<std::mutex> lock(pauseMutex);entered=true;pauseWake.notify_all();pauseWake.wait(lock,[]{return released;});return 0;
}
static void noWrite(void *,const uint8_t *,const uint8_t *,size_t){}static void noOutcome(void *,uint32_t){}
static int compile(void *v,const char *vn,void *f,const char *fn,void *gipa,uint64_t instance,uint64_t physical,const fuxi::CompileOptions *options,fuxi::Translation *out,char *error,size_t cap){
 if(!intercept.exchange(false))return original(v,vn,f,fn,gipa,instance,physical,options,out,error,cap);
 auto &tr=fuxi::Translator::get();int context=0;fuxi::Translator::Scope scope(tr.artifactScope,&context,pauseRead,noWrite,noOutcome);
 return original(v,vn,f,fn,gipa,instance,physical,options,out,error,cap);
}
int main(int argc,char **argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[2048]{};auto *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib && tr.artifactScope);
 {
  fuxi::Backend a,b;fuxi::CompileOptions options{};options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
  options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
  auto *base=a.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
  original=tr.compile;tr.compile=compile;auto changed=options;changed.mask=14;
  auto miss=std::async(std::launch::async,[&]{return a.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",changed);});
  {std::unique_lock<std::mutex> lock(pauseMutex);assert(pauseWake.wait_for(lock,std::chrono::seconds(10),[]{return entered;}));}
  auto hits=std::async(std::launch::async,[&]{
   auto *p=a.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);assert(p==base);a.destroy(p);
   p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);b.destroy(p);
  });
  bool completed=hits.wait_for(std::chrono::seconds(10))==std::future_status::ready;
  {std::lock_guard<std::mutex> lock(pauseMutex);released=true;}pauseWake.notify_all();
  hits.get();auto *p=miss.get();tr.compile=original;assert(completed);a.destroy(p);a.destroy(base);
 }
 tr.close(lib);puts("PASS: paused artifact miss holds neither Backend nor Rust product-cache mutex; object/product hits complete independently");
}
