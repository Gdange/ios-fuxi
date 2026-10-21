/* Original shared smoke inputs and independent integer source-over oracle. */
#pragma once
#include <stdint.h>
struct FuxiM7VertexData {float position[2],uv[2];uint8_t color[4];};
static const struct FuxiM7VertexData fuxiM7Vertices[]={
 {{-1,1},{0,0},{255,255,255,255}},{{1,1},{1,0},{255,255,255,255}},
 {{-1,-1},{0,1},{255,255,255,255}},{{1,-1},{1,1},{255,255,255,255}}};
static const uint16_t fuxiM7Indices[]={0,1,2,2,1,3};
static inline void fuxiM7Texel(unsigned layer,unsigned x,unsigned y,uint8_t out[4]) {
 if(!layer){out[0]=x>=128?255:0;out[1]=y>=128?255:0;out[2]=(x<128)!=(y<128)?255:0;out[3]=255;}
 else {out[0]=x>=128?128:64;out[1]=y>=128?64:128;out[2]=32;out[3]=254;}
}
static inline void fuxiM7Reference(unsigned x,unsigned y,uint8_t out[4]) {
 fuxiM7Texel(0,x,y,out);
 // The upper layer has a half tint and a scissor; these untouched image edges
 // also expose accidental pass clears and viewport/scissor inversion.
 if(x>=16 && x<240 && y>=24 && y<232){uint8_t src[4];fuxiM7Texel(1,x,y,src);
  for(unsigned c=0;c<4;++c)out[c]=(uint8_t)(src[c]/2+(out[c]*(255-src[3]/2)+127)/255);}
}
