/* Original bounded persistence. All Vulkan/cache access is under Backend::mutex.
 * Cache I/O is best effort: no exceptions or driver input before validation. */
#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace fuxi {
class PipelineCache {
public:
 static constexpr size_t maxBytes=32*1024*1024;
 static constexpr size_t headerBytes=336;
private:
 struct FD {int n;explicit FD(int v=-1):n(v){}~FD(){if(n>=0)::close(n);}FD(const FD&)=delete;};
 VkDevice device{};VkPipelineCache cache{};
 PFN_vkCreatePipelineCache create{};PFN_vkDestroyPipelineCache destroy{};PFN_vkGetPipelineCacheData data{};
 VkPhysicalDeviceProperties props{};
 std::array<uint8_t,headerBytes> identity{};
 std::string client="unknown",filename;
 int directory=-1;unsigned pending=0;bool dirty=false;
 std::chrono::steady_clock::time_point lastSave{};
 static inline std::atomic<unsigned> sequence{0};
 static uint32_t word(const uint8_t *p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
 static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
 static bool transfer(int fd,void *p,size_t n,bool writing){
  auto *b=(uint8_t *)p;while(n){ssize_t k=writing ? ::write(fd,b,n) : ::read(fd,b,n);if(k<0 && errno==EINTR)continue;if(k<=0)return false;b+=k;n-=size_t(k);}return true;
 }
 bool secure(int fd,bool dir) const {
  struct stat s{};return fd>=0 && !fstat(fd,&s) && (dir?S_ISDIR(s.st_mode):S_ISREG(s.st_mode)) &&
   s.st_uid==owner() && (s.st_mode&07777)==(dir?0700:0600) && (dir || s.st_nlink==1);
 }
 static uid_t owner(){
#ifdef FUXI_MTL_HOST_TEST
  return geteuid();
#else
  return 0;
#endif
 }
 void disable(){if(directory>=0)::close(directory);directory=-1;}
 const char *validatePayload(const uint8_t *p,size_t n) const {
  // Vulkan v1 header is exactly 32 bytes. Decode fields without casts/alignment.
  if(n<32 || word(p)!=32)return "vk-length";
  if(word(p+4)!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE)return "vk-version";
  if(word(p+8)!=props.vendorID || word(p+12)!=props.deviceID)return "vk-device";
  if(memcmp(p+16,props.pipelineCacheUUID,VK_UUID_SIZE))return "vk-uuid";
  return nullptr;
 }
 const char *load(std::vector<uint8_t> &payload){
  if(directory<0)return nullptr;
  FD f(openat(directory,filename.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
  if(f.n<0)return errno==ENOENT?nullptr:"open";
  if(!secure(f.n,false))return "permissions";
  struct stat s{};if(fstat(f.n,&s) || s.st_size<off_t(headerBytes))return "length";
  if(s.st_size>off_t(headerBytes+maxBytes))return "oversize";
  std::array<uint8_t,headerBytes> h{};if(!transfer(f.n,h.data(),h.size(),false))return "read";
  if(memcmp(h.data(),identity.data(),16))return "format";
  size_t n=word(h.data()+16);if(n<32 || n>maxBytes || off_t(n+headerBytes)!=s.st_size)return "length";
  if(memcmp(h.data()+24,identity.data()+24,headerBytes-24))return "identity";
  payload.resize(n);if(!transfer(f.n,payload.data(),n,false))return "read";
  uint8_t extra{};if(::read(f.n,&extra,1)!=0)return "length";
  if(crc32(payload.data(),n)!=word(h.data()+20))return "checksum";
  return validatePayload(payload.data(),n);
 }
 bool saveData(size_t &bytes){
  if(data(device,cache,&bytes,nullptr)!=VK_SUCCESS || bytes<32 || bytes>maxBytes)return false;
  std::vector<uint8_t> payload(bytes);
  if(data(device,cache,&bytes,payload.data())!=VK_SUCCESS || bytes<32 || bytes>payload.size() || validatePayload(payload.data(),bytes))return false;
  auto h=identity;put(h.data()+16,uint32_t(bytes));put(h.data()+20,crc32(payload.data(),bytes));
  char temp[96];snprintf(temp,sizeof(temp),".cache-%lu-%u",(unsigned long)getpid(),sequence.fetch_add(1));
  FD f(openat(directory,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
  if(f.n<0)return false;
  bool ok=secure(f.n,false) && transfer(f.n,h.data(),h.size(),true) && transfer(f.n,payload.data(),bytes,true) && !fsync(f.n);
  if(ok)ok=!renameat(directory,temp,directory,filename.c_str());
  if(ok)ok=!fsync(directory);
  unlinkat(directory,temp,0);return ok;
 }
 void save() noexcept {
  if(directory<0 || !cache || !dirty)return;
  auto began=std::chrono::steady_clock::now();size_t bytes=0;bool ok=false;
  try {ok=saveData(bytes);}catch(...){}
  lastSave=std::chrono::steady_clock::now();saveResult=ok?1:3;
  if(ok){saveBytes=bytes;++saveCount;dirty=false;pending=0;}else {++saveFailures;disable();}
  fprintf(stderr,"FUXI MTL: pipeline cache save bytes=%zu ms=%.3f result=%s\n",ok?bytes:0,
   std::chrono::duration<double,std::milli>(lastSave-began).count(),ok?"saved":"memory-only");
 }
public:
 // LE: magic[8], format, header length, payload length, CRC32, driver version,
 // driver ID, driver UUID[16], driver info[256], translator SHA256[32].
 // Last open/save outcome, accepted payload bytes, and process-total save counts.
 // load: 0 empty, 1 hit, 2 discard. save: 0 none, 1 saved, 2 unavailable, 3 error.
 static inline std::atomic<uint64_t> loadResult{0},loadBytes{0},saveResult{0},saveBytes{0},saveCount{0},saveFailures{0};
 static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
 void open(VkDevice d,PFN_vkCreatePipelineCache c,PFN_vkDestroyPipelineCache x,PFN_vkGetPipelineCacheData g,
           const VkPhysicalDeviceProperties &p,const VkPhysicalDeviceDriverProperties &driver,
           const VkPhysicalDeviceIDProperties &id,const uint8_t *translator,const char *name) noexcept {
  device=d;create=c;destroy=x;data=g;props=p;loadResult=0;loadBytes=0;saveResult=0;saveBytes=0;
  const char *reason=nullptr;std::vector<uint8_t> payload;
  try {
   // Process basename only, never argv, paths or unchecked text in logs/files.
   bool valid=name && *name && strlen(name)<=63;
   if(valid)for(const char *q=name;*q;++q)if(!((*q>='a' && *q<='z') || (*q>='A' && *q<='Z') || (*q>='0' && *q<='9') || *q=='-' || *q=='_'))valid=false;
   if(valid)client=name;
   filename=client+".cache";memcpy(identity.data(),"FXMTLC01",8);put(identity.data()+8,1);put(identity.data()+12,headerBytes);
   put(identity.data()+24,p.driverVersion);put(identity.data()+28,driver.driverID);
   memcpy(identity.data()+32,id.driverUUID,16);memcpy(identity.data()+48,driver.driverInfo,256);
   if(translator)memcpy(identity.data()+304,translator,32);
   if(valid && translator && geteuid()==owner()) {
#ifdef FUXI_MTL_HOST_TEST
    // Host harness only. No production environment override of trusted storage.
    const char *root=getenv("FUXI_MTL_TEST_CACHE_DIR");
    if(root)directory=::open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
#else
    FD parent(::open("/private/var/db",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if(parent.n>=0 && (!mkdirat(parent.n,"fuxi-metal",0700) || errno==EEXIST))
     directory=openat(parent.n,"fuxi-metal",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
#endif
    if(!secure(directory,true))disable();
   }
   if(directory<0)saveResult=2;
   reason=load(payload);
   if(reason){payload.clear();loadResult=2;}else if(!payload.empty()){loadResult=1;loadBytes=payload.size();}
  }catch(...){reason="allocation";payload.clear();loadResult=2;disable();saveResult=2;}
  // A valid cache may still be refused by a driver: retry empty, never fail open.
  VkPipelineCacheCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;ci.initialDataSize=payload.size();ci.pInitialData=payload.empty()?nullptr:payload.data();
  if(create(device,&ci,nullptr,&cache)!=VK_SUCCESS){cache={};if(!payload.empty()){reason="driver";loadResult=2;loadBytes=0;ci.initialDataSize=0;ci.pInitialData=nullptr;if(create(device,&ci,nullptr,&cache)!=VK_SUCCESS)cache={};}}
  char result[48];if(reason)snprintf(result,sizeof(result),"discard(%s)",reason);
  else snprintf(result,sizeof(result),"%s",loadResult==1?"hit":"empty");
  fprintf(stderr,"FUXI MTL: pipeline cache load %s bytes=%llu result=%s\n",client.c_str(),(unsigned long long)loadBytes.load(),result);
 }
 VkPipelineCache get() const {return cache;}
 void finishedPair(double milliseconds=0) noexcept {
  if(!cache)return;dirty=true;++pending;
  auto now=std::chrono::steady_clock::now();
  // At most one checkpoint per second; called only after new pipeline builds.
  if((pending>=4 || milliseconds>=100) && now-lastSave>=std::chrono::seconds(1))save();
  size_t bytes=0;
  if(data(device,cache,&bytes,nullptr)!=VK_SUCCESS || bytes>maxBytes){
   // Never serialize an oversized generation. Existing VkPipelines survive.
   destroy(device,cache,nullptr);cache={};dirty=false;pending=0;
   VkPipelineCacheCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
   if(create(device,&ci,nullptr,&cache)!=VK_SUCCESS)cache={};
  }
 }
 void close() noexcept {save();if(cache)destroy(device,cache,nullptr);cache={};disable();pending=0;dirty=false;}
 PipelineCache()=default;
 PipelineCache(const PipelineCache&)=delete;PipelineCache& operator=(const PipelineCache&)=delete;
};
}
