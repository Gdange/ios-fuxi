/* Built into fuxi_adreno.c, with access to its proven SMMU/ring helpers.
 * Single privileged diagnostic client, not a security boundary for PM4.
 * Its root has no scanout or surface-cache mappings. The kernel-only runtime
 * pages below va_start are identical in both roots; only idle TTBR switches
 * are allowed. Client leaves never modify the present root.
 */
#define FG_ABI_PT (G_MEM+0x1c00000)
#define FG_ABI_PT_BYTES 0x400000u
#define FG_ABI_FENCE_OFF 128u
_Static_assert(FG_ABI_PT >= G4_DST_PA+G4_BYTES+4096, "ABI table overlap");
_Static_assert(FG_ABI_PT >= G5_META_PA+G5_META_BYTES, "ABI/zap metadata overlap");
_Static_assert(FG_ABI_PT >= FG_FW_BASE+3*FG_FW_SLOT, "ABI/cold handoff or firmware overlap");
_Static_assert(FG_ABI_PT+FG_ABI_PT_BYTES <= G_MEM+G_MEM_SIZE, "ABI tables exceed scratch");
static unsigned fg_abi_live, fg_abi_seq, fg_abi_completed, fg_abi_poisoned, fg_abi_timed_out;
static int fg_abi_drained = 1;
static uint64_t fg_client_deadline;
static thread_call_data_t fg_client_watchdog;
static volatile uint64_t *fg_abi_leaf(uint64_t iova)
{
 unsigned chunk = (unsigned)((iova-FG_ABI_VA_START)>>21);
 return (volatile uint64_t *)G_PTR(FG_ABI_PT+(4u+chunk)*4096)+((iova>>12)&511);
}
static int fg_abi_switch(uint64_t root)
{
 /* Caller proves ring and both threads idle. Invalidate the common ASID
  * before permitting another doorbell; preserved ring/SQE PTEs are equal. */
 unsigned cb = g_cb(g3_bank);
 uint64_t ttbr = root | ((uint64_t)g3_asid<<48);
 __asm__ volatile("dsb sy" ::: "memory");
 if (!g3_sw(cb+0x20,(uint32_t)ttbr) || !g3_sw(cb+0x24,ttbr>>32)) return 0;
 uint32_t tlb[128] = {0};
 return !g4_tlb(tlb) && !fg_access_failed &&
     g3_sr(cb+0x20)==(uint32_t)ttbr && g3_sr(cb+0x24)==(uint32_t)(ttbr>>32);
}
static int fg_abi_fault(const char *why)
{
 fg_abi_poisoned = 1;
 fg_fail(why,0x701);
 fg_abi_drained = fg_isolate();
 fg_client_busy = 0;
 return fg_abi_drained ? -1 : -2;
}
/* No spinning: one observation. The watchdog runs even if the client exits,
 * never waits, or asks WaitFence(timeout_ns=0) indefinitely. */
