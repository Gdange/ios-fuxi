#!/bin/bash
# Repack Metal lanes; metal2h preserves the metal2g inputs and cmdline.
# Supply the same private toolchain/firmware environment used for 0098i.
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:?Usage: build-fuxi-metal2-images.sh BASE_ARTIFACT_DIR [metal2a|metal2b|metal2c|metal2e|metal2g|metal2h]}
lane=${2:-metal2a}
[[ $lane == metal2a || $lane == metal2b || $lane == metal2c || $lane == metal2e || $lane == metal2g || $lane == metal2h ]]
if [[ $lane == metal2e || $lane == metal2g || $lane == metal2h ]]; then
 for var in FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG; do
  [[ -n ${!var:-} ]] || { echo "$lane: $var must be set" >&2; exit 1; }
  [[ ${!var} == /* && -f ${!var} ]] || { echo "$lane: $var must be an absolute path to a file" >&2; exit 1; }
  export "$var"
 done
fi
out=xnu/out-fuxi-$lane
mkdir -p "$out" shim/out
[[ -f $out/kernel.development.fuxi && -f $out/kernel.development.fuxi.unstripped ]]
cp "$base/fuxi.adt" "$out/fuxi.adt"
if [[ $lane == metal2g || $lane == metal2h ]]; then
 cp "$base/cmdline.txt" "$out/cmdline.txt"
 python3 - "$out/cmdline.txt" <<'PY2G'
from pathlib import Path
import sys
line=Path(sys.argv[1]).read_bytes().strip()
args=line.split()
assert args.count(b'sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-gpu-abi-test')==1
assert sum(a.startswith(b'sweetexecswap=') for a in args)==1
assert args.count(b'sweetnoexec=bluetoothd,fairplayd.H2')==1
assert sum(a.startswith(b'sweetnoexec=') for a in args)==1
assert b'fuxigpu=1' in args and b'fuxigpuabi=1' in args
assert not any(a.startswith(b'fuxiwlan=') for a in args)
assert len(line)<608
print('metal2g/2h cmdline:',len(line),'bytes; unchanged peakpowermanagerd host; fuxiwlan absent (off)')
PY2G
elif [[ $lane == metal2e ]]; then
 python3 - "$base/cmdline.txt" "$out/cmdline.txt" <<'PY2E'
from pathlib import Path
import sys
args=Path(sys.argv[1]).read_text().split()
swap='sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test'
blocked='sweetnoexec=fairplayd.H2'
assert args.count(swap)==1 and sum(a.startswith('sweetexecswap=') for a in args)==1
assert args.count(blocked)==1 and sum(a.startswith('sweetnoexec=') for a in args)==1
args[args.index(swap)]='sweetexecswap=IOMFB_FDR_Loader:/usr/local/bin/fuxi-gpu-abi-test'
args[args.index(blocked)]='sweetnoexec=bluetoothd,fairplayd.H2'
assert 'sweetexecswap=IOMFB_FDR_Loader:/usr/local/bin/fuxi-gpu-abi-test' in args
assert 'sweetnoexec=bluetoothd,fairplayd.H2' in args
assert 'fuxigpu=1' in args and 'fuxigpuabi=1' in args
assert not any(a.startswith('fuxiwlan=') for a in args)
line=' '.join(args)
assert len(line.encode())<608
Path(sys.argv[2]).write_text(line+'\n')
print('metal2e cmdline:',len(line.encode()),'bytes; root IOMFB_FDR_Loader host; fuxiwlan absent (off)')
PY2E
elif [[ $lane == metal2b || $lane == metal2c ]]; then
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
