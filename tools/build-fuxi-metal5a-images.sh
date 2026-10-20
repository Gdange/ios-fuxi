#!/bin/bash
# Repack unchanged M4 kernel/ADT/cmdline. Firmware remains external and ignored.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-metal5a-images.sh METAL4_ARTIFACT_DIR}
for var in FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG; do
 [[ -n ${!var:-} && ${!var} == /* && -f ${!var} ]] || { echo "metal5a: $var must name an existing absolute file" >&2; exit 1; }
 export "$var"
done
out=xnu/out-fuxi-metal5a
mkdir -p "$out" shim/out
for file in kernel.development.fuxi kernel.development.fuxi.unstripped fuxi.adt cmdline.txt; do
 cp "$base/$file" "$out/$file"
 cmp "$base/$file" "$out/$file"
done
python3 - "$out/cmdline.txt" <<'PY'
from pathlib import Path
import sys
args=Path(sys.argv[1]).read_text().split()
assert args.count('sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-metal-smoke')==1
assert 'fuxigpuabi=1' in args and 'fuxigpu=1' in args
assert not any(a.startswith('fuxiwlan=') for a in args)
assert len(' '.join(args).encode())<608
PY
python3 tools/mkpayload.py "$out/kernel.development.fuxi" "$out/fuxi.adt" \
 shim/out/fuxi-metal5a-payload.bin --cmdline "$(cat "$out/cmdline.txt")"
bash shim/fuxi-loader/build.sh shim/out/fuxi-metal5a-payload.bin
cp shim/out/fuxi-loader/fuxi-xnu-boot.img shim/out/fuxi-metal5a-boot.img
cp shim/out/fuxi-loader/loader.elf shim/out/fuxi-metal5a-loader.elf
sha256sum "$out/kernel.development.fuxi" "$out/kernel.development.fuxi.unstripped" \
 "$out/fuxi.adt" shim/out/fuxi-metal5a-payload.bin shim/out/fuxi-metal5a-boot.img \
 shim/out/fuxi-metal5a-loader.elf > "$out/SHA256SUMS"
