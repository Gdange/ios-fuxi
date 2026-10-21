/* Host-only model for Objective-C ownership/state tests; never linked into iOS. */
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <unistd.h>
namespace fuxi {
struct Buffer {void *contents;size_t length,allocated;};
struct Texture {size_t allocated{};};
struct Pipeline {bool sampled=false;};
struct Sampler {};
struct Operation {Buffer *src,*dst;size_t from,to,size;uint8_t value;Texture *texture{};Pipeline *pipeline{};float clear[4]{};Texture *sourceTexture{};Sampler *sampler{};uint32_t vertices=3;bool upload=false;};
class Backend {
public:
 static constexpr size_t maxBufferLength=16*1024*1024;
 static inline int live=0,submissions=0,constructed=0;bool poisoned=false;
 Backend() {++constructed;}
 Buffer *allocate(size_t n) {
  if(!n || n>maxBufferLength)throw std::runtime_error("length");
  auto *b=new Buffer{calloc(1,n),n,n};++live;return b;
 }
 void destroy(Buffer *b) {if(b){free(b->contents);delete b;--live;}}
 bool supportsHostImport() const {return false;}
 Texture *importTexture(void *,size_t,size_t) {throw std::runtime_error("no modeled host import");}
 Sampler *allocateSampler(bool,bool) {return new Sampler{};}
 void destroy(Sampler *s) {delete s;}
 Texture *allocateTexture() {throw std::runtime_error("M4 CPU model has no rendering");}
 Pipeline *compilePipeline(void *,const char *,void *,const char *) {throw std::runtime_error("M4 CPU model has no translation");}
 bool destroy(Texture *t) {delete t;return !poisoned;}
 void destroy(Pipeline *p) {delete p;}
 bool requiresSampling(const Pipeline *p) const {return p->sampled;}
 void submit(const std::vector<Operation>& ops) {
  if(getenv("FUXI_TEST_LOST")) {poisoned=true;throw std::runtime_error("modeled device loss");}
  if(getenv("FUXI_TEST_HANG"))for(;;)pause();
  ++submissions;
  for(auto &op:ops) {
   auto *dst=(uint8_t *)op.dst->contents+op.to;
   if(op.src)memcpy(dst,(uint8_t *)op.src->contents+op.from,op.size);
   else memset(dst,op.value,op.size);
  }
  if(getenv("FUXI_TEST_BAD_DATA") && !ops.empty()) ((uint8_t *)ops.back().dst->contents)[ops.back().dst->length-1]^=1;
 }
};
}
