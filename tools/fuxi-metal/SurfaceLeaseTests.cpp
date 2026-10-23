#include "SurfaceLease.hpp"
#include <cassert>
#include <cstdio>
#include <map>
static std::map<uintptr_t,unsigned> held;
static unsigned locks,unlocks;
static uintptr_t fail;
static int lock(uintptr_t p){++locks;if(p==fail)return 1;++held[p];return 0;}
static void unlock(uintptr_t p){assert(held[p]);--held[p];++unlocks;}
int main() {
 assert(!fuxi::gpuScanoutEnabled(nullptr));
 assert(fuxi::gpuScanoutEnabled("other=1\tfuxigpuscanout=1\nquiet"));
 for(const char *args:{"", "fuxigpuscanout=0", "fuxigpuscanout=2", "xfuxigpuscanout=1", "fuxigpuscanout=1x",
     "fuxigpuscanout=1 fuxigpuscanout=1", "fuxigpuscanout=1 fuxigpuscanout=0"})assert(!fuxi::gpuScanoutEnabled(args));
 bool poisoned=false;
 {fuxi::SurfaceLease lease(false,lock,unlock,&poisoned);lease.add(1);assert(!locks);}
 {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);lease.add(0);lease.add(1);lease.add(1);lease.add(2);
  assert(locks==2 && !unlocks && held[1]==1 && held[2]==1);}
 assert(unlocks==2 && !held[1] && !held[2]);
 fail=2;
 try {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);lease.add(1);lease.add(2);assert(false);}
 catch(const std::runtime_error &){}
 assert(unlocks==3 && !held[1] && !held[2]);fail=0;
 {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);lease.add(1);poisoned=true;}
 assert(held[1]==1 && unlocks==3); // unknown DMA cannot release producer ownership
 held[1]=0;poisoned=false;
 try {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);for(uintptr_t i=1;i<=257;i++)lease.add(i);assert(false);}
 catch(const std::runtime_error &){}
 for(auto pair:held)assert(!pair.second);
 puts("PASS: scanout opt-in parsing, per-submit deduplication, completion/rollback release, unknown-DMA retention, bounded leases");
}
