#!/usr/bin/env python3
"""Qualify local QuartzCore fixed pipelines with OUR bridge + Backend on lavapipe.

Requires a clean, external pinned translator and cached Cargo dependencies.
Apple inputs/products remain private; output is counts, bounded refusal labels
and hashes. No device, network, native Mac test, or CI operation is performed.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked(command, **kwargs):
    p = subprocess.run(command, capture_output=True, **kwargs)
    if p.returncode:
        raise RuntimeError('tool FAIL rc=%d diagnostic_sha256=%s' % (p.returncode, digest(p.stdout + p.stderr)))
    return p.stdout


def reason(raw):
    # Never print arbitrary translator diagnostics (they may contain IR). Adapter
    # refusals consist of fixed labels/numeric fields; preserve only those here.
    text = raw.decode(errors='replace').splitlines()[0]
    safe = ('M6 dependency ', 'M6 plan ', 'M6 stage ', 'M6 sets ', 'M6 set=', 'M6 graphics ',
            'unsupported planned descriptor source ', 'draw requires ', 'constant blob ',
            'unbound buffer', 'buffer slot', 'buffer stage', 'sample view/slot',
            'unsupported draw resources', 'unsupported neutral ', 'M6 descriptor ', 'M6 draw ', 'shader resource writes unsupported',
            'vertex input bound', 'push range', 'specialization bound')
    if text.startswith(safe) and re.fullmatch(r'[A-Za-z0-9_ /:,.=?!<>()\[\]{}|&*-]+', text):
        return text
    # Preserve error kinds, never arbitrary symbols/locations from a compiler.
    external = re.match(r'(unsupported|invalid descriptor|needs pipeline input|limit exceeded|budget exceeded): ([A-Za-z][A-Za-z0-9_]*)', text)
    if external:
        return external[1] + '/' + external[2] + '/sha256=' + digest(raw)
    return 'diagnostic-sha256=' + digest(raw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('library', type=Path, help='local default.metallib or pipelinelib functions container')
    ap.add_argument('--source', type=Path, required=True, help='clean external metal2vulkan checkout')
    ap.add_argument('--output', type=Path, default=ROOT / 'research/metal-m9f/offline')
    ap.add_argument('--icd', type=Path, required=True, help='lavapipe shared library')
    ap.add_argument('--headers', type=Path, help='optional Vulkan include root')
    ap.add_argument('--toggle-limit', type=int, default=0, help='0 means every declared constant; bounds expensive generic sweeps')
    ap.add_argument('--pairs', choices=['all', 'spc', 'gen'], default='all')
    ap.add_argument('--ca-pool', action='store_true', help='256 KiB pools with nonzero 256-byte binding offsets')
    ap.add_argument('--sweep', choices=['none', 'toggles'], default='toggles')
    ap.add_argument('--timeout', type=int, default=120)
    ap.add_argument('--pair-limit', type=int, default=0, help='0 means all four pairs')
    a = ap.parse_args()
    if a.toggle_limit < 0 or a.pair_limit < 0 or not 1 <= a.timeout <= 1800:
        ap.error('invalid case/time bounds')
    spec = importlib.util.spec_from_file_location('inventory', ROOT / 'tools/inventory-metal-ca.py')
    inventory = importlib.util.module_from_spec(spec);spec.loader.exec_module(inventory)
    out = inventory.private_output(a.output)
    pin = re.search(r'^pin=([0-9a-f]{40})$', (ROOT / 'tools/build-fuxi-metal-translator.sh').read_text(), re.M)[1]
    if checked(['git', '-C', str(a.source), 'rev-parse', 'HEAD']).decode().strip() != pin or checked(['git', '-C', str(a.source), 'status', '--porcelain', '--untracked-files=no']):
        ap.error('external translator must be clean at the recipe pin')
    build = out / 'bridge';(build / 'src').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'tools/fuxi-metal2vulkan-bridge.rs', build / 'src/lib.rs')
    shutil.copyfile(ROOT / 'tools/metal-ca-offline-info.rs', build / 'src/main.rs')
    (build / 'Cargo.toml').write_text('''[package]
name="fuxi-metal-ca-offline"
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
    checked(['cargo', '+1.98.0', 'build', '--offline', '--manifest-path', str(build / 'Cargo.toml')])
    runner = out / 'runner'
    checked(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
             *(['-I' + str(a.headers)] if a.headers else []), '-I' + str(ROOT / 'tools'), '-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_CA_OFFLINE', '-DFUXI_MTL_HOST_FENCE_NS=120000000000ULL',
             '-DFUXI_MTL_ICD=' + json.dumps(str(a.icd.resolve())),
             '-DFUXI_MTL_TRANSLATOR=' + json.dumps(str(build.resolve() / 'target/debug/libmetal2vulkan.so')),
             str(ROOT / 'tools/test-fuxi-metal-ca-offline.cpp'), '-ldl', '-pthread', '-o', str(runner)])
    data = (a.library / 'functions' if a.library.is_dir() else a.library).read_bytes()
    blobs = list(inventory.embedded_libraries(data))
    if not blobs:
        ap.error('no bounded MTLB library containers found')
    results = []; registry = {}
    for n, blob in enumerate(blobs):
        path = out / ('input-%04d.metallib' % n);path.write_bytes(blob)
        metadata = checked([str(build / 'target/debug/fuxi-metal-ca-offline'), str(path)])
        functions = defaultdict(list)
        for line in metadata.decode().splitlines():
            if line.startswith('F\t'):
                functions[line[2:]] = [];continue
            name, index, ty, cn = line.split('\t')
            functions[name].append((index if index != 'name' else cn, int(ty), cn))
        for name, cs in functions.items():
            if name not in registry or len(cs)>len(registry[name][2]):
                registry[name] = (path, digest(blob), cs)
    pairs = [(v, f) for v in ('fixed_vert_lph_spc', 'fixed_vert_lph_gen')
             for f in ('fixed_frag_lph_cpf', 'fixed_frag_lph_cph') if v in registry and f in registry and (a.pairs == 'all' or v.endswith('_' + a.pairs))]
    for v, f in pairs[:a.pair_limit or None]:
        vp, vh, vc = registry[v];fp, fh, fc = registry[f]
        constants = [vc, fc]
        # Static iOS 16.7 Surface, no images/filters/color conversion. Blend
        # source-over and NONE are both qualified; opaque solid can select
        # either after state/blend simplification. See metal-m9f.md.
        base = {'fc_framebuffer_fetch': 1, 'fc_attachment_count': 1,
                'fc_blend_function': 3, 'fc_dest_count': 1, 'fc_memoryless_index0': 255, 'fc_memoryless_index1': 255,
                'fc_generic': int(v.endswith('_gen'))}
        cases = [('solid-over', base), ('solid-copy', dict(base, fc_blend_function=54, fc_dest_count=0)), ('image', dict(base, fc_texture_function=1, fc_image_count=1, fc_has_image_0A=1, fc_plane_count0=1, fc_texcoord_count0=1)), ('two-image', dict(base, fc_texture_function=5, fc_image_count=2, fc_has_image_0A=1, fc_has_image_1A=1, fc_plane_count0=1, fc_plane_count1=1, fc_texcoord_count0=1, fc_texcoord_count1=1)), ('zero', {})]
        if a.sweep == 'toggles':
            for cn in sorted({c[2] for s in constants for c in s})[:a.toggle_limit or None]:
                cases.append(('toggle/' + cn, {cn: 1}))
        for label, values in cases:
            lines = []
            for stage, cs in enumerate(constants):
                for index, ty, cn in cs:
                    size = 3 if ty == 51 else 1 if ty in (49, 53) else 0
                    if not size:
                        raise RuntimeError('unsupported constant type count=1')
                    lines.append('%d %s %d %d %d\n' % (stage, index, ty, size, values.get(cn, 0)))
            payload = ''.join(lines).encode()
            row = dict(vertex_library_sha256=vh, fragment_library_sha256=fh, pair=v + '/' + f, case=label,
                       constants_sha256=digest(payload), ca_pool=a.ca_pool)
            try:
                p = subprocess.run([str(runner), str(vp), v, f, str(fp), *(['arena'] if a.ca_pool else [])], input=payload,
                                   capture_output=True, timeout=a.timeout,
                                   env=dict(os.environ, LP_NUM_THREADS='2', GALLIVM_PERF='nopt'))
                row['diagnostic_sha256'] = digest(p.stdout + p.stderr)
                row['blob_bindings'] = [[int(v) for v in m] for m in re.findall(rb'^blob_binding (\d+) (\d+) (\d+) (\d+) (\d+)$', p.stdout, re.M)]
                immutable = re.search(rb'^immutable_count (\d+)$', p.stdout, re.M)
                row['immutable_samplers'] = int(immutable[1]) if immutable else 0
                row['blob_sha256'] = [h.decode() for h in re.findall(rb'^blob_sha256 ([0-9a-f]{64})$', p.stdout, re.M)]
                match = re.search(rb'^PASS (\d+) (\d+) ([0-9a-f]+)$', p.stdout, re.M)
                counts = re.search(rb'^counts (\d+) (\d+) (\d+)$', p.stdout, re.M)
                if counts:
                    row.update(descriptors=int(counts[1]), blobs=int(counts[2]), fetch_mask=int(counts[3]))
                if match and p.returncode == 0:
                    row.update(result='PASS', solid_pixels=int(match[1]), changed_pixels=int(match[2]), pixel_fnv1a64=match[3].decode())
                    if label.startswith('solid-') and int(match[1]) != 65536:
                        row['result'] = 'FAIL/pixel-oracle'
                else:
                    fail = re.search(rb'^FAIL (\w+) (.*)$', p.stdout, re.M)
                    row['result'] = 'FAIL/' + (fail[1].decode() if fail else 'process')
                    row['reason'] = reason(fail[2]) if fail else 'process-count=%d' % p.returncode
            except subprocess.TimeoutExpired as exc:
                row['result'] = 'FAIL/timeout'
                partial = exc.stdout or b''
                row['phase'] = 'draw' if b'counts ' in partial else 'compile-or-device'
                row['diagnostic_sha256'] = digest(partial + (exc.stderr or b''))
            results.append(row)
            print(json.dumps(row, sort_keys=True), flush=True)
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    counts = Counter(r['result'] for r in results)
    summary = dict(counts=counts, cases=len(results), libraries=len(blobs), translator_revision=pin,
                   input_sha256=digest(data), icd_sha256=digest(a.icd.read_bytes()), results_sha256=digest(json.dumps(results, sort_keys=True).encode()))
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, sort_keys=True))
    if not results or any(r['result'] != 'PASS' for r in results if r['case'].startswith('solid-')):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
