/* Linux: CI AIR through the actual pinned bridge, imported source and target. */
#include "fuxi-metal/Backend.hpp"
#include <fstream>
#include <iterator>
#include <cassert>
static void require(bool ok,const char *why) {if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv) {
 try {
  require(argc==2,"supply CI metallib");
  std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
  auto &tr=fuxi::Translator::get();char error[2048]{};
  void *lib=tr.open(bytes.data(),bytes.size(),error,sizeof(error));require(lib,error);
  require(tr.function(lib,"fuxi_quad",error,sizeof(error))==1,"quad stage");
  require(tr.function(lib,"fuxi_sample",error,sizeof(error))==2,"sample stage");
  fuxi::Backend backend;
  auto *p=backend.compilePipeline(lib,"fuxi_quad",lib,"fuxi_sample");
  auto *cached=backend.compilePipeline(lib,"fuxi_quad",lib,"fuxi_sample");
  require(p->pushOffset==64 && p->pushSize==4 && p->pushStages==16,"planned fragment push range");
  uint32_t rule=0;memcpy(&rule,p->pushBytes,4);require(rule==0x2222,"planned BGRA8 ColorWriteRule");
  require(p->descriptors.size()==1 && p->descriptors[0].binding==0 && p->setLayout,"sampled descriptor layout");
  require(cached->pushOffset==p->pushOffset && !memcmp(cached->pushBytes,p->pushBytes,4),"cache push bytes");
  void *source=nullptr,*target=nullptr;
  require(!posix_memalign(&source,65536,262144) && !posix_memalign(&target,65536,262144),"host allocations");
  auto *input=(uint8_t *)source;
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
   auto *v=input+(y*256+x)*4;v[0]=x;v[1]=y;v[2]=(x*3+y*5)%256;v[3]=255;
  }
  auto *src=backend.importTexture(source,262144,1024),*dst=backend.importTexture(target,262144,1024);
  auto *alias=backend.importTexture(target,262144,1024);
  auto *nearest=backend.allocateSampler(false,false),*linear=backend.allocateSampler(true,true);
  auto *buffer=backend.allocate(262656);
  fuxi::Operation draw{};draw.texture=dst;draw.pipeline=p;draw.vertices=6;draw.sourceTexture=src;draw.sampler=nearest;
  auto refuse=[&](fuxi::Operation bad) {
   auto before=dst->layout;
   try {backend.submit({bad});throw std::logic_error("accepted invalid sampled draw");}catch(const std::runtime_error&) {}
   require(!backend.poisoned && dst->layout==before,"rejection changed ownership/layout");
  };
  auto bad=draw;bad.sourceTexture=nullptr;refuse(bad);
  bad=draw;bad.sampler=nullptr;refuse(bad);
  bad=draw;bad.sourceTexture=dst;refuse(bad);
  bad=draw;bad.sourceTexture=alias;refuse(bad);
  bad=draw;bad.vertices=4;refuse(bad);
  fuxi::Operation read{};read.texture=dst;read.dst=buffer;read.to=256;
  for(auto *pipeline:{p,cached})for(auto *sampler:{nearest,linear}) {
   memset(target,0,262144);memset(buffer->contents,0x39,buffer->length);
   draw.pipeline=pipeline;draw.sampler=sampler;backend.submit({draw,read});
   require(!memcmp(source,target,262144),"same-memory sampled target pixels");
   auto *raw=(uint8_t *)buffer->contents;require(!memcmp(source,raw+256,262144),"sampled readback pixels");
   for(unsigned i=0;i<256;++i)require(raw[i]==0x39 && raw[262400+i]==0x39,"sentinel edges");
  }
  backend.destroy(buffer);backend.destroy(nearest);backend.destroy(linear);
  backend.destroy(alias);backend.destroy(src);backend.destroy(dst);free(source);free(target);
  backend.destroy(p);backend.destroy(cached);tr.close(lib);
  puts("PASS: CI AIR quad, cache, ColorWriteRule, imported source/target, nearest/linear, 65536 pixels, sentinels, missing-resource/feedback refusal");
 }catch(const std::exception &e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
