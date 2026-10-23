/* Original opt-in policy. Absent preserves the M9i driver-cache path. */
#pragma once
#include "ClientGate.hpp"
#include "ClientStorage.hpp"
#include <cstdlib>
#include <sys/file.h>
namespace fuxi {
// One writer per directory, including across processes. Fixed temporary names
// can then replace crash leftovers without an accumulating orphan per reboot.
struct CacheWriteLease {
 int fd=-1;bool busy=false;
 explicit CacheWriteLease(int directory){
  if(directory<0)return;
  fd=openat(directory,"cache-io.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
  if(!privateStorage(fd,false,geteuid())){if(fd>=0)::close(fd);fd=-1;return;}
  if(flock(fd,LOCK_EX|LOCK_NB)){busy=errno==EWOULDBLOCK || errno==EAGAIN;::close(fd);fd=-1;}
 }
 ~CacheWriteLease(){if(fd>=0)::close(fd);}
 CacheWriteLease(const CacheWriteLease&)=delete;
};
enum class CacheMode { legacy, disabled, persistent };
inline CacheMode cacheMode(const char *args) {
 unsigned count=0;CacheMode mode=CacheMode::legacy;
 if(args)for(const char *p=args;*p;) {
  p+=strspn(p," \t\r\n");size_t n=strcspn(p," \t\r\n");
  if(n>=13 && !strncmp(p,"fuxigpucache=",13)){++count;mode=n==14 && p[13]=='1'?CacheMode::persistent:CacheMode::disabled;}
  p+=n;
 }
 return count>1?CacheMode::disabled:mode;
}
inline CacheMode systemCacheMode() {
#ifdef FUXI_MTL_HOST_TEST
 return cacheMode(getenv("FUXI_MTL_TEST_BOOTARGS"));
#else
 char args[4096]={};systemString("kern.bootargs",args,sizeof(args));return cacheMode(args);
#endif
}
}
