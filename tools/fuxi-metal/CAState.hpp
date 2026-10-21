/* Bounded validation for NativeMetal-derived CA adapters; no Metal SDK needed. */
#pragma once
#include <cstddef>
#include <cstdint>
namespace fuxi {
constexpr size_t resourceSlots=16;
inline bool bindingRange(size_t start,size_t count) {return start<=resourceSlots && count<=resourceSlots-start;}
inline bool bindingOffset(bool bound,bool bytes,uint64_t length,uint64_t offset) {
 return bound && !bytes && offset<length;
}
// Public Metal ordinals: Always=7, Keep=0. With no depth/stencil attachment,
// this state has no effect. Any state requiring tests/writes remains refused.
inline bool neutralStencil(uint64_t compare,uint64_t fail,uint64_t depthFail,uint64_t pass) {
 return compare==7 && !fail && !depthFail && !pass;
}
inline bool neutralDepth(uint64_t compare,bool write) {return compare==7 && !write;}
}
