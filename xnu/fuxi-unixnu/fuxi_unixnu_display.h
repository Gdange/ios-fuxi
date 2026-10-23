// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ported from UniXNU drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp (BSD-2-Clause-Patent)
// See LICENSE.NativeMobileDisplay. Adapted demand-driven timer policy; no private ABI offsets.
#ifndef FUXI_UNIXNU_DISPLAY_H
#define FUXI_UNIXNU_DISPLAY_H
#include <stdint.h>

static inline int
fuxi_unixnu_tick_wanted(int off, int needsBlack, int dirty, int gpu, int notify)
{
	return gpu || notify || (off ? needsBlack : dirty);
}

static inline uint64_t
fuxi_unixnu_next_tick(uint64_t next, uint64_t period, uint64_t now)
{
	/* Skip missed periods; never queue a catch-up storm after a slow copy. */
	next += period;
	return next <= now ? now + period : next;
}
#endif
