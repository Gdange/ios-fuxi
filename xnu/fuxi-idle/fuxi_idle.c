/* FUXI cooling diagnostics (patch 0105). Off unless a boot-arg asks.
 *
 * fuxicpuload=N  every N seconds (1..3600) one console line with each CPU's
 *                busy share (wall time minus recount idle time, interrupts
 *                count as busy), user/system share, and its interrupt, IPI
 *                and timer-interrupt rates per second (cpu_stat counters).
 *                Used to attribute idle heat (docs-lanes/cooling.md).
 *                Three more lines follow it: received IPIs by signal kind
 *                (ipirx), WFI idle entries and the idle L1 flush cost
 *                (idleflush), and which task asked the scheduler for IPIs to
 *                which task's threads (ipiwake). See fuxi_idle_logic.h.
 *
 * fuxinoidleflush=1  cpu_idle() skips platform_cache_idle_enter(), the
 *                set/way clean+invalidate of the L1 D-cache before every WFI.
 *                SWEET is not __ARM_COHERENT_IO__, so the stock non-coherent
 *                variant runs on each idle entry of each CPU. With WFI-only
 *                idle (no PSCI CPU_SUSPEND, the core keeps its L1 and answers
 *                snoops) the clusters are hardware coherent, so the flush only
 *                costs time and cache refills. Unset: unchanged.
 *
 * Ported from HackiOS hackios-xnu-kernel xnu/files/bsd/dev/redwood_cpufreq.c
 * (cpuload=MS, load_thread) and xnu/redwood.patch ml_cpu_idle_time() (APSL 2.0).
 * SPDX-License-Identifier: APSL-2.0
 * Changes: seconds instead of milliseconds, a blocking wait with 100 ms timer
 * leeway so the logger does not add its own wakeups, per-CPU irq/ipi/timer
 * rates and user/system split, printf instead of the loader log; the IPI,
 * idle-flush and wake-attribution lines and fuxinoidleflush are new.
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
#include <kern/task.h>
#include <kern/thread.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "fuxi_idle_logic.h"

_Static_assert(FI_SIGP_DEC == SIGPdec && FI_SIGP_XCALL == SIGPxcall && FI_SIGP_AST == SIGPast &&
    FI_SIGP_XCALL_IMM == SIGPxcallImm && FI_SIGP_DISABLED == SIGPdisabled, "cpu_internal.h SIGP bits");
_Static_assert(SCHED_IPI_IMMEDIATE == 1 && SCHED_IPI_IDLE == 2 && SCHED_IPI_DEFERRED == 3, "sched_ipi_type_t");

extern void proc_name(int pid, char *buf, int size);   /* bsd/kern/kern_proc.c */

/* Hooks (patch 0105): osfmk/arm64/cpu.c, osfmk/arm/cpu_common.c, osfmk/arm64/sleh.c, osfmk/kern/sched_prim.c */
void fuxi_idle_cache_enter(void);
void fuxi_idle_ipi_seen(unsigned int signal);
void fuxi_idle_sgi(unsigned int intid);
void fuxi_idle_ipi_wake(thread_t thread, int ipi_type);

static unsigned fi_seconds;
static unsigned fi_ncpu;
static int fi_noflush;
static struct fi_sample fi_prev[FI_MAX_CPUS], fi_cur[FI_MAX_CPUS];

/*
 * Written only by their own CPU with interrupts masked. The logger reads them
 * without locks: a count can be off by the events of the copy itself.
 * The wake table and types restart whenever the CPU sees a new fi_epoch.
 */
struct fi_xcpu {
	struct fi_xcount n;
	uint32_t epoch;
	uint64_t types[3];              /* SCHED_IPI_IMMEDIATE, _IDLE, _DEFERRED */
	uint64_t lost;                  /* wake pairs that did not fit */
	struct fi_wake wake[FI_WAKE_SLOTS];
} __attribute__((aligned(128)));
static struct fi_xcpu fi_x[FI_MAX_CPUS];
static uint32_t fi_epoch;
static struct fi_xcount fi_xprev[FI_MAX_CPUS], fi_xcur[FI_MAX_CPUS];
static struct fi_wake fi_merge[FI_WAKE_MERGE];
static char fi_names[2 * FI_WAKE_TOP][FI_NAME];

