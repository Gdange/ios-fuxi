#!/bin/bash
# Repack metal2a with the exact 0098i ADT and only the exec routing change.
# Supply the same private toolchain/firmware environment used for 0098i.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-metal2-images.sh BASE_0098I_ARTIFACT_DIR}
out=xnu/out-fuxi-metal2a
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
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
python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
 shim/out/fuxi-metal2a-payload.bin --cmdline "$(cat "$out/cmdline.txt")"
bash shim/fuxi-loader/build.sh shim/out/fuxi-metal2a-payload.bin
cp shim/out/fuxi-loader/fuxi-xnu-boot.img shim/out/fuxi-metal2a-boot.img
cp shim/out/fuxi-loader/loader.elf shim/out/fuxi-metal2a-loader.elf
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" shim/out/fuxi-metal2a-payload.bin shim/out/fuxi-metal2a-boot.img \
 shim/out/fuxi-metal2a-loader.elf tools/out/fuxi-gpu-abi-test > "$out/SHA256SUMS"
