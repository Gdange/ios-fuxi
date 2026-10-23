/* Pure helpers of osfmk/arm64/fuxi_idle.c (patch 0105), shared with the host
 * test tools/test-fuxi-idle.py. No kernel headers: integers in, text out.
 *
 * Ported from HackiOS hackios-xnu-kernel xnu/files/bsd/dev/redwood_cpufreq.c
 * (cpuload=MS: busy share = wall time minus idle time) (APSL 2.0).
 * SPDX-License-Identifier: APSL-2.0
 */
#ifndef FUXI_IDLE_LOGIC_H
#define FUXI_IDLE_LOGIC_H

#define FI_MAX_CPUS     8
#define FI_PERIOD_MAX   3600u   /* seconds */

/* One CPU at one instant. Times are mach_absolute_time() units. */
struct fi_sample {
	uint64_t idle, user, sys;       /* recount: idle time, user and system run time */
	uint64_t irq, ipi, tmr;         /* cpu_stat: irq_ex_cnt, ipi_cnt, timer_cnt */
	int valid;                      /* the processor was registered when sampled */
};

/* fuxicpuload=N: N seconds between lines, 0 = off, capped at an hour. */
static inline unsigned
fi_period(unsigned seconds)
{
	return seconds > FI_PERIOD_MAX ? FI_PERIOD_MAX : seconds;
}

static inline uint64_t
fi_delta(uint64_t now, uint64_t then)
{
	return now >= then ? now - then : 0;
}

/* Busy percent of a wall interval from the idle time in it (interrupts count as busy). */
static inline unsigned
fi_busy_pct(uint64_t wall, uint64_t idle)
{
	if (!wall) {
		return 0;
	}
	if (idle >= wall) {
		return 0;
	}
	return (unsigned)(((wall - idle) * 100 + wall / 2) / wall);
}

/* Share of the wall interval, in percent, of a run time (user or system). */
static inline unsigned
fi_share_pct(uint64_t wall, uint64_t part)
{
	if (!wall) {
		return 0;
	}
	if (part >= wall) {
		return 100;
	}
	return (unsigned)((part * 100 + wall / 2) / wall);
}

/* Events per second over wall_ms milliseconds. */
static inline uint64_t
fi_rate(uint64_t events, uint64_t wall_ms)
{
	if (!wall_ms) {
		return 0;
	}
	return (events * 1000 + wall_ms / 2) / wall_ms;
}

static inline void
fi_append(char *buf, unsigned size, unsigned *at, const char *text)
{
	while (*text && *at + 1 < size) {
		buf[(*at)++] = *text++;
	}
	buf[*at < size ? *at : size - 1] = 0;
}

static inline void
fi_append_u(char *buf, unsigned size, unsigned *at, uint64_t v)
{
	char digits[24];
	unsigned n = 0;
	do {
		digits[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < sizeof(digits));
	char out[24];
	for (unsigned i = 0; i < n; i++) {
		out[i] = digits[n - 1 - i];
	}
	out[n] = 0;
	fi_append(buf, size, at, out);
}

/*
 * One log line:
 *   FUXI IDLE: cpuload 10s up 600s avg 9% irq 5200 ipi 8400 tmr 3100 | c0 23% u5 s17 i812 p1934 t377 | c1 - | ...
 * avg is over the CPUs that were valid at both samples; irq/ipi/tmr totals are
 * per second. A CPU that came online between the samples prints "-".
 * Returns the line length (the buffer is always terminated).
 */
static inline unsigned
fi_format(char *buf, unsigned size, unsigned period, uint64_t up_s, uint64_t wall, uint64_t wall_ms,
    const struct fi_sample *prev, const struct fi_sample *cur, unsigned ncpu)
{
	unsigned at = 0, counted = 0, busy_sum = 0;
	uint64_t irq = 0, ipi = 0, tmr = 0;
	if (!size) {
		return 0;
	}
	buf[0] = 0;
	if (ncpu > FI_MAX_CPUS) {
		ncpu = FI_MAX_CPUS;
	}
	for (unsigned c = 0; c < ncpu; c++) {
		if (!prev[c].valid || !cur[c].valid) {
			continue;
		}
		busy_sum += fi_busy_pct(wall, fi_delta(cur[c].idle, prev[c].idle));
		irq += fi_delta(cur[c].irq, prev[c].irq);
		ipi += fi_delta(cur[c].ipi, prev[c].ipi);
		tmr += fi_delta(cur[c].tmr, prev[c].tmr);
		counted++;
	}
	fi_append(buf, size, &at, "FUXI IDLE: cpuload ");
	fi_append_u(buf, size, &at, period);
	fi_append(buf, size, &at, "s up ");
	fi_append_u(buf, size, &at, up_s);
	fi_append(buf, size, &at, "s avg ");
	fi_append_u(buf, size, &at, counted ? (busy_sum + counted / 2) / counted : 0);
	fi_append(buf, size, &at, "% irq ");
	fi_append_u(buf, size, &at, fi_rate(irq, wall_ms));
	fi_append(buf, size, &at, " ipi ");
	fi_append_u(buf, size, &at, fi_rate(ipi, wall_ms));
	fi_append(buf, size, &at, " tmr ");
	fi_append_u(buf, size, &at, fi_rate(tmr, wall_ms));
	for (unsigned c = 0; c < ncpu; c++) {
		fi_append(buf, size, &at, " | c");
		fi_append_u(buf, size, &at, c);
		if (!prev[c].valid || !cur[c].valid) {
			fi_append(buf, size, &at, " -");
			continue;
		}
		fi_append(buf, size, &at, " ");
		fi_append_u(buf, size, &at, fi_busy_pct(wall, fi_delta(cur[c].idle, prev[c].idle)));
		fi_append(buf, size, &at, "% u");
		fi_append_u(buf, size, &at, fi_share_pct(wall, fi_delta(cur[c].user, prev[c].user)));
		fi_append(buf, size, &at, " s");
		fi_append_u(buf, size, &at, fi_share_pct(wall, fi_delta(cur[c].sys, prev[c].sys)));
		fi_append(buf, size, &at, " i");
		fi_append_u(buf, size, &at, fi_rate(fi_delta(cur[c].irq, prev[c].irq), wall_ms));
		fi_append(buf, size, &at, " p");
		fi_append_u(buf, size, &at, fi_rate(fi_delta(cur[c].ipi, prev[c].ipi), wall_ms));
		fi_append(buf, size, &at, " t");
		fi_append_u(buf, size, &at, fi_rate(fi_delta(cur[c].tmr, prev[c].tmr), wall_ms));
	}
	return at;
}

/* fuxinosleep / fuxixpcid: a boot-arg value is on only when it is exactly 1. */
static inline int
fi_flag(int present, unsigned value)
{
	return present && value == 1;
}

/* Log the 1st, 2nd, 4th, 8th ... event of a repeating refusal. */
static inline int
fi_log_this(uint64_t count)
{
	return count && !(count & (count - 1));
}

#endif /* FUXI_IDLE_LOGIC_H */
