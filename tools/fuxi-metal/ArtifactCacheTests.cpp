/* Actual production snapshot IO and policy, with authored bytes only. */
#define FUXI_MTL_HOST_TEST
#include "ArtifactCache.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc,char **argv){
 assert(argc==2);assert(!setenv("FUXI_MTL_TEST_CACHE_DIR",argv[1],1));
 using Mode=fuxi::CacheMode;using Store=fuxi::ArtifactCache;
 assert(fuxi::cacheMode(nullptr)==Mode::legacy);
 assert(fuxi::cacheMode("fuxigpucache=1")==Mode::persistent);
 for(auto text:{"fuxigpucache=0","fuxigpucache=2","fuxigpucache=11","fuxigpucache=1 fuxigpucache=1","fuxigpucache="})assert(fuxi::cacheMode(text)==Mode::disabled);
 assert(fuxi::cacheMode("otherfuxigpucache=1")==Mode::legacy);
 VkPhysicalDeviceProperties p{};VkPhysicalDeviceDriverProperties driver{};VkPhysicalDeviceIDProperties id{};
 uint8_t translator[32]={},key[32]={};std::vector<uint8_t> bytes(1024,42);const uint8_t *out=nullptr;
 auto open=[&](Store &s){s.open(p,driver,id,translator,"artifact-test");assert(s.available());};
 auto path=std::string(argv[1])+"/artifact-test.translations";
 {Store s;Store::Access access(s);open(s);assert(!Store::read(&access,key,&out));Store::write(&access,key,bytes.data(),bytes.size());s.checkpoint();s.close();}
 auto read=[&]{std::ifstream f(path,std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),{}};};
 auto original=read();assert(original.size()==128+36+bytes.size());
 uint32_t crc=fuxi::PipelineCache::crc32(original.data()+128,original.size()-128);for(unsigned i=0;i<4;++i)assert(original[20+i]==uint8_t(crc>>(8*i)));
 auto write=[&](const std::vector<uint8_t> &b){std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write((const char *)b.data(),b.size());};
 {Store s;Store::Access access(s);open(s);assert(Store::read(&access,key,&out)==bytes.size() && !memcmp(out,bytes.data(),bytes.size()));}
 auto rejected=[&](std::vector<uint8_t> b){write(b);Store s;Store::Access access(s);open(s);assert(!Store::read(&access,key,&out));};
 auto damaged=original;damaged.resize(12);rejected(damaged);
 for(size_t offset:{0u,8u,12u,16u,20u,24u,28u,32u,36u,40u,44u,48u,64u,80u,96u,128u,160u,200u}){damaged=original;damaged[offset]^=1;rejected(damaged);}
 auto put=[](uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));};
 damaged=original;put(damaged.data()+24,2);rejected(damaged); // Valid CRC, missing second record.
 damaged=original;put(damaged.data()+160,UINT32_MAX);put(damaged.data()+20,fuxi::PipelineCache::crc32(damaged.data()+128,damaged.size()-128));rejected(damaged);
 write(original);assert(!chmod(path.c_str(),0644));{Store s;Store::Access access(s);open(s);assert(!Store::read(&access,key,&out));}assert(!chmod(path.c_str(),0600));
 auto link=std::string(argv[1])+"/link";assert(!::link(path.c_str(),link.c_str()));{Store s;Store::Access access(s);open(s);assert(!Store::read(&access,key,&out));}assert(!unlink(link.c_str()));
 unlink(path.c_str());assert(!symlink("absent",path.c_str()));{Store s;Store::Access access(s);open(s);assert(!Store::read(&access,key,&out));}unlink(path.c_str());
 // Bound records/bytes, replace old versions, reject oversized single products.
 {Store s;Store::Access access(s);open(s);for(unsigned i=0;i<300;++i){memcpy(key,&i,sizeof(i));Store::write(&access,key,bytes.data(),bytes.size());}
  memset(key,0,32);assert(!Store::read(&access,key,&out));unsigned newest=299;memcpy(key,&newest,sizeof(newest));assert(Store::read(&access,key,&out)==bytes.size());
  std::vector<uint8_t> huge(Store::maxEntry+1);Store::write(&access,key,huge.data(),huge.size());assert(Store::read(&access,key,&out)==bytes.size());
  huge.resize(Store::maxEntry);memset(key,0,32);Store::write(&access,key,huge.data(),huge.size());s.checkpoint();
  struct stat st{};assert(!stat(path.c_str(),&st) && st.st_size<=off_t(Store::maxBytes+128));}
 // Fixed-name orphan from an interrupted checkpoint is replaced next save.
 {std::ofstream temp(path+".tmp");temp<<"partial";}
 {Store s;Store::Access access(s);open(s);Store::write(&access,key,bytes.data(),bytes.size());s.checkpoint();assert(::access((path+".tmp").c_str(),F_OK));}
 {Store s;Store::Access access(s);open(s);auto before=read();bytes[0]^=1;Store::write(&access,key,bytes.data(),bytes.size());
  int directory=::open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);assert(directory>=0);
  {fuxi::CacheWriteLease lease(directory);assert(lease.fd>=0);s.checkpoint();assert(s.available() && read()==before);}
  ::close(directory);s.checkpoint();assert(read()!=before);}
 assert(!chmod(argv[1],0500));{Store s;Store::Access access(s);s.open(p,driver,id,translator,"artifact-test");assert(!s.available());}assert(!chmod(argv[1],0700));
 puts("PASS: opt-in parsing, actual snapshot CRC/identity/permission/link rejection, LRU/size cap, atomic orphan replacement and memory-only fallback");
}
