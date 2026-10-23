/* Pure policy shared with offline tests. Frequencies always come from EPSS LUT. */
#ifndef FUXI_PERF_LOGIC_H
#define FUXI_PERF_LOGIC_H
static unsigned fp_khz(uint32_t row)
{
 return row >> 30 ? (row & 255u)*19200u : 300000u;
}
/* Exclude one-core turbo rows. Duplicate frequencies terminate the table.
 * Pick the fastest eligible row at/below cap; never infer level ordering. */
static int fp_level(const uint32_t *rows, unsigned count, unsigned cap, unsigned *khz)
{
 int best = -1;
 unsigned freq = 0, prev = 0;
 for (unsigned i = 0; i < count; i++) {
  unsigned f = fp_khz(rows[i]);
  if (i && f == prev) break;
  prev = f;
  if (((rows[i] >> 16) & 7u) == 1 || !f || f > cap) continue;
  if (best < 0 || f > freq) { best = i; freq = f; }
 }
 *khz = freq;
 return best;
}
static int fp_want_boost(int boosted, int valid, int temp,
    uint64_t now, uint64_t input, uint64_t swap, uint64_t hold)
{
 if (!valid || temp > 680 || (!boosted && temp > 600)) return 0;
 int recent_input = input && now >= input && now-input <= hold;
 uint64_t activity = input > swap ? input : swap;
 int recent_activity = activity && now >= activity && now-activity <= hold;
 return recent_activity && (boosted || recent_input);
}
/* K is a floor, bounded by cool so idle can never raise a domain's vote.
 * As with cool/high, skip turbo rows and do not assume level ordering. */
static int fp_idle_level(const uint32_t *rows, unsigned count,
    unsigned floor, unsigned cool, unsigned *khz)
{
 int best = -1;
 unsigned freq = 0, prev = 0;
 if (floor > cool) floor = cool;
 for (unsigned i = 0; i < count; i++) {
  unsigned f = fp_khz(rows[i]);
  if (i && f == prev) break;
  prev = f;
  if (((rows[i] >> 16) & 7u) == 1 || !f || f < floor || f > cool) continue;
  if (best < 0 || f < freq) { best = i; freq = f; }
 }
 *khz = freq;
 return best;
}
enum { FP_COOL, FP_HIGH, FP_IDLE };
static int fp_state(int boosted, int valid, int temp, int safe, int duty_ok,
    uint64_t now, uint64_t input, uint64_t swap, uint64_t hold,
    uint64_t idle_after, uint64_t start, uint64_t idle_entry)
{
 if (idle_after) {
  uint64_t activity = input > swap ? input : swap;
  if (activity < start) activity = start;
  /* Idle is below cool, so thermal cuts may also use it. The thermal and
   * duty guards still veto every high vote, including input wakeups. */
  if (now >= activity && now-activity >= idle_after) return FP_IDLE;
  /* An input preceding idle must not let a swap wake into high, even when
   * the idle timeout is shorter than the legacy boost hold. */
  if (input <= idle_entry) input = 0;
 }
 return safe && duty_ok && fp_want_boost(boosted,valid,temp,now,input,swap,hold)
     ? FP_HIGH : FP_COOL;
}
#endif
