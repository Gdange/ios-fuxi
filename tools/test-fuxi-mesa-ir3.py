#!/usr/bin/env python3
"""Compile actual pinned ir3 rounding/offset blocks against a small host model.

External Mesa only; verifies/applies the recipe without building Mesa. No GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fenv.h>
#pragma STDC FENV_ACCESS ON
#define BITFIELD_BIT(n) (1u << (n))
typedef unsigned nir_alu_type;
enum { nir_type_float = 0x100, nir_type_float16 = 0x110,
       nir_type_float32 = 0x120 };
static unsigned nir_alu_type_get_base_type(nir_alu_type t) { return t & 0xff00; }
static unsigned nir_alu_type_get_type_size(nir_alu_type t) { return t & 0xff; }
enum { TYPE_F16, TYPE_F32, TYPE_S32, TYPE_U32 };
enum { nir_op_f2f16_rtne, nir_op_f2f16_rtz, nir_op_other };
struct ir3_instruction { struct { unsigned round; } cat1; };
struct ir3_instruction_rpt { struct ir3_instruction *rpts[4]; };
struct shader { struct { unsigned float_controls_execution_mode; } info; };
struct context { struct shader *s; };
'''

ROUND_TEST = r'''
static unsigned check_round(unsigned op, unsigned dst_type, unsigned mode,
                            unsigned expected, unsigned nrpt)
{
   struct shader shader = {{mode}};
   struct context ctx = {&shader};
   struct ir3_instruction instrs[4] = {0};
   struct ir3_instruction_rpt cov = {{&instrs[0], &instrs[1], &instrs[2], &instrs[3]}};
   select_round(&ctx, cov, op, dst_type, nrpt);
   for (unsigned i = 0; i < 4; i++)
      assert(instrs[i].cat1.round == (i < nrpt ? expected : ROUND_ZERO));
   return instrs[0].cat1.round;
}

/* Model the selected hardware mode with host IEEE float conversions. This
 * checks the numeric oracle; actual A740 cov execution remains a device test.
 */
static uint32_t u2f_bits(uint32_t value, unsigned mode)
{
   assert(fesetround(mode == ROUND_EVEN ? FE_TONEAREST : FE_TOWARDZERO) == 0);
   volatile uint32_t input = value;
   float output = input;
   uint32_t bits;
   memcpy(&bits, &output, sizeof(bits));
   return bits;
}

static void test_round(void)
{
   for (unsigned n = 1; n <= 4; n++) {
      for (unsigned dst = TYPE_F16; dst <= TYPE_F32; dst++) {
         unsigned rte = dst == TYPE_F16 ? FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16 :
                                         FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP32;
         unsigned rtz = dst == TYPE_F16 ? FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 :
                                         FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP32;
         unsigned other_rtz = dst == TYPE_F16 ? FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP32 :
                                               FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16;
         check_round(nir_op_other, dst, 0, ROUND_EVEN, n);
         check_round(nir_op_other, dst, rte, ROUND_EVEN, n);
         check_round(nir_op_other, dst, rtz, ROUND_ZERO, n);
         check_round(nir_op_other, dst, other_rtz, ROUND_EVEN, n);
         check_round(nir_op_other, dst, rte | other_rtz, ROUND_EVEN, n);
         check_round(nir_op_f2f16_rtne, TYPE_F16, rtz, ROUND_EVEN, n);
         check_round(nir_op_f2f16_rtz, TYPE_F16, rte, ROUND_ZERO, n);
      }
      check_round(nir_op_other, TYPE_S32, 0, ROUND_ZERO, n);
      check_round(nir_op_other, TYPE_U32, FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP32,
                  ROUND_ZERO, n);
   }
   unsigned mode = check_round(nir_op_other, TYPE_F32, 0, ROUND_EVEN, 1);
   const uint32_t inputs[] = {0xffffffff, 0xffffff80, 0x01000001,
                              0x01000003, 0x7fffffff, 0x00ffffff};
   const uint32_t rte[] = {0x4f800000, 0x4f800000, 0x4b800000,
                           0x4b800002, 0x4f000000, 0x4b7fffff};
   const uint32_t rtz[] = {0x4f7fffff, 0x4f7fffff, 0x4b800000,
                           0x4b800001, 0x4effffff, 0x4b7fffff};
   for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
      assert(u2f_bits(inputs[i], mode) == rte[i]);
      assert(u2f_bits(inputs[i], ROUND_ZERO) == rtz[i]);
   }
   assert(fesetround(FE_TONEAREST) == 0);
   puts("PASS: actual ir3 rounding block, scalar/vector, FP16/FP32 defaults, explicit modes and numeric oracle");
}
'''

OFFSET_PREFIX = r'''
typedef struct { uint32_t value; } nir_def;
typedef struct { int unused; nir_def result; } nir_builder;
typedef struct { int offset_shift; } nir_intrinsic_instr;
static int nir_intrinsic_offset_shift(nir_intrinsic_instr *i) { return i->offset_shift; }
static bool nir_intrinsic_has_base(nir_intrinsic_instr *i) { (void)i; return false; }
static nir_def *nir_ushr_imm(nir_builder *b, nir_def *v, int n)
{ b->result.value = v->value >> n; return &b->result; }
static nir_def *nir_ishl_imm(nir_builder *b, nir_def *v, int n)
{ b->result.value = v->value << n; return &b->result; }
static uint32_t lower_offset(uint32_t value, int shift, int cur_shift_value)
{
   nir_def input = {value}, *offset = &input;
   nir_builder builder = {0}, *b = &builder;
   nir_intrinsic_instr intr = {cur_shift_value}, *intrinsic = &intr;
'''

OFFSET_TEST = r'''
static void test_offsets(void)
{
   /* Regression shapes from SSBO struct indexing, unaligned fields, high bits
    * and unsigned wrap. SSBO byte offsets are 32-bit modular NIR arithmetic.
    */
   const uint32_t constants[] = {0, 1, 2, 3, 4, 0xfffffffc, 0xffffffff, 0x80000000};
   const uint32_t variables[] = {0, 4, 24, 48, 72, 0x7ffffffc, 0xfffffffc};
   for (unsigned i = 0; i < sizeof(constants) / sizeof(constants[0]); i++)
      for (unsigned j = 0; j < sizeof(variables) / sizeof(variables[0]); j++)
         for (int shift = 0; shift <= 2; shift++)
            for (int cur = 0; cur <= 2; cur++) {
               uint32_t sum = constants[i] + variables[j];
               uint32_t expected = shift > cur ? sum >> (shift - cur) : sum << (cur - shift);
               assert(lower_offset(sum, shift, cur) == expected);
            }
   for (uint32_t i = 1; i <= 4; i++)
      assert(lower_offset(i * 24 - 4, 2, 0) == i * 6 - 1);
   assert(((24u >> 2) + (0xfffffffcu >> 2)) != lower_offset(24u - 4, 2, 0));
   assert(((3u >> 2) + (1u >> 2)) != lower_offset(3u + 1, 2, 0));
   puts("PASS: actual SSBO shift block retains whole modular offset; negative/unaligned/overflow regressions");
}
'''


def function(source, name):
    start = source.index(name+'(')
    start = source.rfind('\n', 0, source.rfind('\n', 0, start)) + 1
    body = source.index('{', start)
    end, depth = body + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]+'\n'


def enum(source, first, name):
    start = source.rfind('typedef enum {', 0, source.index(first))
    end = source.index('} '+name+';', start) + len('} '+name+';')
    return source[start:end]+'\n'


def model(mesa, compiler):
    nir = (mesa/'src/compiler/nir/nir.h').read_text()
    enums = (mesa/'src/compiler/shader_enums.h').read_text()
    instr = (mesa/'src/freedreno/ir3/instr-a3xx.h').read_text()
    text = PREFIX + enum(nir, 'nir_rounding_mode_undef =', 'nir_rounding_mode')
    text += enum(instr, 'ROUND_ZERO =', 'round_t')
    for line in enums.splitlines():
        if re.match(r'\s+FLOAT_CONTROLS_ROUNDING_MODE_(RTE|RTZ)_FP\d+\s+=', line):
            text += 'enum { '+line.strip().rstrip(',')+' };\n'
    for name in ('nir_is_rounding_mode_rtne', 'nir_is_rounding_mode_rtz',
                 'nir_get_rounding_mode_from_float_controls'):
        text += function(nir, name)
    text += function(compiler, 'set_cat1_round')
    start = compiler.index('   if (op == nir_op_f2f16_rtne) {')
    end = compiler.index('\n   return cov;', start)
    text += '''static void select_round(struct context *ctx, struct ir3_instruction_rpt cov,
                                       unsigned op, unsigned dst_type, unsigned nrpt)\n{\n'''
    text += compiler[start:end]+'\n}\n'+ROUND_TEST
    offsets = (mesa/'src/freedreno/ir3/ir3_nir_lower_io_offsets.c').read_text()
    # A reintroduced create_shift/iadd rewrite needs review, not this model.
    assert 'create_shift(' not in offsets
    body = function(offsets, 'lower_offset_for_ssbo')
    start = body.index('   int cur_shift =')
    end = body.index('   /* Insert the new intrinsic', start)
    text += OFFSET_PREFIX + body[start:end]+'   return offset->value;\n}\n'+OFFSET_TEST
    return text+'\nint main(void) { test_round(); test_offsets(); }\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mesa', type=Path, required=True)
    ap.add_argument('--patches', type=Path, required=True)
    args = ap.parse_args()
    mesa = args.mesa.resolve()
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        subprocess.run(['python3', str(ROOT/'tools/build-fuxi-turnip.py'), '--mesa', str(mesa),
                        '--patches', str(args.patches.resolve()), '--build', str(out/'unused'),
                        '--verify-inputs-only'], check=True)
        compiler = (mesa/'src/freedreno/ir3/ir3_compiler_nir.c').read_text()
        baseline = subprocess.check_output(['git', '-C', str(mesa), 'show',
            'HEAD:src/freedreno/ir3/ir3_compiler_nir.c'], text=True)
        for name, source in (('patched', compiler), ('baseline', baseline)):
            path = out/(name+'.c')
            path.write_text(model(mesa, source))
            exe = out/name
            subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-frounding-math', '-fsanitize=address,undefined', str(path),
                            '-lm', '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            if name == 'patched':
                assert result.returncode == 0, result.stdout+result.stderr
                assert 'runtime error:' not in result.stderr, result.stderr
                print(result.stdout, end='')
            else:
                assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
                print('PASS: unpatched Mesa fails the default-rounding regression')


if __name__ == '__main__':
    main()
