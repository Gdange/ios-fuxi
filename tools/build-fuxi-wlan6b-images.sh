#!/bin/bash
# Repack WLAN M6b with M6's ADT/header/firmware, at full and pre-DMA limits.
# Provide the same private toolchain/GPU firmware environment as the base build.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-wlan6b-images.sh BASE_METAL2A_ARTIFACT_DIR}
out=xnu/out-fuxi-wlan6b
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
for limit in 1 20; do
 suffix=
 [[ $limit == 1 ]] || suffix=-s20
 python3 - "$base/cmdline.txt" "$out/cmdline$suffix.txt" "$limit" <<'PY'
from pathlib import Path
import sys
line = Path(sys.argv[1]).read_text().strip()
assert not any(a.startswith('fuxiwlan=') for a in line.split())
line += ' fuxiwlan=' + sys.argv[3]
assert len(line.encode()) < 608
Path(sys.argv[2]).write_text(line + '\n')
print('WLAN M6b cmdline:', len(line.encode()), 'bytes; limit', sys.argv[3])
PY
 image=shim/out/fuxi-wlan-m6b$suffix
 python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
  "$image-payload.bin" --cmdline "$(cat "$out/cmdline$suffix.txt")"
 bash shim/fuxi-loader/build.sh "$image-payload.bin"
 cp shim/out/fuxi-loader/fuxi-xnu-boot.img "$image-boot.img"
 cp shim/out/fuxi-loader/loader.elf "$image-loader.elf"
done
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" "$out/cmdline.txt" "$out/cmdline-s20.txt" \
 shim/out/fuxi-wlan-m6b-{payload.bin,boot.img,loader.elf} \
 shim/out/fuxi-wlan-m6b-s20-{payload.bin,boot.img,loader.elf} > "$out/SHA256SUMS"
