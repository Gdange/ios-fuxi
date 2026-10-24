/* Synthetic payloads only: production IO, 256 x 700 KiB, no Vulkan driver. */
#ifndef FUXI_MTL_HOST_TEST
#define FUXI_MTL_HOST_TEST
#endif
#include "ArtifactCache.hpp"
#include <cassert>
#include <chrono>
#include <thread>
#include <sys/resource.h>
using Clock=std::chrono::steady_clock;
static long rss(){struct rusage r{};getrusage(RUSAGE_SELF,&r);
#ifdef __APPLE__
 return r.ru_maxrss/1024;
#else
 return r.ru_maxrss;
#endif
}
int main(int argc,char **argv){
 assert(argc==2);assert(!setenv("FUXI_MTL_TEST_CACHE_DIR",argv[1],1));
 using Store=fuxi::ArtifactCache;VkPhysicalDeviceProperties p{};VkPhysicalDeviceDriverProperties d{};VkPhysicalDeviceIDProperties id{};uint8_t translator[32]={},key[32]={};
 auto open=[&](Store &s){s.open(p,d,id,translator,"capacity-test");assert(s.available());};
 auto path=std::string(argv[1])+"/capacity-test.translations";
 const size_t length=700*1024;std::vector<uint8_t> bytes(length,42);const uint8_t *out=nullptr;long before=rss();
 {
  Store s;open(s);Store::Access access(s);
  for(unsigned i=0;i<256;++i){memcpy(key,&i,4);memcpy(bytes.data(),&i,4);Store::write(&access,key,bytes.data(),bytes.size());}
  auto began=Clock::now();s.checkpoint();
  printf("bench checkpoint_256_ms=%.3f\n",std::chrono::duration<double,std::milli>(Clock::now()-began).count());
 }
 struct stat st{};assert(!stat(path.c_str(),&st));assert(st.st_size==off_t(128+256*(36+length)));
 {
  Store s;auto began=Clock::now();open(s);
  assert(rss()-before<32*1024); // A full 175 MiB heap load would violate this.
  printf("bench index_load_256_ms=%.3f rss_delta_kib=%ld disk_bytes=%lld\n",std::chrono::duration<double,std::milli>(Clock::now()-began).count(),rss()-before,(long long)st.st_size);
  Store::Access access(s);began=Clock::now();
  for(unsigned i=0;i<256;++i){memcpy(key,&i,4);assert(Store::read(&access,key,&out)==length);assert(!memcmp(out,&i,4) && out[length-1]==42);}
  printf("bench pread_crc_700k_mean_ms=%.3f\n",std::chrono::duration<double,std::milli>(Clock::now()-began).count()/256);
  // Reads run during save; dirty writes and replacements must survive remap.
  unsigned last=255;memcpy(key,&last,4);memcpy(bytes.data(),&last,4);Store::write(&access,key,bytes.data(),bytes.size());
  std::thread writer([&]{s.checkpoint();});
  for(unsigned i=0;i<16;++i){memcpy(key,&i,4);assert(Store::read(&access,key,&out)==length);}
  unsigned replacement=254;memcpy(key,&replacement,4);memcpy(bytes.data(),&replacement,4);Store::write(&access,key,bytes.data(),bytes.size());
  writer.join();s.checkpoint();assert(Store::read(&access,key,&out)==length && !memcmp(out,&replacement,4));
  // A new generation replaces the pathname; existing readers pin old inodes.
  {Store next;open(next);Store::Access a(next);memset(key,0,32);bytes[0]=99;Store::write(&a,key,bytes.data(),bytes.size());next.checkpoint();}
  memset(key,0,32);assert(Store::read(&access,key,&out)==length && out[0]==0);
 }
 {
  Store s;open(s);Store::Access access(s);memset(key,0,32);assert(Store::read(&access,key,&out)==length && out[0]==99);
  // The changed key is last in the LRU snapshot. Modify it after load; a
  // per-record CRC must catch this without trusting a previously verified file.
  int fd=::open(path.c_str(),O_WRONLY|O_NOFOLLOW);assert(fd>=0);uint8_t bad=98;
  assert(pwrite(fd,&bad,1,128+255*(36+length)+36)==1);::close(fd);
  assert(!Store::read(&access,key,&out) && !out);
 }
 // Exercise the byte cap independently of the 256-entry cap, and force
 // automatic compaction of the bounded append spool before the last write.
 {
  Store s;open(s);Store::Access access(s);std::vector<uint8_t> large(Store::maxEntry,42);
  for(unsigned i=0;i<33;++i){memcpy(key,&i,4);Store::write(&access,key,large.data(),large.size());}
  memset(key,0,32);assert(!Store::read(&access,key,&out));unsigned i=1;memcpy(key,&i,4);assert(!Store::read(&access,key,&out));
  i=2;memcpy(key,&i,4);assert(Store::read(&access,key,&out)==large.size());s.checkpoint();
  assert(!stat(path.c_str(),&st) && st.st_size<=off_t(Store::maxBytes+128));
 }
 {Store s;open(s);Store::Access access(s);unsigned i=32;memcpy(key,&i,4);assert(Store::read(&access,key,&out)==Store::maxEntry);}
 puts("PASS: 256 lazy payloads, bounded allocations, pinned generations, post-load corruption, byte eviction and spool compaction");
}
