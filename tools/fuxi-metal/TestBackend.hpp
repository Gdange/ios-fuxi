/* Host-only model for Objective-C ownership/state tests; never linked into iOS. */
#pragma once
#include <algorithm>
#include <mutex>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <functional>
#include <unistd.h>
#include <atomic>
#include "ResourceLimits.hpp"
#include "TextureLevels.hpp"
#include "ObjectCache.hpp"
#include "Translator.hpp"
namespace fuxi {
struct Buffer {void *contents;size_t length,allocated;};
struct Texture : TextureLevels {size_t allocated{};uint32_t format=80;std::vector<uint8_t> pixels{};
 size_t offset(uint32_t level) const {size_t o=0;for(uint32_t l=0;l<level;++l)o+=levelWidth(l)*levelHeight(l)*(format==115?8:format==1 || format==10?1:format==30?2:4);return o;}
};
struct Pipeline {bool sampled=false;size_t references=1;};
struct Sampler {};
struct Viewport {float x,y,width,height,minDepth,maxDepth;};
struct Rect {struct {int32_t x,y;}offset;struct {uint32_t width,height;}extent;};
struct BufferBind {Buffer *buffer{};uint64_t offset{};bool bytes=false;};
struct ColorTarget {Texture *texture{};uint32_t level{},load=2,store=1;float clear[4]{};};
struct Operation {
 Buffer *src, *dst; size_t from, to, size; uint8_t value;
 Texture *texture{};Pipeline *pipeline{};float clear[4]{};
 Texture *sourceTexture{};Sampler *sampler{};uint32_t vertices=3;bool upload=false,clearOnly=false;
 uint32_t srcX{},srcY{},dstX{},dstY{},width{},height{};size_t rowBytes{};
 Texture *samples[resourceSlots]{};Sampler *samplers[resourceSlots]{};BufferBind buffers[2][resourceSlots]{};Buffer *indices{};uint64_t indexOffset{};uint32_t first{},strip{},load=2,store=1;
 Viewport viewport{0,256,256,-256,0,1};Rect scissor{{0,0},{256,256}};
 uint32_t areaWidth{},areaHeight{};
 uint32_t srcLevel{},dstLevel{},sampleBase[resourceSlots]{},sampleCount[resourceSlots]{};bool generateMips=false;uint32_t mipBase{},mipCount{};
 std::string diagnostic{};uint32_t colorCount=1,colorMask=1;ColorTarget colors[7]{};
 ColorTarget color(unsigned i) const {if(!(colorMask&(1u<<i)))return {};if(i)return colors[i-1];ColorTarget c{texture,dstLevel,load,store,{}};memcpy(c.clear,clear,sizeof(clear));return c;}
 void setColor(unsigned i,const ColorTarget &c){if(i){colors[i-1]=c;return;}texture=c.texture;dstLevel=c.level;load=c.load;store=c.store;memcpy(clear,c.clear,sizeof(clear));}

};
class Backend {
public:
 std::mutex mutex;
 ResourceLimits limits;ObjectCache<Pipeline> objects;
 static constexpr size_t maxBufferLength=ResourceLimits::single;
 static inline std::atomic<uint64_t> pipelineHits{0},pipelineMisses{0},gpuNanos{0},gpuSamples{0};
 static inline std::atomic<uint64_t> pipelineCacheLoadResult{0},pipelineCacheLoadBytes{0},pipelineCacheSaveResult{0},pipelineCacheSaveBytes{0},pipelineCacheSaveCount{0},pipelineCacheSaveFailures{0};
 static inline std::atomic<uint64_t> submitObjectsNanos{0},submitDescriptorsNanos{0},submitRecordNanos{0},shadowCopyNanos{0},submitFlushNanos{0},submitInvalidateNanos{0},flushCalls{0},invalidateCalls{0},emptySubmits{0},submitObjectCreates{0},submitPoolCreates{0},submitFrameCreates{0},submitViewCreates{0};
 static inline std::atomic<uint64_t> submits{0},submitPrepareNanos{0},submitWaitNanos{0},submitRetireNanos{0};
 static inline bool modelPipelines=false;
 uint32_t maxTextureDimension() const{return limits.dimension;}
 ~Backend(){objects.clear([&](Pipeline *p){destroy(p);});}
 static inline bool modelTextures=false;
 static inline int live=0,submissions=0,constructed=0;bool poisoned=false;
 explicit Backend(const char * ="unknown") {++constructed;}
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
 Sampler *allocateSampler(bool,bool,uint32_t=0,float=0,float=0) {return new Sampler{};}
 void destroy(Sampler *s) {delete s;}
 Texture *allocateTexture(uint32_t format=80,uint32_t=5,uint32_t width=256,uint32_t height=256,uint32_t levels=1) {
  if(!modelTextures)throw std::runtime_error("M4 CPU model has no rendering");
  if(!limits.extent(width,height,texelBytes(format)))throw std::runtime_error("extent");
  auto *t=new Texture{};t->format=format;t->width=width;t->height=height;t->levels=levels;t->allocated=t->offset(levels);t->pixels.resize(t->allocated);return t;
 }
 Pipeline *compilePipeline(void *,const char *v,void *,const char *f,const CompileOptions &options=CompileOptions{}) {
  if(!modelPipelines)throw std::runtime_error("M4 CPU model has no translation");
  // The ABI input consists only of initialized 32-bit fields/byte arrays.
  static_assert(sizeof(CompileOptions)==40360);
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
  if(ops.empty()){++emptySubmits;if(scheduled)scheduled();return;}
  ++submissions;if(scheduled)scheduled();
  for(auto &op:ops) {
   if(op.texture){auto *t=op.texture;
    if(op.pipeline)throw std::runtime_error("CPU model has no shaders");
    uint32_t l=op.upload || op.sourceTexture || op.clearOnly?op.dstLevel:op.srcLevel;
    size_t w=t->levelWidth(l),h=t->levelHeight(l),pixel=texelBytes(t->format);auto *data=t->pixels.data()+t->offset(l);
    if(op.generateMips)throw std::runtime_error("model does not generate mips");
    if(op.clearOnly){if(op.load==2){for(size_t i=0;i<w*h*pixel;i+=4){data[i]=(uint8_t)(op.clear[2]*255);data[i+1]=(uint8_t)(op.clear[1]*255);data[i+2]=(uint8_t)(op.clear[0]*255);data[i+3]=(uint8_t)(op.clear[3]*255);}}
     t->setDefined(l,op.store && (op.load==2 || (op.load==1 && t->isDefined(l))));
    }else {
     size_t width=op.width?op.width:w,height=op.height?op.height:h,row=op.rowBytes?op.rowBytes:width*pixel;
     if(op.upload){for(size_t y=0;y<height;++y)memcpy(data+((op.dstY+y)*w+op.dstX)*pixel,(uint8_t *)op.src->contents+op.from+y*row,width*pixel);t->setDefined(l);}
     else if(op.sourceTexture){auto *src=op.sourceTexture;assert(src->isDefined(op.srcLevel));for(size_t y=0;y<height;++y)memcpy(data+((op.dstY+y)*w+op.dstX)*pixel,src->pixels.data()+src->offset(op.srcLevel)+((op.srcY+y)*src->levelWidth(op.srcLevel)+op.srcX)*pixel,width*pixel);t->setDefined(l);}
     else {if(!t->isDefined(l))throw std::runtime_error("undefined model readback");for(size_t y=0;y<height;++y)memcpy((uint8_t *)op.dst->contents+op.to+y*row,data+((op.srcY+y)*w+op.srcX)*pixel,width*pixel);}
    }
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
