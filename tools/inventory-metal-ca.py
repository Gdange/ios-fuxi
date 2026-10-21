#!/usr/bin/env python3
"""Offline QuartzCore inventory and pinned translator census; no device access.

All output (including compiler logs and Apple resources) must be git-ignored.
Requires local ipsw, 7z (for a root image), Rust 1.98 and cached Cargo crates.
Only names/counts/typed result categories are suitable for a tracked summary.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def private_output(path):
    path = path.resolve()
    relative = path.relative_to(ROOT)
    if relative.parts[0] not in ('research', 'deps', 'tools'):
        raise ValueError('output must be in ignored research/, deps/ or tools/out/')
    subprocess.run(['git', 'check-ignore', '--no-index', '-q', str(relative) + '/'],
                   cwd=ROOT, check=True)
    if subprocess.check_output(['git', 'ls-files', '--', str(relative)], cwd=ROOT):
        raise ValueError('output directory contains tracked files')
    path.mkdir(parents=True, exist_ok=True)
    return path


def embedded_libraries(data):
    """Carve bounded MTLB containers, not AIR or target-specific GPU executables."""
    start = 0
    while (offset := data.find(b'MTLB', start)) >= 0:
        start = offset + 4
        if offset + 88 > len(data):
            continue
        size = struct.unpack_from('<Q', data, offset + 16)[0]
        if not 88 <= size <= len(data) - offset:
            continue
        table, length = struct.unpack_from('<QQ', data, offset + 24)
        if table < 88 or length < 4 or table + length > size:
            continue
        count = struct.unpack_from('<I', data, offset + table)[0]
        if not 0 < count <= 65536:
            continue
        yield data[offset:offset + size]
        start = offset + size


def extract_resource(image, suffix, target):
    # 7z reads HFS+ directly; no mount, journal replay, or root-image modification.
    listing = subprocess.check_output(['7z', 'l', '-slt', str(image)], text=True)
    names = [line.removeprefix('Path = ') for line in listing.splitlines()
             if line.startswith('Path = ') and line.endswith(suffix)]
    if len(names) != 1:
        raise ValueError('root image must contain exactly one requested resource')
    with target.open('wb') as out:
        subprocess.run(['7z', 'e', '-so', str(image), names[0]], stdout=out,
                       stderr=subprocess.PIPE, check=True)


def cache_inventory(a, out):
    commands = {'loads': ['macho', str(a.dsc), 'QuartzCore', '-l'],
                'symbols': ['macho', str(a.dsc), 'QuartzCore', '-n'],
                'strings': ['macho', str(a.dsc), 'QuartzCore', '-s'],
                'quartzcore.disass': ['disass', str(a.dsc), '--image', 'QuartzCore',
                                     '--demangle', '--cache', str(out / 'symbol-cache.a2s')]}
    for name, command in commands.items():
        target = out / (name if name.endswith('.disass') else name + '.txt')
        if not (a.reuse and target.exists()):
            with target.open('w') as f, (out / (name + '.log')).open('w') as log:
                subprocess.run([a.ipsw, 'dyld', *command], stdout=f, stderr=log, check=True)
    text = (out / 'quartzcore.disass').read_text()
    functions = re.split(r'\n\n(?=\S)', text)
    groups = {}
    for group, prefixes in {
        'metal': ('CA::OGL::Metal', 'CA::OGL::new_metal_context', 'load_shader_function('),
        'drawables': ('-[CAMetal', 'layer_private_present('),
    }.items():
        chosen = '\n'.join(f for f in functions if f.startswith(prefixes))
        direct = re.findall(r'objc_msgSend\(x0, "([^"\n]+)"', chosen)
        # MetalBufferPool caches IMPs via methodForSelector:, then calls them
        # indirectly. Include those annotated selector operands as well.
        indirect = re.findall(r';\s*([A-Za-z_]\w*(?::\w*)+:)\s*$', chosen, re.M)
        groups[group] = sorted(set(direct + indirect))
        groups[group + '_c_apis'] = sorted(set(re.findall(
            r'\b(?:bl|b)\s+(?:j_)?_((?:IOSurface|IOMobileFramebuffer|MTL)\w+?)(?:_stub)?\s*$', chosen, re.M)))
    (out / 'selectors.json').write_text(json.dumps(groups, indent=2) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True, help='clean external translator checkout at recipe pin')
    ap.add_argument('--output', type=Path, default=ROOT / 'research/metal-m8')
    ap.add_argument('--dsc', type=Path, help='main dyld cache; subcaches remain read-only')
    ap.add_argument('--ipsw', default='ipsw')
    ap.add_argument('--root-image', type=Path, help='optional local HFS+ root image')
    ap.add_argument('--metallib', type=Path, help='alternatively, already extracted default.metallib')
    ap.add_argument('--pipeline-functions', type=Path, help='optional default.pipelinelib/functions container')
    ap.add_argument('--reuse', action='store_true', help='reuse private cache analysis only')
    ap.add_argument('--icd', type=Path, help='Vulkan ICD JSON, e.g. lavapipe')
    a = ap.parse_args()
    out = private_output(a.output)
    if a.dsc and out.is_relative_to(a.dsc.resolve().parent):
        ap.error('output must be outside the read-only cache directory')
    pin = re.search(r'^pin=([0-9a-f]{40})$', (ROOT / 'tools/build-fuxi-metal-translator.sh').read_text(), re.M)[1]
    def git(*args):
        return subprocess.check_output(['git', '-C', str(a.source), *args], text=True).strip()
    if git('rev-parse', 'HEAD') != pin or git('status', '--porcelain', '--untracked-files=no'):
        ap.error('external translator must be clean at the recipe pin')
    if a.dsc:
        cache_inventory(a, out)
    library = out / 'default.metallib'
    catalog = out / 'pipeline-functions'
    if a.root_image:
        extract_resource(a.root_image, '/QuartzCore.framework/default.metallib', library)
        extract_resource(a.root_image, '/QuartzCore.framework/default.pipelinelib/functions', catalog)
    elif a.metallib:
        if a.metallib.resolve() != library:
            shutil.copyfile(a.metallib, library)
    else:
        ap.error('supply --root-image or --metallib; the DSC contains no bundle resources')
    if a.pipeline_functions and a.pipeline_functions.resolve() != catalog:
        shutil.copyfile(a.pipeline_functions, catalog)
    libraries = [library]
    if a.root_image or a.pipeline_functions:
        directory = out / 'specialized'
        directory.mkdir(exist_ok=True)
        for i, blob in enumerate(embedded_libraries(catalog.read_bytes())):
            target = directory / f'catalog-{i:04d}.metallib'
            target.write_bytes(blob)
            libraries.append(target)
    build = out / 'probe'
    (build / 'src').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'tools/metal-ca-probe.rs', build / 'src/main.rs')
    shutil.copyfile(ROOT / 'tools/fuxi-metal2vulkan-bridge.rs', build / 'src/bridge.rs')
    dependency = json.dumps(str(a.source.resolve() / 'crates/metal2vulkan'))
    (build / 'Cargo.toml').write_text('''[package]
name="fuxi-ca-probe"
version="0.1.0"
edition="2021"
[dependencies]
metal2vulkan={path=''' + dependency + ''',features=["ash"]}
ash={version="0.38",default-features=false,features=["std"]}
serde_json="1"
''')
    with (out / 'probe-build.log').open('w') as log:
        subprocess.run(['cargo', '+1.98.0', 'build', '--offline', '--manifest-path', str(build / 'Cargo.toml')],
                       stdout=log, stderr=log, check=True)
    import os
    env = dict(os.environ)
    if a.icd:
        env['VK_ICD_FILENAMES'] = str(a.icd.resolve())
    with (out / 'functions.jsonl').open('w') as rows, (out / 'probe.log').open('w') as log:
        subprocess.run([str(build / 'target/debug/fuxi-ca-probe'), *map(str, libraries)],
                       stdout=rows, stderr=log, env=env, timeout=3600, check=True)
    rows = [json.loads(line) for line in (out / 'functions.jsonl').read_text().splitlines()]
    summary = {'translator_revision': pin, 'libraries': len(libraries),
               'default_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
               'catalog_records': len(libraries) - 1,
               'catalog_unique_containers': len({hashlib.sha256(p.read_bytes()).digest() for p in libraries[1:]}),
               'records': len(rows), 'stages': dict(Counter(r['stage'] for r in rows)),
               'results': dict(Counter(c['case'] + '/' + c['result'] for r in rows for c in r['attempts']))}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    # This is a diagnostic table, never an exported shader product.
    with (out / 'results.tsv').open('w') as table:
        table.write('library\tfunction\tstage\tconstants\tundefined\tzero-typed\tbridge-zero-pair\n')
        for r in rows:
            table.write('\t'.join([r['library'], r['name'], r['stage'], str(len(r['constants'])),
                                   *[c['result'] for c in r['attempts']]]) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
