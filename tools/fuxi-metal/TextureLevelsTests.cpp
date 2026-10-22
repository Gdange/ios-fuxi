/* Overflow and state boundaries used before any Vulkan/SDK access. */
#include "TextureLevels.hpp"
#include "ResourceLimits.hpp"
#include <cassert>
int main(){
 assert(!fuxi::mipCount(0,0));assert(fuxi::mipCount(4096,4096)==13);
 assert(fuxi::mipCount(UINT32_MAX,1)==32);
 fuxi::TextureLevels t;t.width=13;t.height=7;t.levels=4;
 assert(t.levelWidth(0)==13 && t.levelHeight(0)==7 && t.levelWidth(1)==6 && t.levelHeight(1)==3 && t.levelWidth(3)==1 && t.levelHeight(3)==1);
 t.setDefined(2);assert(t.isDefined(2) && !t.isDefined(0));t.setDefined(0);t.setDefined(2,false);assert(t.defined && !t.isDefined(2));
 t.setDefined(UINT32_MAX);assert(!t.isDefined(UINT32_MAX) && !t.levelWidth(UINT32_MAX));
 fuxi::ResourceLimits limits;limits.reserve(90*1024*1024);limits.reserve(90*1024*1024);
 auto before=limits.used;bool failed=false;try{limits.reserve(90*1024*1024);}catch(...){failed=true;}
 assert(failed && limits.used==before);limits.release(before);assert(!limits.used);
}
