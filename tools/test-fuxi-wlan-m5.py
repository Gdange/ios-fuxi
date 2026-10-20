#!/usr/bin/env python3
"""Exercise the actual M5 C block with fake MMIO, VFS and physical memory.

No hardware/USB access. Synthetic ELF bytes are generated in memory. This
checks sequence/containment and ABI bounds, not real SMMU or firmware behavior.
"""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#define printf(...) ((void)0)
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef unsigned ppnum_t;
typedef int kern_return_t;
typedef void *vnode_t;
typedef void *vfs_context_t;
typedef void *event_t;
#define PAGE_SHIFT 14
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)
#define round_page(x) (((x) + PAGE_MASK) & ~(size_t)PAGE_MASK)
#define ptoa_64(x) ((uint64_t)(x) << PAGE_SHIFT)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define kernel_map 0
#define kernel_pmap 0
#define KERN_SUCCESS 0
#define KMA_DATA 1
#define KMA_ZERO 2
#define KMA_NOPAGEWAIT 4
#define VM_KERN_MEMORY_IOKIT 0
#define TRUE 1
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL 0
#define NSEC_PER_MSEC 1000000
#define FREAD 1
#define O_NOFOLLOW 2
#define O_NONBLOCK 4
#define VREG 1
#define UIO_READ 0
#define UIO_SYSSPACE 0
#define WL_BAR0 0x60400000ull
#define WL_EPCFG 0x60100000ull
#define WL_PARF 0x1c00000ull
#define TS_TLMM 0xf100000ull
static uint32_t wl4_report[64], fw_faults;
struct reg { uint64_t pa; uint32_t v; };
struct ev { char kind; uint64_t pa; uint32_t v; };
static struct reg regs[4096];
static struct ev events[8192];
static unsigned nr, ne, sleeps, frees, allocations;
static bool progress = true, fail_bm, fail_perst, no_root;
static bool noncontig, high_memory, fail_alloc;
static int open_error, read_resid;
static uint8_t fixture[0x80000 + 37];
static uint64_t file_size = sizeof(fixture);
struct alloc { uintptr_t va; size_t size; uint64_t pa; };
static struct alloc allocs[32];
static uint64_t next_pa = 0x90000000;
static void record(char k, uint64_t pa, uint32_t v) {
    assert(ne < 8192); events[ne++] = (struct ev){k,pa,v};
}
static uint32_t wl_prd(uint64_t pa) {
    for (unsigned i=0;i<nr;i++) if(regs[i].pa==pa) return regs[i].v;
    return 0;
}
static void setreg(uint64_t pa, uint32_t v) {
    for (unsigned i=0;i<nr;i++) if(regs[i].pa==pa) {regs[i].v=v; return;}
    assert(nr < 4096); regs[nr++] = (struct reg){pa,v};
}
static void wl_pwr(uint64_t pa, uint32_t v) {
    record('W',pa,v);
    if (fail_bm && pa==WL_EPCFG+4) return;
    if (fail_perst && pa==TS_TLMM+0x1000*94+4) return;
    setreg(pa,v);
    if (pa==WL_BAR0+0x38 && v==2) setreg(pa,0);
    if (!progress) return;
    if (pa==WL_BAR0+0x118) {
        setreg(WL_BAR0+0x12c, 2u<<30); setreg(WL_BAR0+0x48, 0x101);
    }
    if (pa==WL_BAR0+0x38 && v==0x200) {
        setreg(WL_BAR0+0x128,1); setreg(WL_BAR0+0x48,0x200);
    }
    if (pa==WL_BAR0+0x260) {
        setreg(WL_BAR0+0x268,(2u<<30)|(v & 0x3fffffff));
        setreg(WL_BAR0+0x128,2);
    }
}
static uintptr_t fw_va(uint64_t pa) { return pa; }
static unsigned fuxi_gpu_access(uintptr_t pa, uint64_t *v, unsigned wr, unsigned unused) {
    if(wr) wl_pwr(pa,(uint32_t)*v); else *v=wl_prd(pa);
    return 0;
}
static const char *wl_mhi_ee(uint32_t e) { return "fake"; }
static const char *wl_mhi_state(uint32_t s) { return "fake"; }
static void assert_wait_timeout(event_t e, int intr, unsigned ms, unsigned ns) { sleeps+=ms; }
static void thread_block(int cont) { }
static uint64_t mach_absolute_time(void) { return 0x12345; }
static int kmem_alloc_contig(int map, uintptr_t *va, size_t size, uintptr_t mask,
    ppnum_t max, ppnum_t pnmask, int flags, int tag) {
    assert(max == (0xffffffffull >> PAGE_SHIFT));
    assert(mask==PAGE_MASK && flags & KMA_NOPAGEWAIT);
    if(fail_alloc) return 1;
    *va=(uintptr_t)aligned_alloc(PAGE_SIZE,size); assert(*va);
    memset((void *)*va,0,size);
    allocs[allocations++] = (struct alloc){*va,size,high_memory?0x100000000ull:next_pa};
    next_pa+=size;
    return 0;
}
static ppnum_t pmap_find_phys(int pmap, uintptr_t va) {
    for(unsigned i=0;i<allocations;i++) if(va>=allocs[i].va && va<allocs[i].va+allocs[i].size)
        return (allocs[i].pa+(va-allocs[i].va)+(noncontig && va!=allocs[i].va?PAGE_SIZE:0)) >> PAGE_SHIFT;
    return 0;
}
static void kmem_free(int map, uintptr_t va, size_t size) {
    record('F',va,0); frees++; free((void *)va);
}
static void flush_dcache64(uint64_t pa, unsigned size, int phys) { record('C',pa,size); }
struct vnode_attr { uint64_t va_data_size; };
#define VATTR_INIT(a) memset((a),0,sizeof(*(a)))
#define VATTR_WANTED(a,f) ((void)0)
#define VATTR_IS_SUPPORTED(a,f) 1
static vfs_context_t vfs_context_kernel(void) { return (void *)1; }
static vnode_t vfs_rootvnode(void) { return no_root?NULL:(void *)1; }
static void vnode_put(vnode_t root) { }
static int vnode_open(const char *p, int mode, int cmode, int flags, vnode_t *vp, vfs_context_t c) {
    assert(mode == (FREAD|O_NOFOLLOW|O_NONBLOCK)); *vp=(void *)1; return open_error;
}
static int vnode_getattr(vnode_t v, struct vnode_attr *a, vfs_context_t c) { a->va_data_size=file_size; return 0; }
static int vnode_vtype(vnode_t v) { return VREG; }
static void *vfs_context_ucred(vfs_context_t c) { return c; }
static void *vfs_context_proc(vfs_context_t c) { return c; }
static int vn_rdwr(int rw, vnode_t v, char *b, int len, off_t off, int seg, int io,
    void *cred, int *resid, void *proc) {
    assert((unsigned)len<=sizeof(fixture)); memcpy(b,fixture,len); *resid=read_resid; return 0;
}
static int vnode_close(vnode_t v, int mode, vfs_context_t c) { return 0; }
'''
TESTS = r'''
static void init(void) {
    nr=ne=sleeps=frees=allocations=0; next_pa=0x90000000;
    progress=true; fail_bm=fail_perst=no_root=false;
    high_memory=noncontig=fail_alloc=false; open_error=read_resid=0;
    fw_faults=0; wl_mhi_valid=wl_dma_started=false;
    memset(&wl_image,0,sizeof(wl_image)); memset(&wl_vectors,0,sizeof(wl_vectors));
    memset(&wl_contexts,0,sizeof(wl_contexts)); memset(wl5_report,0,sizeof(wl5_report));
    memset(fixture,0,sizeof(fixture)); file_size=sizeof(fixture);
    memcpy(fixture,"\177ELF\1\1\1",7);
    fixture[20]=1; fixture[28]=52; fixture[40]=52; fixture[42]=32; fixture[44]=1;
    wl4_report[25]=0x100; wl4_report[26]=0x224;
    setreg(WL_SMMU+0x20,(1u<<27)|4); setreg(WL_SMMU+0x24,16);
    setreg(WL_BAR0+0x80000,0x201); setreg(WL_BAR0+0x10,0x800080);
    setreg(WL_BAR0+0x20,0x700); setreg(WL_EPCFG+4,0x100002);
    setreg(TS_TLMM+0x1000*94+4,2);
}
static void release(void) { wl_dma_free(&wl_contexts); wl_dma_free(&wl_vectors); wl_dma_free(&wl_image); }
static int findwrite(uint64_t pa, uint32_t v, unsigned from) {
    for(unsigned i=from;i<ne;i++) if(events[i].kind=='W' && events[i].pa==pa && events[i].v==v) return i;
    return -1;
}
int main(void) {
    /* End-to-end fake device: ELF, image/table bounds, context ABI, DMA ordering. */
    init();
    for(unsigned s=17;s<=28;s++) assert(wl5_step(s)==0);
    assert(strstr(wl_fw_path,"amss20.bin"));
    assert(wl_fw_size==sizeof(fixture) && wl_image.pa<0x100000000ull);
    struct wl_vector *v=(void *)wl_vectors.va;
    assert(v[0].size==WL_CHUNK && v[1].size==37 && v[1].addr==wl_image.pa+WL_CHUNK);
    assert(wl5_report[19]==32);
    struct wl_context *e=(void *)(wl_contexts.va+WL_ECTX);
    assert(e[0].len==512 && e[1].len==4096 && e[1].base==wl_contexts.pa+WL_ER1);
    for(unsigned i=0;i<128*sizeof(struct wl_context);i++) assert(((uint8_t *)wl_contexts.va)[i]==0);
    int bme=findwrite(WL_EPCFG+4,0x406,0);
    int bhi=findwrite(WL_BAR0+0x118,0x12345,0);
    assert(bme>0 && bhi>bme && events[bme-1].kind=='C');
    assert(findwrite(WL_BAR0+0x10c,0,0)<findwrite(WL_BAR0+0x108,wl_image.pa,0));
    assert(findwrite(WL_BAR0+0x64,0,0)<findwrite(WL_BAR0+0x60,wl_contexts.pa+WL_ECTX,0));
    assert(findwrite(WL_BAR0+0x38,0x200,0)>findwrite(WL_BAR0+0x700,wl_contexts.pa+WL_ER0+496,0));
    wl5_status(28);
    assert(wl5_report[64]==2 && wl5_report[65]==0x200);
    /* Cleanup reset -> BME off -> PERST -> free, even with latched faults. */
    unsigned start=ne; fw_faults=1; wl5_abort(28);
    int rst=findwrite(WL_BAR0+0x38,2,start), off=findwrite(WL_EPCFG+4,0x402,start);
    int perst=findwrite(TS_TLMM+0x1000*94+4,0,start);
    assert(rst>=0 && off>rst && perst>off && frees==3);
    assert(!wl_image.va && fw_faults==1);
    for(unsigned i=start;i<ne;i++) if(events[i].kind=='F') assert(i>(unsigned)perst);

    /* Translated, faulting, wildcard and EXID matches. No writes in step 17. */
    init(); setreg(WL_SMMU+0x800,0x80001401); setreg(WL_SMMU+0xc00,0);
    assert(wl5_step(17)==3 && ne==0);
    setreg(WL_SMMU+0xc00,2u<<16); assert(wl5_step(17)==3 && ne==0);
    setreg(WL_SMMU+0x800,0x80011400); setreg(WL_SMMU+0xc00,1u<<16);
    assert(wl5_step(17)==0 && ne==0);
    setreg(WL_SMMU+0x804,0x80001401); setreg(WL_SMMU+0xc04,1u<<16);
    assert(wl5_step(17)==3 && ne==0); /* ambiguous match */
    init(); setreg(WL_SMMU,8); setreg(WL_SMMU+0x20,(1u<<27)|256|4);
    setreg(WL_SMMU+0x800,0x1401); setreg(WL_SMMU+0xc00,0x400);
    assert(wl5_step(17)==3); /* EXIDVALID translated */
    init(); setreg(WL_SMMU,0x400); assert(wl5_step(17)==3); /* unmatched FAULT */
    init(); setreg(WL_SMMU,1); assert(wl5_step(17)==2); /* CLIENTPD */
    init(); setreg(WL_SMMU+0x800,0x80001200); setreg(WL_SMMU+0xc00,0);
    assert(wl5_step(17)==5); /* stated SIDs bypass, but actual RID route absent */
    setreg(WL_PARF+0x2000+4*0x15,0x01000100); /* CRC8(01 00) = 0x15 */
    assert(wl5_step(17)==0);

    /* Every hardware poll terminates, error EEs are never success. */
    init(); assert(wl5_step(17)==0);
    assert(wl_poll(0)==2 && sleeps==5000);
    setreg(WL_BAR0+0x128,3); assert(wl_poll(0)==1);
    setreg(WL_BAR0+0x128,6); assert(wl_poll(4)==1);
    setreg(WL_BAR0+0x128,0); setreg(WL_BAR0+0x48,4); assert(wl_poll(1)==1);
    setreg(WL_BAR0+0x48,0); wl_seq=9;
    setreg(WL_BAR0+0x268,(2u<<30)|8); assert(wl_poll(3)==2);
    setreg(WL_BAR0+0x268,(2u<<30)|9); assert(wl_poll(3)==0);

    /* Root timeout, missing/short/oversize/non-ELF/truncated ELF, short reads. */
    init(); assert(wl5_step(17)==0 && wl5_step(18)==0);
    no_root=true; assert(wl5_step(19)==1 && sleeps==120000 && allocations==0);
    no_root=false; open_error=2; assert(wl5_step(19)==2); open_error=0;
    file_size=WL_CHUNK-1; assert(wl5_step(19)==3);
    file_size=WL_FW_MAX+1; assert(wl5_step(19)==3);
    file_size=sizeof(fixture); fixture[0]=0; assert(wl5_step(19)==6); release();
    fixture[0]=0x7f; fixture[30]=0xff; assert(wl5_step(19)==6); release();
    fixture[30]=0; read_resid=1; assert(wl5_step(19)==5); release();
    read_resid=0; high_memory=true; assert(wl5_step(19)==4); release();
    high_memory=false; noncontig=true; assert(wl5_step(19)==4); release();
    noncontig=false; fail_alloc=true; assert(wl5_step(19)==4); release();
    fail_alloc=false; setreg(WL_BAR0+0x80000,0x101); assert(wl5_step(18)==0);
    assert(strstr(wl_fw_path,"/amss.bin"));
    assert(wl5_step(19)==0); release();

    /* Both PCI interrupt mechanisms are disabled, malformed cycles bounded. */
    init(); setreg(WL_EPCFG+0x34,0x40);
    setreg(WL_EPCFG+0x40,(1u<<31)|0x5011); /* MSI-X -> 0x50 */
    setreg(WL_EPCFG+0x50,(1u<<16)|5); /* MSI */
    assert(wl_disable_irqs()==0);
    assert(!(wl_prd(WL_EPCFG+0x50)&(1u<<16)));
    assert((wl_prd(WL_EPCFG+0x40)&(3u<<30))==(1u<<30));
    setreg(WL_EPCFG+0x50,0x5005); assert(wl_disable_irqs()==3);

    /* Never free buffers when active DMA cannot be contained. */
    init(); for(unsigned s=17;s<=21;s++) assert(wl5_step(s)==0);
    fail_bm=fail_perst=true; wl5_abort(21);
    assert(frees==0 && wl_image.va && wl_contexts.va && wl_vectors.va);
    fail_bm=fail_perst=false; wl5_abort(21); assert(frees==3);
    puts("M5 fake-MMIO/VFS/DMA sequence and failure tests passed");
    return 0;
}
'''


def main():
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    block = driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    block = '/* ---- milestone 5:' + block
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* host memory barrier */;', block)
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m5-') as tmp:
        cfile, binary = Path(tmp) / 'm5.c', Path(tmp) / 'm5'
        cfile.write_text(PREAMBLE + block + TESTS)
        subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    spec = importlib.util.spec_from_file_location('wlan_probe', ROOT / 'tools/fuxi-wlan-probe.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-log-') as tmp:
        log = Path(tmp) / 'console.log'
        r = [0] * 128
        r[0:4] = [0x574c0005, 28, 28, 0]
        for step in range(17, 29):
            r[32 + step] = 0x1000 | step
        r[16], r[64], r[65] = 0x201, 2, 0x200
        log.write_text('\n'.join('prefix FUXI WLAN: m5 r[%3d] ' % i +
                                  ' '.join(f'{v:08x}' for v in r[i:i+8]) for i in range(0,128,8)))
        reports, seen = probe.from_xnu_log(log)
        assert seen == {'m5'} and reports['m5'] == r
        decoded = probe.decode_m5(r)
        assert 'step 28 OK' in decoded and 'amss20.bin' in decoded and 'state=M0' in decoded
        failed = [0] * 128
        failed[2], failed[49] = 17, (17 << 8) | 1
        assert 'classification incomplete' in probe.decode_m5(failed)
        print('M5 offline log reconstruction/decoder passed')


if __name__ == '__main__':
    main()
