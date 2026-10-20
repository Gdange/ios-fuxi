#!/usr/bin/env python3
"""Compile smoke GLSL into an embedded C array header (external glslang required)."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]


def generate(out, validator='glslangValidator'):
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated from tools/shaders by glslangValidator; do not edit. */', '#include <stdint.h>']
    with tempfile.TemporaryDirectory() as tmp:
        for stage in ('comp', 'vert', 'frag'):
            binary = Path(tmp)/(stage+'.spv')
            subprocess.run([validator, '-V', '--target-env', 'vulkan1.1', '-o', str(binary),
                            str(ROOT/'tools/shaders'/('vk-smoke.'+stage))], check=True)
            raw = binary.read_bytes()
            words = struct.unpack('<%dI' % (len(raw)//4), raw)
            assert words[0] == 0x07230203
            lines += ['static const uint32_t smoke_'+stage+'[] = {']
            lines += ['    '+', '.join('0x%08xu' % w for w in words[i:i+8])+',' for i in range(0,len(words),8)]
            lines += ['};']
    out.write_text('\n'.join(lines)+'\n')


if __name__ == '__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out',type=Path);ap.add_argument('--validator',default='glslangValidator')
    args=ap.parse_args();generate(args.out,args.validator)
