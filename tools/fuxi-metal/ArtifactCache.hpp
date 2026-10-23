/* Original bounded translation store. Inspired by UniXNU NativeMetal's two-layer
 * cache design (MIT); no reference implementation is copied. Backend::mutex owns
 * callbacks, maintenance and teardown. Never serialize library/AIR inputs. */
#pragma once
#include "PipelineCache.hpp"
#include <algorithm>
namespace fuxi {
class ArtifactCache {
 struct Entry {std::array<uint8_t,32> key;std::vector<uint8_t> bytes;};
 std::vector<Entry> entries;
 std::array<uint8_t,128> header{};
 int directory=-1;std::string filename;
 size_t used=0;bool dirty=false;
 uint64_t hits=0,misses=0,rejected=0;
 static uint32_t word(const uint8_t *p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
 static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
 static bool transfer(int fd,void *p,size_t n,bool writing){
  auto *b=(uint8_t *)p;while(n){ssize_t k=writing ? ::write(fd,b,n) : ::read(fd,b,n);if(k<0 && errno==EINTR)continue;if(k<=0)return false;b+=k;n-=size_t(k);}return true;
 }
 const char *load() {
  int fd=openat(directory,filename.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  if(fd<0)return errno==ENOENT?nullptr:"open";
  struct Close {int fd;~Close(){::close(fd);}} close{fd};struct stat s{};
  if(!privateStorage(fd,false,geteuid()))return "permissions";
  if(fstat(fd,&s) || s.st_size<off_t(header.size()) || s.st_size>off_t(header.size()+maxBytes))return "length";
  auto h=header;if(!transfer(fd,h.data(),h.size(),false))return "read";
  if(memcmp(h.data(),header.data(),16) || memcmp(h.data()+28,header.data()+28,100))return "identity";
  size_t n=word(h.data()+16),count=word(h.data()+24);
  if(n>maxBytes || n+header.size()!=size_t(s.st_size) || count>maxEntries)return "length";
  std::vector<uint8_t> bytes(n);if(!transfer(fd,bytes.data(),n,false))return "read";
  uint8_t extra{};if(::read(fd,&extra,1)!=0)return "length";
  if(PipelineCache::crc32(bytes.data(),n)!=word(h.data()+20))return "checksum";
  std::vector<Entry> decoded;size_t at=0;
  for(size_t i=0;i<count;++i){
   if(n-at<36)return "record";Entry e{};memcpy(e.key.data(),bytes.data()+at,32);size_t length=word(bytes.data()+at+32);at+=36;
   if(!length || length>maxEntry || length>n-at || std::any_of(decoded.begin(),decoded.end(),[&](const Entry &x){return x.key==e.key;}))return "record";
   e.bytes.assign(bytes.begin()+at,bytes.begin()+at+length);at+=length;decoded.push_back(std::move(e));
  }
  if(at!=n)return "record";entries=std::move(decoded);used=n;return nullptr;
 }
 bool save(bool &busy) {
  CacheWriteLease lease(directory);if(lease.fd<0){busy=lease.busy;return false;}
  std::vector<uint8_t> bytes;bytes.reserve(used);
  for(const auto &e:entries){bytes.insert(bytes.end(),e.key.begin(),e.key.end());auto at=bytes.size();bytes.resize(at+4);put(bytes.data()+at,uint32_t(e.bytes.size()));bytes.insert(bytes.end(),e.bytes.begin(),e.bytes.end());}
  auto h=header;put(h.data()+16,uint32_t(bytes.size()));put(h.data()+20,PipelineCache::crc32(bytes.data(),bytes.size()));put(h.data()+24,uint32_t(entries.size()));
  char temp[96];snprintf(temp,sizeof(temp),"%s.tmp",filename.c_str());unlinkat(directory,temp,0);
  int fd=openat(directory,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return false;
  bool ok=privateStorage(fd,false,geteuid()) && transfer(fd,h.data(),h.size(),true) && transfer(fd,bytes.data(),bytes.size(),true) && !fsync(fd);
  ::close(fd);if(ok)ok=!renameat(directory,temp,directory,filename.c_str());if(ok)ok=!fsync(directory);unlinkat(directory,temp,0);return ok;
 }
public:
 static constexpr size_t maxBytes=32*1024*1024,maxEntry=8*1024*1024,maxEntries=256;
 void open(const VkPhysicalDeviceProperties &p,const VkPhysicalDeviceDriverProperties &driver,const VkPhysicalDeviceIDProperties &id,const uint8_t *translator,const char *client) noexcept {
  if(!translator || !client || !clientName(client))return;
  try {
   filename=std::string(client)+".translations";
   memcpy(header.data(),"FXMTLA01",8);put(header.data()+8,1);put(header.data()+12,uint32_t(header.size()));put(header.data()+28,uint32_t(driver.driverID));
   put(header.data()+32,p.apiVersion);put(header.data()+36,p.vendorID);put(header.data()+40,p.deviceID);put(header.data()+44,p.driverVersion);
   memcpy(header.data()+48,p.pipelineCacheUUID,16);memcpy(header.data()+64,id.driverUUID,16);memcpy(header.data()+80,id.deviceUUID,16);memcpy(header.data()+96,translator,32);
#ifdef FUXI_MTL_HOST_TEST
   const char *path=getenv("FUXI_MTL_TEST_CACHE_DIR");if(path)directory=::open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
#else
   directory=openClientStorage(client);
#endif
   if(!privateStorage(directory,true,geteuid())){close();fprintf(stderr,"FUXI MTL: translation cache unavailable (storage)\n");return;}
   auto reason=load();
   fprintf(stderr,"FUXI MTL: translation cache loaded %zu bytes / %zu pipelines result=%s%s%s\n",used,entries.size(),reason?"discard(":"ready",reason?reason:"",reason?")":"");
  }catch(...){close();fprintf(stderr,"FUXI MTL: translation cache unavailable (allocation)\n");}
 }
 static size_t read(void *context,const uint8_t *key,const uint8_t **out) noexcept {
  auto &c=*(ArtifactCache *)context;*out=nullptr;
  for(auto i=c.entries.begin();i!=c.entries.end();++i)if(!memcmp(i->key.data(),key,32)) {
   // Move to the end for eviction, but never dirty a disk snapshot on a hit.
   std::rotate(i,i+1,c.entries.end());auto &e=c.entries.back();*out=e.bytes.data();return e.bytes.size();
  }
  return 0;
 }
 static void write(void *context,const uint8_t *key,const uint8_t *bytes,size_t n) noexcept {
  auto &c=*(ArtifactCache *)context;if(c.directory<0 || !n || n>maxEntry)return;
  try {
   Entry e{};memcpy(e.key.data(),key,32);e.bytes.assign(bytes,bytes+n);
   for(auto i=c.entries.begin();i!=c.entries.end();)if(i->key==e.key){c.used-=36+i->bytes.size();i=c.entries.erase(i);}else ++i;
   while(!c.entries.empty() && (c.entries.size()>=maxEntries || c.used+36+n>maxBytes)){c.used-=36+c.entries.front().bytes.size();c.entries.erase(c.entries.begin());}
   c.entries.push_back(std::move(e));c.used+=36+n;c.dirty=true;
  }catch(...){} // Cache allocation failure must not fail shader translation.
 }
 static void outcome(void *context,uint32_t value) noexcept {auto &c=*(ArtifactCache *)context;if(value==1)++c.hits;else {++c.misses;if(value==2)++c.rejected;}}
 bool available() const {return directory>=0;}
 void checkpoint() noexcept {
  if(directory<0 || !dirty)return;bool ok=false,busy=false;try{ok=save(busy);}catch(...){}
  if(busy)return;
  fprintf(stderr,"FUXI MTL: translation cache saved %zu bytes / %zu pipelines result=%s\n",ok?used:0,entries.size(),ok?"saved":"memory-only");
  if(ok)dirty=false;else {::close(directory);directory=-1;}
 }
 void report() noexcept {
  fprintf(stderr,"FUXI MTL: cache period translation hits=%llu misses=%llu rejected=%llu\n",(unsigned long long)hits,(unsigned long long)misses,(unsigned long long)rejected);hits=misses=rejected=0;
 }
 void close() noexcept {if(directory>=0)::close(directory);directory=-1;entries.clear();used=0;dirty=false;}
 ~ArtifactCache(){close();}
 ArtifactCache()=default;ArtifactCache(const ArtifactCache&)=delete;ArtifactCache &operator=(const ArtifactCache&)=delete;
};
}
