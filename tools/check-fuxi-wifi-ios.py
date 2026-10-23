#!/usr/bin/env python3
"""Cross-check P-WIFI syntax with an existing trusted XNU buildtree; never builds/downloads it.

BUILDTREE contains xnu/, fakeroot/, build/xnu.obj and sdk-kernel-platform/.
Only canonical Fuxi sources are compiled. Makefiles are read as text for defines.
Apple-only type-signature builtins use the same substitutions as xnu-cc.py.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'xnu/fuxi-wifi-ios'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('buildtree', type=Path)
    ap.add_argument('--config', default='DEVELOPMENT_ARM64_FUXI')
    ap.add_argument('--header-machine', choices=('FUXI', 'SWEET'), default='FUXI',
                    help='SWEET supports older local artifacts with the same arm64 header ABI')
    ap.add_argument('--clang', default='clang')
    ap.add_argument('--clangxx', default='clang++')
    args = ap.parse_args()
    tree = args.buildtree.resolve()
    xnu = tree / 'xnu'
    objects = tree / 'build/xnu.obj'
    config = objects / args.config
    with tempfile.TemporaryDirectory(prefix='fuxi-wifi-headers-') as tmp:
        overlay = Path(tmp)
        (overlay / 'sys').mkdir()
        for header in SOURCE.glob('*.h'):
            (overlay / 'sys' / header.name).symlink_to(header)
        for source in sorted(SOURCE.iterdir()):
            if source.suffix not in ('.c', '.cpp'):
                continue
            cpp = source.suffix == '.cpp'
            component = 'iokit' if cpp else 'bsd'
            work = config / component / 'DEVELOPMENT'
            makefile = (work / 'Makefile').read_text()
            defines = shlex.split(re.search(r'^export CONFIG_DEFINES = (.+)$', makefile, re.M)[1])
            flags = [args.clangxx if cpp else args.clang, '-target', 'arm64-apple-macos13',
                     '-fsyntax-only', '-nostdinc', '-ffreestanding', '-fblocks', '-Wall', '-Wextra',
                     '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-Wno-reorder-init-list',
                     '-Wno-c99-designator', '-DFUXI=1', '-D' + args.header_machine + '=1',
                     '-DPRIVATE=1', '-DAPPLE=1', '-DXNU_KERN_EVENT_DATA_IS_VLA=1',
                     '-DKERNEL=1', '-DKERNEL_PRIVATE=1', '-DXNU_KERNEL_PRIVATE=1',
                     '-D__APPLE_API_PRIVATE=1', '-Darm64=1', '-DARM64=1', '-D__ARM64__=1',
                     '-DLP64=1', '-DPAGE_SIZE_FIXED=1', '-include', 'meta_features.h',
                     '-D__builtin_xnu_type_signature(...)=""',
                     '-D__builtin_xnu_type_summary(...)=0',
                     '-D__builtin_xnu_types_compatible(...)=1', *defines]
            if cpp:
                flags += ['-std=gnu++14', '-fno-exceptions', '-fno-rtti', '-DIOKIT_BUILD']
            else:
                flags += ['-std=gnu11', '-Werror=implicit-function-declaration', '-DDRIVER_PRIVATE',
                          '-D_KERNEL_BUILD', '-DKERNEL_BUILD', '-DMACH_KERNEL', '-DBSD_BUILD',
                          '-DBSD_KERNEL_PRIVATE', '-DLP64_DEBUG=0']
            includes = [overlay, work, xnu / component]
            includes += [objects / 'EXPORT_HDRS' / name
                         for name in ('bsd', 'osfmk', 'iokit', 'libkern', 'pexpert', 'libsa')]
            includes += [tree / 'fakeroot/usr/local/include', xnu / 'bsd', xnu / 'osfmk',
                         xnu / 'iokit', xnu / 'libkern', xnu / 'pexpert', xnu / 'EXTERNAL_HEADERS',
                         tree / 'sdk-kernel-platform']
            for path in includes:
                flags += ['-I', str(path)]
            subprocess.run([*flags, str(source)], cwd=work, check=True)
            print(f'PASS: arm64 XNU syntax {source.name}', flush=True)


if __name__ == '__main__':
    main()
