#!/usr/bin/env python3
"""Offline production-code scanout models; no device or reference execution.

Checks display ownership, the CPU pixel oracle, real surface-cache reservation
and framebuffer integration. MMIO/clock/cache/DMA completion remain models.
"""
import argparse
import shlex
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define FG_GUARD
static unsigned fg_scanout_requested, fg_busy, fg_client_busy;
static unsigned ready=1,abiLive=1,abiResult,observations,reads,writes,starts,failAddress;
static uint64_t ticks=1000;
static uint64_t mach_absolute_time(void) {return ticks;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t*1000;}
static uint32_t ctl[1024],intf[1024];
static volatile uint32_t *fuxi_ctl0=ctl,*fuxi_intf1=intf;
static int fuxi_gpu_ready(void) {return ready;}
static int fg_abi_live_for_scanout(void) {return abiLive;}
static int fg_abi_observe(void) {observations++;if(!abiResult)fg_client_busy=0;return abiResult;}
static uint64_t model_counter(const char *r) {return !strcmp(r,"cntfrq_el0")?1000000:ticks;}
#define rsr(r) model_counter(r)
static unsigned fg_access(uint64_t pa,uint64_t *value,unsigned wr,unsigned wide) {
 assert(!wide);
 if(pa==failAddress)return 1;
 uint32_t *reg;
 if(pa>=0xae16000 && pa<0xae17000)reg=&ctl[(pa-0xae16000)/4];
 else {assert(pa>=0xae36000 && pa<0xae37000);reg=&intf[(pa-0xae36000)/4];}
 if(wr){writes++;*reg=(uint32_t)*value;if(pa==0xae1601c){assert(!fg_busy && !(intf[0x2b4/4]&0x80000000u));starts++;}}
 else {reads++;*value=*reg;}
 return 0;
}
'''
TEST = r'''
static void reset(void) {
 memset(ctl,0,sizeof(ctl));memset(intf,0,sizeof(intf));intf[0x2b4/4]=0x80000001u;
 fg_scanout_requested=fg_scanout_owned=fg_scanout_failed=fg_scanout_writable=0;
 fg_scanout_reserved=fg_busy=fg_client_busy=0;
 fg_scanout_checked=fg_scanout_checks=0;
 ticks=1000;ready=abiLive=1;abiResult=observations=reads=writes=starts=failAddress=0;
}
int main(void) {
 reset();assert(fuxi_gpu_scanout_acquire()==1 && fuxi_gpu_scanout_publish()==0);
 assert(fuxi_gpu_scanout_reserve());fuxi_gpu_scanout_release();assert(!reads && !writes && !observations);
 uint8_t *source=malloc(4544u*2436),*panel=malloc(4320u*2400);assert(source && panel);
 // Only exact option value 1 activates the lane/checker.
 fuxi_gpu_scanout_configure((uintptr_t)panel,2,1);assert(!fg_scanout_requested && !fg_scanout_checks);
 fuxi_gpu_scanout_configure((uintptr_t)panel,1,2);assert(fg_scanout_requested && !fg_scanout_checks);
 ready=0;assert(fuxi_gpu_scanout_acquire()==1 && !reads && !writes);ready=1;
 assert(!fuxi_gpu_scanout_acquire() && fuxi_gpu_scanout_owned() && !fg_scanout_writable);
 assert(intf[0x2b4/4]==1 && writes==1 && !starts);
 assert(fuxi_gpu_scanout_publish()==-1 && !starts);
 ticks+=19999;assert(!fuxi_gpu_scanout_acquire());
 ticks++;intf[0x2a8/4]=100;assert(!fuxi_gpu_scanout_acquire());
 intf[0x2a8/4]=2400;ctl[0x1c/4]=1;assert(!fuxi_gpu_scanout_acquire());
 ctl[0x1c/4]=0;assert(fuxi_gpu_scanout_acquire()==1 && fg_scanout_writable);
 // Existing console/display kick must not start an owned transfer, even idle.
 fuxi_display_kick();assert(!ctl[0x1c/4]);
 fg_client_busy=1;abiResult=1;assert(!fuxi_gpu_scanout_reserve() && !fg_scanout_reserved);
 abiResult=0;assert(fuxi_gpu_scanout_reserve() && !fg_client_busy && fg_scanout_reserved);
 assert(!fuxi_gpu_scanout_reserve());fuxi_gpu_scanout_release();assert(!fg_scanout_reserved);
 // A submitted copy cannot publish until the fence has retired.
 fg_busy=1;assert(!fuxi_gpu_scanout_acquire() && fuxi_gpu_scanout_publish()==-1 && !starts);
 fg_busy=0;assert(fuxi_gpu_scanout_publish()==1 && starts==1 && !fg_scanout_writable);
 assert(!fuxi_gpu_scanout_acquire());ctl[0x1c/4]=0;ticks+=20000;
 assert(fuxi_gpu_scanout_acquire()==1);
 // Drain timeout and unexpected AR are fail-closed, never an early CPU fallback.
 ctl[0x1c/4]=1;ticks=fg_scanout_timeout;assert(fuxi_gpu_scanout_acquire()==-1);
 assert(!fg_scanout_writable && fuxi_gpu_scanout_publish()==-1 && starts==1);
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);assert(!fuxi_gpu_scanout_acquire());
 intf[0x2b4/4]|=0x80000000u;assert(fuxi_gpu_scanout_acquire()==-1 && !starts);
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);failAddress=0xae362b4;
 assert(fuxi_gpu_scanout_acquire()==-1 && !writes && !starts);
 // CPU row-copy oracle: the exact centered crop, padding and every BGRA/alpha byte.
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,1);
 for(unsigned y=0;y<2436;y++)for(unsigned x=0;x<1136;x++)
  ((uint32_t *)source)[y*1136+x]=(y*0x314159u)^(x*0x1234567u);
 fg_scanout_source=(uintptr_t)source;fg_scanout_pitch=4544;
 fg_scanout_sx=22;fg_scanout_sy=18;fg_scanout_dx=fg_scanout_dy=0;
 fg_scanout_w=1080;fg_scanout_h=2400;
 for(unsigned y=0;y<2400;y++)memcpy(panel+y*4320,source+(y+18)*4544+22*4,4320);
 for(unsigned frame=0;frame<4;frame++)assert(fg_scanout_verify());
 assert(fg_scanout_checked==4);panel[0]^=1;assert(fg_scanout_verify()); // first four only
 fg_scanout_checked=0;assert(!fg_scanout_verify() && fg_scanout_checked==1);panel[0]^=1;
 panel[4320u*2400-1]^=0x80;assert(!fg_scanout_verify());panel[4320u*2400-1]^=0x80;
 fg_scanout_checked=0;fg_scanout_w=17;fg_scanout_h=3;fg_scanout_dx=3;fg_scanout_dy=5;
 memset(panel,0x5a,4320u*2400);
 for(unsigned y=0;y<3;y++)memcpy(panel+(y+5)*4320+3*4,source+(y+18)*4544+22*4,17*4);
 assert(fg_scanout_verify() && panel[0]==0x5a);
 free(source);free(panel);
 puts("PASS: actual scanout gate, delayed transfer/fence/START ordering, busy reservation, timeout/MMIO/AR fail-close, full crop/alpha/tail pixel oracle");
}
'''


def compile_run(tmp, name, source, cpp=False, includes=()):
    src = tmp / (name + ('.cpp' if cpp else '.c'))
    src.write_text(source)
    exe = tmp / name
    subprocess.run(['clang++' if cpp else 'clang', '-std=c++17' if cpp else '-std=gnu11',
                    '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', *['-I'+str(p) for p in includes], str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def kernel_syntax(buildtree):
    """Compile only our sources against saved XNU headers; execute no tree scripts."""
    sync = module('scanout_syntax_sync', 'tools/sync-fuxi-unixnu.py')
    sync.FILES = {'iokit/Kernel/arm/SweetSurface.h': 'SweetSurface.h'}
    surface_header = sync.baseline()['iokit/Kernel/arm/SweetSurface.h']
    config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_FUXI'
    if not config.exists():
        config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
    with tempfile.TemporaryDirectory(prefix='fuxi-scanout-syntax-') as temp:
        temp = Path(temp)
        dev, arm = temp / 'bsd/dev', temp / 'iokit/Kernel/arm'
        dev.mkdir(parents=True)
        arm.mkdir(parents=True)
        (arm / 'SweetSurface.h').write_text(surface_header)
        for path in (ROOT / 'xnu/fuxi-adreno').iterdir():
            if path.suffix in ('.h', '.inc', '.c'):
                (dev / path.name).write_bytes(path.read_bytes())
        for name in ('gpu.c', 'gpu-a740.h', 'gpu-zap.h', 'gpu-handoff.h'):
            (dev / name).write_bytes((ROOT / 'shim/fuxi-probe' / name).read_bytes())
        (arm / 'SweetSurface.cpp').write_bytes((ROOT / 'xnu/fuxi-scanout/SweetSurface.cpp').read_bytes())
        # Check the modified console function against the complete preexisting TU.
        pe = temp / 'pexpert/arm/pe_serial.c'
        pe.parent.mkdir(parents=True)
        original = (buildtree / 'xnu/pexpert/arm/pe_serial.c').read_text()
        before = module('scanout_kick_sync', 'tools/sync-fuxi-scanout.py').KICK_BEFORE
        pe.write_text(original)
        if before not in original:
            # The saved 0081 tree predates Fuxi. Apply only pe_serial text deltas
            # in our temporary tree, never mutate or run the saved checkout.
            for patch in sorted((ROOT / 'xnu/patches').glob('*.patch')):
                if patch.name < '0082' or patch.name >= '0108':
                    continue
                for chunk in re.split(r'(?=^--- a/)', patch.read_text(), flags=re.M):
                    if chunk.startswith('--- a/pexpert/arm/pe_serial.c\n'):
                        check = subprocess.run(['git', 'apply', '--check', '-'], input=chunk,
                                               text=True, cwd=temp, capture_output=True)
                        if check.returncode:
                            subprocess.run(['git', 'apply', '--reverse', '--check', '-'], input=chunk,
                                           text=True, cwd=temp, check=True, capture_output=True)
                        else:
                            subprocess.run(['git', 'apply', '--whitespace=nowarn', '-'], input=chunk,
                                           text=True, cwd=temp, check=True, capture_output=True)
            original = pe.read_text()
        assert before in original
        pe.write_text(original.replace(before, (ROOT / 'xnu/fuxi-scanout/display-kick.c').read_text()))
        for comp, path in [('bsd', dev / 'fuxi_adreno.c'), ('iokit', arm / 'SweetSurface.cpp'), ('pexpert', pe)]:
            cwd = config / comp / 'DEVELOPMENT'
            old_root = re.search(r'^SOURCE_DIR=(.+)/xnu$', (cwd / 'Makefile').read_text(), re.M)[1]
            raw = (cwd / '.CFLAGS').read_text().replace('\0', '')
            raw = re.sub(r'(-DCONFIG_NMBCLUSTERS=\(\(1024 \* 512\) / MCLBYTES\))', lambda m: shlex.quote(m[1]), raw)
            saved = shlex.split(raw)[1:]
            if comp == 'iokit':
                saved += shlex.split((cwd / '.CXXFLAGS').read_text().replace('\0', ''))[1:]
            cmd = ['clang++' if comp == 'iokit' else 'clang', '--target=arm64-apple-macos14',
                   '-fsyntax-only', '-DFUXI', '-mgeneral-regs-only', '-Werror', '-Wno-c99-designator',
                   '-Wno-reorder-init-list', '-D__builtin_xnu_type_signature(...)=""',
                   '-D__builtin_xnu_type_summary(...)=0', '-D__builtin_xnu_types_compatible(...)=1',
                   '-I'+str(dev), '-I'+str(arm)]
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
                             '-I'+str(buildtree / 'sdk-kernel-platform/platform'), arg)
                cmd.append(arg)
            subprocess.run(cmd + [str(path)], cwd=cwd, check=True)
            print('PASS: complete '+path.name+' syntax against saved XNU kernel headers/flags')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu-buildtree', type=Path, help='optional saved XNU build tree for full-TU syntax checks')
    args = parser.parse_args()
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-scanout.py'), '--check'], check=True)
    inc = (ROOT / 'xnu/fuxi-adreno/fuxi_gpu_scanout.inc').read_text()
    inc = re.sub(r'__asm__ volatile\([^;]*\);', '(void)0;', inc)
    kick = (ROOT / 'xnu/fuxi-scanout/display-kick.c').read_text()
    kick = re.sub(r'__asm__ volatile\([^;]*\);', '(void)0;', kick)
    cache = module('scanout_cache', 'tools/test-fuxi-adreno-cache.py')
    display = module('scanout_display', 'tools/test-fuxi-unixnu.py')
    with tempfile.TemporaryDirectory(prefix='fuxi-scanout-test-') as tmp:
        tmp = Path(tmp)
        compile_run(tmp, 'gate', PREFIX + inc + kick + TEST)
        # Compile final production SweetSurface, including the reservation changes.
        source = (ROOT / 'xnu/fuxi-scanout/SweetSurface.cpp').read_text()
        declarations = source[source.index('struct sweet_gpu_backing'):source.index('static bool sweet_gpu_init')]
        funcs = ''.join(cache.submit.function(source, n) for n in (
            'sweet_gpu_init', 'sweet_gpu_drop', 'sweet_gpu_release', 'sweet_surface_gpu_copy',
            'sweet_surface_gpu_poll', 'sweet_gpu_writer', 'sweet_surface_gpu_stats'))
        stubs = '''\nstatic bool reserved,busy,owned;
