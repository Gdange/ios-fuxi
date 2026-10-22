/* A failed pipeline must never become a translator call or Vulkan bind. */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace fuxi {
inline std::atomic<uint64_t> droppedDraws{0};
inline void dropPipelineDraw(){
 if(!droppedDraws.fetch_add(1))fprintf(stderr,"FUXI MTL: draw dropped missing render pipeline (counter continues)\n");
}
}
