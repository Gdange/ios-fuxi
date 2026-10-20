/* M6 platform adapter. gpu.c/gpu-a740.h/gpu-zap.h are exact shared probe
 * sources, copied into patch 0096 by tools/sync-fuxi-adreno-shared.py.
 * Fixed memory is excluded from boot_args AND ADT RAM before VM bootstrap.
 * All mappings, firmware, power votes and failure records survive until reset.
 */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include "fuxi_adreno.h"
#include "gpu-handoff.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern kern_return_t thread_terminate(thread_t);
/* Fault-contained aligned 32/64-bit access in machine_routines_asm.s. */
extern unsigned fuxi_gpu_access(uintptr_t, uint64_t *, unsigned, unsigned);
#define rsr(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
#define delay fg_delay
static void fg_delay(unsigned us) { delay_for_interval(us, NSEC_PER_USEC); }
struct fg_window { uint64_t pa, size; uintptr_t va; };
/* Disjoint apertures; the framebuffer is mapped by SweetFramebuffer only.
 * Stage14 uses scratch, never stage15's panel readback/CPU comparator. */
static struct fg_window fg_windows[] = {
 {0x100000,0x80000,0}, {0x3d00000,0x40000,0},
 {0x3d50000,0x40000,0}, {0x3d90000,0x10000,0},
 {0x3da0000,0x40000,0},
 {0xb280000,0x8000,0}, {0x81c60000,0x20000,0},
 {FG_SCRATCH,FG_SCRATCH_SIZE,0}, {0x9b098000,0x4000,0},
 {0xae04000,0x4000,0},
};
static unsigned fg_access_failed;
uint32_t probe_fault_esr;
uint64_t probe_fault_far;
static uintptr_t fg_pointer(uint64_t pa)
{
 for (unsigned i = 0; i < sizeof(fg_windows)/sizeof(fg_windows[0]); i++) {
  struct fg_window *w = &fg_windows[i];
  if (pa >= w->pa && pa-w->pa < w->size && w->va) return w->va + pa-w->pa;
 }
 return 0;
}
static unsigned fg_access(uint64_t pa, uint64_t *v, unsigned wr, unsigned wide)
{
 uintptr_t va = fg_pointer(pa);
 if (!va || (pa & (wide ? 7 : 3))) return fg_access_failed = 1;
 unsigned bad = fuxi_gpu_access(va, v, wr, wide);
 if (bad) {
  probe_fault_esr = rsr("esr_el1"); probe_fault_far = pa;
  if (!fg_access_failed) printf("FUXI GPU: access fault PA %#llx op %u ESR %#x\n", pa, wr, probe_fault_esr);
  fg_access_failed = 1;
 }
 return bad;
}
uint32_t probe_safe_rd32(uint64_t pa, uint32_t *faulted)
{
 uint64_t v = 0;
 *faulted = fg_access(pa, &v, 0, 0);
 return (uint32_t)v;
}
unsigned probe_safe_wr32(uint64_t pa, uint32_t value)
{
 uint64_t v = value;
 /* After an access fault, allow diagnostic reads and explicit bus isolation
  * only. No continuation of a partly programmed bring-up sequence. */
 if (fg_access_failed) return 1;
 return fg_access(pa, &v, 1, 0);
}
static void fg_store64(uint64_t pa, uint64_t value)
{
 if (!fg_access_failed) (void)fg_access(pa, &value, 1, 1);
}
static const uint8_t *fg_firmware(unsigned which, unsigned *size)
{
 const struct fg_handoff *h = (const struct fg_handoff *)fg_pointer(FG_HANDOFF);
 *size = h->fw_size[which];
 return (const uint8_t *)fg_pointer(FG_FW_BASE + which*FG_FW_SLOT);
}
#define FUXI_GPU_XNU 1
#define G_PTR(pa) fg_pointer(pa)
#define G_RING_INDEX(w) ((w) & 8191u)
#include "gpu.c"

#define FG_SOURCE_VA 0x50000000u
#define FG_SOURCE_BYTES 0x01000000u
#define FG_SOURCE_TABLE (G3_PT + 0x10000)
/* Eight fixed L3 pages, each covers 2 MiB. Installed once; only leaves change
 * with the GPU idle. No per-frame table allocation or leaked old mappings. */
