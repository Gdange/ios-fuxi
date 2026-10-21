#!/usr/bin/env python3
"""Mac regression with 20H392 public defaults and real external function metadata."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--translator',type=Path,required=True,help='native host ABI-v2 dylib at the pinned revision')
    ap.add_argument('--metallib',type=Path,required=True)
    a=ap.parse_args()
    if os.uname().sysname!='Darwin':raise SystemExit('requires Mac Foundation/Metal')
    with tempfile.TemporaryDirectory() as tmp:
        binary=Path(tmp)/'defaults'
        subprocess.run(['xcrun','clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
            '-fno-objc-arc','-fblocks','-fsanitize=address,undefined',
            '-DFUXI_MTL_TRANSLATOR="'+str(a.translator.resolve())+'"',
            str(ROOT/'tools/fuxi-metal/PipelineDefaultsTests.mm'),'-framework','Foundation','-framework','Metal','-framework','IOSurface','-o',str(binary)],check=True)
        p=subprocess.run([str(binary),str(a.metallib.resolve())],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),
            text=True,capture_output=True,timeout=30)
        assert p.returncode==0,(p.stdout,p.stderr)
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr,p.stderr
        assert 'FUXI MTL: function fuxi_vertex class=FuxiFunction stage=1 device=1' in p.stderr,p.stderr
        assert 'FUXI MTL: function fuxi_fragment class=FuxiFunction stage=2 device=1' in p.stderr,p.stderr
        assert p.stderr.count('texture FAIL M4 CPU model has no rendering')==2,p.stderr
        assert 'translation PASS' not in p.stderr,p.stderr
        for field,value in [('vertexDescriptor.attributes[30].format',31),('rasterSampleCount',4),('colorAttachments[0].blendingEnabled',1)]:
            assert 'FUXI MTL: pipeline reject %s %s\n'%(field,value) in p.stderr,p.stderr
        print(p.stdout,end='');print(p.stderr,end='')


if __name__=='__main__':main()
