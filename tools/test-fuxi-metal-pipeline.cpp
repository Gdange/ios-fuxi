/* Original integration test: actual AIR translator + actual software Vulkan backend.
 * GPU behavior here is host-only, not Turnip/A740 device qualification. */
#include "fuxi-metal/Backend.hpp"
#include <fstream>
#include <iterator>
#include <cassert>
static void require(bool ok,const char *message) {if(!ok)throw std::runtime_error(message);}
int main(int argc,char **argv) {
 try {
  require(argc==2,"supply our built metallib");
  std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
  auto &tr=fuxi::Translator::get();char error[2048]{};
  require(!tr.open(nullptr,0,error,sizeof(error)),"accepted empty library");
  auto corrupt=bytes;require(corrupt.size()>32,"missing metallib");corrupt[0]^=0xff;
  require(!tr.open(corrupt.data(),corrupt.size(),error,sizeof(error)),"accepted corrupt library header");
  void *library=tr.open(bytes.data(),bytes.size(),error,sizeof(error));require(library,error);
  require(!tr.function(library,"missing_entry",error,sizeof(error)),"accepted unknown function");
  require(tr.function(library,"fuxi_vertex",error,sizeof(error))==1,"vertex metadata");
  require(tr.function(library,"fuxi_fragment",error,sizeof(error))==2,"fragment metadata");
  for(unsigned reopen=0;reopen<2;++reopen) {
   fuxi::Backend backend;auto *pipeline=backend.compilePipeline(library,"fuxi_vertex",library,"fuxi_fragment");
   auto *texture=backend.allocateTexture();auto *buffer=backend.allocate(256*256*4+512);
   for(unsigned round=0;round<2;++round) {
    memset(buffer->contents,0x39,buffer->length);
    fuxi::Operation render{};render.texture=texture;render.pipeline=pipeline;render.clear[3]=1;
    fuxi::Operation copy{};copy.texture=texture;copy.dst=buffer;copy.to=256;
    backend.submit({render,copy});
    auto *raw=(uint8_t *)buffer->contents;unsigned red=0,black=0;
    for(unsigned i=0;i<256;++i)require(raw[i]==0x39 && raw[buffer->length-256+i]==0x39,"sentinel corruption");
    auto *p=raw+256;
    for(unsigned i=0;i<65536;++i) {
     auto *x=p+i*4;require(!x[0] && !x[1] && x[3]==255 && (x[2]==255 || !x[2]),"invalid BGRA pixel");
     if(x[2])++red;else ++black;
    }
    require(red>=8064 && red<=8320 && red+black==65536,"triangle pixel count");
    const unsigned samples[][3]={{128,128,255},{96,176,255},{160,176,255},{0,0,0},{128,26,0},{64,64,0}};
    for(auto &s:samples)require(p[(s[1]*256+s[0])*4+2]==s[2],"inside/outside pixel");
    printf("PASS: translated pipeline BGRA8 red %u clear %u six samples sentinel edges round %u reopen %u\n",red,black,round,reopen);
   }
   backend.destroy(buffer);backend.destroy(texture);backend.destroy(pipeline);
  }
  tr.close(library);
 }catch(const std::exception &e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
