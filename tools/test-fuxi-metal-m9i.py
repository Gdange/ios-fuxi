#!/usr/bin/env python3
"""Compile/run authored-AIR M9i persistence controls on Linux lavapipe.

--compile-only builds without running test executables or submitting GPU work.
The bridge is built offline against a clean external checkout; nothing is vendored.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zlib
ROOT = Path(__file__).resolve().parents[1]
PIN = '720ef1ce9ae2a2068999a8675717418b9416283e'
HEADER = 336
LIMIT = 32 * 1024 * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compile-only', action='store_true')
    bridge = ap.add_mutually_exclusive_group()
    bridge.add_argument('--source', type=Path, help='external metal2vulkan checkout, verified at the pin')
    bridge.add_argument('--translator', type=Path, help='matching separately built bridge')
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18'))
    a = ap.parse_args()
    if not a.llvm_as:
        ap.error('supply --llvm-as')
    if not a.compile_only and not (a.source or a.translator):
        ap.error('runs require --source or --translator')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        translator = a.translator
        if a.source:
            revision = subprocess.check_output(['git', '-C', str(a.source), 'rev-parse', 'HEAD'], text=True).strip()
            dirty = subprocess.check_output(['git', '-C', str(a.source), 'status', '--porcelain', '--untracked-files=no'])
            if revision != PIN or dirty:
                ap.error('external translator must be clean at the recipe pin')
            build = d / 'bridge'
            (build / 'src').mkdir(parents=True)
            shutil.copyfile(ROOT / 'tools/fuxi-metal2vulkan-bridge.rs', build / 'src/lib.rs')
            (build / 'Cargo.toml').write_text('''[package]
name="fuxi-metal-m9i"
version="0.1.0"
edition="2021"
[lib]
name="metal2vulkan"
crate-type=["cdylib"]
[dependencies]
metal2vulkan={path=''' + json.dumps(str(a.source.resolve() / 'crates/metal2vulkan')) + ''',features=["ash"]}
ash={version="0.38",default-features=false,features=["std"]}
sha2="0.10"
''')
            subprocess.run(['cargo', '+1.98.0', 'build', '--offline', '--manifest-path', str(build / 'Cargo.toml')], check=True)
            translator = build / 'target/debug/libmetal2vulkan.so'
        subprocess.run([a.llvm_as, str(ROOT / 'tools/shaders/metal-m7-control.ll'), '-o', str(d / 'control.bc')], check=True)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_CA_OFFLINE', '-DFUXI_MTL_ICD=' + json.dumps(str(a.icd.resolve())),
                        '-DFUXI_MTL_TRANSLATOR=' + json.dumps(str(translator.resolve()) if translator else 'compile-only'),
                        '-I' + str(a.headers), '-I' + str(ROOT / 'tools'), str(ROOT / 'tools/test-fuxi-metal-m9i.cpp'),
                        '-ldl', '-pthread', '-o', str(d / 'test')], check=True)
        if a.compile_only:
            print('PASS: M9i compile-only with -Werror and ASan/UBSan; no test executables run or GPU submissions')
            if not a.source:
                print('Bridge build skipped (provide --source for pinned cargo +1.98.0 build --offline)')
            return
        cache = d / 'cache'
        cache.mkdir(mode=0o700)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2',
                   FUXI_MTL_TEST_CACHE_DIR=str(cache))
        unit = subprocess.run([str(d / 'test'), '--unit', str(cache)], env=env, text=True, capture_output=True, timeout=30)
        assert unit.returncode == 0, unit.stderr[-6000:]
        assert 'AddressSanitizer' not in unit.stderr and 'runtime error:' not in unit.stderr
        print(unit.stdout, end='')
        def collect(p):
            stdout, stderr = p.communicate(timeout=120)
            assert p.returncode == 0, stderr[-6000:]
            assert 'AddressSanitizer' not in stderr and 'runtime error:' not in stderr
            result = re.search(r'^PASS pixels=([0-9a-f]+) load=(\d+) bytes=(\d+) save=(\d+) saved=(\d+)$', stdout, re.M)
            assert result, stdout
            return result.groups(), stderr
        def launch():
            return subprocess.Popen([str(d / 'test'), str(d / 'control.bc')], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def run():
            return collect(launch())
        first, log = run()
        assert first[1] == '0' and first[3] == '1' and int(first[4]) >= 32, log
        path = cache / 'fuxi-m9i-test.cache'
        original = path.read_bytes()
        assert path.stat().st_mode & 0o777 == 0o600
        def hit():
            result, log = run()
            assert result[0] == first[0] and result[1] == '1' and int(result[2]) >= 32, log
        hit()
        print('PASS: first process saves; second process loads with identical authored pixels')
        cases = {}
        cases['length'] = original[:12]
        flipped = bytearray(original)
        flipped[-1] ^= 1
        cases['checksum'] = flipped
        uuid = bytearray(original)
        uuid[HEADER + 16] ^= 1
        struct.pack_into('<I', uuid, 20, zlib.crc32(uuid[HEADER:]))
        cases['vk-uuid'] = uuid
        for reason, payload in cases.items():
            path.write_bytes(payload)
            result, log = run()
            assert result[0] == first[0] and result[1] == '2', log
            assert 'discard(' + reason + ')' in log, log
            hit()  # Corrupt input is replaced by an atomic, valid save.
        with path.open('wb') as stream:
            stream.truncate(HEADER + LIMIT + 1)
        result, log = run()
        assert result[0] == first[0] and result[1] == '2' and 'discard(oversize)' in log, log
        hit()
        print('PASS: truncated, flipped, wrong UUID (valid CRC), oversized files safely discarded and repaired')
        path.write_bytes(original)
        one, two = launch(), launch()
        for process in (one, two):
            result, log = collect(process)
            assert result[0] == first[0] and result[1] == '1' and result[3] == '1', log
        hit()
        assert not list(cache.glob('.cache-*'))
        print('PASS: concurrent process saves leave a valid complete file')
        cache.chmod(0o500)
        try:
            result, log = run()
            assert result[0] == first[0] and result[1] == '0' and result[3] == '2', log
            assert path.read_bytes()  # Existing file remains intact; no write is attempted.
        finally:
            cache.chmod(0o700)
        hit()
        print('PASS: unwritable directory falls back to memory-only with identical pixels')


if __name__ == '__main__':
    main()
