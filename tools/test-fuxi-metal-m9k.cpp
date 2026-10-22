/* Authored mip/format controls through production Backend + external bridge. */
#include "fuxi-metal/Backend.hpp"
#include "fuxi-metal/M7Reference.h"
#include <cassert>
#include <fstream>
#include <iterator>
static void reject(const std::function<void()> &f){bool failed=false;try{f();}catch(const std::runtime_error &){failed=true;}assert(failed);}
int main(int argc,char **argv){
 assert(argc==2);static_assert(sizeof(fuxi::DrawInput)==1056);
 std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[512]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib);
 fuxi::Backend b;auto baseline=b.resourceBytes();
 auto *upload=b.allocate(4096),*read=b.allocate(4096);
 auto put=[&](fuxi::Texture *t,unsigned l,const std::vector<uint8_t> &bytes){memcpy(upload->contents,bytes.data(),bytes.size());fuxi::Operation op{};op.texture=t;op.dstLevel=l;op.src=upload;op.upload=true;b.submit({op});};
 auto get=[&](fuxi::Texture *t,unsigned l){memset(read->contents,0xa5,read->length);fuxi::Operation op{};op.texture=t;op.srcLevel=l;op.dst=read;op.to=16;b.submit({op});size_t size=t->levelWidth(l)*t->levelHeight(l)*b.texelBytes(t->format);auto *p=(uint8_t *)read->contents;for(unsigned i=0;i<16;++i)assert(p[i]==0xa5 && p[16+size+i]==0xa5);return std::vector<uint8_t>(p+16,p+16+size);};
 // NPOT and skinny images: dimensions clamp at 1; each level has independent data.
 for(auto dims:{std::pair<unsigned,unsigned>{13,7},{1,16},{16,1}}){unsigned count=fuxi::mipCount(dims.first,dims.second);
  auto *t=b.allocateTexture(80,21,dims.first,dims.second,count),*copy=b.allocateTexture(80,0,dims.first,dims.second,count);
  size_t sum=0;for(unsigned l=0;l<count;++l){sum+=t->levelWidth(l)*t->levelHeight(l)*4;std::vector<uint8_t> bytes(t->levelWidth(l)*t->levelHeight(l)*4);for(size_t i=0;i<bytes.size();++i)bytes[i]=(uint8_t)(i+31*l);put(t,l,bytes);assert(get(t,l)==bytes);
   fuxi::Operation op{};op.sourceTexture=t;op.texture=copy;op.srcLevel=op.dstLevel=l;b.submit({op});assert(get(copy,l)==bytes);
  }assert(t->charged>=sum && b.resourceBytes()<=fuxi::ResourceLimits::budget);
  // Partial transfer only touches the named level and zero-defines its untouched texels.
  auto *partial=b.allocateTexture(80,5,dims.first,dims.second,count);fuxi::Operation op{};op.texture=partial;op.dstLevel=count-1;op.src=upload;op.upload=true;op.width=op.height=1;b.submit({op});assert(partial->isDefined(count-1) && !partial->isDefined(0));
  b.destroy(partial);b.destroy(copy);b.destroy(t);
 }
 auto *mips=b.allocateTexture(80,5,16,8,5);std::vector<uint8_t> cpu(16*8*4);
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x)for(unsigned c=0;c<4;++c)cpu[(y*16+x)*4+c]=(uint8_t)((x*11+y*23+c*47)%256);
 put(mips,0,cpu);reject([&]{get(mips,1);});
 fuxi::Operation gen{};gen.texture=mips;gen.generateMips=true;gen.mipCount=5;
 auto samples=b.gpuSamples.load();b.submit({gen});assert(b.gpuSamples>samples);
 // Independent CPU box filter oracle. For 2:1 levels Vulkan linear blits average
 // the same footprint; allow a rounding unit at each chain step.
 unsigned w=16,h=8;
 for(unsigned l=1;l<5;++l){unsigned nw=std::max(1u,w/2),nh=std::max(1u,h/2);std::vector<uint8_t> next(nw*nh*4);
  for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<nw;++x)for(unsigned c=0;c<4;++c){unsigned sum=0,n=0;for(unsigned dy=0;dy<(h>1?2u:1u);++dy)for(unsigned dx=0;dx<(w>1?2u:1u);++dx){sum+=cpu[((2*y+dy)*w+2*x+dx)*4+c];++n;}next[(y*nw+x)*4+c]=(sum+n/2)/n;}
  auto gpu=get(mips,l);for(size_t i=0;i<gpu.size();++i)assert(abs(int(gpu[i])-int(next[i]))<=int(l));cpu=next;w=nw;h=nh;
 }
 // Same-image copies are legal across distinct mip subresources, while aliases
 // within one level remain refused. Copies never silently resize.
 fuxi::Operation same{};same.texture=same.sourceTexture=mips;same.srcLevel=0;same.dstLevel=4;same.width=same.height=1;b.submit({same});
 auto base=get(mips,0);assert(get(mips,4)==std::vector<uint8_t>(base.begin(),base.begin()+4));
 same.dstLevel=0;reject([&]{b.submit({same});});b.submit({gen});
 // Pre-submit failure must restore all level definition bits and layouts.
 mips->setDefined(4,false);auto mask=mips->definedLevels;auto layout=mips->layout;b.failBeforeQueue=true;reject([&]{b.submit({gen});});assert(mips->definedLevels==mask && mips->layout==layout && !b.poisoned);b.submit({gen});
 // Invalid counts/levels/LOD fail before submission, without allocating or poisoning.
 auto charge=b.resourceBytes();reject([&]{b.allocateTexture(80,5,8,4,5);});reject([&]{b.allocateTexture(80,5,8,4,0);});assert(b.resourceBytes()==charge);
 auto bad=gen;bad.mipBase=4;bad.mipCount=2;reject([&]{b.submit({bad});});bad={};bad.texture=mips;bad.srcLevel=5;bad.dst=read;reject([&]{b.submit({bad});});
 reject([&]{b.allocateSampler(false,false,3);});reject([&]{b.allocateSampler(false,false,1,2,1);});
 // Descriptor classes, including packed BGR10A2 and all existing small channels.
 for(uint32_t format:{80u,70u,115u,10u,30u,1u,94u}){auto *t=b.allocateTexture(format,format==1?1:21,8,4,4);for(unsigned l=0;l<4;++l){std::vector<uint8_t> bytes(t->levelWidth(l)*t->levelHeight(l)*b.texelBytes(format));for(size_t i=0;i<bytes.size();++i)bytes[i]=(uint8_t)(17*i+19*l);put(t,l,bytes);assert(get(t,l)==bytes);}b.destroy(t);}
 reject([&]{b.allocateTexture(81,1,8,4,4);});reject([&]{b.allocateTexture(1,4,8,4,4);});reject([&]{b.allocateTexture(80,2,8,4,4);});
 // Full-chain charge is based on Vulkan requirements, not just level zero.
 auto bytes=b.resourceBytes();auto *large=b.allocateTexture(80,5,4096,4096,13);assert(large->charged>=size_t(4096)*4096*4*4/3);b.destroy(large);assert(b.resourceBytes()==bytes);
 // Sample authored level colors with nearest/linear mip filtering, fractional
 // and fixed clamps, nonmip restriction, and rebased level-range views.
 fuxi::CompileOptions options{};options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto *vertices=b.allocate(sizeof(fuxiM7Vertices)),*indices=b.allocate(sizeof(fuxiM7Indices)),*ones=b.allocate(16);
 memcpy(vertices->contents,fuxiM7Vertices,sizeof(fuxiM7Vertices));memcpy(indices->contents,fuxiM7Indices,sizeof(fuxiM7Indices));for(unsigned c=0;c<4;++c)((float *)ones->contents)[c]=1;
 for(unsigned l=0;l<5;++l){std::vector<uint8_t> bytes(mips->levelWidth(l)*mips->levelHeight(l)*4,uint8_t(l*48));put(mips,l,bytes);}
 auto *target=b.allocateTexture(80,5,16,8,5);
 fuxi::Operation draw{};draw.texture=target;draw.pipeline=p;draw.indices=indices;draw.vertices=6;draw.samples[0]=mips;draw.buffers[0][0]={vertices,0,false};draw.buffers[0][2]={ones,0,true};draw.buffers[1][1]={ones,0,true};draw.viewport={0,8,16,-8,0,1};draw.scissor={{0,0},{16,8}};
 auto sample=[&](unsigned mip,float lo,float hi,unsigned base,unsigned count,int expected){auto *sam=b.allocateSampler(false,false,mip,lo,hi);auto op=draw;op.samplers[0]=sam;op.sampleBase[0]=base;op.sampleCount[0]=count;b.submit({op});auto pixels=get(target,0);for(auto v:pixels)if(abs(int(v)-expected)>1){fprintf(stderr,"sample mip=%u lod=%g..%g range=%u+%u got=%u want=%d\n",mip,lo,hi,base,count,v,expected);abort();}b.destroy(sam);};
 sample(1,2,2,0,5,96);sample(2,1.5f,1.5f,0,5,72);sample(0,0,FLT_MAX,0,5,0);sample(0,1,2,0,5,0);sample(1,2000,3000,0,5,192);sample(1,0,FLT_MAX,2,2,96);sample(1,3,3,1,2,96);
 // Render to a nonzero attachment and sample that level back; framebuffer cache
 // entries must differ between levels. Repeated draws reuse the correct view.
 auto *sam=b.allocateSampler(false,false);draw.samplers[0]=sam;draw.sampleBase[0]=3;draw.sampleCount[0]=1;draw.dstLevel=2;draw.viewport={0,2,4,-2,0,1};draw.scissor={{0,0},{4,2}};b.submit({draw});for(auto v:get(target,2))assert(abs(int(v)-144)<=1);
 auto frames=b.cachedFrames();b.submit({draw});assert(b.cachedFrames()==frames);
 // BGR10 render target packing and sampling use the translator's direct format.
 auto ten=options;ten.format=94;auto *p10=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",ten);auto *target10=b.allocateTexture(94,5,4,2,1);auto d10=draw;d10.texture=target10;d10.pipeline=p10;d10.dstLevel=0;b.submit({d10});auto packed=get(target10,0);uint32_t value;memcpy(&value,packed.data(),4);assert(abs(int(value&1023)-578)<=3 && abs(int((value>>10)&1023)-578)<=3 && abs(int((value>>20)&1023)-578)<=3 && (value>>30)==2);
 // Sample the packed target through the real bridge, checking logical channels.
 auto unpack=draw;unpack.dstLevel=2;unpack.samples[0]=target10;unpack.sampleBase[0]=0;unpack.sampleCount[0]=1;b.submit({unpack});auto unpacked=get(target,2);
 for(size_t i=0;i<unpacked.size();i+=4){for(unsigned c=0;c<3;++c)assert(abs(int(unpacked[i+c])-144)<=1);assert(abs(int(unpacked[i+3])-170)<=1);}
 b.destroy(target10);b.destroy(p10);b.destroy(sam);b.destroy(target);b.destroy(mips);b.destroy(p);b.destroy(vertices);b.destroy(indices);b.destroy(ones);b.destroy(upload);b.destroy(read);b.dropPipelineCacheForTest();assert(b.resourceBytes()==baseline);tr.close(lib);
 puts("PASS: M9k mip creation/NPOT/skinny/bytes/copies, GPU box-filter generation, level views/LOD sampling/render-to-level, BGR10/RG/R8/A8/float formats, budget/bounds/rollback");
}
