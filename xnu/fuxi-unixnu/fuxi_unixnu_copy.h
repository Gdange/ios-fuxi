// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ported from UniXNU drivers/NativeMobileDisplay/NativeLegacyRequest.h (BSD-2-Clause-Patent)
// See LICENSE.NativeMobileDisplay. Adapted to Fuxi's cropped iOS 16.7 surfaces.
#ifndef FUXI_UNIXNU_COPY_H
#define FUXI_UNIXNU_COPY_H
#include <stdint.h>
// IOSurface rows and allocation have independent alignment. Copy visible
// pixels only, after bounding every row in the real memory descriptor.
static inline int fuxi_unixnu_surface_bounds(uint32_t width,uint32_t height,
    uint64_t stride,uint64_t allocation,uint64_t descriptorBytes) {
    if (!width || !height || width>16384 || height>16384 ||
        stride<(uint64_t)width*4 || stride>UINT64_MAX/height) return 0;
    return allocation>=(uint64_t)(height-1)*stride+(uint64_t)width*4 && descriptorBytes>=allocation;
}

// The GOP destination is device memory: use aligned scalar pixel stores,
// never a general memcpy that may issue unaligned or wider device accesses.
// The source is normal mapped IOSurface memory and may have padded rows.
static inline int fuxi_unixnu_copy_layout(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height) {
    uintptr_t dst=(uintptr_t)destination, src=(uintptr_t)source;
    if (!dst || !src || (dst&3) || (destinationStride&3) ||
        !fuxi_unixnu_surface_bounds(width,height,destinationStride,destinationBytes,destinationBytes) ||
        !fuxi_unixnu_surface_bounds(width,height,sourceStride,sourceBytes,sourceBytes) ||
        destinationBytes>UINTPTR_MAX || sourceBytes>UINTPTR_MAX ||
        dst>UINTPTR_MAX-destinationBytes || src>UINTPTR_MAX-sourceBytes) return 0;
    return 1;
}
static inline int fuxi_unixnu_fill_pixels(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride,
    uint32_t width, uint32_t height, uint32_t value) {
    uintptr_t dst=(uintptr_t)destination;
    if (!dst || (dst&3) || (destinationStride&3) ||
        !fuxi_unixnu_surface_bounds(width,height,destinationStride,destinationBytes,destinationBytes) ||
        destinationBytes>UINTPTR_MAX || dst>UINTPTR_MAX-destinationBytes) return 0;
    for (uint32_t row=0;row<height;++row) {
        volatile uint32_t *out=(volatile uint32_t *)(dst+row*destinationStride);
        for (uint32_t column=0;column<width;++column)out[column]=value;
    }
    return 1;
}
static inline int fuxi_unixnu_copy_pixels(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height) {
    if (!fuxi_unixnu_copy_layout(destination,destinationBytes,destinationStride,
        source,sourceBytes,sourceStride,width,height)) return 0;
    uintptr_t dst=(uintptr_t)destination, src=(uintptr_t)source;
    // Source and destination are both BGRA8888 little-endian words, so the pixels move unchanged. Use aligned 64-bit
    // device stores (four per iteration) instead of byte-wise pixel assembly: the stores into device memory dominate.
    // The destination base and stride are 8-byte aligned in practice (checked here); otherwise 32-bit stores remain.
    int wide=!(dst&7) && !(destinationStride&7);
    for (uint32_t row=0;row<height;++row) {
        volatile uint32_t *out=(volatile uint32_t *)(dst+row*destinationStride);
        const unsigned char *in=(const unsigned char *)(src+row*sourceStride);
        uint32_t column=0;
        if (wide) {
            volatile uint64_t *out64=(volatile uint64_t *)out;
            for (;column+8<=width;column+=8) {
                uint64_t a,b,c,d;
                __builtin_memcpy(&a,in+(uint64_t)column*4,8);__builtin_memcpy(&b,in+(uint64_t)column*4+8,8);
                __builtin_memcpy(&c,in+(uint64_t)column*4+16,8);__builtin_memcpy(&d,in+(uint64_t)column*4+24,8);
                out64[column/2]=a;out64[column/2+1]=b;out64[column/2+2]=c;out64[column/2+3]=d;
            }
        }
        for (;column<width;++column) {
            uint32_t value;
            __builtin_memcpy(&value,in+(uint64_t)column*4,4);
            out[column]=value;
        }
    }
    return 1;
}

// Differential copy for the slow scanout memory: only 64-byte blocks that differ from the previous frame (kept in an ordinary
// cached shadow buffer, rows packed at width*4) are stored to the device mapping. The shadow must start from a full copy
// (valid=0 copies everything). Returns the number of bytes stored, or UINT64_MAX when the geometry needs the plain copy.
static inline uint64_t fuxi_unixnu_copy_diff(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height,
    void *shadow, uint64_t shadowBytes, int valid) {
    if (!fuxi_unixnu_copy_layout(destination,destinationBytes,destinationStride,
        source,sourceBytes,sourceStride,width,height)) return UINT64_MAX;
    uintptr_t dst=(uintptr_t)destination, src=(uintptr_t)source;
    if (!shadow || shadowBytes<(uint64_t)width*height*4 ||
        (uintptr_t)shadow>UINTPTR_MAX-shadowBytes || ((uintptr_t)shadow&7) || (dst&7) || (destinationStride&7) || (width&1)) return UINT64_MAX;
    uint64_t stored=0;
    const uint32_t words=width/2;
    for (uint32_t row=0;row<height;++row) {
        volatile uint64_t *out=(volatile uint64_t *)(dst+row*destinationStride);
        const unsigned char *in=(const unsigned char *)(src+row*sourceStride);
        uint64_t *old=(uint64_t *)((unsigned char *)shadow+(uint64_t)row*width*4);
        uint32_t w=0;
        for (;w+8<=words;w+=8) {
            uint64_t v[8];
            __builtin_memcpy(v,in+(uint64_t)w*8,64);
            if (valid && !((v[0]^old[w])|(v[1]^old[w+1])|(v[2]^old[w+2])|(v[3]^old[w+3])|
                           (v[4]^old[w+4])|(v[5]^old[w+5])|(v[6]^old[w+6])|(v[7]^old[w+7]))) continue;
            for (unsigned i=0;i<8;++i){out[w+i]=v[i];old[w+i]=v[i];}
            stored+=64;
        }
        for (;w<words;++w) {
            uint64_t v;
            __builtin_memcpy(&v,in+(uint64_t)w*8,8);
            if (valid && v==old[w]) continue;
            out[w]=v;old[w]=v;stored+=8;
        }
    }
    return stored;
}

#endif
