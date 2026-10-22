/* Authored AIR only: production Backend reuse/rollback/pixel controls. */
#include "fuxi-metal/Backend.hpp"
#include "fuxi-metal/M7Reference.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <thread>
#include <condition_variable>
static void reject(const std::function<void()> &f){bool failed=false;try{f();}catch(const std::runtime_error&){failed=true;}assert(failed);}
static bool same(const fuxi::Backend::ReuseCounts &a,const fuxi::Backend::ReuseCounts &b){
 return a.commandPools==b.commandPools && a.commandBuffers==b.commandBuffers && a.fences==b.fences && a.queries==b.queries && a.descriptors==b.descriptors && a.frames==b.frames && a.views==b.views;
}
int main(int argc,char **argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[512]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib);
 fuxi::Backend b;constexpr unsigned width=64,height=64,bytes=width*height*4;
 fuxi::CompileOptions options{};options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto *src=b.allocateTexture(80,1,width,height),*dst=b.allocateTexture(80,5,width,height);
 auto *upload=b.allocate(bytes),*read=b.allocate(bytes),*vertices=b.allocate(sizeof(fuxiM7Vertices)),*indices=b.allocate(sizeof(fuxiM7Indices)),*ones=b.allocate(16);
 memcpy(vertices->contents,fuxiM7Vertices,sizeof(fuxiM7Vertices));memcpy(indices->contents,fuxiM7Indices,sizeof(fuxiM7Indices));
 for(unsigned c=0;c<4;++c)((float *)ones->contents)[c]=1;
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto *q=(uint8_t *)upload->contents+(y*width+x)*4;q[0]=x<32?32:224;q[1]=y<32?192:48;q[2]=64;q[3]=255;}
 fuxi::Operation put{};put.texture=src;put.src=upload;put.upload=true;b.submit({put});
 auto *sam=b.allocateSampler(false,false);fuxi::Operation draw{};draw.texture=dst;draw.pipeline=p;draw.indices=indices;draw.vertices=6;
 draw.samples[0]=src;draw.samplers[0]=sam;draw.buffers[0][0]={vertices,0,false};draw.buffers[0][2]={ones,0,true};draw.buffers[1][1]={ones,0,true};
 draw.viewport={0,height,width,-float(height),0,1};draw.scissor={{0,0},{width,height}};
 fuxi::Operation get{};get.texture=dst;get.dst=read;
 // Reference uses the same recorder but destroys every submit object/cache after each fence.
 b.discardReuseForTest();b.reuseSubmitObjects=false;b.submit({draw,get});
 assert(!memcmp(read->contents,upload->contents,bytes));
 std::vector<uint8_t> reference((uint8_t *)read->contents,(uint8_t *)read->contents+bytes);
 b.reuseSubmitObjects=true;b.submit({draw,get});auto warm=b.reuseCounts;
 assert(b.cachedFrames()==1 && b.cachedViews()==1);
 auto memory=fuxi::liveMemoryObjects.load();auto charged=b.resourceBytes();auto samples=b.gpuSamples.load();
 for(unsigned i=0;i<256;++i){b.submit({draw,get});assert(!memcmp(read->contents,reference.data(),bytes));}
 assert(same(warm,b.reuseCounts) && fuxi::liveMemoryObjects==memory && b.resourceBytes()==charged && b.gpuSamples==samples+256);
 // A bigger frame grows exactly one descriptor pool; steady size never creates again.
 std::vector<fuxi::Operation> large(33,draw);large.push_back(get);b.submit(large);
 assert(b.reuseCounts.descriptors==warm.descriptors+1 && b.reuseCounts.commandPools==warm.commandPools);
 warm=b.reuseCounts;for(unsigned i=0;i<64;++i)b.submit(large);assert(same(warm,b.reuseCounts));
 assert(!memcmp(read->contents,reference.data(),bytes));
 // Failure after recording changed layouts/defined state: restore metadata, destroy uncertain
 // reusable recording state, then recreate once. Cached valid frame/views can survive.
 auto layout=dst->layout;auto defined=dst->defined;draw.store=0;b.failBeforeQueue=true;
 reject([&]{b.submit({draw});});assert(!b.poisoned && dst->layout==layout && dst->defined==defined);
 draw.store=1;b.submit({draw,get});assert(!memcmp(read->contents,reference.data(),bytes));
 assert(b.reuseCounts.commandPools==warm.commandPools+1 && b.reuseCounts.commandBuffers==warm.commandBuffers+1 && b.reuseCounts.fences==warm.fences+1 && b.reuseCounts.queries==warm.queries+1 && b.reuseCounts.descriptors==warm.descriptors+1);
 warm=b.reuseCounts;b.submit({draw,get});assert(same(warm,b.reuseCounts));
 // Failure while recording (after BeginCommandBuffer), then successful reset/recreation.
 auto *undefined=b.allocateTexture(80,1,width,height);auto bad=draw;bad.samples[0]=undefined;
 reject([&]{b.submit({bad});});assert(!b.poisoned);b.submit({draw,get});assert(!memcmp(read->contents,reference.data(),bytes));b.destroy(undefined);
 // Refuse descriptor growth above the bound, then prove the live lane recovers.
 std::vector<fuxi::Operation> excessive(4097,draw);reject([&]{b.submit(excessive);});assert(!b.poisoned);b.submit({draw,get});
 assert(!memcmp(read->contents,reference.data(),bytes));
 // Cache overflow uses fence-retired transient objects; never evict an object
 // already referenced by this recording. All 140 targets remain pixel-correct.
 std::vector<fuxi::Texture *> sources,targets;std::vector<fuxi::Operation> overflow;
 for(unsigned i=0;i<140;++i){auto *source=b.allocateTexture(80,1,width,height),*target=b.allocateTexture(80,5,width,height);sources.push_back(source);targets.push_back(target);
  auto uploadOp=put;uploadOp.texture=source;overflow.push_back(uploadOp);auto d=draw;d.samples[0]=source;d.texture=target;overflow.push_back(d);
 }
 b.submit(overflow);assert(b.cachedFrames()==128 && b.cachedViews()==128);memory=fuxi::liveMemoryObjects.load();
 b.submit(overflow);assert(b.cachedFrames()==128 && b.cachedViews()==128 && fuxi::liveMemoryObjects==memory);
 for(auto *target:targets){auto readOp=get;readOp.texture=target;b.submit({readOp});assert(!memcmp(read->contents,reference.data(),bytes));b.destroy(target);}
 for(auto *source:sources)b.destroy(source);
 assert(b.cachedFrames()==2 && b.cachedViews()==1); // Both store actions of the original target.
 // Empty work waits behind the synchronous lane and invokes its scheduling callback
 // exactly once; it cannot change GPU samples, object creation, or submit counts.
 std::mutex latch;std::condition_variable ready;bool held=false,release=false,emptyEntered=false;unsigned sequence=0;
 std::thread first([&]{b.submit({draw},[&]{std::unique_lock<std::mutex> lock(latch);assert(++sequence==1);held=true;ready.notify_all();ready.wait(lock,[&]{return release;});});});
 {std::unique_lock<std::mutex> lock(latch);ready.wait(lock,[&]{return held;});}
 std::thread second([&]{{std::lock_guard<std::mutex> lock(latch);emptyEntered=true;ready.notify_all();}b.submit({},[&]{assert(++sequence==2);});});
 {std::unique_lock<std::mutex> lock(latch);ready.wait(lock,[&]{return emptyEntered;});assert(sequence==1);release=true;ready.notify_all();}
 first.join();second.join();assert(sequence==2);
 warm=b.reuseCounts;samples=b.gpuSamples.load();auto submits=b.submits.load(),empty=b.emptySubmits.load();
 for(unsigned i=0;i<256;++i)b.submit({});
 assert(same(warm,b.reuseCounts) && b.gpuSamples==samples && b.submits==submits && b.emptySubmits==empty+256);
 // Destroy invalidates every load/store framebuffer and sampled view, before address reuse.
 b.destroy(src);assert(b.cachedViews()==0);b.destroy(dst);assert(b.cachedFrames()==0);
 src=b.allocateTexture(80,1,width,height);dst=b.allocateTexture(80,5,width,height);put.texture=src;b.submit({put});draw.texture=dst;draw.samples[0]=src;get.texture=dst;b.submit({draw,get});
 assert(!memcmp(read->contents,reference.data(),bytes) && b.cachedFrames()==1 && b.cachedViews()==1);
 b.dropPipelineCacheForTest();b.destroy(p);assert(b.cachedFrames()==0 && b.cachedViews()==1);
 b.destroy(sam);b.destroy(src);b.destroy(dst);b.destroy(upload);b.destroy(read);b.destroy(vertices);b.destroy(indices);b.destroy(ones);tr.close(lib);
 // An attempted queue failure keeps conservative poison ownership. Empty work
 // must refuse too; no resource recreation or "recovery" after uncertain ownership.
 {fuxi::Backend lost;auto *buffer=lost.allocate(16);fuxi::Operation fill{};fill.dst=buffer;fill.size=16;lost.failQueueAttempt=true;
  reject([&]{lost.submit({fill});});assert(lost.poisoned);auto counts=lost.reuseCounts;
  reject([&]{lost.submit({});});reject([&]{lost.submit({fill});});assert(same(counts,lost.reuseCounts));lost.destroy(buffer);
 } // Poisoned Vulkan allocations intentionally survive until process exit.

 puts("PASS: M9h bounded reuse, descriptor growth, rollback/recreation, synchronous empty completion, texture invalidation and reference pixels");
}