static inline struct fi_xcpu *
fi_here(void)
{
	unsigned c = (unsigned)cpu_number();
	return c < FI_MAX_CPUS ? &fi_x[c] : NULL;
}

/* cpu_idle(), interrupts masked, just before WFI. */
void
fuxi_idle_cache_enter(void)
{
	struct fi_xcpu *x = fi_seconds ? fi_here() : NULL;
	uint64_t start;

	if (x) {
		x->n.idles++;
	}
	if (fi_noflush) {
		return;
	}
	if (!x) {
		platform_cache_idle_enter();
		return;
	}
	start = mach_absolute_time();
	platform_cache_idle_enter();
	x->n.flush += mach_absolute_time() - start;
}

/* cpu_signal_handler_internal(), interrupts masked: the pending bits of one IPI. */
void
fuxi_idle_ipi_seen(unsigned int signal)
{
	struct fi_xcpu *x = fi_seconds ? fi_here() : NULL;

	if (x) {
		fi_classify(&x->n, signal);
	}
}

/* sleh_irq(), interrupts masked: an SGI that is not SWEET_IPI_SGI. */
void
fuxi_idle_sgi(unsigned int intid __unused)
{
	struct fi_xcpu *x = fi_seconds ? fi_here() : NULL;

	if (x) {
		x->n.sgi++;
	}
}

/* sched_ipi_action() decided to signal another CPU for thread (may be NULL). */
void
fuxi_idle_ipi_wake(thread_t thread, int ipi_type)
{
	if (!fi_seconds || ipi_type < SCHED_IPI_IMMEDIATE || ipi_type > SCHED_IPI_DEFERRED) {
		return;
	}
	boolean_t istate = ml_set_interrupts_enabled(FALSE);
	struct fi_xcpu *x = fi_here();
	if (x) {
		uint32_t epoch = os_atomic_load(&fi_epoch, relaxed);
		if (x->epoch != epoch) {
			memset(x->types, 0, sizeof(x->types));
			memset(x->wake, 0, sizeof(x->wake));
			x->lost = 0;
			os_atomic_store(&x->epoch, epoch, release);
		}
		int32_t waker = task_pid(current_task());
		int32_t woken = thread != THREAD_NULL ? task_pid(get_threadtask(thread)) : -1;
		x->types[ipi_type - 1]++;
		if (!fi_wake_add(x->wake, FI_WAKE_SLOTS, waker, woken, 1)) {
			x->lost++;
		}
	}
	ml_set_interrupts_enabled(istate);
}

