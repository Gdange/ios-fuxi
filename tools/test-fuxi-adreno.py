#!/usr/bin/env python3
"""Offline M6 submit tests, compiled from the actual 0096 C functions.

Models ring consumption, page translation and pixel copies, not SQE, real
cache coherency, secure firmware, bus-drain hardware or display visibility.
"""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sync', ROOT / 'tools/sync-fuxi-adreno-shared.py')
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


def function(source, name):
    match = re.search(r'^(?:static )?(?:unsigned|uint32_t|uintptr_t|kern_return_t|int|void) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef unsigned ppnum_t;
#define PAGE_SHIFT 14
#define PAGE_MASK 16383u
#define FALSE 0
#define FG_SOURCE_VA 0x50000000u
#define FG_SOURCE_BYTES 0x1000000u
#define FG_SOURCE_TABLE 0x800000u
#define G3_FENCE_PA 0x10000u
#define G3_FENCE_VA 0x40029000u
#define G4_FB_VA 0x41000000u
#define GPU_REG_BASE 0x3d00000u
#define GPU_SMMU_BASE 0x3da0000u
#define SMMU_S2CR(i) (0xc00+4*(i))
static uint64_t model_leaves[4096];
#define G_PTR(pa) ((uintptr_t)((pa)==FG_SOURCE_TABLE ? model_leaves : NULL))
static unsigned fg_state=1, fg_force, fg_seq, fg_access_failed, g3_wptr;
static unsigned g3_slot[2]={1,2};
static uint64_t fg_deadline, ticks;
static uint64_t model_counter(const char *r) { return strcmp(r,"cntfrq_el0") ? ++ticks : 1000000; }
#define rsr(r) model_counter(r)
static int fg_expired(void) { return model_counter("ticks") >= fg_deadline; }
static void delay(unsigned n) { ticks+=n; }
static unsigned mode, fail_tlb, tlbs, isolated, doorbells, ring[8192], rptr, wptr, fence;
static unsigned cache_clean, pending, maxwords, wrap_count, checks;
static unsigned gbif, reset_held, faulted_sids;
static uint8_t *source_buf, *panel;
static unsigned expected_pages[4096], paddr_missing;
static unsigned pdst[64*1080], pdata[64*1080];
static void fg_fail(const char *why, unsigned code) { (void)why; (void)code; fg_state=2; }
static unsigned fg_access(uint64_t pa, uint64_t *v, unsigned wr, unsigned wide)
{
 assert(!wide);
 if (pa==GPU_REG_BASE+0x3c45*4 && wr) gbif=*v;
 else if(pa==GPU_REG_BASE+0x3c46*4 && !wr) *v=mode==7 ? 0 : gbif;
 else if(pa==GPU_REG_BASE+0x43*4 && wr) reset_held=*v;
 else if(pa==GPU_SMMU_BASE+SMMU_S2CR(1) && wr) faulted_sids++;
 else if(pa==GPU_SMMU_BASE+SMMU_S2CR(2) && wr) faulted_sids++;
 else assert(0);
 isolated=1;
 return 0;
}
static void g_zero(uint64_t pa,unsigned n) { assert(pa==FG_SOURCE_TABLE && n==32768); memset(model_leaves,0,n); }
#define kernel_pmap 0
static ppnum_t pmap_find_phys(int map, uintptr_t va)
{
 (void)map; assert(va>=(uintptr_t)source_buf && va<(uintptr_t)source_buf+FG_SOURCE_BYTES);
 if(paddr_missing) return 0;
 return 1+3*((va-(uintptr_t)source_buf)>>14); /* deliberately discontiguous 16K pages */
}
static void flush_dcache64(uint64_t va,unsigned n,int phys)
{
 assert(!phys && va>=(uintptr_t)source_buf && va+n<=(uintptr_t)source_buf+FG_SOURCE_BYTES);
 cache_clean=1;
}
static int g4_tlb(uint32_t *d) { (void)d; return ++tlbs==fail_tlb ? 0xe03 : 0; }
static unsigned g3_gr(unsigned reg) { return reg==0x806 ? rptr : reg==0x807 ? wptr : reg==0x210 && mode==6 ? 2 : 0; }
static unsigned g3_read(uint64_t pa) { assert(pa==G3_FENCE_PA+64); return fence; }
static int g3_faulted(void) { return mode==4 && doorbells; }
static void g3_emit(uint32_t value)
{
 assert(((g3_wptr-rptr)&8191)<8190);
 ring[(g3_wptr++)&8191]=value;
}
static int g3_gw(unsigned reg, unsigned value);
'''
MODEL = r'''
static uint32_t translated_pixel(unsigned va)
{
 assert(va>=FG_SOURCE_VA && va<FG_SOURCE_VA+FG_SOURCE_BYTES);
 uint64_t pte=model_leaves[(va-FG_SOURCE_VA)/4096];
 assert((pte&0xfff)==(3|(3u<<6)|(3u<<8)|(1u<<10)));
 uint64_t pa=(pte&~4095ULL)|(va&4095);
 unsigned pn=(pa>>14)-1;
 assert(pn%3==0);
 unsigned off=(pn/3)*16384+(pa&16383);
 assert(off<FG_SOURCE_BYTES);
 uint32_t got; memcpy(&got,source_buf+off,4); return got;
}
static int g3_gw(unsigned reg,unsigned value)
{
 assert(reg==0x807 && value<8192 && cache_clean);
 doorbells++;
 if(mode==2 || mode==7) { wptr=value; return 1; } /* missing fence/stall */
 if(mode==5) { fg_access_failed=1; return 0; }
 unsigned total=(value-rptr)&8191;
 if(value<rptr) wrap_count++;
 if(total>maxwords) maxwords=total;
 assert(total && total<64*32+128);
 for(unsigned i=0;i<4096;i++) assert(!!model_leaves[i]==!!expected_pages[i]);
 uint32_t regs[0xc000]={0};
 unsigned pos=rptr, left=total, br=0, blits=0, fences=0;
 while(left) {
  unsigned hdr=ring[pos++&8191]; left--;
  if((hdr>>28)==4) {
   unsigned n=hdr&0x7f, r=(hdr>>8)&0x3ffff;
   assert(hdr==g4_pkt4(r,n) && r+n<0xc000 && left>=n);
   for(unsigned i=0;i<n;i++) regs[r+i]=ring[pos++&8191];
   left-=n;
  } else {
   unsigned n=hdr&0x3fff, op=(hdr>>16)&0x7f, a[8]={0};
   assert((hdr>>28)==7 && hdr==g3_pkt7(op,n) && n<=8 && n<=left);
   for(unsigned i=0;i<n;i++) a[i]=ring[pos++&8191];
   left-=n;
   if(op==0x17) { assert(n==1 && a[0]==((1u<<27)|1)); br=1; }
   else if(op==0x2c) {
    assert(br && n==1 && a[0]==3 && regs[G4_SP_SRC_INFO]==0x500430);
    assert(regs[G4_RB_BLIT]==0x10f03000 && regs[G4_GRAS_BLIT]==0x10f03000);
    unsigned sx=regs[0x8401]>>8, ex=regs[0x8402]>>8;
    unsigned dx=regs[0x8405], bx=regs[0x8406];
    assert(!(regs[0x8403]|regs[0x8404]) && bx>=dx && ex-sx==bx-dx);
    assert(regs[0xb2c1]==((ex+1)|1u<<15));
    assert((regs[0xb2c4]>>9)*64 >= (ex+1)*4);
    assert(regs[0x8c1a]*64 >= (bx+1)*4);
    assert(!((regs[0xb2c2]|regs[0x8c18])&63));
    for(unsigned x=0;x<=bx-dx;x++) {
     assert(pending<64*1080);
     unsigned dst=regs[0x8c18]+(dx+x)*4-G4_FB_VA;
     assert(dst<4320*2400);
     pdst[pending]=dst; pdata[pending++]=translated_pixel(regs[0xb2c2]+(sx+x)*4);
    }
    blits++;
   } else if(op==0x46 && n==1 && a[0]==26) {
    for(unsigned i=0;i<pending;i++) memcpy(panel+pdst[i],&pdata[i],4);
    pending=0;
   } else if(op==0x46 && n==4) {
    assert(!pending && a[0]==(49u|1u<<27) && a[1]==G3_FENCE_VA+64 && !a[2]);
    fence=a[3]; fences++;
   } else assert((op==0x26 && n==0) || (op==0x65 && n==1 && a[0]==12) || (op==0x46 && n==1));
  }
 }
 assert(blits && blits<=64 && fences==1);
 rptr=wptr=value;
 return 1;
}
static void reset(void)
{
 fg_state=1; fg_force=fg_seq=fg_access_failed=mode=fail_tlb=tlbs=isolated=doorbells=0;
 g3_wptr=rptr=wptr=8190; fence=cache_clean=pending=gbif=reset_held=faulted_sids=paddr_missing=0;
 ticks=0; memset(model_leaves,0,sizeof model_leaves); memset(panel,0x6b,4320*2400+64);
}
static void page_fixture(unsigned off,unsigned pitch,unsigned sx,unsigned sy,unsigned w,unsigned h)
{
 memset(expected_pages,0,sizeof expected_pages);
 for(unsigned y=0;y<h;y++) {
  unsigned a=off+(sy+y)*pitch+sx*4,b=a+w*4;
  for(unsigned p=a/4096;p<=(b-1)/4096;p++) expected_pages[p]=1;
 }
}
static void copy_case(unsigned off,unsigned pitch,unsigned sx,unsigned sy,unsigned dx,unsigned dy,unsigned w,unsigned h)
{
 unsigned len=(sy+h)*pitch;
 page_fixture(off,pitch,sx,sy,w,h);
 assert(fuxi_gpu_copy((uintptr_t)source_buf+off,len,pitch,sx,sy,dx,dy,w,h)==1);
 for(unsigned y=0;y<2400;y++) for(unsigned x=0;x<1080;x++) {
  uint32_t got,expected=0x6b6b6b6b;
  memcpy(&got,panel+y*4320+x*4,4);
  if(y>=dy && y<dy+h && x>=dx && x<dx+w)
   memcpy(&expected,source_buf+off+(sy+y-dy)*pitch+(sx+x-dx)*4,4);
  assert(got==expected);
 }
 for(unsigned i=0;i<4096;i++) assert(!model_leaves[i]);
 for(unsigned i=0;i<64;i++) assert(panel[4320*2400+i]==0x6b);
 checks++;
}
int main(void)
{
 source_buf=aligned_alloc(16384,FG_SOURCE_BYTES); panel=malloc(4320*2400+64);
 assert(source_buf && panel);
 for(unsigned i=0;i<FG_SOURCE_BYTES;i++) source_buf[i]=(i*13u+(i>>12)*17u)&255;
 reset(); copy_case(0,4320,0,0,0,0,1080,2400); /* 38 chunks, many ring wraps */
 for(unsigned phase=0;phase<16;phase++) {
  reset(); copy_case(phase*4,5008,17,3,19,7,1057,2311);
 }
 reset(); copy_case(4092,4352,1,0,1079,2399,1,1); /* both page and panel edge */
 /* More than 600 frames with a continuously reused ring, changing phases. */
 reset();
 for(unsigned i=0;i<1000;i++) {
  memset(panel,0x6b,4320*2400+64);
  copy_case((i%16)*4,4320,3,2,7,6,65,70);
 }
 reset(); assert(fuxi_gpu_copy((uintptr_t)source_buf,4096,4320,0,0,0,0,1080,2400)==0 && !doorbells);
 reset(); assert(fuxi_gpu_copy((uintptr_t)source_buf,FG_SOURCE_BYTES+1ULL,4320,0,0,0,0,1,1)==0);
 reset(); page_fixture(0,4320,0,0,10,10); paddr_missing=1;
 assert(fuxi_gpu_copy((uintptr_t)source_buf,4320*10,4320,0,0,0,0,10,10)==0 && fg_state==2 && !doorbells);
 for(unsigned m=1;m<=7;m++) {
  reset(); page_fixture(0,4320,0,0,10,10);
  if(m==1) fail_tlb=1; else if(m==3) fail_tlb=2; else mode=m;
  int result=fuxi_gpu_copy((uintptr_t)source_buf,4320*10,4320,0,0,0,0,10,10);
  assert(result==(m==7 ? -1 : 0) && fg_state==2);
  assert(ticks<60000); /* bounded total frame and bounded isolation */
  if(doorbells) assert(isolated && gbif==2 && reset_held==1 && faulted_sids==2);
  unsigned before=doorbells;
  assert(fuxi_gpu_copy((uintptr_t)source_buf,4320*10,4320,0,0,0,0,10,10)==0 && doorbells==before);
 }
 reset(); page_fixture(0,4320,0,0,10,10); fg_force=2;
 assert(fuxi_gpu_copy((uintptr_t)source_buf,4320*10,4320,0,0,0,0,10,10)==0 && isolated);
 printf("PASS: %u pixel-exact copies, cropped phases, noncontiguous 16K/4K mappings, full panel, 1000 frames, %u wraps, bounded failures/quarantine/no retry\n",checks,wrap_count);
 free(source_buf);free(panel);return 0;
}
'''


def main():
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-adreno-shared.py'), '--check'], check=True)
    subprocess.run(['python3', str(ROOT / 'tools/test-fuxi-adreno-mapping.py')], check=True)
    probe = sync.added_file('gpu.c')
    adapter = sync.added_file('fuxi_adreno.c')
    windows = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'\{(0x[0-9a-f]+),(0x[0-9a-f]+),0\}', adapter)]
    for pa, size in ((0x100000, 0x80000), (0x3d00000, 0x40000), (0x3d50000, 0x40000),
                     (0x3d90000, 0x10000), (0x3da0000, 0x40000), (0xb280000, 0x8000),
                     (0x81c60000, 0x20000), (0x9b098000, 0x4000), (0xae04000, 0x4000)):
        assert any(a <= pa and pa + size <= a + n for a, n in windows), hex(pa)
    for i, (a, n) in enumerate(windows):
        assert not (a & 16383) and not (n & 16383)
        for b, m in windows[i+1:]:
            assert a + n <= b or b + m <= a, 'overlapping platform mappings'
    functions = ''.join(function(probe, n) for n in ('g3_parity', 'g3_pkt7', 'g4_pkt4', 'g4_reg', 'g4_event', 'g4_wfi', 'g4_setup', 'g4_end'))
    functions += ''.join(function(adapter, n) for n in ('fg_isolate', 'fg_row', 'fuxi_gpu_copy'))
    functions = re.sub(r'__asm__ volatile\([^;]*\);', ';', functions)
    constants = '\n'.join(line for line in sync.added_file('gpu-a740.h').splitlines() if line.startswith('#define G4_'))
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / 'test.c'
        path.write_text(PREFIX + constants + '\n' + functions + MODEL)
        exe = Path(d) / 'test'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined,address', str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
