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
static inline struct FuxiCAComparison fuxiCACompareSize(const uint8_t *gpu,size_t row,const uint8_t *cpu,size_t width,size_t height) {
 if(!gpu || !cpu || !width || !height || width>INT32_MAX || height>INT32_MAX || width>SIZE_MAX/4 ||
    height>UINT32_MAX/width || row<width*4 || row>SIZE_MAX/height) {
  struct FuxiCAComparison invalid={255,UINT32_MAX,255};return invalid;
 }
 struct FuxiCAComparison m={0,0,0};uint64_t sum=0;
 for(size_t y=0;y<height;++y)for(size_t x=0;x<width;++x) {
  const uint8_t *g=gpu+y*row+x*4;
  for(unsigned c=0;c<4;++c){int d=(int)g[c]-(int)cpu[(y*width+x)*4+c];sum+=(unsigned)(d<0?-d:d);}
  unsigned best=~0u;
  for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
   int ny=(int)y+dy,nx=(int)x+dx;if(ny<0 || (size_t)ny>=height || nx<0 || (size_t)nx>=width)continue;
   unsigned e=fuxiCAPixelError(g,cpu+((size_t)ny*width+(size_t)nx)*4);if(e<best)best=e;
  }
  if(best>m.max_error)m.max_error=best;
  if(best>8)++m.bad_pixels;
 }
 m.mean_error=(double)sum/(width*height*4);return m;
}
static inline struct FuxiCAComparison fuxiCACompare(const uint8_t *gpu,size_t row,const uint8_t *cpu) {
 return fuxiCACompareSize(gpu,row,cpu,256,256);
}
static inline int fuxiCAAcceptSize(struct FuxiCAComparison m,size_t width,size_t height) {
 if(!width || !height || width>SIZE_MAX/height)return 0;
 return m.max_error<=64 && m.bad_pixels<=(width*height)/256 && m.mean_error<=1.0;
}
static inline int fuxiCAAccept(struct FuxiCAComparison m) {
 return m.max_error<=64 && m.bad_pixels<=256 && m.mean_error<=1.0;
}
