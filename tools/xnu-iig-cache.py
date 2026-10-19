#!/usr/bin/env python3
"""Replay matching upstream IIG-generated source when building with newer Xcode.

Prepare from the original CI source and object tree:
  xnu-iig-cache.py --prepare XNU OBJROOT CACHE
Then pass IIG=/absolute/path/to/xnu-iig-cache.py and XNU_IIG_CACHE=CACHE to make.
No kernel ABI is changed. Edited .iig input is refused and must be regenerated
with the matching IIG version instead.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(source, objects, cache):
    if cache.exists():
        raise ValueError('cache destination already exists')
    entries = {}
    for path in sorted((source / 'iokit/DriverKit').glob('*.iig')):
        stem = path.stem
        candidates = sorted(objects.rglob(stem + '.iig.cpp'))
        if not candidates:
            raise ValueError(f'no generated implementation for {path.name}')
        implementations = {digest(p) for p in candidates}
        if len(implementations) != 1:
            raise ValueError(f'conflicting generated implementations for {stem}')
        impl = candidates[0]
        header = impl.with_name(stem + '.h')
        if not header.is_file():
            raise ValueError(f'no generated header for {stem}')
        entries[path.name] = {'source_sha256': digest(path),
                             'header_sha256': digest(header),
                             'impl_sha256': digest(impl)}
    if not entries:
        raise ValueError('no IIG inputs found')
    cache.mkdir(parents=True)
    for name in entries:
        impl = sorted(objects.rglob(Path(name).stem + '.iig.cpp'))[0]
        shutil.copyfile(impl, cache / impl.name)
        shutil.copyfile(impl.with_name(Path(name).stem + '.h'),
                        cache / (Path(name).stem + '.h'))
    (cache / 'manifest.json').write_text(json.dumps(entries, indent=2) + '\n')
    print(f'{cache}: {len(entries)} source/header/implementation pairs')


def replay(argv):
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument('--def', dest='definition', type=Path, required=True)
    ap.add_argument('--header', type=Path, required=True)
    ap.add_argument('--impl', type=Path, required=True)
    args, _ = ap.parse_known_args(argv)
    cache = Path(os.environ['XNU_IIG_CACHE'])
    entries = json.loads((cache / 'manifest.json').read_text())
    entry = entries[args.definition.name]
    if digest(args.definition) != entry['source_sha256']:
        raise ValueError(f'{args.definition}: source changed; cannot replay generated code')
    stem = args.definition.stem
    for src, dst, key in ((cache / (stem + '.h'), args.header, 'header_sha256'),
                          (cache / (stem + '.iig.cpp'), args.impl, 'impl_sha256')):
        if digest(src) != entry[key]:
            raise ValueError(f'{src}: generated file hash mismatch')
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)


def main():
    try:
        if len(sys.argv) == 5 and sys.argv[1] == '--prepare':
            prepare(*(Path(p) for p in sys.argv[2:]))
        else:
            replay(sys.argv[1:])
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(f'IIG cache: {error}') from error


if __name__ == '__main__':
    main()
