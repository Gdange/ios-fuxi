#!/usr/bin/env python3
"""Host checks for xnu/patches/0105-fuxi-idle.patch (cooling diagnostics).

The patch snapshots must equal xnu/fuxi-idle, every hook must be behind FUXI
and an exact boot-arg, and the pure line formatter is compiled and run here.
"""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = (ROOT/'xnu/patches/0105-fuxi-idle.patch').read_text()


def block(path):
    m = re.search(r'^--- a/' + re.escape(path) + r'\n.*?(?=^--- a/|\Z)', PATCH, re.M | re.S)
    assert m, path
    return m[0]


def added(path):
    return ''.join(l[1:] for l in block(path).splitlines(True) if l.startswith('+') and not l.startswith('+++'))


# New files are exact snapshots of the sources kept in the repo.
for name in ('fuxi_idle.c', 'fuxi_idle_logic.h'):
    assert added('osfmk/arm64/' + name) == (ROOT/'xnu/fuxi-idle'/name).read_text(), name
    assert 'Ported from HackiOS hackios-xnu-kernel' in added('osfmk/arm64/' + name)
    assert 'APSL' in added('osfmk/arm64/' + name)
assert added('osfmk/conf/files.arm64') == 'osfmk/arm64/fuxi_idle.c\tstandard\n'

# Default boot unchanged: each hook is FUXI-only and needs its boot-arg set to 1.
source = added('osfmk/arm64/fuxi_idle.c')
assert source.index('#if defined(FUXI)') < source.index('fuxi_idle_init')
assert 'PE_parse_boot_argn("fuxicpuload"' in source and '!fi_period(seconds)' in source
assert 'STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, fuxi_idle_init);' in source
for path, arg in (('iokit/Kernel/IOPMrootDomain.cpp', 'fuxinosleep'), ('bsd/kern/kern_exec.c', 'fuxixpcid')):
    text = added(path)
    assert text.startswith('#if defined(FUXI)\n'), path
    assert text.count('#if defined(FUXI)') == text.count('#endif'), path
    assert re.search(r'PE_parse_boot_argn\("%s", &\w+, sizeof\(\w+\)\) && \w+ == 1' % arg, text), path
    assert 'FUXI IDLE:' in text, path
exec_block = block('bsd/kern/kern_exec.c')
# The stock pidversion bump stays the only path when FUXI is off or the arg is unset.
assert exec_block.count('+\t\t\tproc_setpidversion(p, OSIncrementAtomic(&nextpidversion));') == 1
assert '+#else\n \tproc_setpidversion(p, OSIncrementAtomic(&nextpidversion));\n+#endif' in exec_block
assert 'strcmp(p->p_comm, "xpcproxy") == 0' in exec_block
pm = added('iokit/Kernel/IOPMrootDomain.cpp')
assert 'err = kPMConfigPreventSystemSleep;' in pm and 'gFuxiNoSleep = 1;' in pm
HOOKS = {  # upstream file -> the fuxi_idle.c function it calls
    'osfmk/arm64/cpu.c': 'fuxi_idle_cache_enter',
    'osfmk/arm/cpu_common.c': 'fuxi_idle_ipi_seen',
    'osfmk/arm64/sleh.c': 'fuxi_idle_sgi',
    'osfmk/kern/sched_prim.c': 'fuxi_idle_ipi_wake',
}
for path, fn in HOOKS.items():
    text = added(path)
    assert text.startswith('#if defined(FUXI)\n'), path
    assert text.count('#if defined(FUXI)') == text.count('#endif') == 2, path
    assert re.search(r'^extern void %s\(' % fn, text, re.M) and re.search(r'^\t+%s\(' % fn, text, re.M), path
    assert re.search(r'^void\n%s\(' % fn, source, re.M), fn
# The stock flush stays the non-FUXI path; FUXI calls it unless fuxinoidleflush=1.
assert ('+#if defined(FUXI)\n+\t\t/* Same flush unless fuxinoidleflush=1; fuxicpuload also times it. */\n'
        '+\t\tfuxi_idle_cache_enter();\n+#else\n \t\tplatform_cache_idle_enter();\n #endif\n+#endif\n'
        in block('osfmk/arm64/cpu.c'))
