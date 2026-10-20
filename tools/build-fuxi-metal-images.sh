#!/bin/bash
# Pack this lane's native kernel using the unchanged 0097b ADT/cmdline.
# Firmware/toolchain paths come from the caller's private build environment.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-metal-images.sh BASE_0097B_ARTIFACT_DIR}
out=xnu/out-fuxi-metal1
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
python3 - "$base/cmdline.txt" "$out/cmdline.txt" <<'PY'
from pathlib import Path
import sys
base=Path(sys.argv[1]).read_text().strip()
assert 'fuxigpu=1' in base.split() and 'fuxiperf=1' in base.split()
assert not any(arg.startswith('fuxigpuabi=') for arg in base.split())
line=base+' fuxigpuabi=1'
assert len(line.encode())<608
Path(sys.argv[2]).write_text(line+'\n')
PY
python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
 shim/out/fuxi-metal1-payload.bin --cmdline "$(cat "$out/cmdline.txt")"
bash shim/fuxi-loader/build.sh shim/out/fuxi-metal1-payload.bin
cp shim/out/fuxi-loader/fuxi-xnu-boot.img shim/out/fuxi-metal1-boot.img
cp shim/out/fuxi-loader/loader.elf shim/out/fuxi-metal1-loader.elf
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" shim/out/fuxi-metal1-payload.bin shim/out/fuxi-metal1-boot.img \
 tools/out/fuxi-gpu-abi-test > "$out/SHA256SUMS"
