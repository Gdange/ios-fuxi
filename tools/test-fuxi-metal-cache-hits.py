#!/usr/bin/env python3
"""Offline translation hit/capacity tests and host timings; no timing thresholds.

The translator must be built from the current bridge and pinned dependency.
--baseline may point to the previous bridge for an authored-AIR comparison.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--baseline', type=Path)
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--llvm-as', default='llvm-as-18')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2')
        flags = ['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(a.headers), '-DFUXI_MTL_HOST_TEST']
        # Separate unsanitized measurements from sanitizer correctness controls.
        subprocess.run(flags + [str(ROOT/'tools/fuxi-metal/ArtifactCacheBench.cpp'), '-o', str(d/'capacity')], check=True)
        capacity = d/'capacity-cache';capacity.mkdir(mode=0o700)
        p = subprocess.run([str(d/'capacity'), str(capacity)], env=env, text=True, capture_output=True, timeout=120)
        assert p.returncode == 0, p.stderr
        print(p.stdout, end='', flush=True)
        subprocess.run([a.llvm_as, str(ROOT/'tools/shaders/metal-m7-control.ll'), '-o', str(d/'control.bc')], check=True)
        for label, translator in [('current', a.translator)] + ([('baseline', a.baseline)] if a.baseline else []):
            cache = d/(label+'-cache');cache.mkdir(mode=0o700)
            opts = flags + ['-DFUXI_MTL_ICD='+json.dumps(str(a.icd.resolve())), '-DFUXI_MTL_TRANSLATOR='+json.dumps(str(translator.resolve()))]
            subprocess.run(opts + [str(ROOT/'tools/fuxi-metal/TranslationTests.cpp'), '-ldl', '-o', str(d/'replay')], check=True)
            run_env = dict(env, FUXI_MTL_TEST_CACHE_DIR=str(cache),
                           FUXI_MTL_TEST_BOOTARGS='fuxigpucache=1 fuxigpustages=1 fuxigputrace=1')
            results = []
            for run in (('cold', 'warm', 'warm-v1') if label == 'current' else ('cold', 'warm')):
                p = subprocess.run([str(d/'replay'), str(d/'control.bc')], env=dict(run_env, **({'FUXI_MTL_TEST_CACHE_V1': '1'} if run == 'warm-v1' else {})), text=True, capture_output=True, timeout=180)
                assert p.returncode == 0, p.stderr[-8000:]
                products = re.findall(r'^product \d+ [a-f0-9]{64}$', p.stdout, re.M)
                if results:
                    assert products == results
                results = products
                phases = {}
                for phase, ms in re.findall(r'translation detail phase=(\S+) ms=([\d.]+)', p.stderr):
                    phases[phase] = round(phases.get(phase, 0) + float(ms), 3)
                hits = len(re.findall(r'translation artifact hit=1', p.stderr))
                assert (hits == 104 if run.startswith('warm') else hits < 10), p.stderr[-8000:]
                if label == 'current' and run.startswith('warm'):
                    assert 'keys-specialize' not in phases and 'pair-link' not in phases and 'cache-read' not in ' '.join(phases)
                compile_ms = sum(map(float, re.findall(r'translation timing result=miss .*?compile_ms=([\d.]+)', p.stderr)))
                hash_ms = sum(map(float, re.findall(r'translation timing result=miss .*?hash_ms=([\d.]+)', p.stderr)))
                sizes = [tuple(map(int, m)) for m in re.findall(r'translation sizes stored=(\d+) spirv=(\d+) blobs=(\d+) debug=(\d+)', p.stderr)]
                snapshots = list(cache.glob('*.translations'))
                snapshot = snapshots[0].read_bytes()
                first_record_bytes = struct.unpack_from('<I', snapshot, 160)[0]
                print(json.dumps(dict(disk_bytes=len(snapshot), first_record_bytes=first_record_bytes, label=label, run=run, artifact_hits=hits, compile_mean_ms=round(compile_ms/104, 3), hash_mean_ms=round(hash_ms/104, 3), phase_ms=phases,
                                      sizes_first=sizes[0] if sizes else None), sort_keys=True), flush=True)
            if label == 'current':
                subprocess.run(opts + ['-fsanitize=address,undefined', str(ROOT/'tools/fuxi-metal/ArtifactConcurrencyTests.cpp'), '-ldl', '-o', str(d/'concurrent')], check=True)
                p = subprocess.run([str(d/'concurrent'), str(d/'control.bc')], env=run_env, text=True, capture_output=True, timeout=60)
                assert p.returncode == 0 and 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
                print(p.stdout, end='', flush=True)
        print('PASS: fresh-process artifact replay preserves every product and skips key specialization; concurrent hits and lazy capacity')

if __name__ == '__main__':
    main()
