#!/usr/bin/env python3
"""Compile the actual SCM wrapper and ARM64 primitive; never accesses a device.

Mach-O and ELF disassembly must contain SMC with the right argument/result
registers. The host model checks migration, interrupt restoration, retained
attempts and bounded retry policy, not TZ service or real scheduling.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r'^static (?:__attribute__\(\(noinline\)\) )?void ' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    assert start, name
    end, depth = start.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start.start():end] + '\n'


def check_disassembly(dis, symbol):
    match = re.search(r'<_?' + symbol + r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)', dis, re.S)
    assert match, symbol
    body = match[1]
    assert len(re.findall(r'\bsmc\s+#0(?:x0)?\b', body)) == 1, body
    assert 'd4000003' in body and not re.search(r'\bhvc\b', body), body
    assert re.search(r'dsb\s+sy\s*\n[^\n]*smc\s+#0(?:x0)?\s*\n[^\n]*dsb\s+sy', body), body
    # Symbolically execute MOV and return-buffer stores around the actual SMC.
    # Seed AAPCS64 arguments to g5_smc_raw(ret, fn, info, a, b, c).
    args = [0x42000601, 0x82, 0x1122334455667788, 0x8877665544332211, 0xfedcba9876543210, 0, 0, 0]
    returns = [0x123456789abcdef0 + i for i in range(4)]
    registers = {f'x{i}': 0xbad000+i for i in range(31)}
    registers.update({f'x{i}': v for i, v in enumerate([0x10000] + args[:5])})
    registers.update(xzr=0, sp=0x20000)
    memory, called = {}, False

    def read(reg):
        return registers.get('x' + reg[1:] if reg.startswith('w') else reg, 0)

    for line in body.splitlines():
        if re.search(r'\bsmc\s', line):
            assert [read(f'x{i}') for i in range(8)] == args, body
            registers.update({f'x{i}': value for i, value in enumerate(returns)})
            called = True
        move = re.search(r'\bmov\s+([xw]\d+),\s*([xw]\d+|[xw]zr|#(?:0x[0-9a-f]+|\d+))', line)
        if move:
            dst, src = move.groups()
            val = int(src[1:], 0) if src.startswith('#') else read(src)
            registers['x' + dst[1:]] = val & (0xffffffff if dst.startswith('w') else 0xffffffffffffffff)
        store = re.search(r'\b(stp|str)\s+(x\d+)(?:,\s*(x\d+))?,\s*\[(x\d+)(?:,\s*#(0x[0-9a-f]+|\d+))?\]', line)
        if store and called:
            _, r1, r2, base, off = store.groups()
            addr = read(base) + int(off or '0', 0)
            memory[addr] = read(r1)
            if r2:
                memory[addr+8] = read(r2)
    assert [memory.get(0x10000+i*8) for i in range(4)] == returns, body


MODEL = r'''
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int processor_t;
typedef int boolean_t;
#define FALSE 0
#define THREAD_CONTINUE_NULL 0
static unsigned cpu, bound, migrations, binds, calls, delays, irq_sets, logs, fail_bind;
static boolean_t irqs, entry_irqs;
static uint64_t answers[2][4];
static int model_printf(const char *fmt, ...) { (void)fmt; assert(irqs==entry_irqs); logs++; return 0; }
#define printf model_printf
static processor_t cpu_to_processor(int n) { assert(n==0); return n; }
static processor_t thread_bind(processor_t p) { unsigned old=bound; bound=p; binds++; return old; }
static int thread_block(int c) { assert(c==0 && bound==0); migrations++; if(!fail_bind) cpu=0; return 0; }
static unsigned ml_get_cpu_number_local(void) { assert(!irqs); return cpu; }
static boolean_t ml_set_interrupts_enabled(boolean_t e) { boolean_t old=irqs; irqs=e; irq_sets++; return old; }
static void delay(unsigned us) { assert(irqs==entry_irqs && us==100 && calls==1); delays++; }
static void g5_smc_raw(uint64_t ret[4], uint32_t fn, uint32_t info, uint64_t a, uint64_t b, uint64_t c)
{
 assert(!irqs && cpu==0 && bound==0 && calls<2);
 assert(fn==0x42000601 && info==1 && a==0x02000207 && b==0 && c==0);
 memcpy(ret,answers[calls++],4*sizeof(*ret));
}
'''
TEST = r'''
static void run(uint64_t first, uint64_t second, unsigned expected_calls, unsigned expected_delays)
{
 memset((void *)fg_scm_records,0,sizeof(fg_scm_records));
 cpu=7; bound=9; migrations=binds=calls=delays=irq_sets=logs=0; irqs=entry_irqs;
 for(unsigned j=0;j<2;j++) for(unsigned i=0;i<4;i++) answers[j][i]=0x123456789abcdef0ull+i+j;
 answers[0][0]=first; answers[1][0]=second;
 uint64_t ret[4];
 fg_scm_call(ret,7,3,0x42000601,1,0x02000207,0,0);
 assert(bound==9 && binds==2 && migrations==1 && irqs==entry_irqs);
 assert(calls==expected_calls && delays==expected_delays && irq_sets==(calls ? calls : 1)*2);
 assert(fg_scm_records[7].fn==0x42000601 && fg_scm_records[7].info==1);
 if(fail_bind) { assert(ret[0]==UINT64_MAX && fg_scm_records[7].cpu[0]==7); return; }
 assert(fg_scm_records[7].attempts==calls);
 for(unsigned j=0;j<calls;j++) for(unsigned i=0;i<4;i++)
  assert(fg_scm_records[7].ret[j][i]==answers[j][i] && fg_scm_records[7].cpu[j]==0);
 assert(!memcmp(ret,answers[calls-1],sizeof(ret)));
}
int main(void)
{
 for(entry_irqs=0;entry_irqs<=1;entry_irqs++) {
  run(0,0,1,0); /* x1, x2, x3 must survive even when nonzero */
  run(0x42000601,0,2,1);
  run(0x42000601,0x42000601,2,1);
  run(0x42000601,1,2,1);
  run(1,0,1,0); /* INTERRUPTED: no unimplemented token continuation */
  run(2,0,1,0); /* WAITQ: no event/IRQ machinery */
  run((uint64_t)-12,0,1,0); /* EBUSY: no replay of serviced calls */
  run((uint64_t)-1,0,1,0);
  fail_bind=1; run(0,0,0,0); fail_bind=0;
 }
 puts("PASS: CPU0 migration, prior binding/interrupt restoration, all four results, both attempts, one retry only on x0==fn, off-CPU skip");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, help='also verify the final unstripped native kernel')
    args = parser.parse_args()
    raw = function((ROOT / 'shim/fuxi-probe/gpu-zap.h').read_text(), 'g5_smc_raw')
    adapter = (ROOT / 'xnu/fuxi-adreno/fuxi_adreno.c').read_text()
    records = re.search(r'static volatile struct \{.*?\} fg_scm_records\[8\];', adapter, re.S)[0]
    with tempfile.TemporaryDirectory(prefix='fuxi-scm-') as d:
        source = Path(d) / 'raw.c'
        source.write_text('typedef unsigned long long uint64_t; typedef unsigned uint32_t;\n' + raw +
                          'void test(uint64_t *r, uint32_t f, uint32_t i, uint64_t a, uint64_t b, uint64_t c) { g5_smc_raw(r,f,i,a,b,c); }\n')
        for target in ('arm64-apple-darwin', 'aarch64-none-elf'):
            obj = Path(d) / (target + '.o')
            subprocess.run(['clang-18', '-target', target, '-O2', '-ffreestanding', '-mgeneral-regs-only', '-c', str(source), '-o', str(obj)], check=True)
            dis = subprocess.check_output(['llvm-objdump-18', '-d', str(obj)], text=True)
            check_disassembly(dis, 'g5_smc_raw')
            print(f'PASS: {target}: actual SCM primitive has barriers/SMC, x0..x7 arguments, x0..x3 results')
        source.write_text(MODEL + records + '\n' + function(adapter, 'fg_scm_call') + TEST)
        exe = Path(d) / 'context'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    if args.kernel:
        dis = subprocess.check_output(['llvm-objdump-18', '--disassemble-symbols=_g5_smc_raw', str(args.kernel)], text=True)
        check_disassembly(dis, 'g5_smc_raw')
        print('PASS: final native kernel SCM primitive instruction and register audit')


if __name__ == '__main__':
    main()
