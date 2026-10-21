/* Original bounded BGRA comparison, shared by the native smoke and host tests. */
#pragma once
#include <stdint.h>
#include <stddef.h>
struct FuxiCAComparison {unsigned max_error,bad_pixels;double mean_error;};
// Largest channel difference between a GPU pixel and one reference pixel.
static inline unsigned fuxiCAPixelError(const uint8_t *g,const uint8_t *c) {
 unsigned worst=0;
 for(unsigned k=0;k<4;++k){int d=(int)g[k]-(int)c[k];unsigned e=(unsigned)(d<0?-d:d);if(e>worst)worst=e;}
 return worst;
}
// The mean is over exact positions. A pixel's error, for the max and the outlier
// count, is the smallest against the reference's 3x3 neighbourhood: a sampling or
// coverage convention that moves an edge by one pixel is not a rendering error,
// while a flip, swap or offset still mismatches its whole neighbourhood.
static inline struct FuxiCAComparison fuxiCACompare(const uint8_t *gpu,size_t row,const uint8_t *cpu) {
 struct FuxiCAComparison m={0,0,0};uint64_t sum=0;
 for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
  const uint8_t *g=gpu+y*row+x*4;
  for(unsigned c=0;c<4;++c){int d=(int)g[c]-(int)cpu[(y*256+x)*4+c];sum+=(unsigned)(d<0?-d:d);}
  unsigned best=~0u;
  for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
   int ny=(int)y+dy,nx=(int)x+dx;if(ny<0 || ny>255 || nx<0 || nx>255)continue;
   unsigned e=fuxiCAPixelError(g,cpu+(ny*256+nx)*4);if(e<best)best=e;
  }
  if(best>m.max_error)m.max_error=best;
  if(best>8)++m.bad_pixels;
 }
 m.mean_error=(double)sum/(256*256*4);return m;
}
static inline int fuxiCAAccept(struct FuxiCAComparison m) {
 return m.max_error<=64 && m.bad_pixels<=256 && m.mean_error<=1.0;
}
