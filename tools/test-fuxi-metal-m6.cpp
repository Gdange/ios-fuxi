/* Actual Vulkan transfers and host import; no Apple SDK, shaders or device. */
#include "fuxi-metal/Backend.hpp"
#include <cassert>
#include <cstdio>
int main(int argc,char **argv) {
 (void)argv;
 try {
  fuxi::Backend backend;
  auto *src=backend.allocate(262144),*dst=backend.allocate(262656);
  auto *a=backend.allocateTexture(),*b=backend.allocateTexture();
  auto *nearest=backend.allocateSampler(false,false),*linear=backend.allocateSampler(true,true);
  for(unsigned i=0;i<262144;++i)((uint8_t *)src->contents)[i]=(i/4+i/1024)%251;
  fuxi::Operation upload{};upload.src=src;upload.texture=a;upload.upload=true;
  fuxi::Operation copy{};copy.sourceTexture=a;copy.texture=b;
  fuxi::Operation read{};read.texture=b;read.dst=dst;read.to=256;
  // Undefined input is refused before submission and layouts roll back.
  try {backend.submit({copy});assert(false);}catch(const std::runtime_error&) {}
  assert(a->layout==VK_IMAGE_LAYOUT_UNDEFINED && b->layout==VK_IMAGE_LAYOUT_UNDEFINED);
  for(unsigned repeat=0;repeat<4;++repeat) {
   memset(dst->contents,0x39,dst->length);backend.submit({upload,copy,read});
   assert(!memcmp(src->contents,(uint8_t *)dst->contents+256,262144));
   for(unsigned i=0;i<256;++i)assert(((uint8_t *)dst->contents)[i]==0x39 && ((uint8_t *)dst->contents)[262400+i]==0x39);
  }
  // The Linux software ICD may support the same standard import path that the
  // current NativeAdreno kernel cannot. Either import or a precise refusal is valid.
  void *host=nullptr;assert(!posix_memalign(&host,65536,262144));memcpy(host,src->contents,262144);
  if(backend.supportsHostImport()) {
   try {backend.importTexture((uint8_t *)host+1,262144,1024);assert(false);}catch(const std::runtime_error&) {}
   try {backend.importTexture(host,262144,2048);assert(false);}catch(const std::runtime_error&) {}
  }
  fuxi::Texture *imported=nullptr;
  try {imported=backend.importTexture(host,262144,1024);}
  catch(const std::runtime_error &e) {printf("REFUSED: host image import: %s\n",e.what());}
  if(imported) {
   assert(imported->imported && imported->mapped);
   read.texture=imported;backend.submit({read});assert(!memcmp(host,(uint8_t *)dst->contents+256,262144));
   upload.texture=imported;backend.submit({upload});assert(!memcmp(host,src->contents,262144));
   if(argc==2) {
    // This uses original GLSL control SPIR-V, NOT the AIR translator. The actual
    // translator/sampling contract is separately exercised by the Mac stage 5.
    auto *p=backend.compilePipeline((void *)1,"control_vertex",(void *)1,"control_fragment");
    fuxi::Operation render{};render.texture=imported;render.sourceTexture=a;render.pipeline=p;render.vertices=6;
    for(auto *sampler:{nearest,linear}) {
     memset(host,0,262144);render.sampler=sampler;backend.submit({render,read});
     assert(!memcmp(host,src->contents,262144));assert(!memcmp(host,(uint8_t *)dst->contents+256,262144));
    }
    backend.destroy(p);puts("PASS: GLSL control sampling into same-memory host target, nearest/linear, full-quad pixels");
   }
   backend.destroy(imported);puts("PASS: same-memory host image import/read/write");
  }
  free(host);backend.destroy(nearest);backend.destroy(linear);backend.destroy(a);backend.destroy(b);backend.destroy(src);backend.destroy(dst);
  puts("PASS: actual Vulkan upload/image copy/readback, sampler creation, four reuse rounds, sentinel edges, undefined-image refusal");
 }catch(const std::exception &e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
