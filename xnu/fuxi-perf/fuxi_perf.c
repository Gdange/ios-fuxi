/* Opt-in touch boost for SM8550. One thread-call owns all EPSS/TSENS MMIO.
 * Input/swap hooks only publish timestamps and queue work; no MMIO or sleeps.
 * No voltage, PLL, thermal-trip or power-collapse registers are programmed.
 */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#define MACH_KERNEL_PRIVATE 1
#include <kern/thread_call.h>
#undef MACH_KERNEL_PRIVATE
#include "fuxi_perf_logic.h"
/* Same abort-contained access primitive as the built-in GPU driver; available
 * even when fuxigpu=0. Each register access is naturally aligned. */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern unsigned fuxi_gpu_access(uintptr_t, uint64_t *, unsigned, unsigned);
static uintptr_t fp_epss[3], fp_tsens[2];
static const uint64_t fp_epss_pa[3] = {0x17d91000,0x17d92000,0x17d93000};
static const unsigned fp_cores[3] = {3,4,1};
static const unsigned fp_cool_cap[3] = {1228800,1401600,1363200};
static unsigned fp_cool[3], fp_high[3], fp_cool_khz[3], fp_high_khz[3];
static unsigned fp_percore[3];
static uint64_t fp_input, fp_swap, fp_hold, fp_last, fp_window, fp_window_start;
static uint64_t fp_boost_time, fp_boosts;
static int fp_max_temp, fp_boosted;
static unsigned fp_enabled, fp_failed, fp_started, fp_ready;
static thread_call_data_t fp_call;
static int fp_read(uintptr_t base, unsigned off, uint32_t *value)
{
 uint64_t v = 0;
 if (fuxi_gpu_access(base+off, &v, 0, 0)) return 0;
 *value = (uint32_t)v;
 return 1;
}
static int fp_vote(unsigned d, unsigned level)
{
 for (unsigned c = 0; c < (fp_percore[d] ? fp_cores[d] : 1); c++) {
  uint64_t v = level;
  unsigned off = 0x320+4*c;
  if (fuxi_gpu_access(fp_epss[d]+off, &v, 1, 0)) return 0;
  uint32_t got;
  if (!fp_read(fp_epss[d], off, &got) || got != level) return 0;
 }
 return 1;
}
/* CPU sensors: tsens0 cpuss-0..3 and cpu-1-0..10 (1..15),
 * tsens1 cpu-0-0..2 (1..3). Exclude aoss and non-CPU sensors.
 * Every CPU sensor must be ready, valid and plausible; otherwise fail cool. */
