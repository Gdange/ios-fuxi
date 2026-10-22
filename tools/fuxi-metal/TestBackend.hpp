/* Host-only model for Objective-C ownership/state tests; never linked into iOS. */
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <functional>
#include <unistd.h>
#include <atomic>
#include "ResourceLimits.hpp"
#include "ObjectCache.hpp"
#include "Translator.hpp"
namespace fuxi {
struct Buffer {void *contents;size_t length,allocated;};
struct Texture {size_t allocated{};uint32_t format=80,width=256,height=256;bool defined=false;std::vector<uint8_t> pixels{};};
struct Pipeline {bool sampled=false;size_t references=1;};
struct Sampler {};
struct Viewport {float x,y,width,height,minDepth,maxDepth;};
struct Rect {struct {int32_t x,y;}offset;struct {uint32_t width,height;}extent;};
struct BufferBind {Buffer *buffer{};uint64_t offset{};bool bytes=false;};
struct Operation {
 Buffer *src, *dst; size_t from, to, size; uint8_t value;
 Texture *texture{};Pipeline *pipeline{};float clear[4]{};
 Texture *sourceTexture{};Sampler *sampler{};uint32_t vertices=3;bool upload=false,clearOnly=false;
 uint32_t srcX{},srcY{},dstX{},dstY{},width{},height{};size_t rowBytes{};
 Texture *samples[resourceSlots]{};Sampler *samplers[resourceSlots]{};BufferBind buffers[2][resourceSlots]{};Buffer *indices{};uint64_t indexOffset{};uint32_t first{},strip{},load=2,store=1;
 Viewport viewport{0,256,256,-256,0,1};Rect scissor{{0,0},{256,256}};
 uint32_t areaWidth{},areaHeight{};
};
class Backend {
public:
 ResourceLimits limits;ObjectCache<Pipeline> objects;
 static constexpr size_t maxBufferLength=ResourceLimits::single;
 static inline std::atomic<uint64_t> pipelineHits{0},pipelineMisses{0},gpuNanos{0},gpuSamples{0};
 static inline std::atomic<uint64_t> submits{0},submitPrepareNanos{0},submitWaitNanos{0},submitRetireNanos{0};
 static inline bool modelPipelines=false;
 uint32_t maxTextureDimension() const{return limits.dimension;}
 ~Backend(){objects.clear([&](Pipeline *p){destroy(p);});}
 static inline bool modelTextures=false;
 static inline int live=0,submissions=0,constructed=0;bool poisoned=false;
 Backend() {++constructed;}
 Buffer *allocate(size_t n) {
  if(!n || n>maxBufferLength)throw std::runtime_error("length");
  auto *b=new Buffer{calloc(1,n),n,n};++live;return b;
 }
 void destroy(Buffer *b) {if(b){free(b->contents);delete b;--live;}}
 static size_t texelBytes(uint32_t f){return f==115?8:(f==1 || f==10)?1:f==30?2:4;}
 bool supportsOrderedColorAccess() const {return false;}
 bool supportsHostImport() const {return false;}
 Texture *importTexture(void *,size_t,size_t,uint32_t=256,uint32_t=256) {throw std::runtime_error("no modeled host import");}
 Buffer *importBuffer(void *,size_t) {throw std::runtime_error("no modeled host buffer import");}
 Texture *linearTexture(Buffer *,uint32_t,uint32_t,uint32_t,uint32_t,size_t,size_t) {throw std::runtime_error("no modeled linear texture");}
 Sampler *allocateSampler(bool,bool) {return new Sampler{};}
 void destroy(Sampler *s) {delete s;}
 Texture *allocateTexture(uint32_t format=80,uint32_t=5,uint32_t width=256,uint32_t height=256) {
  if(!modelTextures)throw std::runtime_error("M4 CPU model has no rendering");
  if(!limits.extent(width,height,texelBytes(format)))throw std::runtime_error("extent");
  auto *t=new Texture{};t->format=format;t->width=width;t->height=height;t->allocated=width*height*texelBytes(format);t->pixels.resize(t->allocated);return t;
 }
 Pipeline *compilePipeline(void *,const char *v,void *,const char *f,const CompileOptions &options=CompileOptions{}) {
  if(!modelPipelines)throw std::runtime_error("M4 CPU model has no translation");
  // The ABI input consists only of initialized 32-bit fields/byte arrays.
  static_assert(sizeof(CompileOptions)==40100);
  std::string key=std::string(v)+"/"+f+"/";key.append((const char *)&options,sizeof(options));
  if(auto *p=objects.acquire(key)){++pipelineHits;return p;}
  ++pipelineMisses;auto *p=new Pipeline{};objects.insert(key,key,p,sizeof(Pipeline),[&](Pipeline *old){destroy(old);});return p;
 }
 bool destroy(Texture *t) {delete t;return !poisoned;}
 void destroy(Pipeline *p) {if(p && !--p->references)delete p;}
 bool requiresSampling(const Pipeline *p) const {return p->sampled;}
 void submit(const std::vector<Operation>& ops,const std::function<void()> &scheduled={}) {
  if(getenv("FUXI_TEST_LOST")) {poisoned=true;throw std::runtime_error("modeled device loss");}
  if(getenv("FUXI_TEST_HANG"))for(;;)pause();
  ++submissions;if(scheduled)scheduled();
  for(auto &op:ops) {
   if(op.texture){auto *t=op.texture;
    if(op.pipeline)throw std::runtime_error("CPU model has no shaders");
    if(op.clearOnly){if(op.load==2){for(size_t i=0;i<t->pixels.size();i+=4){t->pixels[i]=(uint8_t)(op.clear[2]*255);t->pixels[i+1]=(uint8_t)(op.clear[1]*255);t->pixels[i+2]=(uint8_t)(op.clear[0]*255);t->pixels[i+3]=(uint8_t)(op.clear[3]*255);}}
     t->defined=op.store && (op.load==2 || (op.load==1 && t->defined));
    }else if(op.upload){memcpy(t->pixels.data(),(uint8_t *)op.src->contents+op.from,t->pixels.size());t->defined=true;}
    else if(op.sourceTexture){t->pixels=op.sourceTexture->pixels;t->defined=true;}
    else {if(!t->defined)throw std::runtime_error("undefined model readback");memcpy((uint8_t *)op.dst->contents+op.to,t->pixels.data(),t->pixels.size());}
    continue;
   }
   auto *dst=(uint8_t *)op.dst->contents+op.to;
   if(op.src)memcpy(dst,(uint8_t *)op.src->contents+op.from,op.size);
   else memset(dst,op.value,op.size);
  }
  if(getenv("FUXI_TEST_BAD_DATA") && !ops.empty()) ((uint8_t *)ops.back().dst->contents)[ops.back().dst->length-1]^=1;
 }
};
}
