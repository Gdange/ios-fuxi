#!/bin/bash
# Repack WLAN M5 with the metal2a ADT/cmdline and its loader packing.
# Provide the same private toolchain/GPU firmware environment as the base build.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-wlan5-images.sh BASE_METAL2A_ARTIFACT_DIR}
out=xnu/out-fuxi-wlan5
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
python3 - "$base/cmdline.txt" "$out/cmdline.txt" <<'PY'
from pathlib import Path
import sys
line = Path(sys.argv[1]).read_text().strip()
assert not any(a.startswith('fuxiwlan=') for a in line.split())
line += ' fuxiwlan=1'
assert len(line.encode()) < 608
Path(sys.argv[2]).write_text(line + '\n')
print('WLAN M5 cmdline:', len(line.encode()), 'bytes')
PY
python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
 shim/out/fuxi-wlan-m5-payload.bin --cmdline "$(cat "$out/cmdline.txt")"
bash shim/fuxi-loader/build.sh shim/out/fuxi-wlan-m5-payload.bin
cp shim/out/fuxi-loader/fuxi-xnu-boot.img shim/out/fuxi-wlan-m5-boot.img
cp shim/out/fuxi-loader/loader.elf shim/out/fuxi-wlan-m5-loader.elf
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" "$out/cmdline.txt" shim/out/fuxi-wlan-m5-payload.bin \
 shim/out/fuxi-wlan-m5-boot.img shim/out/fuxi-wlan-m5-loader.elf > "$out/SHA256SUMS"
