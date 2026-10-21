/* Host-only model for Objective-C ownership/state tests; never linked into iOS. */
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include "Translator.hpp"
namespace fuxi {
struct Buffer {void *contents;size_t length,allocated;};
struct Texture {size_t allocated{};uint32_t format=80;};
struct Pipeline {bool sampled=false;};
struct Sampler {};
struct Viewport {float x,y,width,height,minDepth,maxDepth;};
struct Rect {struct {int32_t x,y;}offset;struct {uint32_t width,height;}extent;};
struct BufferBind {Buffer *buffer{};uint64_t offset{};bool bytes=false;};
struct Operation {
 Buffer *src, *dst; size_t from, to, size; uint8_t value;
 Texture *texture{};Pipeline *pipeline{};float clear[4]{};
 Texture *sourceTexture{};Sampler *sampler{};uint32_t vertices=3;bool upload=false;
 Texture *samples[4]{};Sampler *samplers[4]{};BufferBind buffers[2][4]{};Buffer *indices{};uint64_t indexOffset{};uint32_t first{},strip{},load=2,store=1;
 Viewport viewport{0,256,256,-256,0,1};Rect scissor{{0,0},{256,256}};
};
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
 static size_t texelBytes(uint32_t f){return f==115?8:(f==1 || f==10)?1:f==30?2:4;}
 bool supportsHostImport() const {return false;}
 Texture *importTexture(void *,size_t,size_t) {throw std::runtime_error("no modeled host import");}
 Sampler *allocateSampler(bool,bool) {return new Sampler{};}
 void destroy(Sampler *s) {delete s;}
 Texture *allocateTexture(uint32_t=80,uint32_t=5) {throw std::runtime_error("M4 CPU model has no rendering");}
 Pipeline *compilePipeline(void *,const char *,void *,const char *,const CompileOptions &options=CompileOptions{}) {(void)options;throw std::runtime_error("M4 CPU model has no translation");}
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
