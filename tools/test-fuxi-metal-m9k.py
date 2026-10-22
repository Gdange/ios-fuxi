#!/usr/bin/env python3
"""Compile or run authored M9k lavapipe controls with the separately built bridge.

Build the pinned bridge using the existing offline or M9a harness first. This
runner never consumes Apple libraries and never accesses a device.
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
    ap.add_argument('--compile-only', action='store_true')
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18'))
    a = ap.parse_args()
    if not a.llvm_as:
        ap.error('supply --llvm-as')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        subprocess.run([a.llvm_as, str(ROOT/'tools/shaders/metal-m7-control.ll'), '-o', str(d/'control.bc')], check=True)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_CA_OFFLINE', '-DFUXI_MTL_ICD="'+str(a.icd.resolve())+'"',
                        '-DFUXI_MTL_TRANSLATOR="'+str(a.translator.resolve())+'"', '-I'+str(a.headers), '-I'+str(ROOT/'tools'),
                        str(ROOT/'tools/test-fuxi-metal-m9k.cpp'), '-ldl', '-pthread', '-o', str(d/'test')], check=True)
        if a.compile_only:
            print('PASS: M9k Backend compile-only; no executables run or GPU submissions')
            return
        # The ABI guard runs before any other bridge symbol is consumed.
        (d/'guard.cpp').write_text('#include "fuxi-metal/Translator.hpp"\n#include <string>\nint main(){try{(void)fuxi::Translator::get();return 2;}catch(const std::exception &e){return std::string(e.what())=="metal2vulkan C ABI mismatch"?0:3;}}\n')
        for version in (None, 6, 8):
            (d/'old.cpp').write_text('int unused;' if version is None else 'extern "C" unsigned fuxi_m2v_abi(){return '+str(version)+';}')
            subprocess.run(['clang++','-shared','-fPIC',str(d/'old.cpp'),'-o',str(d/'old.so')],check=True)
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'tools'),
                '-DFUXI_MTL_TRANSLATOR="'+str(d/'old.so')+'"',str(d/'guard.cpp'),'-ldl','-pthread','-o',str(d/'guard')],check=True)
            subprocess.run([str(d/'guard')],check=True,timeout=10)
        print('PASS: missing, ABI-6 and future bridge refused before symbol consumption')
        p = subprocess.run([str(d/'test'), str(d/'control.bc')], env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2'),
                           text=True, capture_output=True, timeout=120)
        assert p.returncode == 0, p.stderr[-6000:]
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr
        print(p.stdout, end='')
if __name__ == '__main__':
    main()
