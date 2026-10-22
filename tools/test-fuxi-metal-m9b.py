#!/usr/bin/env python3
"""Linux M9b production-cache checks plus existing owned-source bridge controls.

No device access or GPU submissions. Native CA adapters require Mac tests.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compile-only', action='store_true', help='compile C++ controls without executing tests or submissions')
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18') or shutil.which('llvm-as'))
    ap.add_argument('--spirv-val', default=shutil.which('spirv-val'))
    a = ap.parse_args()
    if not a.llvm_as or (not a.compile_only and not a.spirv_val):
        ap.error('supply --llvm-as and --spirv-val')
    # This builds the exact pinned external bridge and validates synthetic AIR,
    # constant/cache controls, compositor denial and complete notice staging.
    if not a.compile_only:subprocess.run(['python3', str(ROOT / 'tools/test-fuxi-metal-m9a.py'),
                    '--source', str(a.source), '--build', str(a.build),
                    '--llvm-as', a.llvm_as, '--spirv-val', a.spirv_val,
                    '--icd', str(a.icd)], check=True)
    flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-I' + str(a.headers), '-I' + str(ROOT / 'tools')]
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        subprocess.run(flags + [str(ROOT / 'tools/test-fuxi-metal-m9b.cpp'), '-o', str(d / 'cache')], check=True)
        if not a.compile_only:subprocess.run([str(d / 'cache')], env=env, check=True, timeout=15)
        # Compile the actual Backend with an authored AIR control. Make and
        # destroy pipelines repeatedly, including after cache retirement. Never
        # allocate/submit a command buffer. This exercises real Vulkan caching.
        spec = importlib.util.spec_from_file_location('m9a', ROOT / 'tools/test-fuxi-metal-m9a.py')
        m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        (d / 'control.ll').write_text(m.control())
        subprocess.run([a.llvm_as, str(d / 'control.ll'), '-o', str(d / 'control.bc')], check=True)
        (d / 'backend.cpp').write_text(r'''
#include "fuxi-metal/Backend.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc,char **argv) {
 assert(argc==3);std::ifstream in(argv[1],std::ios::binary);
 std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char why[2048]{};
 void *lib=tr.open(bytes.data(),bytes.size(),why,sizeof(why));assert(lib);
 fuxi::CompileOptions options{};options.constantCount[1]=58;
 for(unsigned i=0;i<58;++i){auto &c=options.constants[1][i];c.index=i;c.type=i==57?51:i%2?49:53;c.size=i==57?3:1;}
 for(unsigned reopen=0;reopen<2;++reopen){fuxi::Backend backend;
  for(unsigned i=0;i<10;++i){auto *p=backend.compilePipeline(lib,"control_vertex",lib,"control_fragment",options);assert(p);backend.destroy(p);}}
 std::ifstream fetchInput(argv[2],std::ios::binary);std::vector<uint8_t> fetchBytes{std::istreambuf_iterator<char>(fetchInput),{}};
 void *fetchLib=tr.open(fetchBytes.data(),fetchBytes.size(),why,sizeof(why));assert(fetchLib);
 for(unsigned reopen=0;reopen<2;++reopen){fuxi::Backend backend;assert(backend.supportsOrderedColorAccess());
  fuxi::CompileOptions fetch{};fetch.attributes[0]={29,0,0};fetch.attributes[1]={29,8,0};fetch.attributes[2]={9,16,0};fetch.strides[0]=20;
  for(unsigned warm=0;warm<10;++warm){auto *p=backend.compilePipeline(fetchLib,"fuxi_layer_vertex",fetchLib,"fuxi_fetch_fragment",fetch);assert(p->fetchMask==1 && p->orderedColorAccess);backend.destroy(p);}}
 tr.close(fetchLib);tr.close(lib);puts("PASS: actual backend 40 requests / 4 shared pipeline pairs, ordered fetch priming, object reuse and device reopen; no command buffers or submissions");
}
''')
        subprocess.run([a.llvm_as, str(ROOT / 'tools/shaders/metal-m7-control.ll'), '-o', str(d / 'fetch.bc')], check=True)
        subprocess.run(flags + ['-DFUXI_MTL_HOST_TEST',
                       '-DFUXI_MTL_ICD="' + str(a.icd.resolve()) + '"',
                       '-DFUXI_MTL_TRANSLATOR="' + str(a.build.resolve() / 'target/debug/libmetal2vulkan.so') + '"',
                       str(d / 'backend.cpp'), '-ldl', '-pthread', '-o', str(d / 'backend')], check=True)
        if a.compile_only:
            print('PASS: M9b compile-only controls; no test executables run')
            return
        p = subprocess.run([str(d / 'backend'), str(d / 'control.bc'), str(d / 'fetch.bc')], env=env,
                           capture_output=True, text=True, timeout=60)
        assert p.returncode == 0, p.stderr
        assert p.stderr.count('Vulkan pipelines PASS') == 4 and p.stderr.count('cache=1') == 4 and p.stderr.count('pipeline cache HIT') == 36
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr
        print(p.stdout, end='')


if __name__ == '__main__':
    main()