static int fg_abi_observe(void)
{
 if (fg_abi_poisoned) return fg_abi_timed_out ? (fg_abi_drained ? -3 : -4) : (fg_abi_drained ? -1 : -2);
 if (!fg_client_busy) return 0;
 if (fg_access_failed || g3_faulted()) return fg_abi_fault("client GPU fault");
 if (g3_read(G3_FENCE_PA+FG_ABI_FENCE_OFF+4)==fg_abi_seq &&
     g3_gr(0x806)==g3_wptr && g3_gr(0x807)==g3_wptr && !(g3_gr(0x210)&~1u)) {
  if (!fg_abi_switch(G3_PT)) return fg_abi_fault("client restore TTBR");
  fg_abi_completed = fg_abi_seq;
  fg_client_busy = 0;
  return 0;
 }
 if (rsr("cntpct_el0") >= fg_client_deadline) {
  fg_abi_timed_out=1; (void)fg_abi_fault("client fence deadline (100 ms)");
  return fg_abi_drained ? -3 : -4;
 }
 return 1;
}
static void fg_abi_watch(thread_call_param_t arg, thread_call_param_t unused)
{
 (void)arg; (void)unused;
 FG_GUARD;
 if (fg_abi_observe()==1) {
  uint64_t deadline;
  clock_interval_to_deadline(2,NSEC_PER_MSEC,&deadline);
  (void)thread_call_enter_delayed(&fg_client_watchdog,deadline);
 }
}
static int fg_abi_init(void)
{
 /* 772 4K pages: root, two L2s, kernel runtime L3, 768 client L3s.
  * Allocate all table storage from unused reserved NC scratch, never VM.
  * No existing GMU or present PTE is reused as mutable client storage. */
 /* Verify programmed UCHE/GMEM/HBB facts before advertising DeviceInfo. */
 if (g3_gr(0xe07)!=0xfffff000u || g3_gr(0xe08)!=0x1ffff ||
     g3_gr(0xe0b)!=0x1000000 || g3_gr(0xe0c) ||
     g3_gr(0xe0d)!=0x12fffff || g3_gr(0xe0e) ||
     ((g3_gr(0xe01)>>21)&3)!=3 ||
     ((g3_sr(SMMU_ID2)>>4)&15)<1 || fg_access_failed) return 0;
 /* SM8550 UBWC4 configuration, same NC values as the proven present path;
  * initialize both BR and BV banks before exposing a context to Turnip. */
 if (!g3_gw(G4_RB_NC,0x816) || !g3_gw(G4_TPL1_NC,6) || !g3_gw(G4_SP_NC,0x26)) return 0;
 for (unsigned pipe=1;pipe<=2;pipe++)
  if (!g3_gw(G3_APERTURE_HOST,pipe<<12) || !g3_gw(G4_GRAS_NC,0x60)) return 0;
 if (!g3_gw(G3_APERTURE_HOST,0)) return 0;
 g_zero(FG_ABI_PT,FG_ABI_PT_BYTES);
 volatile uint64_t *root = (volatile uint64_t *)G_PTR(FG_ABI_PT);
 volatile uint64_t *l20 = (volatile uint64_t *)G_PTR(FG_ABI_PT+4096);
 volatile uint64_t *l21 = (volatile uint64_t *)G_PTR(FG_ABI_PT+8192);
 volatile uint64_t *runtime = (volatile uint64_t *)G_PTR(FG_ABI_PT+12288);
 root[0]=(FG_ABI_PT+4096)|3; root[1]=(FG_ABI_PT+8192)|3;
 l20[G3_SQE_VA>>21]=(FG_ABI_PT+12288)|3;
 for (unsigned i=0;i<0x2a000/4096;i++) {
  uint64_t pa = G3_SQE_PA+(uint64_t)i*4096;
  unsigned ap = i<0x20 ? 3u<<6 : i==0x28 ? 0 : 1u<<6;
  runtime[i]=pa|3|(3u<<8)|(1u<<10)|ap;
 }
 for (unsigned i=0;i<768;i++) {
  uint64_t va=FG_ABI_VA_START+(uint64_t)i*0x200000;
  volatile uint64_t *l2=va>>30 ? l21 : l20;
  l2[(va>>21)&511]=(FG_ABI_PT+(4u+i)*4096)|3;
 }
 g_wr(G3_FENCE_PA,FG_ABI_FENCE_OFF+4,0);
 g_wr(G3_FENCE_PA,FG_ABI_FENCE_OFF+12,0);
 __asm__ volatile("dsb sy" ::: "memory");
 thread_call_setup_with_options(&fg_client_watchdog,fg_abi_watch,NULL,
     THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
 fg_abi_live=1;
 return 1;
}
int fuxi_gpu_abi_available(void)
{
 FG_GUARD;
 return fg_abi_live && fuxi_gpu_ready() && !fg_abi_poisoned;
}
int fuxi_gpu_abi_map(uint64_t iova, uintptr_t source, uint64_t bytes,
    unsigned permissions, unsigned wb, unsigned sh)
{
 FG_GUARD;
 if (!fg_abi_live || !fuxi_gpu_ready() || fg_abi_poisoned) return -1;
 if (fg_client_busy) return 1;
 if (!fg_abi_map_valid(iova,bytes,permissions,wb,sh,0) || !source || (source&4095)) return -1;
 /* Validate the entire range before publishing any leaf. 16K XNU pages
  * provide four distinct 4K SMMU pages, including noncontiguous BOs. */
 for (uint64_t off=0;off<bytes;off+=4096) {
  ppnum_t ppn=pmap_find_phys(kernel_pmap,source+off);
  if (!ppn || ((uint64_t)ppn<<PAGE_SHIFT)>=(1ULL<<FG_ABI_PA_BITS) || *fg_abi_leaf(iova+off)) return -1;
 }
 for (uint64_t off=0;off<bytes;off+=4096) {
  uint64_t pa=((uint64_t)pmap_find_phys(kernel_pmap,source+off)<<PAGE_SHIFT)|((source+off)&PAGE_MASK);
  *fg_abi_leaf(iova+off)=pa|3|(permissions==3 ? 1u<<6 : 3u<<6)|(wb ? 1u<<2 : 0)|sh<<8|1u<<10;
 }
 __asm__ volatile("dsb sy" ::: "memory");
 /* Root is inactive; TLBI is mandatory before its next doorbell. */
 return 0;
}
int fuxi_gpu_abi_unmap(uint64_t iova, uint64_t bytes)
{
 FG_GUARD;
 if (fg_abi_poisoned) return fg_abi_drained ? 0 : -2;
 if (!fg_abi_live || !fg_abi_range(iova,bytes)) return -1;
 if (fg_client_busy) return 1;
 for (uint64_t off=0;off<bytes;off+=4096) *fg_abi_leaf(iova+off)=0;
 __asm__ volatile("dsb sy" ::: "memory");
 /* Remove stale ASID translations before backing pages can be released.
  * Safe with an in-flight present: only the inactive root was changed. */
 uint32_t tlb[128]={0};
 if (g4_tlb(tlb) || fg_access_failed) return fg_abi_fault("client unmap TLB sync");
 return 0;
}
int fuxi_gpu_abi_submit(const uint64_t *iovas, const uint64_t *sizes,
    unsigned count, uint64_t *fence)
{
 FG_GUARD;
 if (!fg_abi_live || !fuxi_gpu_ready() || fg_abi_poisoned || fg_abi_seq==UINT32_MAX) return -1;
 if (fg_busy || fg_client_busy) return 1;
 if (g3_faulted() || g3_gr(0x806)!=g3_wptr || g3_gr(0x807)!=g3_wptr || (g3_gr(0x210)&~1u))
  return fg_abi_fault("client pre-submit idle/fault");
 uint32_t words[160];
 unsigned n=fg_abi_frame(words,iovas,sizes,count,G3_FENCE_VA+FG_ABI_FENCE_OFF,fg_abi_seq,fg_abi_seq+1,1);
 if (!n) return -1;
 if (!fg_abi_switch(FG_ABI_PT)) return fg_abi_fault("client install TTBR");
 /* NC BO mappings on both CPUs and GPU; this also publishes user stores.
  * Prologue flush+invalidate handles prior GPU cache contents. */
 __asm__ volatile("dsb sy" ::: "memory");
 for (unsigned i=0;i<n;i++) g3_emit(words[i]);
 g3_wptr &= 8191;
 __asm__ volatile("dsb sy" ::: "memory");
 fg_client_deadline=rsr("cntpct_el0")+rsr("cntfrq_el0")/10;
 fg_client_busy=1;
 if (!g3_gw(0x807,g3_wptr) || fg_access_failed) return fg_abi_fault("client doorbell");
 *fence=++fg_abi_seq;
 uint64_t deadline;
 clock_interval_to_deadline(2,NSEC_PER_MSEC,&deadline);
 (void)thread_call_enter_delayed(&fg_client_watchdog,deadline);
 return 0;
}
int fuxi_gpu_abi_poll(uint64_t fence)
{
 FG_GUARD;
 if (!fg_abi_live || fence>fg_abi_seq) return -1;
 int err=fg_abi_observe();
 if (err<0) return err;
 return fence<=fg_abi_completed ? 0 : 1;
}
