#!/bin/bash
# Repack metal2a from 0098i, or metal2b/metal2c with byte-identical base inputs.
# Supply the same private toolchain/firmware environment used for 0098i.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-metal2-images.sh BASE_ARTIFACT_DIR [metal2a|metal2b|metal2c]}
lane=${2:-metal2a}
[[ $lane == metal2a || $lane == metal2b || $lane == metal2c ]]
out=xnu/out-fuxi-$lane
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
if [[ $lane == metal2b || $lane == metal2c ]]; then
 cp "$base/cmdline.txt" "$out/cmdline.txt"
 python3 - "$out/cmdline.txt" <<'PY2B'
from pathlib import Path
import sys
line=Path(sys.argv[1]).read_bytes().strip()
args=line.split()
assert b'fuxigpu=1' in args and b'fuxigpuabi=1' in args
assert b'sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test' in args
assert not any(a.startswith(b'fuxiwlan=') for a in args)
assert len(line)<608
PY2B
else
python3 - "$base/cmdline.txt" "$out/cmdline.txt" <<'PY'
from pathlib import Path
import sys
args=Path(sys.argv[1]).read_text().split()
assert 'fuxigpu=1' in args and 'fuxigpuabi=1' in args
assert not any(a.startswith(('sweetexecswap=', 'fuxiwlan=')) for a in args)
assert sum(a.startswith('sweetnoexec=') for a in args)==1
for i,arg in enumerate(args):
    if arg.startswith('sweetnoexec='):
        names=arg.split('=',1)[1].split(',')
        assert names.count('bluetoothd')==1
        args[i]='sweetnoexec='+','.join(n for n in names if n!='bluetoothd')
args.append('sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test')
line=' '.join(args)
assert len(line.encode())<608
Path(sys.argv[2]).write_text(line+'\n')
print('metal2a cmdline:',len(line.encode()),'bytes; fuxiwlan absent (off)')
PY
fi
python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
 shim/out/fuxi-$lane-payload.bin --cmdline "$(cat "$out/cmdline.txt")"
bash shim/fuxi-loader/build.sh shim/out/fuxi-$lane-payload.bin
cp shim/out/fuxi-loader/fuxi-xnu-boot.img shim/out/fuxi-$lane-boot.img
cp shim/out/fuxi-loader/loader.elf shim/out/fuxi-$lane-loader.elf
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" shim/out/fuxi-$lane-payload.bin shim/out/fuxi-$lane-boot.img \
 shim/out/fuxi-$lane-loader.elf tools/out/fuxi-gpu-abi-test > "$out/SHA256SUMS"
