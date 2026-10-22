/* A failed pipeline must never become a translator call or Vulkan bind. */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace fuxi {
enum class Topology : uint32_t { Triangle, TriangleStrip, Line, LineStrip };
inline bool validTopology(Topology t){return uint32_t(t)<=uint32_t(Topology::LineStrip);}
inline bool primitiveRestart(Topology t){return t==Topology::TriangleStrip || t==Topology::LineStrip;}
inline uint32_t primitiveCount(Topology t,uint32_t n){return t==Topology::Line?n&~1u:t==Topology::LineStrip && n<2?0:n;}
inline bool primitiveBounds(Topology t,uint32_t start,uint32_t count){
 return validTopology(t) && count<=65536 && start<=65536-count &&
  ((t==Topology::Line || t==Topology::LineStrip) || (count>=3 && (t==Topology::TriangleStrip || count%3==0)));
}
inline std::atomic<uint64_t> droppedDraws{0};
inline void dropPipelineDraw(){
 if(!droppedDraws.fetch_add(1))fprintf(stderr,"FUXI MTL: draw dropped missing render pipeline (counter continues)\n");
}
}