assert 'PE_parse_boot_argn("fuxinoidleflush", &noflush, sizeof(noflush)) && noflush == 1' in source
assert 'FUXI IDLE: idle L1 flush skipped (fuxinoidleflush=1)' in source
cache_enter = re.search(r'^fuxi_idle_cache_enter\(void\)\n\{\n(.*?)^\}', source, re.M | re.S)[1]
assert cache_enter.count('platform_cache_idle_enter();') == 2 and 'if (fi_noflush) {\n\t\treturn;' in cache_enter
# Diagnostic hooks do nothing until fuxicpuload armed fi_seconds.
for fn in ('fuxi_idle_ipi_seen', 'fuxi_idle_sgi'):
    body = re.search(r'^%s\(.*?\)\n\{\n(.*?)^\}' % fn, source, re.M | re.S)[1]
    assert 'fi_seconds ? fi_here() : NULL' in body, fn
assert 'if (!fi_seconds || ipi_type < SCHED_IPI_IMMEDIATE' in source
assert 'if (intid != SWEET_IPI_SGI) {' in block('osfmk/arm64/sleh.c')
for path in ('iokit/Kernel/IOPMrootDomain.cpp', 'bsd/kern/kern_exec.c', 'osfmk/conf/files.arm64', *HOOKS):
    assert not [l for l in block(path).splitlines()[2:] if l.startswith('-')], path  # additions only
# The patch is generated: tools/sync-fuxi-idle.py --check.
subprocess.run(['python3', str(ROOT/'tools/sync-fuxi-idle.py'), '--check'], check=True, capture_output=True)