static int fp_temperature(int *hottest)
{
 int hottest_cpu = -400;
 for (unsigned d = 0; d < 2; d++) {
  uint32_t ready;
  if (!fp_read(fp_tsens[d], 0xe4, &ready) || !(ready & 1)) return 0;
  for (unsigned n = 1; n <= (d ? 3 : 15); n++) {
   uint32_t status;
   if (!fp_read(fp_tsens[d], 0xa0+4*n, &status) || !(status & (1u<<21))) return 0;
   int temp = (int)(status & 0xfff);
   if (temp & 0x800) temp -= 4096;
   if (temp < -400 || temp > 1500) return 0;
   if (temp > hottest_cpu) hottest_cpu = temp;
  }
 }
 *hottest = hottest_cpu;
 return 1;
}
static int fp_initialize(void)
{
 for (unsigned d = 0; d < 3; d++) {
  fp_epss[d] = ml_io_map(fp_epss_pa[d], 0x1000);
  if (!fp_epss[d]) return 0;
  uint32_t enable, dcvs, rows[40];
  if (!fp_read(fp_epss[d],0,&enable) || !(enable & 1) ||
      !fp_read(fp_epss[d],0xb0,&dcvs)) return 0;
  fp_percore[d] = dcvs & 1;
  for (unsigned i = 0; i < 40; i++)
   if (!fp_read(fp_epss[d],0x100+4*i,&rows[i])) return 0;
  int cool = fp_level(rows, fp_cool_cap[d], &fp_cool_khz[d]);
  /* Highest multi-core LUT row is the high vote. Hardware owns CPR/LMh.
   * Silver stays cool: raising its shared rail also raises the monitor core. */
  int high = fp_level(rows, UINT32_MAX, &fp_high_khz[d]);
  if (cool < 0 || high < 0) return 0;
  fp_cool[d] = (unsigned)cool;
  fp_high[d] = d ? (unsigned)high : (unsigned)cool;
  if (!d) fp_high_khz[d] = fp_cool_khz[d];
 }
 fp_tsens[0] = ml_io_map(0xc271000,0x1000);
 fp_tsens[1] = ml_io_map(0xc272000,0x1000);
 if (!fp_tsens[0] || !fp_tsens[1]) return 0;
 printf("FUXI PERF: ready, LUT cool %u/%u/%u kHz high %u/%u/%u kHz, hold 1500 ms guard 60/68 C\n",
     fp_cool_khz[0],fp_cool_khz[1],fp_cool_khz[2],fp_high_khz[0],fp_high_khz[1],fp_high_khz[2]);
 return 1;
}
static void fp_worker(thread_call_param_t arg, thread_call_param_t result)
{
 (void)arg; (void)result;
 if (!fp_ready) {
  if (!fp_initialize()) {
   /* Initialization never raises votes. Restore every discovered domain to
    * its known cool level; leave undiscovered domains untouched. */
   for (unsigned d = 0; d < 3; d++) if (fp_cool_khz[d]) (void)fp_vote(d,fp_cool[d]);
   printf("FUXI PERF: disabled, EPSS/LUT mapping or access failed\n");
   __atomic_store_n(&fp_enabled,0,__ATOMIC_RELEASE);
   return;
  }
  fp_ready = 1;
 }
 uint64_t now = mach_absolute_time();
 if (fp_boosted && fp_last) fp_boost_time += now-fp_last;
 fp_last = now;
 int temp = -400;
 int valid = fp_temperature(&temp);
 if (valid && temp > fp_max_temp) fp_max_temp = temp;
 int boost = !fp_failed && fp_want_boost(fp_boosted,valid,temp,now,
     __atomic_load_n(&fp_input,__ATOMIC_ACQUIRE),
     __atomic_load_n(&fp_swap,__ATOMIC_ACQUIRE),fp_hold);
 /* First sample also establishes cool votes. No boost without fresh TSENS. */
 if (boost != fp_boosted || !fp_window_start) {
  int good = 1;
  for (unsigned d = 0; d < 3; d++)
   if (!fp_vote(d,boost ? fp_high[d] : fp_cool[d])) good = 0;
  if (!good) {
   fp_failed = 1;
   for (unsigned d = 0; d < 3; d++) (void)fp_vote(d,fp_cool[d]);
   boost = 0;
   printf("FUXI PERF: vote/readback failed, boosting disabled, cool requested\n");
  }
  if (boost && !fp_boosted) fp_boosts++;
  fp_boosted = boost;
 }
 if (!fp_window_start) fp_window_start = now;
 if (now-fp_window_start >= fp_window) {
  uint64_t ns;
  absolutetime_to_nanoseconds(fp_boost_time,&ns);
  printf("FUXI PERF: stats boosts %llu boosted %llu ms max temp %d dC valid %d state %s\n",
      fp_boosts,ns/1000000,fp_max_temp,valid,fp_boosted ? "high" : "cool");
  fp_boosts = fp_boost_time = 0;
  fp_max_temp = -400;
  fp_window_start = now;
 }
 uint64_t deadline;
 clock_interval_to_deadline(100,NSEC_PER_MSEC,&deadline);
 (void)thread_call_enter_delayed(&fp_call,deadline);
}
void fuxi_perf_start(void)
{
 unsigned enable = 0;
 (void)PE_parse_boot_argn("fuxiperf",&enable,sizeof(enable));
 if (enable != 1 || fp_started) return;
 fp_started = 1;
 nanoseconds_to_absolutetime(1500ULL*NSEC_PER_MSEC,&fp_hold);
 nanoseconds_to_absolutetime(10ULL*NSEC_PER_SEC,&fp_window);
 fp_max_temp = -400;
 thread_call_setup_with_options(&fp_call,fp_worker,NULL,
     THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
 __atomic_store_n(&fp_enabled,1,__ATOMIC_RELEASE);
 (void)thread_call_enter(&fp_call);
}
void fuxi_perf_input(void)
{
 if (!__atomic_load_n(&fp_enabled,__ATOMIC_ACQUIRE)) return;
 __atomic_store_n(&fp_input,mach_absolute_time(),__ATOMIC_RELEASE);
 (void)thread_call_enter(&fp_call);
}
void fuxi_perf_swap(void)
{
 if (__atomic_load_n(&fp_enabled,__ATOMIC_ACQUIRE))
  __atomic_store_n(&fp_swap,mach_absolute_time(),__ATOMIC_RELEASE);
}
#endif