static int fuxi_gpu_scanout_owned() {return owned;}
static int fuxi_gpu_scanout_reserve() {if(busy)return 0;assert(!reserved);reserved=true;return 1;}
static void fuxi_gpu_scanout_release() {assert(reserved);reserved=false;}
'''
        extra = '''
 ready=true;busy=true;owned=true;
 IOMemoryDescriptor pendingMemory;SweetSurface pendingSurface;pendingSurface.memory=&pendingMemory;
 unsigned beforeMaps=maps,beforeCopies=copies;
 assert(sweet_surface_gpu_copy(&pendingSurface,0x10000,0,0,0,0,1080,10)==3);
 assert(!reserved && maps==beforeMaps && copies==beforeCopies && pendingSurface.refs==1);
 busy=false;pendingSurface.gpuWriters=1;
 assert(sweet_surface_gpu_copy(&pendingSurface,0x10000,0,0,0,0,1080,10)==3 && !reserved);
 pendingSurface.gpuWriters=0;owned=false;
'''
        model = cache.MODEL.replace(' IOLockFree(sweet_gpu_lock);', extra + ' IOLockFree(sweet_gpu_lock);')
        compile_run(tmp, 'cache', cache.PREFIX + stubs + declarations + funcs + model, cpp=True)
        source = (ROOT / 'xnu/fuxi-unixnu/SweetFramebuffer.cpp').read_text()
        declaration = source[source.index('class AppleCLCD :'):source.index('OSDefineMetaClassAndStructors(AppleCLCD')]
        declaration = declaration.replace(': public IOMobileFramebuffer', '').replace('OSDeclareDefaultStructors(AppleCLCD);', 'public:')
        methods = ''.join(display.function(source, n) for n in (
            'AppleCLCD::presentWantedLocked', 'AppleCLCD::wakePresentLocked', 'sweet_fb_rearm',
            'AppleCLCD::vsyncFired', 'AppleCLCD::scanoutLog', 'get32', 'AppleCLCD::swap', 'AppleCLCD::present'))
        methods = re.sub(r'__asm__ volatile\s*\([^;]*\);', '(void)0;', methods)
        extra = '''
 // Gate deferral preserves dirty swap/pixels; no publication while waiting.
 fb.fScanout=true;fb.fScanoutLog=true;scanOwned=true;scanPublish=1;gpuMode=2;pollMode=1;
 fb.swap(rec);scanAcquire=0;
 unsigned prior=scanPublications;uint64_t saved=pixels[0];
 fb.vsyncFired(&fb,&timer);assert(fb.fDirty && !fb.fGpuPending && pixels[0]==saved && scanPublications==prior);
 scanAcquire=1;fb.vsyncFired(&fb,&timer);assert(fb.fGpuPending && scanPublications==prior);
 pollMode=0;fb.vsyncFired(&fb,&timer);assert(scanPublications==prior);
 pollMode=1;fb.vsyncFired(&fb,&timer);assert(!fb.fGpuPending && scanPublications==prior+1);
 // Busy GPU ring keeps newest swap; no CPU copy or premature transfer.
 gpuMode=3;fb.swap(rec);prior=scanPublications;fb.vsyncFired(&fb,&timer);
 assert(fb.fDirty && !fb.fGpuPending && scanPublications==prior);
 gpuMode=-1;fb.vsyncFired(&fb,&timer);assert(fb.fDirty && scanPublications==prior);
 // CPU fallback is legal after acquisition and publishes only after stores.
 gpuMode=0;fb.vsyncFired(&fb,&timer);assert(!fb.fDirty && scanPublications==prior+1);
 // Power-off is gated too; the completed black frame is explicitly sent once.
 fb.fPowerOff=true;fb.fFadeStep=0;scanAcquire=0;prior=scanPublications;
 pixels[0]=0x55;fb.vsyncFired(&fb,&timer);assert(pixels[0]==0x55 && !fb.fFadeStep && scanPublications==prior);
 scanAcquire=1;fb.vsyncFired(&fb,&timer);assert(!pixels[0] && fb.fFadeStep==1 && scanPublications==prior+1);
 // Fast ownership polls must not produce extra 60 Hz notifications/counts.
 fb.fPowerOff=false;fb.fDirty=false;client.fPort=1;
 uint64_t notifications=client.notifications,count=fb.fVsyncCount;
 fb.fNextTick=ticks+100;
 fb.vsyncFired(&fb,&timer);assert(client.notifications==notifications && fb.fVsyncCount==count);
 ticks=fb.fNextTick;fb.vsyncFired(&fb,&timer);
 assert(client.notifications==notifications+1 && fb.fVsyncCount==count+1);
 client.fPort=0;
 // Periodic diagnostic branch, including actual log formatting arguments.
 fb.fScanoutFrames=119;fb.scanoutLog(false,216,42000,0);assert(fb.fScanoutFrames==120);
'''
        model = display.DISPLAY_TEST.replace(' puts("PASS: actual framebuffer', extra + ' puts("PASS: actual framebuffer')
        compile_run(tmp, 'framebuffer', display.DISPLAY_PREFIX + declaration + methods + model,
                    cpp=True, includes=(ROOT / 'xnu/fuxi-unixnu',))
    print('PASS: production scanout integration; real panel completion/cache coherency still require device qualification')
    if args.xnu_buildtree:
        kernel_syntax(args.xnu_buildtree.resolve())


if __name__ == '__main__':
    main()
