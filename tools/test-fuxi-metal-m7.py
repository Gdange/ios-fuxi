#!/usr/bin/env python3
"""Pinned bridge + verified CI M6b regression + original AIR M7 lavapipe control.

The hand-authored AIR control does not qualify Xcode's stage-6 MSL. Add --ci-m7
only after supplying the newly CI-built metallib and its matching manifest.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--metallib', type=Path, required=True)
    ap.add_argument('--sums', type=Path, required=True)
    ap.add_argument('--headers', type=Path, required=True)
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--llvm-as', default=next((p for n in ('llvm-as', 'llvm-as-18', 'llvm-as-19')
                                             if (p := shutil.which(n))), None))
    ap.add_argument('--ci-m7', action='store_true')
    a = ap.parse_args()
    if not a.llvm_as:
        ap.error('supply --llvm-as (LLVM 18 or compatible)')
    # This also builds/tests the bridge and verifies the external pin and CI hash.
    subprocess.run(['python3', str(ROOT / 'tools/test-fuxi-metal-m6b.py'),
                    '--source', str(a.source), '--build', str(a.build),
                    '--metallib', str(a.metallib), '--sums', str(a.sums),
                    '--headers', str(a.headers), '--icd', str(a.icd)], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        control = d / 'control.bc'
        subprocess.run([a.llvm_as, str(ROOT / 'tools/shaders/metal-m7-control.ll'),
                        '-o', str(control)], check=True)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DFUXI_MTL_HOST_TEST',
                        '-DFUXI_MTL_ICD="' + str(a.icd.resolve()) + '"',
                        '-DFUXI_MTL_TRANSLATOR="' + str(a.build.resolve() / 'target/debug/libmetal2vulkan.so') + '"',
                        '-I' + str(a.headers), '-I' + str(ROOT / 'tools'),
                        str(ROOT / 'tools/test-fuxi-metal-m7.cpp'), '-ldl', '-pthread',
                        '-o', str(d / 'test')], check=True)
        for library in [control] + ([a.metallib.resolve()] if a.ci_m7 else []):
            p = subprocess.run([str(d / 'test'), str(library)],
                               env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'),
                               text=True, capture_output=True, timeout=60)
            print(p.stdout, end='')
            if p.returncode:
                print(p.stderr[-6000:], end='')
            assert p.returncode == 0, 'M7 AIR control failed'
            assert 'pipeline cache HIT' in p.stderr, 'cache path missing'
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr
    if not a.ci_m7:
        print('UNVALIDATED: new Xcode stage-6 MSL; requires CI metallib rebuild and --ci-m7')


if __name__ == '__main__':
    main()