static void
fi_sample_all(struct fi_sample *s, struct fi_xcount *xs)
{
	for (unsigned c = 0; c < fi_ncpu; c++) {
		processor_t p = processor_array[c];
		cpu_data_t *cd = CpuDataEntries[c].cpu_data_vaddr;
		xs[c] = fi_x[c].n;
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

/* Merge every CPU's wake table of the current epoch, then start the next one. */
static uint64_t
fi_collect_wake(uint64_t *types)
{
	uint32_t epoch = os_atomic_load(&fi_epoch, relaxed);
	uint64_t lost = 0;

	memset(fi_merge, 0, sizeof(fi_merge));
	types[0] = types[1] = types[2] = 0;
	for (unsigned c = 0; c < fi_ncpu; c++) {
		struct fi_xcpu *x = &fi_x[c];
		if (os_atomic_load(&x->epoch, acquire) != epoch) {
			continue;       /* sent no scheduler IPI this interval */
		}
		for (unsigned t = 0; t < 3; t++) {
			types[t] += x->types[t];
		}
		lost += x->lost;
		for (unsigned i = 0; i < FI_WAKE_SLOTS; i++) {
			struct fi_wake w = x->wake[i];
			if (w.count && !fi_wake_add(fi_merge, FI_WAKE_MERGE, w.waker, w.woken, w.count)) {
				lost += w.count;
			}
		}
	}
	os_atomic_store(&fi_epoch, epoch + 1, relaxed);
	return lost;
}

static void
fi_task_name(int32_t pid, char *name)
{
	name[0] = 0;
	if (pid == 0) {
		strlcpy(name, "kernel_task", FI_NAME);
	} else if (pid > 0) {
		proc_name(pid, name, FI_NAME);
	}
}

static void
fi_thread(void *arg __unused, wait_result_t wr __unused)
{
	static char line[640];
	static int valid[FI_MAX_CPUS];
	uint64_t then = mach_absolute_time();

	fi_sample_all(fi_prev, fi_xprev);
	(void)fi_collect_wake((uint64_t[3]){ 0 });
	for (;;) {
		uint64_t now, wall_ns, up_ns, types[3], lost;
		unsigned top[FI_WAKE_TOP], ntop;

		assert_wait_timeout_with_leeway((event_t)&fi_seconds, THREAD_UNINT,
		    TIMEOUT_URGENCY_SYS_NORMAL | TIMEOUT_URGENCY_LEEWAY, fi_seconds * 1000, 100, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
		now = mach_absolute_time();
		fi_sample_all(fi_cur, fi_xcur);
		lost = fi_collect_wake(types);
		absolutetime_to_nanoseconds(now - then, &wall_ns);
		absolutetime_to_nanoseconds(now, &up_ns);
		(void)fi_format(line, sizeof(line), fi_seconds, up_ns / NSEC_PER_SEC, now - then,
		    wall_ns / NSEC_PER_MSEC, fi_prev, fi_cur, fi_ncpu);
		printf("%s\n", line);

		for (unsigned c = 0; c < fi_ncpu; c++) {
			valid[c] = fi_prev[c].valid && fi_cur[c].valid;
		}
		(void)fi_format_ipirx(line, sizeof(line), fi_seconds, wall_ns / NSEC_PER_MSEC,
		    fi_xprev, fi_xcur, valid, fi_ncpu);
		printf("%s\n", line);
		(void)fi_format_flush(line, sizeof(line), fi_seconds, !fi_noflush, now - then, wall_ns,
		    fi_xprev, fi_xcur, valid, fi_ncpu);
		printf("%s\n", line);

		ntop = fi_wake_top(fi_merge, FI_WAKE_MERGE, top, FI_WAKE_TOP);
		for (unsigned i = 0; i < ntop; i++) {
			fi_task_name(fi_merge[top[i]].waker, fi_names[2 * i]);
			fi_task_name(fi_merge[top[i]].woken, fi_names[2 * i + 1]);
		}
		(void)fi_format_wake(line, sizeof(line), fi_seconds, wall_ns / NSEC_PER_MSEC, types,
		    fi_merge, FI_WAKE_MERGE, top, ntop, fi_names, lost);
		printf("%s\n", line);

		memcpy(fi_prev, fi_cur, sizeof(fi_prev));
		memcpy(fi_xprev, fi_xcur, sizeof(fi_xprev));
		then = now;
	}
}

static void
fuxi_idle_init(void)
{
	unsigned seconds = 0, noflush = 0;
	thread_t thread;
	int max;

	if (PE_parse_boot_argn("fuxinoidleflush", &noflush, sizeof(noflush)) && noflush == 1) {
		fi_noflush = 1;
		printf("FUXI IDLE: idle L1 flush skipped (fuxinoidleflush=1)\n");
	}
	if (!PE_parse_boot_argn("fuxicpuload", &seconds, sizeof(seconds)) || !fi_period(seconds)) {
		return;
	}
	max = ml_get_max_cpu_number();
	fi_ncpu = max < 0 ? 1 : (unsigned)max + 1;
	if (fi_ncpu > FI_MAX_CPUS) {
		fi_ncpu = FI_MAX_CPUS;
	}
	fi_seconds = fi_period(seconds);        /* also arms the IPI and idle hooks */
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
