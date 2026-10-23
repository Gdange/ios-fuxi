#!/usr/bin/env python3
"""GPU-CACHE: pinned external translator + actual lavapipe pixels across processes.

No Apple inputs or device access. Lavapipe does not establish Turnip cache hits.
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--source', type=Path)
    source.add_argument('--translator', type=Path)
    ap.add_argument('--build', type=Path, help='optional external bridge build directory')
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18'))
    a = ap.parse_args()
    if not a.llvm_as:
        ap.error('supply --llvm-as')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        translator = a.translator
        if a.source:
            revision = subprocess.check_output(['git', '-C', str(a.source), 'rev-parse', 'HEAD'], text=True).strip()
            dirty = subprocess.check_output(['git', '-C', str(a.source), 'status', '--porcelain', '--untracked-files=no'])
            if revision != PIN or dirty:
                ap.error('external translator must be clean at the recipe pin')
            build = (a.build or d/'bridge').resolve()
            (build/'src').mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT/'tools/fuxi-metal2vulkan-bridge.rs', build/'src/lib.rs')
            (build/'Cargo.toml').write_text('''[package]
name="fuxi-metal-gpu-cache"
version="0.1.0"
edition="2021"
[lib]
name="metal2vulkan"
crate-type=["cdylib"]
[dependencies]
metal2vulkan={path=''' + json.dumps(str(a.source.resolve()/'crates/metal2vulkan')) + ''',features=["ash"]}
ash={version="0.38",default-features=false,features=["std"]}
sha2="0.10"
''')
            for command in ('test', 'build'):
                subprocess.run(['cargo', '+1.98.0', command, '--offline', '--manifest-path', str(build/'Cargo.toml')], check=True)
            translator = build/'target/debug/libmetal2vulkan.so'
        flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-I'+str(a.headers), '-I'+str(ROOT/'tools')]
        subprocess.run(flags + [str(ROOT/'tools/fuxi-metal/ArtifactCacheTests.cpp'), '-o', str(d/'unit')], check=True)
        subprocess.run([a.llvm_as, str(ROOT/'tools/shaders/metal-m7-control.ll'), '-o', str(d/'control.bc')], check=True)
        subprocess.run(flags + ['-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_CA_OFFLINE',
                               '-DFUXI_MTL_ICD='+json.dumps(str(a.icd.resolve())),
                               '-DFUXI_MTL_TRANSLATOR='+json.dumps(str(translator.resolve())),
                               str(ROOT/'tools/test-fuxi-metal-m9i.cpp'), '-ldl', '-pthread', '-o', str(d/'pixels')], check=True)
        cache = d/'cache';cache.mkdir(mode=0o700)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2',
                   FUXI_MTL_TEST_CACHE_DIR=str(cache), FUXI_MTL_TEST_BOOTARGS='fuxigpucache=1')
        unit = subprocess.run([str(d/'unit'), str(cache)], env=env, check=True, timeout=30, capture_output=True, text=True)
        assert 'AddressSanitizer' not in unit.stderr and 'runtime error:' not in unit.stderr, unit.stderr
        print(unit.stdout, end='')
        def run(extra=None):
            p = subprocess.run([str(d/'pixels'), str(d/'control.bc')], env=dict(env, **(extra or {})),
                               text=True, capture_output=True, timeout=120)
            assert p.returncode == 0, p.stderr[-8000:]
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            return p
        def stats(p):
            result = re.search(r'PASS pixels=([0-9a-f]+) load=(\d+) bytes=(\d+) save=(\d+) saved=(\d+)', p.stdout)
            assert result, p.stdout
            return result.groups()
        def outcomes(p):
            values = re.findall(r'cache period translation hits=(\d+) misses=(\d+) rejected=(\d+)', p.stderr)
            assert values, p.stderr
            return tuple(sum(int(v[i]) for v in values) for i in range(3))
        cold = run({'FUXI_MTL_TEST_IDLE_CHECKPOINT': '1', 'FUXI_MTL_TEST_CRASH_AFTER_IDLE': '1'})
        assert 'PASS: idle checkpoint before teardown' in cold.stdout
        assert outcomes(cold)[1] > 0 and outcomes(cold)[0] == 0, cold.stderr
        path = cache/'fuxi-m9i-test.translations';vk = cache/'fuxi-m9i-test.pipelines'
        assert path.stat().st_mode & 0o777 == 0o600 and vk.stat().st_mode & 0o777 == 0o600
        warm = run();baseline = stats(warm)
        assert baseline[1] == '1' and outcomes(warm)[0] > 0 and outcomes(warm)[1] == 0, warm.stderr
        assert re.search(r'translation cache loaded [1-9]\d* bytes / [1-9]\d* pipelines', warm.stderr), warm.stderr
        assert re.search(r'cache period driver hits=\d+ misses=\d+ unknown=\d+', warm.stderr), warm.stderr
        print('PASS: idle worker saves before _exit; fresh process reloads driver/artifact caches and renders exact authored pixels')
        original = path.read_bytes()
        # Outer envelope with a repaired CRC: the upstream artifact decoder must
        # reject its own corrupt bytes, then compile/replace rather than abort.
        broken = bytearray(original);broken[128+36] ^= 1
        struct.pack_into('<I', broken, 20, zlib.crc32(broken[128:]))
        path.write_bytes(broken);repaired = run()
        assert stats(repaired)[0] == baseline[0] and outcomes(repaired)[2] > 0, repaired.stderr
        assert outcomes(run())[1] == 0
        for offset in (28, 48, 64, 80, 96):  # driver ID/UUID, device UUID, translator digest
            broken = bytearray(original);broken[offset] ^= 1;path.write_bytes(broken)
            p = run();assert stats(p)[0] == baseline[0] and 'result=discard(identity)' in p.stderr, p.stderr
        original_vk = vk.read_bytes()
        for offset in (24, 32, 304, 336):
            broken = bytearray(original_vk);broken[offset] ^= 1;vk.write_bytes(broken)
            p = run();assert stats(p)[0] == baseline[0] and 'discard(identity)' in p.stderr, p.stderr
        print('PASS: stale translator/driver/device identities and corrupt translator artifacts miss safely and repair with identical pixels')
        before = {f.name: (f.read_bytes(), f.stat().st_mtime_ns) for f in (path, vk)}
        off = run({'FUXI_MTL_TEST_BOOTARGS': 'fuxigpucache=0'})
        assert stats(off)[0] == baseline[0] and stats(off)[1] == '0' and stats(off)[3] == '2', off.stderr
        assert 'disk caches disabled fuxigpucache=0' in off.stderr and 'translation cache loaded' not in off.stderr
        assert before == {f.name: (f.read_bytes(), f.stat().st_mtime_ns) for f in (path, vk)}
        legacy = run({'FUXI_MTL_TEST_BOOTARGS': ''})
        assert stats(legacy)[0] == baseline[0] and 'translation cache loaded' not in legacy.stderr and 'cache period' not in legacy.stderr
        assert (cache/'fuxi-m9i-test.cache').is_file()
        assert before == {f.name: (f.read_bytes(), f.stat().st_mtime_ns) for f in (path, vk)}
        cache.chmod(0o500)
        try:
            p = run();assert stats(p)[0] == baseline[0] and stats(p)[1] == '0' and stats(p)[3] == '2', p.stderr
        finally:
            cache.chmod(0o700)
        assert not list(cache.glob('*.tmp'))
        print('PASS: disable/absent preserve new snapshots, legacy default retained; unavailable storage falls back without changing pixels')


if __name__ == '__main__':
    main()
