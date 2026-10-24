/* Authored AIR only: compare products across new pipeline pairs, not phone FPS. */
#include "Backend.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc,char **argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[2048]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib);
 {
 fuxi::Backend b;fuxi::CompileOptions base{};
 base.attributes[0]={29,0,0};base.attributes[1]={29,8,0};base.attributes[2]={9,16,0};base.strides[0]=20;
 base.constantCount[1]=1;base.constants[1][0].type=53;base.constants[1][0].size=1;base.constants[1][0].bytes[0]=1;
 auto start=std::chrono::steady_clock::now();
 for(unsigned i=0;i<104;++i){
  auto options=base;options.mask=i%16;options.blend=1;options.srcRGB=i/16%6;
  if(i>=96){options.constants[1][0].bytes[0]=(i&1);options.attributes[1].offset=(i&2)?4:8;options.strides[0]=(i&4)?24:20;}
  auto *p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
  uint8_t hash[32]{};assert(tr.product_hash(p->translation,hash));
  printf("product %u ",i);for(auto byte:hash)printf("%02x",byte);puts("");b.destroy(p);
 }
 printf("elapsed_ms %.3f\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
 }
 tr.close(lib);
}
