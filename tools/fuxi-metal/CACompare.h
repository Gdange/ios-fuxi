/* Original bounded BGRA comparison, shared by the native smoke and host tests. */
#pragma once
#include <stdint.h>
#include <stddef.h>
struct FuxiCAComparison {unsigned max_error,bad_pixels;double mean_error;};
static inline struct FuxiCAComparison fuxiCACompare(const uint8_t *gpu,size_t row,const uint8_t *cpu) {
 struct FuxiCAComparison m={0,0,0};uint64_t sum=0;
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
  unsigned bad=0;
  for(unsigned c=0;c<4;++c) {
   int d=(int)gpu[y*row+x*4+c]-(int)cpu[(y*256+x)*4+c];unsigned e=(unsigned)(d<0?-d:d);
   if(e>m.max_error)m.max_error=e;
   sum+=e;if(e>8)bad=1;
  }
  m.bad_pixels+=bad;
 }
 m.mean_error=(double)sum/(256*256*4);return m;
}
static inline int fuxiCAAccept(struct FuxiCAComparison m) {
 return m.max_error<=64 && m.bad_pixels<=256 && m.mean_error<=1.0;
}
