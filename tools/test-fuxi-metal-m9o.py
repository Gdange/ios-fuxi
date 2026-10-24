#!/usr/bin/env python3
"""Local M9o Vulkan controls; optional authored AIR draws with an external bridge.
No phone access, extracted shaders, downloads or network."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compile-only', action='store_true')
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--translator', type=Path)
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18'))
    a = ap.parse_args()
    if a.translator and not a.compile_only and not a.llvm_as:
        ap.error('supply --llvm-as for authored AIR draws')
    with tempfile.TemporaryDirectory() as tmp:
        test = Path(tmp)/'test'
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wmissing-field-initializers', '-fsanitize=address,undefined',
                        '-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_ICD="'+str(a.icd.resolve())+'"',
                        '-DFUXI_MTL_TRANSLATOR="'+(str(a.translator.resolve()) if a.translator else 'unavailable-m9o-control')+'"', '-I'+str(a.headers), '-I'+str(ROOT/'tools'),
                        str(ROOT/'tools/test-fuxi-metal-m9o.cpp'), '-ldl', '-pthread', '-o', str(test)], check=True)
        if a.compile_only:
            print('PASS: M9o production Backend compile-only')
            return
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2')
        args = [str(test)]
        if a.translator:
            air = Path(tmp)/'control.bc'
            subprocess.run([a.llvm_as, str(ROOT/'tools/shaders/metal-m7-control.ll'), '-o', str(air)], check=True)
            args.append(str(air))
        p = subprocess.run(args, env=env,
                           text=True, capture_output=True, timeout=60)
        assert p.returncode == 0, p.stderr
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr
        print(p.stdout, end='')
        if a.translator:
            traced = subprocess.run(args, env=dict(env, FUXI_MTL_TEST_BOOTARGS='fuxigpuframetrace=1'),
                                    text=True, capture_output=True, timeout=60)
            assert traced.returncode == 0, traced.stderr
            assert 'AddressSanitizer' not in traced.stderr and 'runtime error:' not in traced.stderr
            assert traced.stdout == p.stdout
            print('PASS: traced authored draws retain pixels and count pipelines/descriptors/draws/passes')
if __name__ == '__main__':
    main()
