#!/usr/bin/env python3
"""Offline authored AIR: exact reference/opt-in products, stage reuse and timings.

Supply an ABI-8 host library built from the current Fuxi adapter and pinned
external translator (test-fuxi-metal-cache.py --source builds one offline).
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--llvm-as', default='llvm-as-18')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        subprocess.run([a.llvm_as, str(ROOT/'tools/shaders/metal-m7-control.ll'), '-o', str(d/'control.bc')], check=True)
        subprocess.run(['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DFUXI_MTL_HOST_TEST', '-I'+str(a.headers),
                        '-DFUXI_MTL_ICD='+json.dumps(str(a.icd.resolve())),
                        '-DFUXI_MTL_TRANSLATOR='+json.dumps(str(a.translator.resolve())),
                        str(ROOT/'tools/fuxi-metal/TranslationTests.cpp'), '-ldl', '-pthread', '-o', str(d/'test')], check=True)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2')
        # Fresh processes keep product/stage/object caches independent. Repeat to
        # expose order/noise; no timing threshold is a correctness assertion.
        timings = {False: [], True: []}
        expected = None
        for reuse in (False, True, True, False):
            p = subprocess.run([str(d/'test'), str(d/'control.bc')], env=dict(env,
                FUXI_MTL_TEST_BOOTARGS='fuxigpucache=0 fuxigputrace=1 fuxigpustages='+str(int(reuse))),
                capture_output=True, text=True, timeout=180)
            assert p.returncode == 0, p.stderr[-8000:]
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            products = re.findall(r'^product \d+ [a-f0-9]{64}$', p.stdout, re.M)
            assert len(products) == 104, p.stdout
            if expected is None:
                expected = products
            assert products == expected, 'cached stages changed a compiled product'
            assert len(set(x.split()[-1] for x in products)) > 96
            assert len(re.findall(r'translation timing result=miss', p.stderr)) == 104, p.stderr
            elapsed = float(re.search(r'elapsed_ms ([\d.]+)', p.stdout)[1])
            timings[reuse].append(elapsed)
            phases = {}
            for phase, ms in re.findall(r'translation detail phase=(\S+) ms=([\d.]+)', p.stderr):
                phases[phase] = round(phases.get(phase, 0) + float(ms), 3)
            if reuse:
                hits = re.findall(r'translation stage=(vertex|fragment) hit=1', p.stderr)
                misses = re.findall(r'translation stage=(vertex|fragment) hit=0', p.stderr)
                assert len(hits) >= 190 and len(misses) >= 3, (len(hits), len(misses))
                assert 'phase=pair-link' in p.stderr and 'phase=pair-compile' not in p.stderr
            else:
                assert 'translation stage=' not in p.stderr and 'phase=pair-compile' in p.stderr
            stalls = [float(x) for x in re.findall(r'translation timing result=miss .*?total_ms=([\d.]+)', p.stderr)]
            compile_ms = sum(float(x) for x in re.findall(r'translation timing result=miss .*?compile_ms=([\d.]+)', p.stderr))
            print(json.dumps({'stages': reuse, 'elapsed_ms': elapsed, 'compile_ms': round(compile_ms, 3),
                              'request_ms': round(sum(stalls), 3), 'worst_request_ms': max(stalls),
                              'stage_hits': len(re.findall(r'translation stage=\S+ hit=1', p.stderr)),
                              'stage_misses': len(re.findall(r'translation stage=\S+ hit=0', p.stderr)),
                              'phase_ms': phases}, sort_keys=True), flush=True)
        print('PASS: 104 distinct pipeline requests produce identical hashes with stage reuse; constants, attributes and strides included; fresh-process A/B/B/A timings', timings)


if __name__ == '__main__':
    main()
