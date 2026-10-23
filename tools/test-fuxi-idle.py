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
for path in ('iokit/Kernel/IOPMrootDomain.cpp', 'bsd/kern/kern_exec.c', 'osfmk/conf/files.arm64'):
    assert not [l for l in block(path).splitlines()[2:] if l.startswith('-')], path  # additions only

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
	return 0;
}
'''

with tempfile.TemporaryDirectory() as d:
    src, exe = Path(d)/'idle.c', Path(d)/'idle'
    src.write_text(TEST)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT/'xnu/fuxi-idle'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

print('PASS: fuxi idle patch 0105 (snapshots, FUXI + boot-arg gating, cpuload line formatter)')
