/* Public MTLDataType scalar/vector payloads; no Apple private layouts. */
#pragma once
#include <cstdint>
namespace fuxi {
constexpr uint32_t maxFunctionConstants=128;
constexpr uint32_t constantPayloadSize(uint32_t type) {
 // float, half, int, uint, short, ushort, char, uchar, bool; 1..4 lanes.
 const uint32_t bases[]={3,16,29,33,37,41,45,49,53};
 const uint32_t widths[]={4,2,4,4,2,2,1,1,1};
 for(unsigned i=0;i<9;++i)if(type>=bases[i] && type<bases[i]+4)return (type-bases[i]+1)*widths[i];
 return 0; // matrices, pointers, aggregates and 64-bit values remain refused.
}
constexpr uint32_t constantElementStride(uint32_t type) {
 uint32_t size=constantPayloadSize(type);
 // Metal three-lane vectors have the alignment/array stride of four lanes.
 return size==3 || size==6 || size==12?size/3*4:size;
}
}
