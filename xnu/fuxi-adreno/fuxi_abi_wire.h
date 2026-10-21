/* SPDX-License-Identifier: MIT
 * A7xx framing adapted from UniXNU NativeAdrenoRing.cpp.
 * Copyright (c) 2026 NativeAdreno contributors. See LICENSE.NativeAdreno.
 */
#ifndef FUXI_ABI_WIRE_H
#define FUXI_ABI_WIRE_H
#include "NativeAdrenoABI.h"
#define FG_ABI_VA_START UINT64_C(0x20000000)
#define FG_ABI_VA_SIZE UINT64_C(0x60000000)
#define FG_ABI_PA_BITS 36u /* Fuxi ID2.OAS=1; existing present mapper has the same limit. */
#define FG_ABI_MAX_BYTES (UINT64_C(1) << 28)
/* Exact direct-selector wire lengths, including the unsupported methods. */
static inline unsigned fg_abi_input_size(unsigned method)
{
 static const unsigned sizes[17] = {0,0,16,32,8,416,16,0,0,80,80,8,8,8,16,0,16};
 return method <= 16 ? sizes[method] : 0;
}
static inline unsigned fg_abi_output_size(unsigned method)
{
 static const unsigned sizes[17] = {0,24,8,8,0,8,0,8,16,0,80,8,0,16,0,64,8};
 return method <= 16 ? sizes[method] : 0;
}
static inline int fg_abi_range(uint64_t iova, uint64_t bytes)
{
 return bytes && !((iova|bytes)&4095) && iova >= FG_ABI_VA_START &&
     iova-FG_ABI_VA_START < FG_ABI_VA_SIZE && bytes <= FG_ABI_VA_SIZE-(iova-FG_ABI_VA_START);
}
static inline int fg_abi_alloc_valid(uint64_t bytes, unsigned align, unsigned contiguous)
{
 return bytes && !(bytes&4095) && bytes <= FG_ABI_MAX_BYTES && align >= 12 && align <= 24 && contiguous <= 1;
}
static inline int fg_abi_import_valid(uint64_t address, uint64_t bytes)
{
 return address && bytes && !((address|bytes)&(NativeAdrenoAbiHostPageBytes-1)) &&
     bytes<=FG_ABI_MAX_BYTES && address<(UINT64_C(1)<<47) && bytes<=(UINT64_C(1)<<47)-address;
}
static inline int fg_abi_map_valid(uint64_t iova, uint64_t bytes, unsigned perms,
    unsigned wb, unsigned sh, unsigned reserved)
{
 return fg_abi_range(iova,bytes) && (perms==1 || perms==3) && wb<=1 && (sh==2 || sh==3) && !reserved;
}
static inline int fg_abi_ib_valid(uint64_t base, uint64_t length, uint64_t iova, uint64_t bytes)
{
 return length && bytes && !((iova|bytes)&3) && bytes/4 <= 0xfffff &&
     iova>=base && iova-base<length && bytes<=length-(iova-base);
}
static inline unsigned fg_abi_parity(unsigned x)
{
 x^=x>>16; x^=x>>8; x^=x>>4; return (0x9669u>>(x&15))&1;
}
static inline uint32_t fg_abi_pkt7(unsigned op, unsigned n)
{ return 0x70000000u | n | fg_abi_parity(n)<<15 | op<<16 | fg_abi_parity(op)<<23; }
static inline uint32_t fg_abi_pkt4(unsigned reg, unsigned n)
{ return 0x40000000u | n | fg_abi_parity(n)<<7 | reg<<8 | fg_abi_parity(reg)<<27; }
/* Caller supplies 160 words. Fixed-size framing: 37 + count*4 + 31 + WFI.
 * fence_base+4 is BR completion, +12 is BV completion; +0/+8 unused here.
 * Reset on each return from the present context, which changes CP state. */
static inline unsigned fg_abi_frame(uint32_t *w, const uint64_t *iova,
    const uint64_t *bytes, unsigned count, uint32_t fence_base,
    uint32_t previous, uint32_t seq, int reset)
{
 if (!w || !iova || !bytes || !count || count>16 || !seq || (fence_base&15)) return 0;
 for (unsigned i=0;i<count;i++)
  if (!bytes[i] || ((iova[i]|bytes[i])&3) || iova[i]>>32 || bytes[i]/4>0xfffff) return 0;
 unsigned n=0;
#define P1(op,a) do { w[n++]=fg_abi_pkt7(op,1); w[n++]=(a); } while (0)
#define TS(op,event,addr,val) do { w[n++]=fg_abi_pkt7(op,4); w[n++]=(event); w[n++]=(addr); w[n++]=0; w[n++]=(val); } while (0)
 P1(0x17,0x80000001u); /* sync BR */
 if (reset) {
  TS(0x14,0,fence_base+4,previous);
  P1(0x17,0x80000003u); P1(0x1f,0xf); P1(0x17,0x80000003u);
  P1(0x46,0x26); P1(0x17,0x80000001u);
  w[n++]=fg_abi_pkt4(0x050e,1); w[n++]=1;
  P1(0x46,0x31);
  w[n++]=fg_abi_pkt7(0x3c,6); w[n++]=3; w[n++]=0x050f;
  w[n++]=0; w[n++]=1; w[n++]=1; w[n++]=0;
 }
 P1(0x46,0x32); P1(0x46,0x33); w[n++]=fg_abi_pkt7(0x26,0);
 P1(0x17,3); P1(0x65,0x101); /* BOTH, IFPC off */
 for (unsigned i=0;i<count;i++) {
  w[n++]=fg_abi_pkt7(0x3f,3); w[n++]=(uint32_t)iova[i]; w[n++]=0; w[n++]=(uint32_t)(bytes[i]/4);
 }
 w[n++]=fg_abi_pkt4(0x0885,1); w[n++]=seq;
 P1(0x17,1); P1(0x46,0x18); P1(0x46,0x19);
 P1(0x17,2); TS(0x46,0x08000031u,fence_base+12,seq);
 P1(0x17,1); TS(0x14,0,fence_base+12,seq);
 TS(0x46,0x88000031u,fence_base+4,seq);
 P1(0x17,3); P1(0x65,0x100); w[n++]=fg_abi_pkt7(0x26,0);
#undef P1
#undef TS
 return n;
}
#endif
