/* Actual translator plan + lavapipe; AIR control is original, not Xcode AIR. */
#include "fuxi-metal/Backend.hpp"
#include "fuxi-metal/M7Reference.h"
#include <fstream>
#include <iterator>
static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv){try {
 require(argc==2,"supply control AIR or CI metallib");std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(file),{}};
 auto &tr=fuxi::Translator::get();char error[1024]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));require(lib,error);
 fuxi::Backend b;fuxi::CompileOptions options{};options.blend=1;options.dstRGB=options.dstAlpha=5;
 options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=sizeof(FuxiM7VertexData);
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto *cached=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto *vertices=b.allocate(sizeof(fuxiM7Vertices)+16),*indices=b.allocate(sizeof(fuxiM7Indices)+4),*readback=b.allocate(262656);
 memcpy((uint8_t *)vertices->contents+16,fuxiM7Vertices,sizeof(fuxiM7Vertices));memcpy((uint8_t *)indices->contents+4,fuxiM7Indices,sizeof(fuxiM7Indices));
 void *pages[3]{};fuxi::Texture *t[3]{};
 for(unsigned i=0;i<3;++i){require(!posix_memalign(&pages[i],65536,262144),"pages");auto *base=(uint8_t *)pages[i];
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){if(i<2)fuxiM7Texel(i,x,y,base+4*(y*256+x));else memset(base+4*(y*256+x),0x39,4);}
  t[i]=b.importTexture(pages[i],262144,1024);
 }
 auto *sam=b.allocateSampler(false,false);auto *full=b.allocate(16),*half=b.allocate(16);for(unsigned c=0;c<4;++c){((float *)full->contents)[c]=1;((float *)half->contents)[c]=0.5;}
 fuxi::Operation base{};base.texture=t[2];base.pipeline=p;base.vertices=6;base.indices=indices;base.indexOffset=4;base.clear[3]=1;
 base.samples[0]=t[0];base.samplers[0]=sam;base.buffers[0][0]={vertices,16,false};base.buffers[1][1]={full,0,true};base.buffers[0][2]={full,0,true};
 auto overlay=base;overlay.load=1;overlay.samples[0]=t[1];overlay.buffers[1][1]={half,0,true};overlay.scissor={{16,24},{224,208}};
 fuxi::Operation read{};read.texture=t[2];read.dst=readback;read.to=256;
 auto refused=[&](const std::vector<fuxi::Operation> &ops){auto layout=t[2]->layout;bool defined=t[2]->defined;try{b.submit(ops);throw std::logic_error("bad draw accepted");}catch(const std::runtime_error&){}require(!b.poisoned && t[2]->layout==layout && t[2]->defined==defined,"refusal state changed");};
 auto bad=base;bad.buffers[1][1]={};refused({bad});bad=base;bad.buffers[0][0].offset=vertices->length-1;refused({bad});bad=base;bad.indexOffset=indices->length-1;refused({bad});bad=base;bad.samples[0]=t[2];refused({bad});
 bad=base;bad.first=UINT32_MAX;refused({bad});bad=base;bad.indexOffset=UINT64_MAX-1;refused({bad});
 uint16_t saved;memcpy(&saved,(uint8_t *)indices->contents+4,2);uint16_t outOfRange=4;memcpy((uint8_t *)indices->contents+4,&outOfRange,2);refused({base});memcpy((uint8_t *)indices->contents+4,&saved,2);
 for(auto *pipeline:{p,cached}){base.pipeline=overlay.pipeline=pipeline;memset(readback->contents,0x39,readback->length);b.submit({base,overlay,read});
  const auto *raw=(const uint8_t *)readback->contents;for(unsigned i=0;i<256;++i)require(raw[i]==0x39 && raw[262400+i]==0x39,"sentinels");
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t want[4];fuxiM7Reference(x,y,want);auto got=raw+256+4*(y*256+x);if(memcmp(got,want,4)){fprintf(stderr,"pixel %u,%u got %u,%u,%u,%u want %u,%u,%u,%u\n",x,y,got[0],got[1],got[2],got[3],want[0],want[1],want[2],want[3]);throw std::runtime_error("CPU reference");}}
  require(!memcmp(raw+256,pages[2],262144),"same memory");
 }
 // DontCare store makes later load/read fail, with all state restored.
 bad=base;bad.store=0;refused({bad,read});
 bad=base;bad.load=0;refused({bad,read}); // Conservatively undefined after DontCare load.
 fuxi::Operation overwrite{};overwrite.dst=indices;overwrite.size=4;refused({overwrite,base});
 // Indexed strips share the same linked pipeline; restart-free four-vertex quad.
 const uint16_t strip[]={0,1,2,3};memcpy((uint8_t *)indices->contents+4,strip,sizeof(strip));base.strip=overlay.strip=1;base.vertices=overlay.vertices=4;b.submit({base,overlay,read});
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t want[4];fuxiM7Reference(x,y,want);require(!memcmp((uint8_t *)readback->contents+256+4*(y*256+x),want,4),"strip pixels");}
 // Four independently planned texture/sampler slots; none may be omitted.
 memcpy((uint8_t *)indices->contents+4,fuxiM7Indices,sizeof(fuxiM7Indices));
 auto fourOptions=options;fourOptions.blend=0;auto *four=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_four_fragment",fourOptions);
 auto draw=base;draw.pipeline=four;draw.strip=0;draw.vertices=6;draw.buffers[1][1]={};
 fuxi::Texture *inputs[4]{};fuxi::Sampler *samplers[4]{};auto *uploadBuffer=b.allocate(262144);
 for(unsigned slot=0;slot<4;++slot){inputs[slot]=b.allocateTexture();samplers[slot]=b.allocateSampler(false,false);auto *raw=(uint8_t *)uploadBuffer->contents;
  for(unsigned i=0;i<65536;++i){raw[4*i]=32+32*slot;raw[4*i+1]=48+32*slot;raw[4*i+2]=64+32*slot;raw[4*i+3]=255;}
  fuxi::Operation upload{};upload.texture=inputs[slot];upload.src=uploadBuffer;upload.upload=true;b.submit({upload});draw.samples[slot]=inputs[slot];draw.samplers[slot]=samplers[slot];
 }
 auto missing=draw;missing.samples[3]=nullptr;refused({missing});missing=draw;missing.samplers[3]=nullptr;refused({missing});
 b.submit({draw,read});for(unsigned i=0;i<65536;++i){const uint8_t want[]={80,96,112,255};require(!memcmp((uint8_t *)readback->contents+256+4*i,want,4),"four texture pixels");}
 for(unsigned slot=0;slot<4;++slot){b.destroy(inputs[slot]);b.destroy(samplers[slot]);}b.destroy(uploadBuffer);b.destroy(four);
 puts("PASS: four planned fragment texture/sampler slots and missing slot3 refusal");
 // Sample/render roundtrip for each requested format, using raw constant texels.
 for(uint32_t format:{80u,70u,115u,10u,30u}){
  size_t size=65536*fuxi::Backend::texelBytes(format);auto *src=b.allocateTexture(format,1),*dst=b.allocateTexture(format,5);auto *bytes=b.allocate(size+512);
  const uint8_t texel8[]={64,128,192,255};const uint16_t texel16[]={0x3400,0x3800,0x3a00,0x3c00};auto pixelSize=fuxi::Backend::texelBytes(format);
  for(unsigned i=0;i<65536;++i)memcpy((uint8_t *)bytes->contents+256+i*pixelSize,format==115?(const void *)texel16:(const void *)texel8,pixelSize);
  fuxi::Operation upload{};upload.src=bytes;upload.from=256;upload.texture=src;upload.upload=true;b.submit({upload});
  auto formatOptions=options;formatOptions.format=format;formatOptions.blend=0;auto *formatPipeline=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",formatOptions);
  auto sample=base;sample.pipeline=formatPipeline;sample.strip=0;sample.vertices=6;sample.texture=dst;sample.samples[0]=src;
  auto copy=read;copy.texture=dst;copy.dst=bytes;
  // Different allocations exercise ordinary buffer uniforms and residual offsets.
  auto *uniform=b.allocate(48);memcpy((uint8_t *)uniform->contents+16,full->contents,16);sample.buffers[1][1]={uniform,16,false};sample.buffers[0][2]={uniform,16,false};
  memset(bytes->contents,0x39,bytes->length);b.submit({sample,copy});
  auto *raw=(uint8_t *)bytes->contents;for(unsigned i=0;i<256;++i)require(raw[i]==0x39 && raw[size+256+i]==0x39,"format sentinels");
  for(unsigned i=0;i<65536;++i)require(!memcmp(raw+256+i*pixelSize,format==115?(const void *)texel16:(const void *)texel8,pixelSize),"format sample/render roundtrip");
  printf("PASS: format %u sample/render + vertex/fragment buffer offset16 uniform + sentinels\n",format);
  b.destroy(uniform);b.destroy(formatPipeline);b.destroy(bytes);b.destroy(src);b.destroy(dst);
 }
 // Function constants must change the shader, including index/name resolution.
 auto constantOptions=options;constantOptions.blend=0;constantOptions.constants[1][0].bytes[0]=0;
 for(bool byName:{false,true}){
  if(byName)strcpy(constantOptions.constants[1][0].name,"fuxi_m7_uniform");
  auto *off=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",constantOptions);
  auto draw=base;draw.pipeline=off;draw.strip=0;draw.vertices=6;b.submit({draw,read});
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t expected[4];fuxiM7Texel(0,x,y,expected);expected[0]=expected[1]=0;
   require(!memcmp((uint8_t *)readback->contents+256+4*(y*256+x),expected,4),"false function constant");}
  b.destroy(off);
 }
 puts("PASS: true/false function constants by index/name change pixels");
 auto compileRefused=[&](fuxi::CompileOptions o){try{auto *unexpected=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",o);b.destroy(unexpected);throw std::logic_error("bad pipeline accepted");}catch(const std::runtime_error&) {}};
 auto invalid=options;invalid.constantCount[1]=0;compileRefused(invalid);
 invalid=options;invalid.constants[1][0].type=3;invalid.constants[1][0].size=4;compileRefused(invalid);
 invalid=options;invalid.srcRGB=15;compileRefused(invalid); // dual-source needs an unsupported feature.
 invalid=options;invalid.format=1;compileRefused(invalid); // A8 attachment needs a native A8 format.
 try{auto *unexpected=b.allocateTexture(1,5);b.destroy(unexpected);throw std::logic_error("A8 render allocation accepted");}catch(const std::runtime_error&){}
 // A8 is sampled through the plan's R8 -> (0,0,0,R) component mapping.
 auto *alpha=b.allocateTexture(1,1);auto *alphaBytes=b.allocate(65536);memset(alphaBytes->contents,128,65536);
 fuxi::Operation upload{};upload.texture=alpha;upload.src=alphaBytes;upload.upload=true;b.submit({upload});
 draw=base;draw.strip=0;draw.vertices=6;draw.samples[0]=alpha;
 auto unblended=options;unblended.blend=0;auto *plain=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",unblended);draw.pipeline=plain;b.submit({draw,read});
 for(unsigned i=0;i<65536;++i){const uint8_t want[]={0,0,0,128};require(!memcmp((uint8_t *)readback->contents+256+4*i,want,4),"A8 planned swizzle");}
 b.destroy(alpha);b.destroy(alphaBytes);puts("PASS: A8 sampled swizzle; A8 rendering/missing or mistyped constants/dual-source refused");
 // sRGB plans can request resources outside the bounded consumer.
 invalid=options;invalid.format=81;compileRefused(invalid);
 try{auto *unexpected=b.allocateTexture(81,5);b.destroy(unexpected);throw std::logic_error("sRGB allocation accepted");}catch(const std::runtime_error&){}
 puts("PASS: sRGB profile deferred/refused");
 // Metal's Red bit is 8; Vulkan's is 1. Preserve the clear in other channels.
 auto maskedOptions=unblended;maskedOptions.mask=8;auto *masked=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",maskedOptions);
 draw=base;draw.pipeline=masked;draw.strip=0;draw.vertices=6;draw.clear[0]=draw.clear[1]=draw.clear[2]=draw.clear[3]=1;
 draw.viewport={32,224,192,-192,0,1};draw.scissor={{48,56},{160,144}};b.submit({draw,read});
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t want[]={255,255,255,255};
  if(x>=48 && x<208 && y>=56 && y<200){uint8_t texel[4];fuxiM7Texel(0,(x-32)*256/192,(y-32)*256/192,texel);want[2]=texel[2];}
  require(!memcmp((uint8_t *)readback->contents+256+4*(y*256+x),want,4),"write mask/viewport/scissor");}
 b.destroy(masked);b.destroy(plain);puts("PASS: planned Metal->Vulkan red mask + inset viewport/scissor exact pixels");
 // Common fixed-function Porter-Duff pairs (Metal blend-factor ordinals).
 const unsigned pairs[][2]={{1,0},{1,1},{9,1},{8,0},{0,4},{9,0},{0,5},{8,5},{9,4}};
 for(auto &pair:pairs){auto blendOptions=options;blendOptions.srcRGB=blendOptions.srcAlpha=pair[0];blendOptions.dstRGB=blendOptions.dstAlpha=pair[1];
  auto *blended=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",blendOptions);
  draw=base;draw.pipeline=blended;draw.strip=0;draw.vertices=6;draw.samples[0]=t[1];draw.buffers[1][1]={half,0,true};
  draw.clear[0]=1;draw.clear[1]=0;draw.clear[2]=1;draw.clear[3]=1;b.submit({draw,read});
  auto factor=[](unsigned f){switch(f){case 0:return 0u;case 1:case 8:return 255u;case 4:return 127u;case 5:return 128u;case 9:return 0u;default:throw std::logic_error("factor oracle");}};
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t src[4],want[4];fuxiM7Texel(1,x,y,src);const unsigned dst[]={255,0,255,255};
   for(unsigned c=0;c<4;++c)want[c]=std::min(255u,((src[c]/2)*factor(pair[0])+dst[c]*factor(pair[1])+127)/255);
   require(!memcmp((uint8_t *)readback->contents+256+4*(y*256+x),want,4),"Porter-Duff blend pixels");}
  b.destroy(blended);
 }
 puts("PASS: copy, plus, destination-over, source/destination in/out/atop fixed-function pairs");
 // Float4 fetch in addition to the normalized uchar4 color used by stage 6.
 float floatVertices[4][8];for(unsigned i=0;i<4;++i){memcpy(floatVertices[i],&fuxiM7Vertices[i],16);for(unsigned c=4;c<8;++c)floatVertices[i][c]=1;}
 auto *floatBuffer=b.allocate(sizeof(floatVertices));memcpy(floatBuffer->contents,floatVertices,sizeof(floatVertices));
 auto floatOptions=unblended;floatOptions.attributes[2].format=31;floatOptions.strides[0]=32;
 auto *floatPipeline=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",floatOptions);
 draw=base;draw.pipeline=floatPipeline;draw.strip=0;draw.vertices=6;draw.buffers[0][0]={floatBuffer,0,false};b.submit({draw,read});
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t want[4];fuxiM7Texel(0,x,y,want);require(!memcmp((uint8_t *)readback->contents+256+4*(y*256+x),want,4),"float4 attribute");}
 b.destroy(floatPipeline);b.destroy(floatBuffer);puts("PASS: Float4 vertex color fetch");
 // M9d: exact-width half coordinates plus a separate tightly sized color
 // binding. Red exposes BGRA swizzles and normalization/packed conversion.
 const uint16_t halfCoordinates[4][4]={{0xbc00,0x3c00,0,0},{0x3c00,0x3c00,0x3c00,0},
  {0xbc00,0xbc00,0,0x3c00},{0x3c00,0xbc00,0x3c00,0x3c00}};
 auto *coordinates=b.allocate(sizeof(halfCoordinates));memcpy(coordinates->contents,halfCoordinates,sizeof(halfCoordinates));
 for(uint32_t format:{27u,30u,21u,42u,40u,41u,54u}) {
  uint8_t color[16]{};uint32_t word=0;
  if(format==27){const uint16_t rgba[4]={0x3c00,0,0,0x3c00};memcpy(color,rgba,8);}
  else if(format==30){const float rgb[3]={1,0,0};memcpy(color,rgb,12);}
  else if(format==21){const uint16_t rgba[4]={65535,0,0,65535};memcpy(color,rgba,8);}
  else if(format==42){color[2]=255;color[3]=255;}
  else {word=format==40?0x400001ffu:format==41?0xc00003ffu:0x3c0u;memcpy(color,&word,4);}
  uint32_t size=fuxi::vertexFormatInfo(format).bytes;auto *colors=b.allocate(4*size);
  for(unsigned i=0;i<4;++i)memcpy((uint8_t *)colors->contents+i*size,color,size);
  auto widened=unblended;widened.attributes[0]={25,0,0};widened.attributes[1]={25,4,0};
  widened.attributes[2]={format,0,1};widened.strides[0]=8;widened.strides[1]=size;
  auto *pipeline=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",widened);
  draw=base;draw.pipeline=pipeline;draw.buffers[0][0]={coordinates,0,false};draw.buffers[0][1]={colors,0,false};
  b.submit({draw,read});
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){uint8_t want[4];fuxiM7Texel(0,x,y,want);want[0]=want[1]=0;
   require(!memcmp((uint8_t *)readback->contents+256+4*(y*256+x),want,4),"M9d half/multiple-buffer vertex pixels");}
  auto shortDraw=draw;shortDraw.buffers[0][1].offset=1;refused({shortDraw});
  b.destroy(pipeline);b.destroy(colors);
 }
 b.destroy(coordinates);puts("PASS: Half2 + Half4/Float3/UShort4Normalized/BGRA/packed vertex fetch, independent bindings and exact buffer bounds");
 for(auto *buffer:{vertices,indices,readback,full,half})b.destroy(buffer);b.destroy(sam);for(unsigned i=0;i<3;++i){b.destroy(t[i]);free(pages[i]);}b.destroy(p);b.destroy(cached);tr.close(lib);
 puts("PASS: M7 AIR control source-over, float2/uchar4norm vertices with offset16, uint16 indices offset4, strips, function constant, inline uniform, two passes/load/scissor, 65536 exact pixels + same-memory + sentinels + refusals");
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