static volatile unsigned fg_state; /* 0 disabled/starting, 1 ready, 2 permanently failed */
static unsigned fg_started, fg_limit, fg_force, fg_seq;
int fuxi_gpu_ready(void) { return __atomic_load_n(&fg_state, __ATOMIC_ACQUIRE) == 1; }
static uint64_t fg_deadline;
static int fg_expired(void) { return rsr("cntpct_el0") >= fg_deadline; }
static void fg_fail(const char *reason, unsigned code)
{
 __atomic_store_n(&fg_state, 2, __ATOMIC_RELEASE);
 printf("FUXI GPU: disabled permanently: %s (%#x), CPU present\n", reason, code);
}
/* Linux6.12 a6xx_bus_clear_pending_transactions + sw_reset: stop client
 * requests, drain AXI arbiter, then hold reset. Unlike recovery, never clear
 * halt or restart. Mask SID0/1 to FAULT as a second containment barrier.
 * If ACK is missing, keep the source prepared/retained rather than freeing
 * pages still reachable by DMA; CPU fallback resumes with isolation asserted. */
static int fg_isolate(void)
{
 uint64_t v = 1;
 int good = !fg_access(GPU_REG_BASE + 0x3c45*4, &v, 1, 0);
 for (unsigned step = 1; step <= 2; step++) {
  unsigned i;
  for (i = 0; i < 1000; i++) {
   v = 0;
   if (fg_access(GPU_REG_BASE + 0x3c46*4, &v, 0, 0)) { good = 0; break; }
   if (v & step) break;
   delay(1);
  }
  if (i == 1000) good = 0;
  if (step == 1) {
   v = 2;
   if (fg_access(GPU_REG_BASE + 0x3c45*4, &v, 1, 0)) good = 0;
  }
 }
 v = 1; (void)fg_access(GPU_REG_BASE + 0x43*4, &v, 1, 0);
 delay(100);
 for (unsigned i = 0; i < 2; i++) {
  v = 2u << 16; (void)fg_access(GPU_SMMU_BASE + SMMU_S2CR(g3_slot[i]), &v, 1, 0);
 }
 __asm__ volatile("dsb sy" ::: "memory");
 printf("FUXI GPU: GBIF drain %s, reset held, SID0/1 faulted%s\n",
     good ? "ACK" : "FAILED", good ? "" : "; source quarantined until reset");
 return good;
}
static int fg_source_tables(void)
{
 volatile uint64_t *root = (volatile uint64_t *)G_PTR(G3_PT);
 volatile uint64_t *l2 = (volatile uint64_t *)G_PTR(root[1] & ~4095ULL);
 if (!l2 || g3_next > 16) return 0;
 for (unsigned i = 0; i < 8; i++) {
  unsigned slot = ((FG_SOURCE_VA >> 21) & 511) + i;
  if (l2[slot]) return 0;
  l2[slot] = (FG_SOURCE_TABLE + i*4096) | 3;
 }
 g_zero(FG_SOURCE_TABLE, 8*4096);
 return 1;
}
static void fg_thread(void *arg, wait_result_t result)
{
 (void)arg; (void)result;
 /* Scheduling is after AppleCLCD registration; sleep off the boot path. */
 uint64_t deadline;
 clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
 (void)assert_wait_deadline(&fg_started, THREAD_UNINT, deadline);
 thread_block(THREAD_CONTINUE_NULL);
 DTEntry chosen; void *prop; unsigned bytes;
 if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
     SecureDTGetProperty(chosen, "fuxi-gpu-memory", &prop, &bytes) != kSuccess ||
     bytes != 16 || ((uint64_t *)prop)[0] != FG_SCRATCH || ((uint64_t *)prop)[1] != FG_SCRATCH_SIZE) {
  fg_fail("missing loader/ADT reservation (use --gpu-reserve)", 1); goto out;
 }
 /* Verify actual VM bootstrap bound as well as the DT declaration. */
 extern vm_offset_t gPhysBase; extern vm_size_t gPhysSize;
 if (gPhysBase != FG_RAM_BASE || gPhysSize > FG_RAM_SIZE) {
  fg_fail("scratch is inside XNU RAM", 2); goto out;
 }
 for (unsigned i = 0; i < sizeof(fg_windows)/sizeof(fg_windows[0]); i++) {
  struct fg_window *w = &fg_windows[i];
  w->va = ml_io_map(w->pa, w->size);
  if (!w->va) { fg_fail("uncached mapping failed", i); goto out; }
 }
 const struct fg_handoff *h = (const struct fg_handoff *)G_PTR(FG_HANDOFF);
 if (h->magic != FG_MAGIC || h->version != 1 || h->bytes != sizeof(*h) || h->reserved ||
     h->base != FG_SCRATCH || h->size != FG_SCRATCH_SIZE) {
  fg_fail("missing/invalid cold firmware handoff", 3); goto out;
 }
 for (unsigned i = 0; i < 3; i++) {
  unsigned n = h->fw_size[i];
  if (!n || n > FG_FW_SLOT || (n & 3) ||
      fg_hash((const uint8_t *)G_PTR(FG_FW_BASE+i*FG_FW_SLOT), n) != h->fw_hash[i]) {
   fg_fail("firmware size/checksum", i); goto out;
  }
 }
 printf("FUXI GPU: cold bring-up, 32 MiB reserved NC, limit %u\n", fg_limit);
 gpu_probe(fg_limit == 13 ? 13 : 16);
 if (fg_access_failed || gpu_extra[5][4] != 1 ||
     (fg_limit != 13 && gpu_extra[13][2] != 1)) {
  printf("FUXI GPU: stage %u results clock=%#x GMU=%#x HFI=%#x GX=%#x "
      "CB=%#x CP=%#x fence=%#x zap=%#x clear/copy=%#x\n", gpu_stage,
      gpu_report[65], gpu_extra[0][2], gpu_extra[0][3], gpu_extra[0][60],
      gpu_extra[5][2], gpu_extra[5][3], gpu_extra[5][4], gpu_extra[13][2], gpu_extra[11][2]);
  fg_fail("bring-up/self-test", gpu_stage); goto out;
 }
 printf("FUXI GPU: through stage %u PASS, GMU CB%u GPU CB%u ASID%u, "
     "fence=%#x TRUST=%#x zap=%#x pixels=%#x\n", fg_limit == 13 ? 13 : 16,
     g_bank, g3_bank, g3_asid, g3_read(G3_FENCE_PA), g3_gr(0xf400),
     gpu_extra[13][2], gpu_extra[11][2]);
 if (fg_limit < 17 || fg_force == 1) { fg_fail("diagnostic stop after self-test", fg_limit); goto out; }
 if (g3_read(0xae05014) != G4_FB_PA || g3_read(0xae05024) != 4320 ||
     g3_read(0xae05000) != (2400u<<16 | 1080u) || g3_read(0xae05030) != 0x237ff ||
     !g_map_at(G3_PT, &g3_next, G4_FB_VA, G4_FB_PA, (4320*2400+4095)&~4095u, 1u<<6) ||
     !fg_source_tables()) { fg_fail("scanout geometry/mapping", 4); goto out; }
 uint32_t tlb[128] = {0};
 if (g4_tlb(tlb) || fg_access_failed) { fg_fail("initial TLB sync", 5); goto out; }
 printf("FUXI GPU: ready, BGRA row blits + fence, 220 MHz, polling\n");
 __atomic_store_n(&fg_state, 1, __ATOMIC_RELEASE);
 out: (void)thread_terminate(current_thread());
}
void fuxi_gpu_start(uint64_t fb, unsigned width, unsigned height, unsigned pitch)
{
 unsigned enable = 0;
 (void)PE_parse_boot_argn("fuxigpu", &enable, sizeof(enable));
 if (enable != 1 || fg_started) return;
 fg_started = 1; fg_limit = 17;
 (void)PE_parse_boot_argn("fuxigpu_stage", &fg_limit, sizeof(fg_limit));
 (void)PE_parse_boot_argn("fuxigpu_fail", &fg_force, sizeof(fg_force));
 if ((fg_limit != 13 && fg_limit != 16 && fg_limit != 17) ||
     fb != G4_FB_PA || width != 1080 || height != 2400 || pitch != 4320) {
  fg_fail("unsupported boot framebuffer/stage", fg_limit); return;
 }
 thread_t thread;
 if (kernel_thread_start(fg_thread, NULL, &thread) != KERN_SUCCESS) fg_fail("thread start", 6);
 else thread_deallocate(thread);
}

