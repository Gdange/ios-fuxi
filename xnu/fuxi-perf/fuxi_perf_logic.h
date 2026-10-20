/* Pure policy shared with offline tests. Frequencies always come from EPSS LUT. */
#ifndef FUXI_PERF_LOGIC_H
#define FUXI_PERF_LOGIC_H
static unsigned fp_khz(uint32_t row)
{
 return row >> 30 ? (row & 255u)*19200u : 300000u;
}
/* Exclude one-core turbo rows. Duplicate frequencies terminate the table.
 * Pick the fastest eligible row at/below cap; never infer level ordering. */
static int fp_level(const uint32_t *rows, unsigned cap, unsigned *khz)
{
 int best = -1;
 unsigned freq = 0, prev = 0;
 for (unsigned i = 0; i < 40; i++) {
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
#endif
