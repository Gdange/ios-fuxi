/* Authored AIR/pixels only. Production persistence and deterministic driver controls. */
#include "fuxi-metal/Backend.hpp"
#include "fuxi-metal/M7Reference.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
using Cache=fuxi::PipelineCache;
static std::vector<uint8_t> fakePayload(64);
static unsigned fakeSeeds=0,fakeCreates=0,fakeDestroys=0;
static bool refuseSeed=false;
static VkResult fakeDataResult=VK_SUCCESS;
static VkResult VKAPI_CALL fakeCreate(VkDevice,const VkPipelineCacheCreateInfo *ci,const VkAllocationCallbacks *,VkPipelineCache *out){
 ++fakeCreates;if(ci->initialDataSize){++fakeSeeds;assert(ci->initialDataSize==fakePayload.size());assert(!memcmp(ci->pInitialData,fakePayload.data(),fakePayload.size()));if(refuseSeed)return VK_ERROR_INITIALIZATION_FAILED;}
 *out=(VkPipelineCache)(uintptr_t)1;return VK_SUCCESS;
}
static void VKAPI_CALL fakeDestroy(VkDevice,VkPipelineCache,const VkAllocationCallbacks *){++fakeDestroys;}
static VkResult VKAPI_CALL fakeData(VkDevice,VkPipelineCache,size_t *n,void *p){
 if(fakeDataResult!=VK_SUCCESS)return fakeDataResult;
 if(!p){*n=fakePayload.size();return VK_SUCCESS;}assert(*n>=fakePayload.size());memcpy(p,fakePayload.data(),fakePayload.size());*n=fakePayload.size();return VK_SUCCESS;
}
static void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(i*8));}
static void unit(const char *directory){
 assert(!setenv("FUXI_MTL_TEST_CACHE_DIR",directory,1));
 VkPhysicalDeviceProperties props{};props.vendorID=11;props.deviceID=22;memset(props.pipelineCacheUUID,33,16);
 put(fakePayload.data(),32);put(fakePayload.data()+4,1);put(fakePayload.data()+8,11);put(fakePayload.data()+12,22);memset(fakePayload.data()+16,33,16);
 VkPhysicalDeviceDriverProperties driver{};VkPhysicalDeviceIDProperties id{};uint8_t translator[32]{};
 auto open=[&](Cache &c){c.open({},fakeCreate,fakeDestroy,fakeData,props,driver,id,translator,"unit");};
 auto path=std::string(directory)+"/unit.cache";unlink(path.c_str());
 {Cache c;open(c);assert(Cache::loadResult==0);for(unsigned i=0;i<3;++i)c.finishedPair();assert(Cache::saveCount==0);c.finishedPair();assert(Cache::saveCount==1 && Cache::saveBytes==64);
  // Slow-build checkpoints are rate bounded; close flushes pending changes.
  c.finishedPair(101);assert(Cache::saveCount==1);c.close();assert(Cache::saveCount==2);c.close();assert(Cache::saveCount==2);}
 {Cache c;open(c);assert(Cache::loadResult==1 && fakeSeeds==1);c.close();}
 auto read=[&]{std::ifstream in(path,std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(in),{}};};
 auto original=read();assert(original.size()==Cache::headerBytes+64);
 auto write=[&](std::vector<uint8_t> bytes){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write((const char *)bytes.data(),bytes.size());};
 auto rejected=[&](std::vector<uint8_t> bytes){write(bytes);auto seeds=fakeSeeds;Cache c;open(c);assert(Cache::loadResult==2 && Cache::loadBytes==0 && fakeSeeds==seeds);c.close();};
 auto damaged=original;damaged.resize(12);rejected(damaged);
 damaged=original;damaged.back()^=1;rejected(damaged);
 damaged=original;damaged[Cache::headerBytes+16]^=1;put(damaged.data()+20,Cache::crc32(damaged.data()+Cache::headerBytes,64));rejected(damaged);
 for(unsigned offset:{0u,8u,12u,16u,24u,28u,32u,48u,304u}){damaged=original;damaged[offset]^=1;rejected(damaged);}
 for(unsigned offset:{0u,4u,8u,12u}){damaged=original;damaged[Cache::headerBytes+offset]^=1;put(damaged.data()+20,Cache::crc32(damaged.data()+Cache::headerBytes,64));rejected(damaged);}
 write(original);refuseSeed=true;{Cache c;auto creates=fakeCreates;open(c);assert(Cache::loadResult==2 && fakeCreates==creates+2 && c.get());c.close();}refuseSeed=false;
 // Never follow files or accept unsafe owner/mode; no driver seeds on refusal.
 write(original);assert(!chmod(path.c_str(),0644));{Cache c;auto seeds=fakeSeeds;open(c);assert(Cache::loadResult==2 && fakeSeeds==seeds);c.close();}assert(!chmod(path.c_str(),0600));
 struct stat permissions{};assert(!stat(path.c_str(),&permissions));assert(permissions.st_uid==geteuid() && permissions.st_nlink==1);
 auto hardlink=std::string(directory)+"/hardlink";assert(!link(path.c_str(),hardlink.c_str()));
 {Cache c;auto seeds=fakeSeeds;open(c);assert(Cache::loadResult==2 && fakeSeeds==seeds);c.close();}assert(!unlink(hardlink.c_str()));
 unlink(path.c_str());assert(!symlink("absent",path.c_str()));{Cache c;open(c);assert(Cache::loadResult==2);c.close();}unlink(path.c_str());
 // The 32 MiB cap is checked before allocation/read (sparse file is sufficient).
 {int fd=::open(path.c_str(),O_CREAT|O_WRONLY,0600);assert(fd>=0);assert(!ftruncate(fd,Cache::headerBytes+Cache::maxBytes+1));::close(fd);}
 {Cache c;open(c);assert(Cache::loadResult==2);c.close();}unlink(path.c_str());
 // Read-only directory policy works even when this harness runs as root.
 assert(!chmod(directory,0500));{Cache c;open(c);assert(Cache::loadResult==0 && Cache::saveResult==2 && c.get());c.finishedPair(101);c.close();}assert(!chmod(directory,0700));
 assert(!chmod(directory,0755));{Cache c;open(c);assert(Cache::loadResult==0 && Cache::saveResult==2);c.close();}assert(!chmod(directory,0700));
 assert(fakeCreates==fakeDestroys+1); // The intentionally refused seeded create has no object.
 // Fresh child processes exercise the production backend and optional-cache fault paths.
 for(unsigned i=0;i<3;++i){pid_t pid=fork();assert(pid>=0);if(!pid){
   char boot[37];snprintf(boot,sizeof(boot),"aaaaaaaa-1111-2222-3333-%012u",i);
   assert(fuxi::backboardGate.admit(boot,::open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW)));
   if(i<2){try {fuxi::check(i?VK_TIMEOUT:VK_ERROR_DEVICE_LOST);assert(false);}catch(const std::runtime_error &) {}}
   else {fakeDataResult=VK_ERROR_DEVICE_LOST;Cache c;open(c);c.finishedPair(101);c.close();}
   assert(fuxi::backboardGate.blocked());_exit(0);
  }int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
 }
 // A refused host-pointer import falls back to a shadow copy and must not latch the CPU.
 {pid_t pid=fork();assert(pid>=0);if(!pid){
   assert(fuxi::backboardGate.admit("aaaaaaaa-1111-2222-3333-000000000009",::open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW)));
   try {fuxi::importCheck(VK_ERROR_INVALID_EXTERNAL_HANDLE);assert(false);}catch(const std::runtime_error &) {}
   assert(!fuxi::backboardGate.blocked());_exit(0);
  }int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));}
 puts("PASS: M9j cache UID/mode/nlink checks and backend loss/timeout/optional-cache loss latch CPU fallback, host-import refusal does not");
 puts("PASS: M9i validation-before-driver, checkpoints, retry, permissions, cap and memory-only controls");
}
int main(int argc,char **argv){
 if(argc==3 && !strcmp(argv[1],"--unit")){unit(argv[2]);return 0;}
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[512]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib);
 fuxi::Backend b("fuxi-m9i-test");constexpr unsigned width=64,height=64,bytes=width*height*4;
 fuxi::CompileOptions options{};options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *p=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto *src=b.allocateTexture(80,1,width,height),*dst=b.allocateTexture(80,5,width,height);
 auto *upload=b.allocate(bytes),*read=b.allocate(bytes),*vertices=b.allocate(sizeof(fuxiM7Vertices)),*indices=b.allocate(sizeof(fuxiM7Indices)),*ones=b.allocate(16);
 memcpy(vertices->contents,fuxiM7Vertices,sizeof(fuxiM7Vertices));memcpy(indices->contents,fuxiM7Indices,sizeof(fuxiM7Indices));
 for(unsigned c=0;c<4;++c)((float *)ones->contents)[c]=1;
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto *q=(uint8_t *)upload->contents+(y*width+x)*4;q[0]=x<32?32:224;q[1]=y<32?192:48;q[2]=64;q[3]=255;}
 fuxi::Operation putOp{};putOp.texture=src;putOp.src=upload;putOp.upload=true;b.submit({putOp});
 auto *sam=b.allocateSampler(false,false);fuxi::Operation draw{};draw.texture=dst;draw.pipeline=p;draw.indices=indices;draw.vertices=6;
 draw.samples[0]=src;draw.samplers[0]=sam;draw.buffers[0][0]={vertices,0,false};draw.buffers[0][2]={ones,0,true};draw.buffers[1][1]={ones,0,true};
 draw.viewport={0,height,width,-float(height),0,1};draw.scissor={{0,0},{width,height}};
 fuxi::Operation get{};get.texture=dst;get.dst=read;b.submit({draw,get});assert(!memcmp(read->contents,upload->contents,bytes));
 auto pixels=Cache::crc32((const uint8_t *)read->contents,bytes);
 if(getenv("FUXI_MTL_TEST_IDLE_CHECKPOINT")){
  // The GPU is idle and the device remains alive: checkpoint must not depend
  // on another pipeline build, submit, atexit or orderly process shutdown.
  std::this_thread::sleep_for(std::chrono::seconds(6));
  auto path=std::string(getenv("FUXI_MTL_TEST_CACHE_DIR"))+"/fuxi-m9i-test.translations";
  struct stat s{};assert(!stat(path.c_str(),&s) && s.st_size>128);
  puts("PASS: idle checkpoint before teardown");fflush(stdout);
  if(getenv("FUXI_MTL_TEST_CRASH_AFTER_IDLE"))_exit(0);
 }
 b.destroy(p);b.destroy(sam);b.destroy(src);b.destroy(dst);b.destroy(upload);b.destroy(read);b.destroy(vertices);b.destroy(indices);b.destroy(ones);tr.close(lib);b.close();
 printf("PASS pixels=%08x load=%llu bytes=%llu save=%llu saved=%llu\n",pixels,(unsigned long long)Cache::loadResult.load(),
  (unsigned long long)Cache::loadBytes.load(),(unsigned long long)Cache::saveResult.load(),(unsigned long long)Cache::saveBytes.load());
}