TEST = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "fuxi_idle_logic.h"
#define MS 24000ull /* 24 MHz timebase ticks per ms */
int main(void)
{
	assert(fi_period(0) == 0 && fi_period(10) == 10 && fi_period(99999) == 3600);
	assert(fi_busy_pct(0, 5) == 0 && fi_busy_pct(100, 100) == 0 && fi_busy_pct(100, 250) == 0);
	assert(fi_busy_pct(1000, 770) == 23 && fi_busy_pct(1000, 0) == 100);
	assert(fi_share_pct(1000, 50) == 5 && fi_share_pct(1000, 5000) == 100 && fi_share_pct(0, 1) == 0);
	assert(fi_rate(18120, 10000) == 1812 && fi_rate(5, 0) == 0 && fi_rate(1, 3) == 333);
	assert(fi_delta(5, 7) == 0 && fi_delta(7, 5) == 2);
	assert(fi_flag(1, 1) && !fi_flag(1, 2) && !fi_flag(0, 1) && !fi_flag(1, 0));
	assert(!fi_log_this(0) && fi_log_this(1) && fi_log_this(2) && !fi_log_this(3) && fi_log_this(1024));

	struct fi_sample prev[FI_MAX_CPUS] = {0}, cur[FI_MAX_CPUS] = {0};
	uint64_t wall = 10000 * MS;
	prev[0] = (struct fi_sample){ 100, 7, 9, 1000, 2000, 3000, 1 };
	cur[0] = prev[0];
	cur[0].idle += 7700 * MS; cur[0].user += 500 * MS; cur[0].sys += 1700 * MS;
	cur[0].irq += 8120; cur[0].ipi += 19340; cur[0].tmr += 3770;
	prev[1] = (struct fi_sample){ 0, 0, 0, 0, 0, 0, 1 };
	cur[1] = prev[1];
	cur[1].idle += 9900 * MS; cur[1].irq += 50; cur[1].ipi += 60; cur[1].tmr += 70;
	cur[2].valid = 1; /* came online during the interval */
	char line[640];
	unsigned n = fi_format(line, sizeof(line), 10, 600, wall, 10000, prev, cur, 3);
	const char *want = "FUXI IDLE: cpuload 10s up 600s avg 12% irq 817 ipi 1940 tmr 384"
	    " | c0 23% u5 s17 i812 p1934 t377 | c1 1% u0 s0 i5 p6 t7 | c2 -";
	if (strcmp(line, want)) {
		printf("got  %s\nwant %s\n", line, want);
		return 1;
	}
	assert(n == strlen(want));

	/* Eight busy CPUs fit the kernel's 640-byte line; truncation stays terminated. */
	for (unsigned c = 0; c < FI_MAX_CPUS; c++) {
		prev[c] = (struct fi_sample){ 0, 0, 0, 0, 0, 0, 1 };
		cur[c] = (struct fi_sample){ 0, wall, wall, 99999999, 99999999, 99999999, 1 };
	}
	n = fi_format(line, sizeof(line), 3600, 99999999, wall, 10000, prev, cur, 12);
	assert(n == strlen(line) && n < sizeof(line) - 1 && strstr(line, "| c7 100% u100 s100"));
	char small[20];
	n = fi_format(small, sizeof(small), 10, 1, wall, 10000, prev, cur, 8);
	assert(n == sizeof(small) - 1 && strlen(small) == n && !strcmp(small, "FUXI IDLE: cpuload "));
	assert(fi_format(small, 0, 10, 1, wall, 10000, prev, cur, 8) == 0);

	/* cpu_signal bits -> kinds; SIGPdisabled alone is a plain kick. */
	struct fi_xcount k = {0};
	fi_classify(&k, 0); fi_classify(&k, FI_SIGP_DISABLED); fi_classify(&k, FI_SIGP_AST);
	fi_classify(&k, FI_SIGP_XCALL | FI_SIGP_AST); fi_classify(&k, FI_SIGP_XCALL_IMM);
	fi_classify(&k, FI_SIGP_DEC | FI_SIGP_DISABLED); fi_classify(&k, 0x10); fi_classify(&k, 0x100 | FI_SIGP_AST);
	assert(k.nop == 2 && k.ast == 3 && k.xc == 2 && k.dec == 1 && k.other == 2 && k.sgi == 0);

	struct fi_xcount xp[FI_MAX_CPUS] = {0}, xc[FI_MAX_CPUS] = {0};
	int valid[FI_MAX_CPUS] = { 1, 0, 1 };
	xc[0].nop = 29000; xc[0].ast = 410;
	xc[0].idles = 30070; xc[0].flush = 721680;     /* 1000 ns per flush at 24 MHz */
	xc[2] = (struct fi_xcount){ 10, 5, 20, 30, 40, 7, 0, 0 };
	char l2[640];
	n = fi_format_ipirx(l2, sizeof(l2), 10, 10000, xp, xc, valid, 3);
	want = "FUXI IDLE: ipirx 10s | c0 n2900 a41 x0 d0 o0 g0 | c1 - | c2 n1 a1 x2 d3 o4 g1";
	if (strcmp(l2, want)) {
		printf("got  %s\nwant %s\n", l2, want);
		return 1;
	}
	assert(n == strlen(want));
	n = fi_format_flush(l2, sizeof(l2), 10, 1, wall, 10000000000ull, xp, xc, valid, 3);
	want = "FUXI IDLE: idleflush 10s on | c0 n3007 a1000 m3 | c1 - | c2 n0 a0 m0";
	if (strcmp(l2, want)) {
		printf("got  %s\nwant %s\n", l2, want);
		return 1;
	}
	assert(n == strlen(want));
	(void)fi_format_flush(l2, sizeof(l2), 10, 0, wall, 10000000000ull, xp, xc, valid, 3);
	assert(!strcmp(l2, "FUXI IDLE: idleflush 10s off | c0 n3007 | c1 - | c2 n0"));
	assert(fi_ticks_ns(24000, 24000, 1000000) == 1000000 && fi_ticks_ns(5, 0, 7) == 0);
	assert(fi_ticks_ns(UINT64_MAX / 2, UINT64_MAX / 2, 3600000000000ull) == 3600000000000ull);

	/* Wake pairs: merge, a full table refuses, top-k with ties by index. */
	struct fi_wake t[4] = {{0}};
	assert(fi_wake_add(t, 4, 1, 93, 20000) && fi_wake_add(t, 4, 50, -1, 2000) && fi_wake_add(t, 4, 0, 1, 31000));
	assert(fi_wake_add(t, 4, 1, 93, 11000) && fi_wake_add(t, 4, 7, 7, 1000) && !fi_wake_add(t, 4, 9, 9, 1));
	assert(t[0].count == 31000);
	unsigned top[FI_WAKE_TOP];
	assert(fi_wake_top(t, 4, top, 3) == 3 && top[0] == 0 && top[1] == 2 && top[2] == 1);
	assert(fi_wake_top(t, 4, top, FI_WAKE_TOP) == 4);
	struct fi_wake none[2] = {{0}};
	assert(fi_wake_top(none, 2, top, 2) == 0);
	char names[2 * FI_WAKE_TOP][FI_NAME] = { "launchd", "mediaserverd", "kernel_task", "launchd", "", "" };
	uint64_t types[3] = { 400, 91000, 0 };
	assert(fi_wake_top(t, 4, top, 3) == 3);
	n = fi_format_wake(l2, sizeof(l2), 10, 10000, types, t, 4, top, 3, (const char (*)[FI_NAME])names, 120);
	want = "FUXI IDLE: ipiwake 10s imm 40 idle 9100 def 0 | launchd[1]>mediaserverd[93] 3100"
	    " | kernel_task[0]>launchd[1] 3100 | ?[50]>?[-] 200 | rest 112";
	if (strcmp(l2, want)) {
		printf("got  %s\nwant %s\n", l2, want);
		return 1;
	}
	assert(n == strlen(want));
	n = fi_format_wake(l2, sizeof(l2), 10, 10000, types, none, 2, top, 0, (const char (*)[FI_NAME])names, 0);
	assert(!strcmp(l2, "FUXI IDLE: ipiwake 10s imm 40 idle 9100 def 0 | rest 0"));

	/* 1e8 events/s on 8 CPUs for an hour still fit the kernel's 640-byte buffer. */
	const uint64_t ev = 100000000ull * 3600, hour_ms = 3600000, hour = 3600 * 1000 * MS;
	for (unsigned c = 0; c < FI_MAX_CPUS; c++) {
		valid[c] = 1;
		xc[c] = (struct fi_xcount){ ev, ev, ev, ev, ev, ev, ev, hour };
	}
	n = fi_format_ipirx(l2, sizeof(l2), 3600, hour_ms, xp, xc, valid, 12);
	assert(n == strlen(l2) && n < sizeof(l2) - 1 && strstr(l2, "| c7 n100000000 a100000000"));
	n = fi_format_flush(l2, sizeof(l2), 3600, 1, hour, hour_ms * 1000000, xp, xc, valid, 12);
	assert(n == strlen(l2) && n < sizeof(l2) - 1 && strstr(l2, "| c7 n100000000 a10 m1000"));
	/* Overflowing counters only cost digits: the line stays terminated. */
	for (unsigned c = 0; c < FI_MAX_CPUS; c++) {
		xc[c] = (struct fi_xcount){ UINT64_MAX / 4, UINT64_MAX / 4, UINT64_MAX / 4, UINT64_MAX / 4,
		    UINT64_MAX / 4, UINT64_MAX / 4, UINT64_MAX / 4, UINT64_MAX / 4 };
	}
	n = fi_format_ipirx(l2, sizeof(l2), 3600, 1, xp, xc, valid, 12);
	assert(n == sizeof(l2) - 1 && strlen(l2) == n);
	char longnames[2 * FI_WAKE_TOP][FI_NAME];
	memset(longnames, 'x', sizeof(longnames));
	for (unsigned i = 0; i < 2 * FI_WAKE_TOP; i++) {
		longnames[i][FI_NAME - 1] = 0;
	}
	struct fi_wake big[FI_WAKE_TOP];
	for (unsigned i = 0; i < FI_WAKE_TOP; i++) {
		big[i] = (struct fi_wake){ INT32_MAX, INT32_MAX, ev };
		top[i] = i;
	}
	uint64_t bigtypes[3] = { ev, ev, ev };
	n = fi_format_wake(l2, sizeof(l2), 3600, hour_ms, bigtypes, big, FI_WAKE_TOP, top, FI_WAKE_TOP,
	    (const char (*)[FI_NAME])longnames, 0);
	assert(n == strlen(l2) && n < sizeof(l2) - 1 && strstr(l2, " | rest "));
	n = fi_format_wake(small, sizeof(small), 10, 10000, types, t, 4, top, 0, (const char (*)[FI_NAME])names, 0);
	assert(n == sizeof(small) - 1 && strlen(small) == n);
	return 0;
}
'''

with tempfile.TemporaryDirectory() as d:
    src, exe = Path(d)/'idle.c', Path(d)/'idle'
    src.write_text(TEST)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT/'xnu/fuxi-idle'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

print("PASS: fuxi idle patch 0105 (sync, snapshots, FUXI + boot-arg gating, idle-flush and IPI hooks, line formatters)")
