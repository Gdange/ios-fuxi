#!/usr/bin/env python3
"""Exercise the actual M6 C block with fake MMIO, VFS and physical memory.

No hardware/USB access. Synthetic ELF bytes are generated in memory. This
checks sequence/containment and ABI bounds, not real SMMU or firmware behavior.
"""
from pathlib import Path
import importlib.util
import os
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
static bool noncontig, high_memory, fail_alloc, tlb_busy, mismatch, inject_fault;
static uint64_t mismatch_pa, read_fault_pa;
static int open_error, read_resid;
static uint8_t fixture[0x80000 + 37];
static uint64_t file_size = sizeof(fixture);
struct alloc { uintptr_t va; size_t size; uint64_t pa; };
static struct alloc allocs[32];
static uint64_t next_pa = 0x8c0000000ull;
static void record(char k, uint64_t pa, uint32_t v) {
    assert(ne < 8192); events[ne++] = (struct ev){k,pa,v};
}
static uint32_t wl_prd(uint64_t pa) {
    if (read_fault_pa && pa==read_fault_pa) {fw_faults++; return ~0u;}
    for (unsigned i=0;i<nr;i++) if(regs[i].pa==pa) return (tlb_busy && (pa & 0xfff)==0x7f4) ? 1 : regs[i].v;
    return 0;
}
static void setreg(uint64_t pa, uint32_t v) {
    for (unsigned i=0;i<nr;i++) if(regs[i].pa==pa) {regs[i].v=v; return;}
    assert(nr < 4096); regs[nr++] = (struct reg){pa,v};
}
static void raw_pwr(uint64_t pa, uint32_t v) {
    record('W',pa,v);
    if (fail_bm && pa==WL_EPCFG+4) return;
    if (fail_perst && pa==TS_TLMM+0x1000*94+4) return;
    if (mismatch && pa==mismatch_pa) return;
    if (pa>=0x15080000ull && pa<0x15100000ull && (pa&0xfff)==0x58) {setreg(pa,wl_prd(pa)&~v); return;}
    setreg(pa,v);
    if (inject_fault && pa==WL_BAR0+0x118) setreg(0x15081058ull,2);
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
static void wl_pwr(uint64_t pa, uint32_t v) { if (!fw_faults) raw_pwr(pa,v); }
static uintptr_t fw_va(uint64_t pa) { return pa; }
static unsigned fuxi_gpu_access(uintptr_t pa, uint64_t *v, unsigned wr, unsigned unused) {
    if(wr) raw_pwr(pa,(uint32_t)*v); else *v=wl_prd(pa);
    return 0;
}
static const char *wl_mhi_ee(uint32_t e) { return "fake"; }
static const char *wl_mhi_state(uint32_t s) { return "fake"; }
static void assert_wait_timeout(event_t e, int intr, unsigned ms, unsigned ns) { sleeps+=ms; }
static void thread_block(int cont) { }
static uint64_t mach_absolute_time(void) { return 0x12345; }
static int kmem_alloc(int map, uintptr_t *va, size_t size, int flags, int tag) {
    assert(flags & KMA_NOPAGEWAIT);
    if(fail_alloc) return 1;
    *va=(uintptr_t)aligned_alloc(PAGE_SIZE,size); assert(*va);
    memset((void *)*va,0,size);
    allocs[allocations++] = (struct alloc){*va,size,high_memory?0x1000000000ull:next_pa};
    next_pa+=size*2;
    return 0;
}
static ppnum_t pmap_find_phys(int pmap, uintptr_t va) {
    for(unsigned i=0;i<allocations;i++) if(va>=allocs[i].va && va<allocs[i].va+allocs[i].size)
        return (allocs[i].pa+(va-allocs[i].va)+(noncontig ? (va-allocs[i].va) : 0)) >> PAGE_SHIFT;
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
    nr=ne=sleeps=frees=allocations=0; next_pa=0x8c0000000ull;
    progress=true; fail_bm=fail_perst=no_root=false;
    mismatch_pa=0x15001004ull; read_fault_pa=0;
    high_memory=noncontig=fail_alloc=tlb_busy=mismatch=inject_fault=false; open_error=read_resid=0;
    fw_faults=0; wl_mhi_valid=false;
    memset(&wl_image,0,sizeof(wl_image)); memset(&wl_vectors,0,sizeof(wl_vectors));
    memset(&wl_tables,0,sizeof(wl_tables)); wl_owned=wl_enabled=wl_route_owned=false; wl_iova_next=WL_IOVA;
    memset(&wl_contexts,0,sizeof(wl_contexts)); memset(wl5_report,0,sizeof(wl5_report));
    memset(fixture,0,sizeof(fixture)); file_size=sizeof(fixture);
    memcpy(fixture,"\177ELF\1\1\1",7);
    fixture[20]=1; fixture[28]=52; fixture[40]=52; fixture[42]=32; fixture[44]=1;
    wl4_report[25]=0x100; wl4_report[26]=0x224;
    setreg(WL_SMMU,0x400); setreg(WL_SMMU+0x20,0x48001e04); setreg(WL_SMMU+0x24,0x60000046);
    setreg(WL_SMMU+0x28,0x1000|0x22);
    for(unsigned i=0;i<4;i++) setreg(WL_SMMU+0xc00+4*i,0x20000);
    setreg(WL_BAR0+0x80000,0x201); setreg(WL_BAR0+0x10,0x800080);
    setreg(WL_BAR0+0x268,0xcab04cf5); /* undefined before first BHIe DB */
    setreg(WL_BAR0+0x20,0x700); setreg(WL_EPCFG+4,0x100002);
    setreg(TS_TLMM+0x1000*94+4,2);
}
static void release(void) { wl_dma_free(&wl_tables); wl_dma_free(&wl_contexts); wl_dma_free(&wl_vectors); wl_dma_free(&wl_image); }
static uint64_t walk(uint64_t addr) {
    if (addr >= (1ull<<32)) return 0;
    uint64_t *root=(void *)wl_tables.va;
    if (!root[addr>>30]) return 0;
    uint64_t *l2=(void *)(wl_tables.va+4096);
    if (!l2[(addr>>21)&511]) return 0;
    unsigned n=(addr-WL_IOVA)>>21;
    uint64_t *l3=(void *)(wl_tables.va+(2+n)*4096);
    return l3[(addr>>12)&511];
}
static void check_mapping(void) {
    struct wl_dma *bufs[]={&wl_image,&wl_vectors,&wl_contexts};
    uint64_t expected=wl_pt_pa(0)|((uint64_t)wl_cb<<48);
    assert(wl_prd(wl_cb_addr(wl_cb)+0x20)==(uint32_t)expected);
    assert(wl_prd(wl_cb_addr(wl_cb)+0x24)==expected>>32);
    assert(wl_prd(wl_cb_addr(wl_cb)+0x30)==0x803520);
    assert(wl_prd(wl_cb_addr(wl_cb)+0x10)==0x38011);
    for(unsigned i=0;i<3;i++) for(size_t off=0;off<bufs[i]->size;off+=4096) {
        uint64_t pte=walk(bufs[i]->iova+off);
        assert((pte&0xfffffffff000ull)==wl_phys(bufs[i]->va+off));
        assert((pte&0x60000000000fffull)==0x60000000000f63ull);
    }
    assert(!walk(WL_IOVA-4096) && !walk(WL_IOVA_END) && !walk(wl_tables.iova));
}
static int findwrite(uint64_t pa, uint32_t v, unsigned from) {
    for(unsigned i=from;i<ne;i++) if(events[i].kind=='W' && events[i].pa==pa && events[i].v==v) return i;
    return -1;
}
int main(void) {
    /* End-to-end fake device: ELF, image/table bounds, context ABI, DMA ordering. */
    init();
    for(unsigned s=17;s<=28;s++) assert(wl5_step(s)==0);
    check_mapping();
    assert(strstr(wl_fw_path,"amss20.bin"));
    assert(wl_fw_size==sizeof(fixture) && wl_image.pa>0x100000000ull && wl_image.iova==WL_IOVA);
    struct wl_vector *v=(void *)wl_vectors.va;
    assert(v[0].size==WL_CHUNK && v[1].size==37 && v[1].addr==wl_image.iova+WL_CHUNK);
    assert(wl5_report[19]==32);
    struct wl_context *e=(void *)(wl_contexts.va+WL_ECTX);
    assert(e[0].len==512 && e[1].len==4096 && e[1].base==wl_contexts.iova+WL_ER1);
    for(unsigned i=0;i<128*sizeof(struct wl_context);i++) assert(((uint8_t *)wl_contexts.va)[i]==0);
    int bme=findwrite(WL_EPCFG+4,0x406,0);
    int bhi=findwrite(WL_BAR0+0x118,0x12345,0);
    assert(bme>0 && bhi>bme);
    bool cleaned=false; for(int i=0;i<bme;i++) if(events[i].kind=='C') cleaned=true; assert(cleaned);
    assert(findwrite(WL_BAR0+0x10c,0,0)<findwrite(WL_BAR0+0x108,wl_image.iova,0));
    assert(findwrite(WL_BAR0+0x64,0,0)<findwrite(WL_BAR0+0x60,wl_contexts.iova+WL_ECTX,0));
    assert(findwrite(WL_BAR0+0x38,0x200,0)>findwrite(WL_BAR0+0x700,wl_contexts.iova+WL_ER0+496,0));
    wl5_status(28);
    assert(wl5_report[64]==2 && wl5_report[65]==0x200);
    /* Cleanup reset -> BME off -> PERST -> free, even with latched faults. */
    unsigned start=ne; fw_faults=1; wl5_abort(28);
    int rst=findwrite(WL_BAR0+0x38,2,start), off=findwrite(WL_EPCFG+4,0x402,start);
    int perst=findwrite(TS_TLMM+0x1000*94+4,0,start);
    assert(rst>=0 && off>rst && perst>off && frees==4);
    int invalid=findwrite(WL_SMMU+0x800,0x1401,start);
    int restore=findwrite(WL_SMMU+0xc00,0x20000,start);
    int disable=findwrite(0x15081000,0,start);
    int tlbi=findwrite(0x15081610,1,start);
    int sync=findwrite(0x150817f0,0,start);
    assert(invalid>perst && restore>invalid && disable>restore && tlbi>disable && sync>tlbi);
    assert(wl_prd(WL_PARF+0x2054)==0 && wl_prd(WL_SMMU+0x800)==0);
    for(unsigned i=start;i<ne;i++) if(events[i].kind=='F') assert(i>(unsigned)sync);
    assert(!wl_image.va && fw_faults==1);
    for(unsigned i=start;i<ne;i++) if(events[i].kind=='F') assert(i>(unsigned)perst);

    /* Proven vacancies; existing valid streams and active/referenced CBs survive. */
    init(); setreg(WL_SMMU+0x800,0x80001200); setreg(WL_SMMU+0xc00,2);
    setreg(0x15081000,1); /* CB1 active, CB2 referenced: select CB3 */
    assert(wl5_step(17)==0 && wl_slot==1 && wl_cb==3);
    for(unsigned s=18;s<=20;s++) assert(wl5_step(s)==0);
    assert(wl_prd(WL_SMMU+0x800)==0x80001200 && wl_prd(0x15081000)==1);
    wl5_abort(20); assert(frees==4);
    init(); setreg(WL_SMMU+0x800,0x80001401); assert(wl5_step(17)==2 && ne==0);
    init(); setreg(WL_SMMU+0x800,0x80011400); assert(wl5_step(17)==2 && ne==0);
    init(); for(unsigned i=0;i<4;i++) setreg(WL_SMMU+0x800+4*i,0x80001000+i);
    assert(wl5_step(17)==3 && ne==0);
    init(); for(unsigned i=0;i<70;i++) setreg(0x15080000+4096*i,1);
    assert(wl5_step(17)==3 && ne==0);
    init(); setreg(WL_SMMU,1); assert(wl5_step(17)==1 && ne==0);
    init(); setreg(WL_SMMU,8); assert(wl5_step(17)==1 && ne==0);
    init(); setreg(WL_SMMU+0x28,0); assert(wl5_step(17)==1 && ne==0);
    init(); setreg(WL_PARF+0x234,1); assert(wl5_step(17)==5 && ne==0);
    init(); setreg(WL_PARF+0x2054,0x01000200); assert(wl5_step(17)==5 && ne==0);
    init(); setreg(WL_PARF+0x2054,0x01000100); assert(wl5_step(17)==0 && ne==0);
    /* Changed ownership, readback mismatch, timeout and latched DMA fault. */
    init(); for(unsigned s=17;s<20;s++) assert(wl5_step(s)==0);
    setreg(WL_SMMU+0x800,0x80001111); assert(wl5_step(20)==3); wl5_abort(20);
    init(); for(unsigned s=17;s<20;s++) assert(wl5_step(s)==0);
    mismatch=true; assert(wl5_step(20)==4 && !wl_enabled); mismatch=false; wl5_abort(20); assert(frees==4);
    init(); for(unsigned s=17;s<20;s++) assert(wl5_step(s)==0);
    tlb_busy=true; setreg(0x150817f4,1); assert(wl5_step(20)==5 && !wl_enabled);
    wl5_abort(20); assert(frees==0); tlb_busy=false; setreg(0x150817f4,0); wl5_abort(20); assert(frees==4);
    init(); for(unsigned s=17;s<=20;s++) assert(wl5_step(s)==0);
    inject_fault=true; assert(wl5_step(21)==0); assert(wl_smmu_fault(21)==1 && wl_poll(0)==3);
    assert(wl5_report[100]==2); wl5_abort(21); assert(frees==4);

    const uint64_t mismatch_regs[]={0x15001804,0x15081010,0x15081030,0x15081024,
        0x15081020,0x1508102c,0x15081038,0x15081000,WL_SMMU+0xc00,WL_SMMU+0x800};
    for(unsigned i=0;i<sizeof(mismatch_regs)/sizeof(*mismatch_regs);i++) {
        init(); for(unsigned s=17;s<20;s++) assert(wl5_step(s)==0);
        mismatch=true; mismatch_pa=mismatch_regs[i];
        assert(wl5_step(20)!=0 && !wl_enabled && !(wl_prd(WL_EPCFG+4)&4));
        mismatch=false; wl5_abort(20); assert(frees==4);
    }
    /* A fault-contained MMIO abort latches normal writes off; owned undo still runs. */
    init(); for(unsigned s=17;s<20;s++) assert(wl5_step(s)==0);
    read_fault_pa=0x15081030; assert(wl5_step(20)==4 && fw_faults);
    read_fault_pa=0; unsigned latched=fw_faults; wl5_abort(20);
    assert(frees==4 && fw_faults==latched);

    /* Shared global USF is read-only; a fault naming our SID blocks DMA. */
    init(); for(unsigned s=17;s<=20;s++) assert(wl5_step(s)==0);
    setreg(WL_SMMU+0x48,2); setreg(WL_SMMU+0x54,0x1401);
    assert(wl5_step(21)==6 && !(wl_prd(WL_EPCFG+4)&4)); wl5_abort(21); assert(frees==4);
    assert(wl_prd(WL_SMMU+0x48)==2);

    /* High noncontiguous XNU pages all translate correctly; no adjacent maps. */
    init(); noncontig=true; for(unsigned s=17;s<=20;s++) assert(wl5_step(s)==0);
    check_mapping(); wl5_abort(20); assert(frees==4);

    /* Every hardware poll terminates, error EEs are never success. */
    init(); assert(wl5_step(17)==0);
    assert(wl_poll(0)==2 && sleeps==5000);
    setreg(WL_BAR0+0x128,3); assert(wl_poll(0)==1);
    setreg(WL_BAR0+0x128,6); assert(wl_poll(4)==1);
    setreg(WL_BAR0+0x128,0); setreg(WL_BAR0+0x48,4); assert(wl_poll(1)==1);
    setreg(WL_BAR0+0x48,0); wl_seq=9;
    setreg(WL_BAR0+0x268,(2u<<30)|8); assert(wl_poll(3)==2);
    setreg(WL_BAR0+0x268,(2u<<30)|9); assert(wl_poll(3)==0);

    setreg(WL_BAR0+0x268,3u<<30); assert(wl_poll(3)==1);

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
    high_memory=false; noncontig=true; assert(wl5_step(19)==0); release();
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
    fail_bm=fail_perst=false; wl5_abort(21); assert(frees==4);
    puts("M6 fake-MMIO/VFS/DMA sequence and failure tests passed");
    return 0;
}
'''


def main():
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    patch = (ROOT / 'xnu/patches/0099-fuxi-wlan.patch').read_text()
    added = patch.split('+++ b/bsd/dev/fuxi_wlan.c\n', 1)[1].split('\n--- a/', 1)[0]
    reconstructed = '\n'.join(line[1:] for line in added.splitlines() if line.startswith('+')) + '\n'
    assert reconstructed == driver, 'integration patch and standalone WLAN C differ'
    block = driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    block = '/* ---- milestone 5:' + block
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* host memory barrier */;', block)
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6-') as tmp:
        cfile, binary = Path(tmp) / 'm6.c', Path(tmp) / 'm6'
        cfile.write_text(PREAMBLE + block + TESTS)
        subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    # Packing must fail before reading firmware when any required path is absent/relative.
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-pack-') as tmp:
        payload = Path(tmp) / 'payload'
        payload.write_bytes(b'fixture')
        required = ['MKBOOTIMG', 'FUXI_GMU_FIRMWARE', 'FUXI_SQE_FIRMWARE', 'FUXI_GPU_ZAP_MBN']
        env = dict(os.environ, CLANG='unused', LLD='unused', OBJCOPY='unused', READELF='unused')
        env.update({k: str(payload) for k in required})
        for key in required:
            missing = dict(env); missing.pop(key, None)
            result = subprocess.run(['bash', str(ROOT / 'shim/fuxi-loader/build.sh'), str(payload)],
                                    env=missing, capture_output=True, text=True)
            assert result.returncode and key in result.stderr
        for key in required:
            relative = dict(env); relative[key] = 'relative/input'
            result = subprocess.run(['bash', str(ROOT / 'shim/fuxi-loader/build.sh'), str(payload)],
                                    env=relative, capture_output=True, text=True)
            assert result.returncode and 'absolute file paths' in result.stderr
    print('M6 packing rejects unset/relative required inputs')
    spec = importlib.util.spec_from_file_location('wlan_probe', ROOT / 'tools/fuxi-wlan-probe.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-log-') as tmp:
        log = Path(tmp) / 'console.log'
        r = [0] * 128
        r[0:4] = [0x574c0006, 28, 28, 0]
        for step in range(17, 29):
            r[32 + step] = 0x1000 | step
        r[16], r[64], r[65] = 0x201, 2, 0x200
        log.write_text('\n'.join('prefix FUXI WLAN: m6 r[%3d] ' % i +
                                  ' '.join(f'{v:08x}' for v in r[i:i+8]) for i in range(0,128,8)))
        reports, seen = probe.from_xnu_log(log)
        assert seen == {'m6'} and reports['m6'] == r
        decoded = probe.decode_m6(r)
        assert 'step 28 OK' in decoded and 'amss20.bin' in decoded and 'state=M0' in decoded
        failed = [0] * 128
        failed[2], failed[49] = 17, (17 << 8) | 1
        assert 'FAIL reason 1' in probe.decode_m6(failed)
        r[0], r[2], r[3], r[49] = 0x574c0005, 17, 0x1103, 0x1103
        r[11] = r[15] = 2
        assert 'SID 0x1401 type=FAULT' in probe.decode_m5(r)
        print('M6 offline log reconstruction/decoder and historical M5 decoder passed')


if __name__ == '__main__':
    main()
