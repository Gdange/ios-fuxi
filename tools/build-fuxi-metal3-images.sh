#!/bin/bash
# Repack M3 with the metal2h ADT and explicit firmware, without device access.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-metal3-images.sh METAL2H_ARTIFACT_DIR}
for var in FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG; do
 [[ -n ${!var:-} && ${!var} == /* && -f ${!var} ]] || { echo "metal3: $var must name an existing absolute file" >&2; exit 1; }
 export "$var"
done
out=xnu/out-fuxi-metal3
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
python3 - "$base/cmdline.txt" "$out/cmdline.txt" <<'PY'
from pathlib import Path
import sys
args=Path(sys.argv[1]).read_text().split()
old='sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-gpu-abi-test'
assert args.count(old)==1 and sum(a.startswith('sweetexecswap=') for a in args)==1
assert 'fuxigpu=1' in args and 'fuxigpuabi=1' in args
assert not any(a.startswith('fuxiwlan=') for a in args)
args[args.index(old)]='sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-vk-smoke'
assert sum(a.startswith('sweetnull=') for a in args)<=1
if not any(a.startswith('sweetnull=') for a in args):
    args.append('sweetnull=backboardd,SpringBoard,fuxi-vk-smoke,peakpowermanagerd')
args=[('sweetnull=backboardd,SpringBoard,fuxi-vk-smoke,peakpowermanagerd' if a.startswith('sweetnull=') else a) for a in args]
line=' '.join(args)
assert len(line.encode())<608
Path(sys.argv[2]).write_text(line+'\n')
print('metal3 cmdline:',len(line.encode()),'bytes; smoke stdout/stderr hosted by root daemon')
PY
python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
 shim/out/fuxi-metal3-payload.bin --cmdline "$(cat "$out/cmdline.txt")"
bash shim/fuxi-loader/build.sh shim/out/fuxi-metal3-payload.bin
cp shim/out/fuxi-loader/fuxi-xnu-boot.img shim/out/fuxi-metal3-boot.img
cp shim/out/fuxi-loader/loader.elf shim/out/fuxi-metal3-loader.elf
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" shim/out/fuxi-metal3-payload.bin shim/out/fuxi-metal3-boot.img \
 shim/out/fuxi-metal3-loader.elf > "$out/SHA256SUMS"