/* Validated one-row variant of g4_blit: neither surface pitch is rounded down.
 * Each row has its own 64-byte base and X phase, including arbitrary crop X.
 * Nearest inclusive coordinates, BGRA/WXYZ, no blending/scaling/rotation. */
static void fg_row(unsigned src, unsigned dst, unsigned width)
{
 unsigned sx = (src & 63)/4, dx = (dst & 63)/4;
 g3_emit(g4_pkt4(G4_SP_SRC_INFO, 5));
 g3_emit(0x00500430); g3_emit((width+sx) | 1u<<15);
 g3_emit(src & ~63u); g3_emit(0); g3_emit(((width+sx)*4+63)/64 << 9);
 g3_emit(g4_pkt4(G4_RB_DST_INFO, 4));
 g3_emit(0x430); g3_emit(dst & ~63u); g3_emit(0); g3_emit(((width+dx)*4+63)/64);
 g3_emit(g4_pkt4(G4_GRAS_SRC_X, 6));
 g3_emit(sx<<8); g3_emit((sx+width-1)<<8); g3_emit(0); g3_emit(0);
 g3_emit(dx); g3_emit(dx+width-1);
 g4_event(63); g3_emit(g3_pkt7(0x2c, 1)); g3_emit(3); g4_wfi();
}
int fuxi_gpu_copy(uintptr_t source, uint64_t length, unsigned pitch,
    unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned width, unsigned height)
{
 if (__atomic_load_n(&fg_state, __ATOMIC_ACQUIRE) != 1) return 0;
 if (!width || !height || width > 1080 || height > 2400 || dx > 1080-width || dy > 2400-height ||
     (source & 3) || (pitch & 3) || ((uint64_t)sx+width)*4 > pitch ||
     length > FG_SOURCE_BYTES || ((uint64_t)sy+height-1)*pitch+((uint64_t)sx+width)*4 > length)
  return 0;
 /* A single display workloop serializes callers. Its retained, prepared
  * descriptor owns every source page until fence+idle and unmap+TLBSYNC. */
 unsigned baseoff = source & 4095;
 if (length + baseoff > FG_SOURCE_BYTES) return 0;
 volatile uint64_t *leaves = (volatile uint64_t *)G_PTR(FG_SOURCE_TABLE);
 g_zero(FG_SOURCE_TABLE, 8*4096);
 for (unsigned y = 0; y < height; y++) {
  uint64_t begin = baseoff + (uint64_t)(sy+y)*pitch + sx*4;
  uint64_t end = begin + width*4;
  for (uint64_t off = begin & ~4095ULL; off < end; off += 4096) {
   uintptr_t va = (source & ~(uintptr_t)4095) + off;
   ppnum_t ppn = pmap_find_phys(kernel_pmap, va);
   if (!ppn) { fg_fail("unmapped prepared source page", 7); return 0; }
   uint64_t pa = ((uint64_t)ppn << PAGE_SHIFT) | (va & PAGE_MASK);
   /* Source read-only/unprivileged, Normal NC, inner-shareable, AF. */
   leaves[off/4096] = pa | 3 | (3u<<6) | (3u<<8) | (1u<<10);
  }
 }
 uint64_t start = (uint64_t)sy*pitch+sx*4, span = (uint64_t)(height-1)*pitch+width*4;
 flush_dcache64(source+start, (unsigned)span, FALSE);
 uint32_t tlb[128] = {0};
 int err = g4_tlb(tlb);
 if (err) { fg_fail("source TLB sync", err); return 0; }
 /* One 50 ms wall deadline covers the ENTIRE frame, including all batches.
  * Each wait is also bounded by 5000 iterations in the shared probe helper. */
 fg_deadline = rsr("cntpct_el0") + rsr("cntfrq_el0")/20;
 unsigned submitted = 0;
 for (unsigned y = 0; y < height; y += 64) {
  if (fg_expired() || g3_faulted()) { err = 0x601; break; }
  unsigned rows = height-y < 64 ? height-y : 64;
  /* Previous fence retired the ring. At most 64*32+128 <8192 words:
   * modulo writes may cross the ring end but can never lap hardware. */
  g4_setup();
  for (unsigned j = 0; j < rows; j++) fg_row(FG_SOURCE_VA+baseoff+(sy+y+j)*pitch+sx*4,
      G4_FB_VA+(dy+y+j)*4320+dx*4, width);
  unsigned magic = ++fg_seq;
  if (!magic) magic = ++fg_seq;
  g4_end(G3_FENCE_VA+64, magic);
  g3_wptr &= 8191;
  __asm__ volatile("dsb sy" ::: "memory");
  submitted = 1;
  if (!g3_gw(0x807, g3_wptr)) { err = 0x602; break; }
  if (fg_force == 2) { err = 0x6ff; break; }
  unsigned polls;
  /* Same fault/fence/rptr/idle gates, tightened to the per-frame deadline. */
  for (polls = 0; polls < 5000; polls++) {
   if (fg_expired() || (!(polls & 15) && g3_faulted())) { err = 0x603; break; }
   if (g3_read(G3_FENCE_PA+64) == magic && g3_gr(0x806) == g3_wptr &&
       g3_gr(0x807) == g3_wptr && !(g3_gr(0x210) & ~1u)) break;
   delay(1);
  }
  if (err || polls == 5000 || fg_access_failed) { if (!err) err = 0x604; break; }
 }
 if (err) {
  fg_fail("present fence/fault/deadline", err);
  return submitted && !fg_isolate() ? -1 : 0;
 }
 /* Drop every transient source leaf only after all chunks have retired. */
 g_zero(FG_SOURCE_TABLE, 8*4096);
 err = g4_tlb(tlb);
 if (err || g3_faulted()) {
  fg_fail("retirement TLB/fault", err);
  return !fg_isolate() ? -1 : 0;
 }
 __asm__ volatile("dsb sy" ::: "memory");
 return 1;
}
#endif
