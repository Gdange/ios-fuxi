/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 NativeMetal contributors
 * Ported from UniXNU drivers/NativeMetal/GpuScanoutClient.h (MIT).
 * The CPU pixel oracle is adapted below; Fuxi display ownership is original.
 * Finished-frame copy/fence design: NativeMetal/GpuScanoutPresenter.h (MIT).
 * See LICENSE.NativeMetal.
 */
/* GPU-SCANOUT: Fuxi command-mode display ownership. Original implementation.
 * No panel/DSI reset, address flip, clock change or firmware dependency.
 * Register meanings already used by the board's inherited-display probe:
 * INTF1 AUTOREFRESH_CONFIG +0x2b4 bit31; INT_COUNT_VAL +0x2a8 low16
 * write line; CTL START +0x1c bit0. Do not copy while a transfer can read RAM.
 */
#define FG_SCAN_CTL 0x0ae16000u
#define FG_SCAN_INTF 0x0ae36000u
#define FG_SCAN_DRAIN_US 20000u
static unsigned fg_scanout_owned, fg_scanout_failed, fg_scanout_writable;
static unsigned fg_scanout_reserved, fg_scanout_checks, fg_scanout_checked;
static uintptr_t fg_scanout_pixels, fg_scanout_source;
static unsigned fg_scanout_pitch, fg_scanout_sx, fg_scanout_sy;
static unsigned fg_scanout_dx, fg_scanout_dy, fg_scanout_w, fg_scanout_h;
static uint64_t fg_scanout_after, fg_scanout_timeout;
static int fg_abi_observe(void);

void fuxi_gpu_scanout_configure(uintptr_t pixels, unsigned enabled, unsigned check)
{
 /* AppleCLCD calls once, before fuxi_gpu_start publishes the GPU. */
 fg_scanout_pixels = pixels;
 fg_scanout_requested = enabled == 1;
 fg_scanout_checks = fg_scanout_requested && check == 1 ? 4 : 0;
 if (fg_scanout_requested)
  printf("FUXI SCANOUT: requested, fence then manual transfer, drain_us=20000 check=%u\n", fg_scanout_checks);
}
int fuxi_gpu_scanout_owned(void)
{
 return __atomic_load_n(&fg_scanout_owned, __ATOMIC_ACQUIRE);
}
static int fg_scanout_read(unsigned pa, unsigned *value)
{
 uint64_t v = 0;
 if (fg_access(pa, &v, 0, 0)) return 0;
 *value = (unsigned)v;
 return 1;
}
static int fg_scanout_write(unsigned pa, unsigned value)
{
 uint64_t v = value;
 return !fg_access(pa, &v, 1, 0);
}
static int fg_scanout_stop(const char *reason)
{
 if (!fg_scanout_failed)
  printf("FUXI SCANOUT: stopped reason=%s; panel held, no framebuffer writes\n", reason);
 fg_scanout_failed = 1;
 fg_scanout_writable = 0;
 return -1;
}
/* 1 writable, 0 wait, -1 held after error. No spinning or sleeping. First
 * disable AR, then allow a complete 60 Hz interval plus margin before testing
 * the write-line and CTL idle indicators. The same guard follows every START.
 * This deliberately conservative policy is not a TE/vblank interrupt driver.
 */
int fuxi_gpu_scanout_acquire(void)
{
 FG_GUARD;
 if (!fg_scanout_requested) return 1;
 if (fg_scanout_failed) return -1;
 if (!fuxi_gpu_scanout_owned()) {
  if (!fuxi_gpu_ready()) return 1; /* startup/unsupported GPU keeps old path */
  unsigned ar;
  if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar)) return fg_scanout_stop("read autorefresh");
  /* Block all generic START callers before changing display ownership. */
  __atomic_store_n(&fg_scanout_owned, 1, __ATOMIC_RELEASE);
  if (!fg_scanout_write(FG_SCAN_INTF+0x2b4, ar & ~0x80000000u))
   return fg_scanout_stop("disable autorefresh");
  __asm__ volatile("dsb sy" ::: "memory");
  fg_scanout_after = rsr("cntpct_el0") + rsr("cntfrq_el0")*FG_SCAN_DRAIN_US/1000000;
  fg_scanout_timeout = fg_scanout_after + rsr("cntfrq_el0")/10;
  printf("FUXI SCANOUT: owned, autorefresh disabled; draining inherited transfer\n");
  return 0;
 }
 unsigned ar, line, start;
 if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar) || (ar & 0x80000000u))
  return fg_scanout_stop("autorefresh changed");
 if (fg_busy) return 0;
 if (rsr("cntpct_el0") < fg_scanout_after) return 0;
 if (!fg_scanout_read(FG_SCAN_INTF+0x2a8, &line) ||
     !fg_scanout_read(FG_SCAN_CTL+0x1c, &start)) return fg_scanout_stop("read transfer status");
 if ((start & 1) || ((line & 65535) != 0 && (line & 65535) < 2400)) {
  if (rsr("cntpct_el0") >= fg_scanout_timeout) return fg_scanout_stop("transfer drain timeout");
  return 0;
 }
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_writable = 1;
 return 1;
}
/* The caller has acquired scanout and finished ALL CPU writes or observed the
 * GPU copy's cache-clean fence. Generic console/display kicks are suppressed
 * while owned. Autorefresh stays off; only this function starts transfers.
 */
