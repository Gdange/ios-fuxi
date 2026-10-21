#!/usr/bin/env python3
"""Linux M9a bridge specialization and staging controls; original generated AIR.

No Apple shader inputs or device access. Native CA compilation belongs to the
Mac CI metal build; this test never claims a CA rendering result.
"""
import argparse
import contextlib
import io
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def control():
    # Authored control with 58 used constants, rather than extracted shader IR.
    lines = ['target triple = "air64-apple-ios16.0.0"']
    for i in range(58):
        ty = '<3 x i8>' if i == 57 else 'i8'
        lines.append(f'@c{i} = addrspace(2) externally_initialized constant {ty} undef, section "air.fc_initializer"')
    lines += ['define <4 x float> @control_vertex() { ret <4 x float> <float 0.0,float 0.0,float 0.0,float 1.0> }',
              'define <4 x float> @control_fragment() {']
    for i in range(58):
        ty = '<3 x i8>' if i == 57 else 'i8'
        lines += [f'%v{i} = load {ty},ptr addrspace(2) @c{i}']
        if i == 57:
            lines += [f'%b{i} = extractelement <3 x i8> %v{i},i32 0']
        elif i % 2 == 0:
            lines += [f'%bit{i} = trunc i8 %v{i} to i1', f'%b{i} = zext i1 %bit{i} to i8']
        else:
            lines += [f'%b{i} = or i8 %v{i},0']
        lines += [f'%f{i} = uitofp i8 %b{i} to float']
        if i:
            lines += [f'%s{i} = fadd float %s{i-1},%f{i}']
        else:
            lines += ['%s0 = fadd float 0.0,%f0']
    lines += ['%result = insertelement <4 x float> <float 0.0,float 0.0,float 0.0,float 1.0>,float %s57,i32 0',
              'ret <4 x float> %result', '}',
              '!air.version = !{!0}', '!air.language_version = !{!1}',
              '!air.vertex = !{!2}', '!air.fragment = !{!6}',
              '!air.function_constants = !{' + ','.join(f'!{20+i}' for i in range(58)) + '}',
              '!0 = !{i32 2,i32 4,i32 0}', '!1 = !{!"Metal",i32 2,i32 4,i32 0}',
              '!2 = !{ptr @control_vertex,!3,!5}', '!3 = !{!4}',
              '!4 = !{!"air.position",!"air.arg_type_name",!"float4"}', '!5 = !{}',
              '!6 = !{ptr @control_fragment,!7,!5}', '!7 = !{!8}',
              '!8 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}']
    for i in range(58):
        ty = 'uchar3' if i == 57 else 'uchar' if i % 2 else 'bool'
        lines += [f'!{20+i} = !{{ptr addrspace(2) @c{i},!"{ty}",!"c{i}",i32 {i}}}']
    return '\n'.join(lines) + '\n'


