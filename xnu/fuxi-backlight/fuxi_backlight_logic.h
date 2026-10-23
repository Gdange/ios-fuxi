/* Observation-only backlight policy; no panel units or hardware access. */
#ifndef FUXI_BACKLIGHT_LOGIC_H
#define FUXI_BACKLIGHT_LOGIC_H
#include <stdint.h>
#define FUXI_BACKLIGHT_MAX 65536u
static inline int fbl_enabled(int present, int value)
{
	return present && value == 1;
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