int fuxi_gpu_scanout_publish(void)
{
 if (!fuxi_gpu_scanout_owned()) return 0;
 FG_GUARD;
 if (fg_scanout_failed || fg_busy || !fg_scanout_writable) return -1;
 unsigned ar, start;
 if (!fg_scanout_read(FG_SCAN_INTF+0x2b4, &ar) || (ar & 0x80000000u) ||
     !fg_scanout_read(FG_SCAN_CTL+0x1c, &start) || (start & 1))
  return fg_scanout_stop("publish transfer status");
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_writable = 0;
 if (!fg_scanout_write(FG_SCAN_CTL+0x1c, 1)) return fg_scanout_stop("start transfer");
 __asm__ volatile("dsb sy" ::: "memory");
 fg_scanout_after = rsr("cntpct_el0") + rsr("cntfrq_el0")*FG_SCAN_DRAIN_US/1000000;
 fg_scanout_timeout = fg_scanout_after + rsr("cntfrq_el0")/10;
 return 1;
}
/* Reserve the idle ring across the surface-cache transaction. Client submit
 * returns busy until release; map/TLBI/copy cannot race a client TTBR switch.
 * Called under SweetSurface's lock, matching the existing surface -> GPU order.
 */
int fuxi_gpu_scanout_reserve(void)
{
 if (!fg_scanout_requested) return 1;
 FG_GUARD;
 if (!fuxi_gpu_ready()) return 1;
 if (fg_scanout_failed || fg_scanout_reserved || fg_busy) return 0;
 if (fg_abi_live_for_scanout() && fg_abi_observe() != 0) return 0;
 if (fg_client_busy) return 0;
 fg_scanout_reserved = 1;
 return 1;
}
void fuxi_gpu_scanout_release(void)
{
 if (!fg_scanout_requested) return;
 FG_GUARD;
 fg_scanout_reserved = 0;
}
/* Compare the whole copied rectangle to exactly the
 * CPU row-copy oracle, after the fence and before releasing the source pin.
 * Scalar aligned device loads only; no CPU framebuffer writes in this check.
 */
static int fg_scanout_verify(void)
{
 if (!fg_scanout_checks || fg_scanout_checked >= fg_scanout_checks) return 1;
 uint64_t began = mach_absolute_time(), ns;
 unsigned mismatch = 0, first_x = 0, first_y = 0;
 __asm__ volatile("dsb sy" ::: "memory");
 for (unsigned y = 0; y < fg_scanout_h; y++) {
  const uint32_t *src = (const uint32_t *)(fg_scanout_source +
      (uint64_t)(fg_scanout_sy+y)*fg_scanout_pitch + fg_scanout_sx*4);
  const volatile uint32_t *dst = (const volatile uint32_t *)(fg_scanout_pixels +
      (uint64_t)(fg_scanout_dy+y)*4320 + fg_scanout_dx*4);
  for (unsigned x = 0; x < fg_scanout_w; x++) if (src[x] != dst[x]) {
   if (!mismatch) { first_x = x; first_y = y; }
   mismatch++;
  }
 }
 absolutetime_to_nanoseconds(mach_absolute_time()-began, &ns);
 printf("FUXI SCANOUT: pixel frame=%u pixels=%u mismatches=%u first=%u,%u check_us=%llu\n",
     ++fg_scanout_checked, fg_scanout_w*fg_scanout_h, mismatch, first_x, first_y,
     (unsigned long long)(ns/1000));
 return !mismatch;
}
