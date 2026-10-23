/* Observation-only backlight policy; no panel units or hardware access. */
#ifndef FUXI_BACKLIGHT_LOGIC_H
#define FUXI_BACKLIGHT_LOGIC_H
#include <stdint.h>
#define FUXI_BACKLIGHT_MAX 65536u
static inline int fbl_enabled(int present, int value)
{
	return present && (value == 1 || value == 2 || value == 3);
}
static inline int fbl_power_notifications(int present, int value)
{
	return present && (value == 2 || value == 3);
}
static inline int fbl_withhold_on(int present, int value, int on)
{
	/* Mode 3 isolates ON delivery; OFF and framebuffer ordering match mode 2. */
	return present && value == 3 && on;
}
static inline int fbl_startup_trace(int present, int value, int trace_present, int trace)
{
	/* Only the notification-free mode 1 is eligible for this experiment. */
	return present && value == 1 && trace_present && trace == 1;
}
static inline int fbl_read_log_this(uint32_t event, int trace)
{
	return event && (event <= (trace ? 512u : 64u) || (event & 255u) == 0);
}
static inline int fbl_log_this(uint32_t event)
{
	return event && (event <= 64 || (event & 255u) == 0);
}
static inline int fbl_valid(uint64_t value)
{
	return value <= FUXI_BACKLIGHT_MAX;
}
#endif
