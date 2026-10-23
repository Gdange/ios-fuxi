#pragma once
// GPU-SCANOUT ownership for imported IOSurfaces. Original Fuxi implementation.
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <cstring>
namespace fuxi {
inline bool gpuScanoutEnabled(const char *args) {
 if(!args)return false;
 unsigned keys=0;bool enabled=false;
 for(const char *p=args;*p;) {
  while(*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')++p;
  const char *begin=p;while(*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n')++p;
  size_t n=size_t(p-begin);
  if(n>=15 && !strncmp(begin,"fuxigpuscanout=",15)){++keys;enabled=n==16 && begin[15]=='1';}
 }
 return keys==1 && enabled;
}
class SurfaceLease {
 using Lock=int (*)(uintptr_t);
 using Unlock=void (*)(uintptr_t);
 bool enabled_;Lock lock_;Unlock unlock_;const bool *poisoned_;
 std::vector<uintptr_t> held_;
public:
 SurfaceLease(bool enabled,Lock lock,Unlock unlock,const bool *poisoned)
  :enabled_(enabled),lock_(lock),unlock_(unlock),poisoned_(poisoned) {}
 SurfaceLease(const SurfaceLease &)=delete;
 SurfaceLease &operator=(const SurfaceLease &)=delete;
 void add(uintptr_t surface) {
  if(!enabled_ || !surface || std::find(held_.begin(),held_.end(),surface)!=held_.end())return;
  if(held_.size()==256)throw std::runtime_error("scanout IOSurface lease limit");
  held_.push_back(surface);
  if(lock_(surface)){held_.pop_back();throw std::runtime_error("scanout IOSurface lock failed");}
 }
 ~SurfaceLease() {
  // An uncertain submission keeps both the producer lock and the allocation
  // alive. The kernel quarantines DMA; no framebuffer reader may reuse it.
  if(poisoned_ && *poisoned_)return;
  for(auto it=held_.rbegin();it!=held_.rend();++it)unlock_(*it);
 }
};
}
