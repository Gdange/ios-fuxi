/* SDK-independent production bounds: compile/run with the frontend harness. */
#include "VertexProfile.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
 unsigned accepted=0;
 for(unsigned format=0;format<256;++format) {
  auto info=fuxi::vertexFormatInfo(format);
  bool expected=(format>=1 && format<=42) || (format>=45 && format<=54);
  assert((info.bytes!=0)==expected);
  if(!expected){assert(fuxi::vertexAttributeRejection(0,format,0,0));continue;}
  ++accepted;assert(!fuxi::vertexAttributeRejection(7,format,3,256-info.bytes));
  assert(fuxi::vertexAttributeRejection(8,format,3,0));
  assert(fuxi::vertexAttributeRejection(7,format,4,0));
  assert(fuxi::vertexAttributeRejection(7,format,3,257-info.bytes));
  assert(fuxi::vertexFetchFits(256,0,0,256,256-info.bytes,format));
  assert(!fuxi::vertexFetchFits(255,0,0,256,256-info.bytes,format));
 }
 assert(accepted==52);
 assert(fuxi::vertexFormatInfo(27).bytes==8 && fuxi::vertexFormatInfo(30).bytes==12);
 assert(!fuxi::vertexLayoutRejection(3,256,1,1,true));
 assert(fuxi::vertexLayoutRejection(4,16,1,1,false));
 assert(fuxi::vertexLayoutRejection(3,257,1,1,true));
 assert(fuxi::vertexLayoutRejection(3,0,1,1,true));
 assert(fuxi::vertexLayoutRejection(3,0,0,0,false));
 assert(fuxi::vertexLayoutRejection(3,16,2,1,true));
 assert(fuxi::vertexLayoutRejection(3,16,1,2,true));
 // Last half4 in a tightly sized compact quad: the old 16-byte guess overread.
 assert(fuxi::vertexFetchFits(64,0,3,16,8,27));
 assert(!fuxi::vertexFetchFits(63,0,3,16,8,27));
 assert(fuxi::vertexFetchFits(80,16,3,16,8,27));
 assert(!fuxi::vertexFetchFits(UINT64_MAX,UINT64_MAX-2,0,16,0,27));
 assert(!fuxi::vertexFetchFits(64,UINT64_MAX,0,16,0,27));
 assert(!fuxi::vertexFetchFits(64,0,0,0,0,27));
 assert(!fuxi::vertexFetchFits(64,0,0,16,0,55));
 puts("PASS: 52 exact vertex widths, descriptor and fetch bounds, half4 quad and offset overflow");
}
