#!/usr/bin/env python3
"""Linux source/policy checks; Mac wrapper and actual native frontend tests.

Source paths must be disposable PINNED external checkouts. Tests apply patches,
verify repeated preparation and reject altered sources. No network unless the
build helper must fetch an absent source checkout. Never controls a device.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('native_build', ROOT/'tools/build-fuxi-nativemetal.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def run(*args, **kwargs):
    return subprocess.run([str(v) for v in args], check=True, **kwargs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--native', required=True, type=Path)
    ap.add_argument('--metal2vulkan', required=True, type=Path)
    ap.add_argument('--bridge', type=Path, help='host Rust dylib/so: Linux C ABI or Mac actual port tests')
    ap.add_argument('--icd',type=Path,default=Path('/lib/x86_64-linux-gnu/libvulkan.so.1'),help='Vulkan loader exporting vkGetInstanceProcAddr; Linux defaults to system loader + lavapipe')
    ap.add_argument('--compile-only', action='store_true')
    args = ap.parse_args()
    native, m2v = args.native.resolve(), args.metal2vulkan.resolve()
    with tempfile.TemporaryDirectory(prefix='fuxi-native-tests-') as tmp:
        tmp = Path(tmp)
        manifest = build.prepare(native, m2v, tmp/'build')
        build.prepare(native, m2v, tmp/'build')
        assert 'crate-type = ["cdylib"]' in manifest.read_text()
        assert 'staticlib' not in manifest.read_text()
        for source, path in [(native, 'NativeAdrenoMetalPlugin.mm'), (m2v, 'crates/metal2vulkan/src/lib.rs')]:
            file = source/path
            original = file.read_bytes()
            try:
                file.write_bytes(original+b'\n// unexpected input\n')
                try:
                    build.prepare(native, m2v, tmp/'build')
                except SystemExit:
                    pass
                else:
                    raise AssertionError('Unexpected source change accepted')
            finally:
                file.write_bytes(original)
        plugin = (native/'NativeAdrenoMetalPlugin.mm').read_text()
        frontend = (native/'NativeBridgeMetalFrontend.mm').read_text()
        for forbidden in ['vm_write', 'vm_read_overwrite', '0x189429000', '0x143214', '0x1f491',
                          '0x4147', '0x7000', '0x7100', 'sheng-', '/private/var/ngpu',
                          '.publish', '.clients', '.primelist', '.m2vpack', 'pthread_create',
                          'NativeOsLogExport', '#include "CacheExport.h"', 'AdrenoPM4']:
            assert forbidden not in plugin+frontend, forbidden
        assert 'class_getInstanceSize(base)!=528' in frontend
        assert 'fuxiBlocked()' in frontend and 'fuxiNoop=YES' in frontend
        assert frontend.index('class_getInstanceSize(guardedBase)!=528') < frontend.index('gDevice=nmb_device_open(')
        assert 'class_replaceMethod(cls,sel' in frontend
        assert 'nmb_command_buffer_wait(_handle,0)!=0' in frontend
        assert 'storedSurface' in frontend and 'gFuxiHooks.completed' in frontend
        for script in ['tools/build-fuxi-nativemetal.sh', 'tools/nativemetal-rebuild.sh', 'tools/ci/build.sh']:
            run('bash', '-n', ROOT/script)
        for flags in [('none', 'false', 'false', 'false', 'true'), ('all', 'true', 'true', 'true', 'true')]:
            plan = subprocess.check_output(['bash', str(ROOT/'tools/ci/build.sh'), '--plan', *flags], text=True)
            assert 'nativemetal' in plan
        run('bash', ROOT/'tools/ci/build.sh', 'nativemetal', '--dry-run')
        compiler = ['xcrun', 'clang++'] if platform.system() == 'Darwin' else ['clang++']
        run(*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            ROOT/'tools/fuxi-metal/NativeSelectionTests.cpp', '-o', tmp/'selection')
        if not args.compile_only:
            run(tmp/'selection')
        print('PASS: pinned patch stacks, idempotence/tamper rejection, removed iOS17/kernel extras, manifest, CI and selection')
        if platform.system() != 'Darwin':
            if args.bridge:
                run('clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I'+str(native/'bridge'),ROOT/'tools/test-fuxi-nativemetal-bridge.cpp',args.bridge.resolve(),
                    '-Wl,--export-dynamic','-pthread','-o',tmp/'rust-bridge')
                if not args.compile_only:
                    cache=tmp/'cache';cache.mkdir()
                    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',LP_NUM_THREADS='2')
                    lvp=Path('/usr/share/vulkan/icd.d/lvp_icd.x86_64.json')
                    if lvp.is_file() and not any(key in env for key in ('VK_ICD_FILENAMES','VK_DRIVER_FILES')):env['VK_ICD_FILENAMES']=str(lvp)
                    run(tmp/'rust-bridge',args.icd.resolve(),cache,env=env,timeout=30)
            print('SKIP: Mac NativeGateFrontendTests and NativePortTests; no Mac SDK on Linux')
            return
        frameworks = ['-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface']
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-O1', '-g', '-fblocks', '-fsanitize=address,undefined']
        binary = tmp/'native-gate'
        run(*compiler, *flags, '-fno-objc-arc', ROOT/'tools/fuxi-metal/NativeGateFrontendTests.mm', *frameworks, '-o', binary)
        if not args.compile_only:
            def gate(config, bootargs, **extra):
                with tempfile.TemporaryDirectory(dir=tmp) as storage:
                    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', N1_GATE=storage,
                               N1_CONFIG=config, N1_BOOTARGS=bootargs, **extra)
                    run(binary, env=env, timeout=15)
            gate('', 'fuximetal=native fuxibbgpu=1')
            gate('backboardd\n', 'fuximetal=native')
            gate('SpringBoard\n', 'fuximetal=native fuxibbgpu=1', N1_PROCESS='/test/SpringBoard')
            gate('backboardd\n', 'fuximetal=bad fuxibbgpu=1')
            gate('census smoke\n', 'fuximetal=native', N1_PROCESS='/test/smoke')
            for arg in ['fuxibbgpu=1', 'fuxibbgpu=2']:
                gate('backboardd\n', 'fuximetal=native '+arg)
            for option in ['N1_PREFLIGHT_FAIL', 'N1_NIL', 'N1_EXCEPTION']:
                gate('backboardd\n', 'fuximetal=native fuxibbgpu=1', **{option: '1'})
        if not args.bridge:
            raise SystemExit('Mac actual frontend tests require --bridge HOST_DYLIB (build helper --target host)')
        run(*compiler, *flags, '-fobjc-arc', '-I'+str(ROOT/'tools/fuxi-metal'),
            '-DFUXI_NATIVE_FRONTEND="'+str(native/'NativeBridgeMetalFrontend.mm')+'"',
            ROOT/'tools/fuxi-metal/NativePortTests.mm', args.bridge.resolve(), *frameworks, '-o', tmp/'native-port')
        if not args.compile_only:
            run(tmp/'native-port', env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'), timeout=15)
        print('PASS: Mac native tests '+('compiled' if args.compile_only else 'executed'))


if __name__ == '__main__':
    main()
