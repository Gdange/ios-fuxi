/* FUXI cooling diagnostics (patch 0105). Off unless a boot-arg asks.
 *
 * fuxicpuload=N  every N seconds (1..3600) one console line with each CPU's
 *                busy share (wall time minus recount idle time, interrupts
 *                count as busy), user/system share, and its interrupt, IPI
 *                and timer-interrupt rates per second (cpu_stat counters).
 *                Used to attribute idle heat (docs-lanes/cooling.md).
 *
 * Ported from HackiOS hackios-xnu-kernel xnu/files/bsd/dev/redwood_cpufreq.c
 * (cpuload=MS, load_thread) and xnu/redwood.patch ml_cpu_idle_time() (APSL 2.0).
 * SPDX-License-Identifier: APSL-2.0
 * Changes: seconds instead of milliseconds, a blocking wait with 100 ms timer
 * leeway so the logger does not add its own wakeups, per-CPU irq/ipi/timer
 * rates and user/system split, printf instead of the loader log.
 */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <stdint.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/kern_types.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/sched_prim.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <arm/cpu_data_internal.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "fuxi_idle_logic.h"

static unsigned fi_seconds;
static unsigned fi_ncpu;
static struct fi_sample fi_prev[FI_MAX_CPUS], fi_cur[FI_MAX_CPUS];

static void
fi_sample_all(struct fi_sample *s)
{
	for (unsigned c = 0; c < fi_ncpu; c++) {
		processor_t p = processor_array[c];
		cpu_data_t *cd = CpuDataEntries[c].cpu_data_vaddr;
		if (p == PROCESSOR_NULL || cd == NULL) {
			s[c].valid = 0;
			continue;
		}
		struct recount_usage usage = { 0 };
		uint64_t idle = 0;
		recount_processor_usage(&p->pr_recount, &usage, &idle);
		s[c].idle = idle;
		s[c].user = usage.ru_user_time_mach;
		s[c].sys = usage.ru_system_time_mach;
		s[c].irq = cd->cpu_stat.irq_ex_cnt;
		s[c].ipi = cd->cpu_stat.ipi_cnt;
		s[c].tmr = cd->cpu_stat.timer_cnt;
		s[c].valid = 1;
	}
}

static void
fi_thread(void *arg __unused, wait_result_t wr __unused)
{
	static char line[640];
	uint64_t then = mach_absolute_time();

	fi_sample_all(fi_prev);
	for (;;) {
		uint64_t now, wall_ns, up_ns;

		assert_wait_timeout_with_leeway((event_t)&fi_seconds, THREAD_UNINT,
		    TIMEOUT_URGENCY_SYS_NORMAL | TIMEOUT_URGENCY_LEEWAY, fi_seconds * 1000, 100, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
		now = mach_absolute_time();
		fi_sample_all(fi_cur);
		absolutetime_to_nanoseconds(now - then, &wall_ns);
		absolutetime_to_nanoseconds(now, &up_ns);
		(void)fi_format(line, sizeof(line), fi_seconds, up_ns / NSEC_PER_SEC, now - then,
		    wall_ns / NSEC_PER_MSEC, fi_prev, fi_cur, fi_ncpu);
		printf("%s\n", line);
		memcpy(fi_prev, fi_cur, sizeof(fi_prev));
		then = now;
	}
}

static void
fuxi_idle_init(void)
{
	unsigned seconds = 0;
	thread_t thread;
	int max;

	if (!PE_parse_boot_argn("fuxicpuload", &seconds, sizeof(seconds)) || !fi_period(seconds)) {
		return;
	}
	fi_seconds = fi_period(seconds);
	max = ml_get_max_cpu_number();
	fi_ncpu = max < 0 ? 1 : (unsigned)max + 1;
	if (fi_ncpu > FI_MAX_CPUS) {
		fi_ncpu = FI_MAX_CPUS;
	}
	if (kernel_thread_start(fi_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("FUXI IDLE: cpuload thread NOT started\n");
		return;
	}
	thread_set_thread_name(thread, "fuxi_cpuload");
	thread_deallocate(thread);
	printf("FUXI IDLE: cpuload every %us over %u CPUs (fuxicpuload=%u)\n", fi_seconds, fi_ncpu, seconds);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, fuxi_idle_init);
#endif /* FUXI */
