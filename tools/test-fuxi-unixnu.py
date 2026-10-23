#!/usr/bin/env python3
"""P-UNIXNU production-code host models. No device, firmware or upstream execution."""
import argparse
import importlib.util
from pathlib import Path
import shlex
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-unixnu'


def function(source, name):
    match = re.search(r'^(?:static (?:inline )?)?(?:int|void|bool|uint32_t|uint64_t)\s*\n?' +
                      re.escape(name) + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


UFS_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include "fuxi_unixnu_ufs.h"
#define FUXI 1
#define TRUE 1
#define MIN(a,b) ((a)<(b)?(a):(b))
#define NSEC_PER_SEC 1000ull
#define NSEC_PER_USEC 1ull
#define THREAD_UNINT 0
#define TIMEOUT_URGENCY_SYS_CRITICAL 0
#define THREAD_WAITING 1
#define THREAD_CONTINUE_NULL 0
#define __unused __attribute__((unused))
#define B_READ 1
#define EROFS 30
#define LCK_GRP_DECLARE(n,g) int n
#define LCK_MTX_DECLARE(n,g) int n
#define __builtin_arm_dsb(x) ((void)(x))
typedef uintptr_t vm_offset_t;
typedef uint64_t addr64_t;
typedef void *event_t;
static bool fx_root=true;
static int sweet_ufs_lock;
static bool fx_write_allowed(uint64_t lba,uint64_t n) {return lba>=10 && n && lba+n<=100;}
static bool fx_map(uint64_t lba,uint64_t *dev,uint64_t *run) {*dev=10+lba;*run=100-lba;return true;}
struct buffer {char data[4096]; unsigned flags,count,resid;int error,done;};
typedef struct buffer *buf_t;
static unsigned buf_blkno(buf_t b) {(void)b;return 0;}
static unsigned buf_count(buf_t b) {return b->count;}
static unsigned buf_flags(buf_t b) {return b->flags;}
static void buf_setresid(buf_t b,unsigned n) {b->resid=n;}
static int buf_map(buf_t b,char **p) {*p=b->data;return 0;}
static void buf_unmap(buf_t b) {(void)b;}
static void buf_seterror(buf_t b,int n) {b->error=n;}
static void buf_biodone(buf_t b) {b->done++;}
static void lck_mtx_lock(int *p) {(void)p;}
static void lck_mtx_unlock(int *p) {(void)p;}
'''

UFS_MODEL = r'''
static unsigned mode,reads,rings,clears,logs,invalidates,flushes,sleeps;
static uint32_t doorbell,status,registers[64];
static unsigned mmioWrites;
static bool submitted;
static uint64_t ticks;
static void response(void) {
 struct utrd *d=(void *)(su.dma+DMA_UTRL);struct ucd *u=(void *)(su.dma+DMA_UCD);
 d->dw2=mode==12?2:0;
 u->resp[0]=0x21;u->resp[2]=su.lun;u->resp[3]=su.tag;
 if(mode==6)u->resp[3]++;
 if(mode==7)u->resp[2]++;
 if(mode==8)u->resp[15]=1;
 if(mode==9){u->resp[10]=1;u->resp[11]=0xff;}
 if(mode==10)u->resp[8]=1;
 if(mode==11)u->resp[7]=2; /* check condition is a completed command, not DMA uncertainty */
 memset((void *)(su.dma+DMA_BOUNCE),0x5a,4096);
 doorbell=0;status|=1;
}
static uint32_t ufs_rd(uint32_t off) {
 if(off==REG_UTRLDBR) {
  if(submitted) {
   reads++;
   if(mode==4)status|=0x10000;
   else if(mode==1){if(reads>=3)response();}
   else if(mode==2){doorbell=0;if(reads>=3)response();}
   else if(mode==3)doorbell=1;
   else if(mode==5)doorbell=0; /* descriptor remains invalid */
   else response();
  }
  return doorbell;
 }
 if(off==REG_IS)return status;
 assert(off/4<64);return registers[off/4];
}
static void ufs_wr(uint32_t off,uint32_t v) {
 mmioWrites++;
 if(off==REG_UTRLDBR){assert(v==1);submitted=true;rings++;doorbell=1;return;}
 if(off==REG_UTRLCLR){clears++;doorbell=0;return;}
 if(off==REG_IS)status&=~v;else {assert(off/4<64);registers[off/4]=v;}
}
static void dma_sync(uint32_t off,uint32_t len) {(void)off;(void)len;flushes++;}
static void dma_clean(uint32_t off,uint32_t len) {(void)off;(void)len;flushes++;}
static void fuxi_ufs_cache(uint32_t off,uint32_t len,bool inv) {
 assert(off+len<=DMA_SIZE && !su.poisoned);
 if(inv)invalidates++;else flushes++;
}
static uint64_t mach_absolute_time(void) {return ticks++;}
static void nanoseconds_to_absolutetime(uint64_t n,uint64_t *p) {*p=n;}
static bool ml_get_interrupts_enabled(void) {return true;}
static bool ml_at_interrupt_context(void) {return false;}
static int get_preemption_level(void) {return 0;}
static int assert_wait_deadline_with_leeway(event_t e,int a,int b,uint64_t t,int l) {
 (void)e;(void)a;(void)b;(void)l;assert(t<=5002);ticks=t;sleeps++;return THREAD_WAITING;
}
static void thread_block(int n) {(void)n;}
static void delay(int n) {ticks+=n;}
static int model_printf(const char *fmt,...) {if(strstr(fmt,"quarantined"))logs++;return 0;}
#define printf model_printf
'''

UFS_TEST = r'''
static unsigned char dma[DMA_SIZE] __attribute__((aligned(64)));
static void reset(unsigned m,bool enabled) {
 memset(&su,0,sizeof(su));memset(dma,0xcc,sizeof(dma));su.dma=(uintptr_t)dma;
 su.dma_pa=0x100000000ull;su.unixnu=enabled;su.ready=true;su.writable=true;
 su.secsize=512;su.nblocks=80;mode=m;reads=rings=clears=logs=invalidates=flushes=sleeps=0;
 doorbell=status=mmioWrites=0;submitted=false;ticks=0;
 memset(registers,0,sizeof(registers));registers[REG_HCE/4]=1;registers[REG_HCS/4]=HCS_DP|HCS_UTRLRDY;registers[REG_CAP/4]=CAP_64AS;
}
int main(void) {
 uint8_t read[10]={0x28}, write[10]={0x2a,0,0,0,0,10,0,0,1};
 for(unsigned cache=0;cache<2;cache++)for(unsigned wait=0;wait<2;wait++)for(unsigned m=0;m<=12;m++) {
  reset(m,true);su.cache=cache;su.wait=wait;
  int rc=ufs_scsi(read,10,4096,false);
  bool poison=m>=3 && m<=10;
  if(m<=2)assert(!rc && !su.poisoned && invalidates>=3 && !clears);
  else if(m==11 || m==12)assert(rc==EIO && !su.poisoned);
  else {
   assert(rc==(m==3 || m==5?ETIMEDOUT:EIO) && su.poisoned && logs==1 && !clears);
   unsigned char saved[DMA_SIZE];memcpy(saved,dma,sizeof(dma));
   unsigned previous=rings, previousCache=flushes+invalidates;
   assert(ufs_scsi(write,10,4096,true)==EIO);
   assert(ufs_scsi(read,10,4096,false)==EIO);
   struct buffer b={.flags=0,.count=4096};memset(b.data,0x9c,4096);
   sweet_ufs_strategy(&b);assert(b.error==EIO && b.resid==4096 && b.done==1);
   assert(!memcmp(saved,dma,sizeof(dma)) && rings==previous && logs==1 && flushes+invalidates==previousCache);
  }
  if(poison && (m==3 || m==5) && wait)assert(sleeps);
 }
 for(unsigned fatal=0;fatal<2;fatal++) {
  reset(0,true);if(fatal)status=0x800;else doorbell=2;
  unsigned char saved[DMA_SIZE];memcpy(saved,dma,sizeof(dma));
  struct buffer b={.count=4096};sweet_ufs_strategy(&b);
  assert(b.error==EIO && su.poisoned && !rings && !memcmp(saved,dma,sizeof(dma)));
 }
 reset(0,true);status=0x800;assert(ufs_take_over_host()==EIO && su.poisoned && !mmioWrites);
 reset(0,true);status=0x401;assert(!ufs_take_over_host() && status==0x400);
 reset(0,false);status=0x401;assert(!ufs_take_over_host() && status==0);
 reset(0,true);assert(!ufs_scsi(write,10,4096,true)); /* valid root extent */
 write[5]=1;assert(ufs_scsi(write,10,4096,true)==EROFS); /* outside extents */
 assert(ufs_scsi(NULL,10,0,false)==EINVAL);
 assert(ufs_scsi(read,17,0,false)==EINVAL);
 reset(3,false);assert(ufs_scsi(read,10,4096,false)==ETIMEDOUT && clears==1 && !su.poisoned);
 reset(0,false);assert(!ufs_scsi(read,10,4096,false) && !invalidates && !logs);
 // Pure completion/response boundaries in addition to real transport fault injection.
 assert(fuxi_unixnu_ufs_end(0,0,0,1)==FUXI_UFS_COMPLETE);
 assert(fuxi_unixnu_ufs_end(0,0x800,0,0)==FUXI_UFS_FATAL);
 uint8_t r[512]={0x21,0,3,7};assert(fuxi_unixnu_ufs_response(r,3,7));
 r[11]=1;assert(!fuxi_unixnu_ufs_response(r,3,7));
 r[11]=2;assert(fuxi_unixnu_ufs_response(r,3,7));r[33]=1;assert(!fuxi_unixnu_ufs_response(r,3,7));
 r[10]=1;r[11]=224;r[32]=1;r[33]=222;assert(fuxi_unixnu_ufs_response(r,3,7));
 r[11]=225;assert(!fuxi_unixnu_ufs_response(r,3,7));
 puts("PASS: actual UFS submit/strategy: fast/delayed/phantom completion, fatal, timeout, response validation, no DMA reuse, default path");
}
'''

COPY_TEST = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "fuxi_unixnu_copy.h"
int main(void) {
 uint64_t src[128],dst[128],shadow[128];
 for(unsigned i=0;i<128;i++){src[i]=0x33221100ull+i;dst[i]=0xccccccccccccccccull;shadow[i]=0;}
 // 18 visible pixels (72 bytes): one 64-byte block and one 8-byte tail; padded rows.
 assert(fuxi_unixnu_copy_diff(dst,sizeof(dst),96,src,sizeof(src),104,18,3,shadow,sizeof(shadow),0)==216);
 assert(fuxi_unixnu_copy_diff(dst,sizeof(dst),96,src,sizeof(src),104,18,3,shadow,sizeof(shadow),1)==0);
 src[1]^=42;assert(fuxi_unixnu_copy_diff(dst,sizeof(dst),96,src,sizeof(src),104,18,3,shadow,sizeof(shadow),1)==64);
 src[8]^=42;assert(fuxi_unixnu_copy_diff(dst,sizeof(dst),96,src,sizeof(src),104,18,3,shadow,sizeof(shadow),1)==8);
 for(unsigned y=0;y<3;y++){assert(!memcmp((char *)dst+y*96,(char *)src+y*104,72));assert(dst[y*12+9]==0xccccccccccccccccull);}
 assert(fuxi_unixnu_copy_diff(dst,sizeof(dst),96,src,sizeof(src),104,18,3,shadow,215,1)==UINT64_MAX);
 assert(fuxi_unixnu_copy_diff(dst,sizeof(dst),96,src,sizeof(src),104,17,3,shadow,sizeof(shadow),1)==UINT64_MAX);
 assert(!fuxi_unixnu_copy_pixels((char *)dst+1,sizeof(dst)-1,96,src,sizeof(src),104,18,3));
 assert(!fuxi_unixnu_copy_pixels(dst,263,96,src,sizeof(src),104,18,3)); // last pixel missing
 assert(!fuxi_unixnu_copy_pixels(dst,sizeof(dst),96,src,279,104,18,3));
 assert(!fuxi_unixnu_copy_pixels(dst,sizeof(dst),96,src,sizeof(src),104,0,3));
 assert(!fuxi_unixnu_surface_bounds(18,3,UINT64_MAX,UINT64_MAX,UINT64_MAX));
 assert(!fuxi_unixnu_surface_bounds(18,3,64,512,512));
 assert(!fuxi_unixnu_surface_bounds(18,3,96,512,511));
 // 32-bit destination alignment and odd width use scalar tail stores; last row needs visible bytes only.
 memset(dst,0xcc,sizeof(dst));
 assert(fuxi_unixnu_copy_pixels((char *)dst+4,268,96,src,276,104,17,3));
 for(unsigned y=0;y<3;y++)assert(!memcmp((char *)dst+4+y*96,(char *)src+y*104,68));
 assert(fuxi_unixnu_fill_pixels(dst,sizeof(dst),96,18,3,0));
 for(unsigned y=0;y<3;y++)for(unsigned x=0;x<9;x++)assert(dst[y*12+x]==0);
 puts("PASS: actual bounded/differential copy: padding, crop, dirty blocks/tails, alignment, size and overflow refusal");
}
'''


DISPLAY_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include "fuxi_unixnu_copy.h"
#include "fuxi_unixnu_display.h"
#define FUXI 1
#define OSDeclareDefaultStructors(n)
#define APPLE_KEXT_OVERRIDE
#define MACH_PORT_NULL 0
#define SWEET_FB_SWAP_REC_SIZE 0x46c
#define SWEET_FB_LAYERS 4
#define SWEET_FB_NOTIFY_TYPES 8
#define SWEET_FB_NOTIFY_VSYNC 5
#define SWEET_FB_FORMAT_BGRA 0x42475241
#define kIOTimeOptionsWithLeeway 1
#define OSSafeReleaseNULL(p) do {if(p){p->release();p=nullptr;}}while(0)
typedef void OSObject;
struct IOService {};
struct IOMemoryMap {};
struct IOWorkLoop {};
struct IOLock {bool held=false;};
static void IOLockLock(IOLock *p) {assert(!p->held);p->held=true;}
static void IOLockUnlock(IOLock *p) {assert(p->held);p->held=false;}
static uint64_t ticks=1000;
static unsigned arms,kicks,pollMode=1,gpuMode=0,refs,logs;
static uint64_t mach_absolute_time(void) {return ticks++;}
static void absolutetime_to_nanoseconds(uint64_t n,uint64_t *out) {*out=n;}
static bool PE_parse_boot_argn(const char *,int *n,unsigned) {*n=1;return true;}
static void fuxi_perf_swap(void) {}
static void fuxi_display_kick(void) {kicks++;}
static int IOLog(const char *,...) {logs++;return 0;}
static void *IOMallocData(unsigned n) {return malloc(n);}
static void IOFreeData(void *p,unsigned) {free(p);}
struct IOTimerEventSource {uint64_t next=0;void wakeAtTime(int,uint64_t n,int){next=n;arms++;}};
struct OSArray {
 std::vector<void *> objects;
 unsigned getCount(){return objects.size();}
 void *getObject(unsigned i){return objects[i];}
};
struct Memory {uint64_t size;uint64_t getLength(){return size;}};
struct SweetSurface {
 uint32_t id=1,width=18,height=3,pixelFormat=SWEET_FB_FORMAT_BGRA,bytesPerRow=104;
 uint32_t bytesPerElement=4,elementWidth=1,elementHeight=1,offset=0;
 uintptr_t address;Memory *memory;
 uintptr_t kernelAddress(){return address;}
 void release(){assert(refs);refs--;}
};
static SweetSurface *inputSurface;
static SweetSurface *sweet_surface_copy(uint32_t id) {assert(id==1);refs++;return inputSurface;}
static int sweet_console_muted=1;
static int sweet_surface_gpu_copy(SweetSurface *,uintptr_t,int,int,int,int,int,int){return gpuMode;}
static int sweet_surface_gpu_poll(uint64_t *elapsed) {*elapsed=42;return pollMode;}
struct fg_times {uint64_t pin,lookup,map,clean,submit,wait,unmap,cpu;unsigned attempts,hits,misses,sleeps,blocks,groups;};
static void sweet_surface_gpu_stats(fg_times *t){memset(t,0,sizeof(*t));}
static unsigned sweet_fb_swaps_logged=16;
static uint64_t sweet_fb_swap_count;
class IOMobileFramebufferUserClient {
 public:uint64_t fFunc[8]={},fPort=0,notifications=0;
 void notify(unsigned type,const uint64_t *,unsigned){if(fFunc[type] && fPort)notifications++;}
};
'''

DISPLAY_TEST = r'''
static void put32(uint8_t *r,unsigned offset,uint32_t v){memcpy(r+offset,&v,4);}
int main(void) {
 AppleCLCD fb={};IOLock lock;OSArray clients;IOTimerEventSource timer;
 uint64_t pixels[128]={},source[128]={},shadow[128]={};Memory memory={sizeof(source)};
 SweetSurface surface;surface.address=(uintptr_t)source;surface.memory=&memory;inputSurface=&surface;
 fb.fLock=&lock;fb.fClients=&clients;fb.fVsync=&timer;fb.fTickAbs=100;fb.fNextTick=ticks+100;
 fb.fWidth=18;fb.fHeight=3;fb.fRowBytes=96;fb.fPixels=(uintptr_t)pixels;
 fb.fShadow=shadow;fb.fShadowBytes=sizeof(shadow);fb.fUnixnuDiff=true;fb.fFlushTicks=1;
 fb.fConsolePaused=true;
 uint8_t rec[SWEET_FB_SWAP_REC_SIZE]={};put32(rec,0x104,1);put32(rec,0x54,1);
 put32(rec,0xc4+8,18);put32(rec,0xc4+12,3);
 for(unsigned i=0;i<128;i++)source[i]=0xabcdef00ull+i;
 // Default continuous timer; then opt-in idle, wake on swap and return to idle after present.
 sweet_fb_rearm(&fb,&timer);assert(arms==1);
 fb.fUnixnuIdle=true;fb.fTickArmed=true;sweet_fb_rearm(&fb,&timer);assert(!fb.fTickArmed && arms==1);
 fb.swap(rec);assert(fb.fTickArmed && fb.fDirty && arms==2);
 fb.vsyncFired(&fb,&timer);assert(!fb.fTickArmed && !fb.fDirty && fb.fShadowValid && fb.fDiffStored==216 && !refs);
 fb.swap(rec);fb.vsyncFired(&fb,&timer);assert(fb.fDiffStored==216 && fb.fDiffFull==432);
 // GPU offer invalidates the shadow, polling keeps the timer alive until retirement.
 gpuMode=2;fb.swap(rec);fb.vsyncFired(&fb,&timer);assert(fb.fGpuPending && fb.fTickArmed && !fb.fShadowValid);
 pollMode=0;unsigned before=arms;fb.vsyncFired(&fb,&timer);assert(fb.fGpuPending && fb.fTickArmed && arms==before+1);
 memset(pixels,0x55,sizeof(pixels));pollMode=1;fb.vsyncFired(&fb,&timer);assert(!fb.fGpuPending && !fb.fTickArmed);
 gpuMode=0;fb.swap(rec);fb.vsyncFired(&fb,&timer);assert(fb.fDiffStored==432 && fb.fShadowValid);
 // Blanking invalidates the shadow; swaps while off remain pending without waking a cleared black screen.
 fb.fPowerOff=true;fb.fFadeStep=0;fb.fDirty=true;
 IOLockLock(&lock);fb.wakePresentLocked();IOLockUnlock(&lock);
 fb.vsyncFired(&fb,&timer);assert(!fb.fTickArmed && !fb.fShadowValid);
 for(unsigned y=0;y<3;y++)for(unsigned x=0;x<9;x++)assert(pixels[y*12+x]==0);
 fb.swap(rec);assert(!fb.fTickArmed && fb.fDirty);
 fb.fPowerOff=false;
 IOLockLock(&lock);fb.wakePresentLocked();IOLockUnlock(&lock);
 fb.vsyncFired(&fb,&timer);assert(fb.fShadowValid && fb.fDiffStored==648);
 // A callback during an armed tick coalesces; rearm sees pending work. A callback after idle explicitly arms.
 fb.fTickArmed=true;fb.swap(rec);before=arms;sweet_fb_rearm(&fb,&timer);assert(arms==before+1);
 fb.fDirty=false;sweet_fb_rearm(&fb,&timer);assert(!fb.fTickArmed);
 fb.swap(rec);assert(fb.fTickArmed);
 // Enabled notifications keep pacing even without dirty swaps; removing the last subscriber stops it.
 IOMobileFramebufferUserClient client;client.fFunc[5]=1;client.fPort=1;clients.objects.push_back(&client);
 fb.fDirty=false;fb.vsyncFired(&fb,&timer);assert(fb.fTickArmed && client.notifications==1);
 client.fPort=0;fb.vsyncFired(&fb,&timer);assert(!fb.fTickArmed && client.notifications==1);
 // A failed GPU copy replays through CPU and reinitializes the shadow after confirmed retirement.
 fb.fGpuPending=true;fb.fShadowValid=false;pollMode=-1;fb.vsyncFired(&fb,&timer);
 assert(fb.fGpuFailed==1 && !fb.fGpuPending && fb.fShadowValid && !refs);
 assert(fuxi_unixnu_next_tick(10,100,50)==110);
 assert(fuxi_unixnu_next_tick(10,100,110)==210);
 for(int off=0;off<2;off++)for(int black=0;black<2;black++)for(int dirty=0;dirty<2;dirty++)
 for(int gpu=0;gpu<2;gpu++)for(int notify=0;notify<2;notify++)
  assert(!!fuxi_unixnu_tick_wanted(off,black,dirty,gpu,notify)==!!(gpu||notify||(off?black:dirty)));
 puts("PASS: actual framebuffer present/tick/swap: demand wake/idle, GPU retirement/fallback, blanking and shadow invalidation");
}
'''


def kernel_syntax(buildtree):
    """Optional real 8796 header check using saved flags; never run its make/scripts."""
    spec = importlib.util.spec_from_file_location('unixnu_sync', ROOT / 'tools/sync-fuxi-unixnu.py')
    sync = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sync)
    sync.FILES = {'iokit/Kernel/arm/SweetSurface.h': 'SweetSurface.h'}
    surface_header = sync.baseline()['iokit/Kernel/arm/SweetSurface.h']
    config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_FUXI'
    if not config.exists():
        config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
    with tempfile.TemporaryDirectory(prefix='fuxi-unixnu-headers-') as tmp:
        tmp = Path(tmp)
        arm = tmp / 'iokit/Kernel/arm'
        dev = tmp / 'bsd/dev'
        arm.mkdir(parents=True)
        dev.mkdir(parents=True)
        (arm / 'SweetSurface.h').write_text(surface_header)
        (dev / 'fuxi_adreno.h').write_bytes((ROOT / 'xnu/fuxi-adreno/fuxi_adreno.h').read_bytes())
        for path in SRC.iterdir():
            if path.suffix in ('.c', '.cpp', '.h'):
                (arm / path.name).write_bytes(path.read_bytes())
        for comp, name in [('bsd', 'sweet_ufs.c'), ('iokit', 'SweetFramebuffer.cpp')]:
            cwd = config / comp / 'DEVELOPMENT'
            old_root = re.search(r'^SOURCE_DIR=(.+)/xnu$', (cwd / 'Makefile').read_text(), re.M)[1]
            raw = (cwd / '.CFLAGS').read_text().replace('\0', '')
            # REPLACECONTENTS saved this single macro without its shell quoting.
            raw = re.sub(r'(-DCONFIG_NMBCLUSTERS=\(\(1024 \* 512\) / MCLBYTES\))',
                         lambda m: shlex.quote(m[1]), raw)
            saved = shlex.split(raw)[1:]
            if comp == 'iokit':
                saved += shlex.split((cwd / '.CXXFLAGS').read_text().replace('\0', ''))[1:]
            command = ['clang++' if comp == 'iokit' else 'clang',
                       '--target=arm64-apple-macos14', '-fsyntax-only', '-DFUXI',
                       '-mgeneral-regs-only', '-Werror', '-Wno-c99-designator', '-Wno-reorder-init-list',
                       '-D__builtin_xnu_type_signature(...)=""', '-D__builtin_xnu_type_summary(...)=0',
                       '-D__builtin_xnu_types_compatible(...)=1', '-I' + str(arm)]
            skip = False
            for arg in saved:
                if skip:
                    skip = False
                    continue
                if arg == '-arch':
                    skip = True
                    continue
                arg = arg.replace(old_root, str(buildtree))
                arg = re.sub(r'-I.*?\.sdk/+System/Library/Frameworks/Kernel\.framework/Versions/A/PrivateHeaders/platform',
                             '-I' + str(buildtree / 'sdk-kernel-platform/platform'), arg)
                command.append(arg)
            subprocess.run(command + [str(arm / name)], cwd=cwd, check=True)
            print('PASS: complete ' + name + ' syntax against saved XNU kernel headers/flags')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu-buildtree', type=Path,
                        help='optional extracted XNU build tree with saved component .CFLAGS/.CXXFLAGS')
    args = parser.parse_args()
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-unixnu.py'), '--check'], check=True)
    source = (SRC / 'sweet_ufs.c').read_text()
    # The source has paired FUXI/SWEET write guards with duplicate opening braces.
    # Select FUXI before extracting complete functions; the compiled target also defines it.
    active, selected = [True], []
    for line in source.splitlines(True):
        if line.startswith('#if '):
            assert line.strip() in ('#if defined(FUXI)', '#if defined(ARM64_BOARD_CONFIG_SWEET)')
            active.append(active[-1])
        elif line.startswith('#else'):
            active[-1] = active[-2] and not active[-1]
        elif line.startswith('#endif'):
            active.pop()
        elif active[-1]:
            selected.append(line)
    source = ''.join(selected)
    constants = source[source.index('#define REG_CAP'):source.index('/*\n * Fuxi root')]
    funcs = ''.join(function(source, n) for n in (
        'fuxi_ufs_poison', 'fuxi_ufs_check_idle', 'ufs_scsi', 'ufs_read_blocks',
        'ufs_write_blocks', 'ufs_take_over_host', 'sweet_ufs_strategy'))
    # Replace only the ARM store barrier in the host model. Cache/MMIO/clock are modeled above.
    funcs = re.sub(r'__asm__ volatile\("dsb sy" ::: "memory"\);', '(void)0;', funcs)
    with tempfile.TemporaryDirectory(prefix='fuxi-unixnu-test-') as tmp:
        tmp = Path(tmp)
        for name, text in [('ufs', UFS_PREFIX + constants + UFS_MODEL + funcs + UFS_TEST),
                           ('copy', COPY_TEST)]:
            path = tmp / (name + '.c')
            path.write_text(text)
            exe = tmp / name
            subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I', str(SRC), str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        display = (SRC / 'SweetFramebuffer.cpp').read_text()
        declaration = display[display.index('class AppleCLCD :'):display.index('OSDefineMetaClassAndStructors(AppleCLCD')]
        declaration = declaration.replace(': public IOMobileFramebuffer', '').replace(
            'OSDeclareDefaultStructors(AppleCLCD);', 'public:')
        methods = ''.join(function(display, name) for name in (
            'AppleCLCD::presentWantedLocked', 'AppleCLCD::wakePresentLocked', 'sweet_fb_rearm',
            'AppleCLCD::vsyncFired', 'get32', 'AppleCLCD::swap', 'AppleCLCD::present'))
        methods = methods.replace('__asm__ volatile ("dsb sy" ::: "memory");', '(void)0;')
        cpp = tmp / 'display.cpp'
        cpp.write_text(DISPLAY_PREFIX + declaration + methods + DISPLAY_TEST)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(SRC), str(cpp), '-o', str(tmp / 'display')], check=True)
        subprocess.run([str(tmp / 'display')], check=True)
        # Compile our ARM cache primitive as real AArch64 code (never run reference code).
        arm = tmp / 'cache.c'
        arm.write_text('typedef __UINTPTR_TYPE__ uintptr_t; typedef unsigned uint32_t;\n'
                       'typedef _Bool bool; static struct {uintptr_t dma;} su;\n' +
                       function(source, 'fuxi_ufs_cache') +
                       '\nvoid check(void) {fuxi_ufs_cache(0,64,1);fuxi_ufs_cache(0,64,0);}\n')
        subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-mgeneral-regs-only',
                        '-Wall', '-Wextra', '-Werror', '-c', str(arm), '-o', str(tmp / 'cache.o')], check=True)
        copy_arm = tmp / 'copy-arm.c'
        copy_arm.write_text('#include "fuxi_unixnu_copy.h"\n'
                            'uint64_t check(void *d,const void *s,void *p) {return '
                            'fuxi_unixnu_copy_diff(d,512,96,s,512,104,18,3,p,512,0); }\n')
        subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-mgeneral-regs-only',
                        '-Wall', '-Wextra', '-Werror', '-I', str(SRC), '-c', str(copy_arm),
                        '-o', str(tmp / 'copy-arm.o')], check=True)
    print('PASS: actual AArch64 cache/copy primitives compile; full XNU/macOS build and hardware remain untested')
    if args.xnu_buildtree:
        kernel_syntax(args.xnu_buildtree.resolve())


if __name__ == '__main__':
    main()