def staging():
    spec = importlib.util.spec_from_file_location('staging', ROOT / 'tools/stage-fuxi-metal.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for raw in (b'backboardd', b'SpringBoard', b'census backboardd', b'census SpringBoard'):
            p = d / 'clients';p.write_bytes(raw)
            try:
                m.validate_clients(p)
            except ValueError:
                continue
            raise AssertionError('compositor admission accepted')
        p.write_bytes(b'fuxi-metal-smoke\nfuxi-ca-smoke\n');m.validate_clients(p)
        # Minimal format fixtures, never represented as real signed executables.
        out = d / 'out';out.mkdir()
        for name, kind in [('libvulkan_freedreno.dylib', 6), ('libmetal2vulkan.dylib', 6),
                           ('fuxi-metal-smoke', 2), ('fuxi-ca-smoke', 2), ('FuxiMetal.bundle/FuxiMetal', 6)]:
            target = out / name;target.parent.mkdir(exist_ok=True)
            target.write_bytes(struct.pack('<8I', 0xfeedfacf,0x100000c,0,kind,2,40,0,0) +
                               struct.pack('<6I',0x32,24,2,0x100000,0,0) + struct.pack('<4I',0x1d,16,72,1) + b'X')
        bundle = out / 'FuxiMetal.bundle'
        shutil.copyfile(ROOT / 'tools/fuxi-metal/Info.plist', bundle / 'Info.plist')
        (bundle / '_CodeSignature').mkdir();(bundle / '_CodeSignature/CodeResources').write_bytes(b'fixture')
        (out / 'metal-smoke.metallib').write_bytes(b'MTLBfixture');(out / 'metal2vulkan-source.tar.gz').write_bytes(b'\x1f\x8bfixture')
        other = importlib.util.spec_from_file_location('stage_tests', ROOT / 'tools/test-fuxi-metal-stage.py')
        tests = importlib.util.module_from_spec(other);other.loader.exec_module(tests)
        from unittest.mock import patch
        with patch('sys.argv', ['test', '--out', str(out)]), contextlib.redirect_stdout(io.StringIO()):
            tests.main()
    print('PASS: Linux format fixtures cover both smoke staging, configs, permanent compositor denial and containment')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18') or shutil.which('llvm-as'))
    ap.add_argument('--spirv-val', default=shutil.which('spirv-val'))
    a = ap.parse_args()
    if not a.llvm_as or not a.spirv_val:
        ap.error('supply LLVM assembler and SPIR-V validator via --llvm-as / --spirv-val')
    pin = re.search(r'^pin=([0-9a-f]{40})$', (ROOT / 'tools/build-fuxi-metal-translator.sh').read_text(), re.M)[1]
    def git(*args):
        return subprocess.check_output(['git', '-C', str(a.source), *args], text=True).strip()
    assert git('rev-parse', 'HEAD') == pin and not git('status', '--porcelain', '--untracked-files=no')
    build = a.build.resolve();(build / 'src').mkdir(parents=True, exist_ok=True)
    (build / 'src/lib.rs').write_bytes((ROOT / 'tools/fuxi-metal2vulkan-bridge.rs').read_bytes())
    source = json.dumps(str(a.source.resolve() / 'crates/metal2vulkan'))
    (build / 'Cargo.toml').write_text('''[package]
name="fuxi-metal-m9a-tests"
version="0.1.0"
edition="2021"
[lib]
name="metal2vulkan"
crate-type=["cdylib"]
[dependencies]
metal2vulkan={path=''' + source + ''',features=["ash"]}
ash={version="0.38",default-features=false,features=["std"]}
''')
    for command in ('test', 'build'):
        subprocess.run(['cargo', '+1.98.0', command, '--offline', '--manifest-path', str(build / 'Cargo.toml')], check=True)
    staging()
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp);(d / 'control.ll').write_text(control())
        subprocess.run([a.llvm_as, str(d / 'control.ll'), '-o', str(d / 'control.bc')], check=True)
        flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I' + str(ROOT / 'tools')]
        subprocess.run(flags + ['-DFUXI_MTL_TRANSLATOR="' + str(build / 'target/debug/libmetal2vulkan.so') + '"',
                               str(ROOT / 'tools/test-fuxi-metal-m9a.cpp'), '-ldl', '-pthread', '-o', str(d / 'test')], check=True)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        for source in ['tools/fuxi-metal/ClientGateTests.cpp']:
            subprocess.run(flags + [str(ROOT / source), '-o', str(d / 'gate')], check=True)
            subprocess.run([str(d / 'gate')], env=env, check=True, timeout=15)
        p = subprocess.run([str(d / 'test'), str(d / 'control.bc'), str(a.icd)], env=env, capture_output=True, text=True, timeout=60)
        print(p.stdout, end='');print(p.stderr, end='')
        assert p.returncode == 0
        for spv in sorted(d.glob('*.spv')):
            subprocess.run([a.spirv_val, '--target-env', 'vulkan1.2', str(spv)], check=True)
        assert 'translation cache HIT' in p.stderr
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr


if __name__ == '__main__':
    main()
