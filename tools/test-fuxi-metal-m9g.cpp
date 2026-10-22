/* Original authored AIR / production Backend lavapipe regression. No Apple data. */
#include "fuxi-metal/Backend.hpp"
#include "fuxi-metal/M7Reference.h"
#include <cassert>
#include <fstream>
#include <iterator>
static void reject(const std::function<void()> &action){bool failed=false;try{action();}catch(const std::runtime_error&){failed=true;}assert(failed);}
int main(int argc,char **argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[512]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib);
 fuxi::Backend b;assert(b.maxTextureDimension()>=2400 && b.maxTextureDimension()<=4096);
 fuxi::CompileOptions options{};options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto misses=b.pipelineMisses.load(),hits=b.pipelineHits.load();
 auto *same=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 assert(same==p && p->references==3 && b.pipelineMisses==misses && b.pipelineHits==hits+1);
 b.destroy(p);p=same; // Producer wrapper retirement leaves the shared backend live.
 // Every render-state / specialization field contributes to the identity hash.
 auto identity=[&](const fuxi::CompileOptions &o){std::array<uint8_t,32> hash{};assert(tr.identity(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",&o,hash.data(),error,sizeof(error)));return hash;};
 auto original=identity(options);auto different=options;
 different.mask=8;assert(identity(different)!=original);different=options;different.format=70;assert(identity(different)!=original);
 different=options;different.blend=1;assert(identity(different)!=original);different=options;different.srcRGB=4;assert(identity(different)!=original);
 different=options;different.dstAlpha=5;assert(identity(different)!=original);different=options;different.rgbOp=1;assert(identity(different)!=original);
 different=options;different.strides[0]=24;assert(identity(different)!=original);different=options;different.attributes[0].offset=4;assert(identity(different)!=original);
 different=options;different.constants[1][0].bytes[0]=0;assert(identity(different)!=original);different=options;different.orderedColorAccess=1;assert(identity(different)!=original);
 different=options;different.mask=8;auto *masked=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",different);assert(masked!=p);b.destroy(masked);
 // Eviction drops the cache ref, while the original caller can still draw.
 for(unsigned i=0;i<32;++i){auto churn=options;churn.strides[0]=24+4*i;
  auto *temporary=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",churn);b.destroy(temporary);}
 assert(p->references==1);
 constexpr unsigned width=1080,height=2400;constexpr size_t row=width*4,bytes=row*height;
 auto *src=b.allocateTexture(80,1,width,height),*dst=b.allocateTexture(80,5,width,height);
 auto *upload=b.allocate(bytes),*read=b.allocate(bytes+512),*vertices=b.allocate(sizeof(fuxiM7Vertices)),*indices=b.allocate(sizeof(fuxiM7Indices)),*ones=b.allocate(16);
 memcpy(vertices->contents,fuxiM7Vertices,sizeof(fuxiM7Vertices));memcpy(indices->contents,fuxiM7Indices,sizeof(fuxiM7Indices));
 for(unsigned c=0;c<4;++c)((float *)ones->contents)[c]=1;
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto *q=(uint8_t *)upload->contents+y*row+x*4;q[0]=x<540?32:224;q[1]=y<1200?192:48;q[2]=64;q[3]=255;}
 fuxi::Operation put{};put.texture=src;put.src=upload;put.upload=true;b.submit({put});
 auto *sam=b.allocateSampler(false,false);fuxi::Operation draw{};draw.texture=dst;draw.pipeline=p;draw.indices=indices;draw.vertices=6;
 draw.samples[0]=src;draw.samplers[0]=sam;draw.buffers[0][0]={vertices,0,false};draw.buffers[0][2]={ones,0,true};draw.buffers[1][1]={ones,0,true};
 draw.viewport={0,height,width,-float(height),0,1};draw.scissor={{300,400},{700,1800}};
 fuxi::Operation get{};get.texture=dst;get.dst=read;get.to=256;memset(read->contents,0x39,read->length);
 auto before=b.gpuSamples.load();b.submit({draw,get});assert(b.gpuSamples>before && b.gpuNanos>0);
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const uint8_t zero[4]{};const uint8_t *want=x>=300 && x<1000 && y>=400 && y<2200?(uint8_t *)upload->contents+y*row+x*4:zero;
  assert(!memcmp((uint8_t *)read->contents+256+y*row+x*4,want,4));}
 for(unsigned i=0;i<256;++i)assert(((uint8_t *)read->contents)[i]==0x39 && ((uint8_t *)read->contents)[bytes+256+i]==0x39);
 auto bad=draw;bad.scissor.extent.width=781;reject([&]{b.submit({bad});});bad=draw;bad.viewport.width=1081;reject([&]{b.submit({bad});});assert(!b.poisoned);
 reject([&]{b.allocate(b.maxBufferLength+1);});
 reject([&]{b.allocateTexture(80,5,4097,1);});reject([&]{b.allocateTexture(115,5,UINT32_MAX,UINT32_MAX);});
 reject([&]{b.linearTexture(upload,80,1,width,height,SIZE_MAX,row);});reject([&]{b.linearTexture(upload,80,1,width,height,0,SIZE_MAX);});
 // Large buffer-backed image, padded Vulkan rows and a nonzero memory offset.
 // Lavapipe's BGRA row pitch for this width is exactly 4352 bytes.
 constexpr size_t paddedRow=4352,linearOffset=65536;
 auto *backing=b.allocate(paddedRow*height+linearOffset);auto charged=b.resourceBytes();auto *linear=b.linearTexture(backing,80,5,width,height,linearOffset,paddedRow);
 assert(b.resourceBytes()==charged);
 fuxi::Operation clear{};clear.texture=linear;clear.clearOnly=true;clear.clear[1]=clear.clear[3]=1;b.submit({clear});
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const uint8_t green[]={0,255,0,255};assert(!memcmp((uint8_t *)backing->contents+linearOffset+y*paddedRow+4*x,green,4));}
 b.destroy(linear);b.destroy(backing);
 // External same-memory import at panel size (lavapipe external-host support).
 if(b.supportsHostImport()){
  size_t allocation=(paddedRow*height+65535)&~size_t(65535);void *host=nullptr;assert(!posix_memalign(&host,65536,allocation));memset(host,0,allocation);
  auto *imported=b.importTexture(host,allocation,paddedRow,width,height);clear.texture=imported;b.submit({clear});
  for(unsigned y=0;y<height;++y){const uint8_t green[]={0,255,0,255};assert(!memcmp((uint8_t *)host+y*paddedRow+4*(width-1),green,4));}
  b.destroy(imported);free(host);
 }
 b.destroy(p);b.destroy(sam);b.destroy(src);b.destroy(dst);b.destroy(upload);b.destroy(read);b.destroy(vertices);b.destroy(indices);b.destroy(ones);
 // Cached pipelines can retain constant uploads; resource charge remains bounded.
 assert(b.resourceBytes()<fuxi::ResourceLimits::budget);tr.close(lib);
 puts("PASS: M9g shared pipeline identity/refcounts, large sampled/scissored pixels, padded linear/import targets, bounds and GPU timestamps");
}
