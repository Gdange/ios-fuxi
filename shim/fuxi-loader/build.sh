#!/usr/bin/env bash
set -euo pipefail
payload=$(realpath "${1:?Usage: build.sh FUXI_PAYLOAD}")
cd "$(dirname "$0")"
: "${CLANG:?Set CLANG to an AArch64-capable clang}"
: "${LLD:?Set LLD to ld.lld}"
: "${OBJCOPY:?Set OBJCOPY to llvm-objcopy}"
: "${READELF:?Set READELF to llvm-readelf}"
: "${MKBOOTIMG:?Set MKBOOTIMG to AOSP mkbootimg.py}"
out=../out/fuxi-loader
mkdir -p "$out"
# Reject a payload made from a different board, truncated segments, or unsafe RAM.
python3 - "$payload" <<'PY'
import struct,sys
b=open(sys.argv[1],'rb').read()
assert len(b)>=680 and b[:8]==b'SWEETXNU'
assert b'DEVELOPMENT_ARM64_FUXI' in b, 'Payload is not a FUXI kernel'
version,n=struct.unpack_from('<II',b,8)
assert version==1 and 0<n<=32 and len(b)>=680+32*n
assert struct.unpack_from('<QQ',b,56)==(0x8c0000000,0x40000000)
for i in range(n):
 va,vs,off,sz=struct.unpack_from('<4Q',b,680+32*i)
 assert off<=len(b) and sz<=len(b)-off and sz<=vs
PY
# Optional local firmware input; only the ignored build directory receives bytes.
# An empty section gives non-GPU builds the same symbols with size zero.
python3 - "${FUXI_GMU_FIRMWARE:-}" "${FUXI_SQE_FIRMWARE:-}" "$out/gpu-fw.S" <<'FWPY'
import json, pathlib, sys
gmu, sqe, dst = sys.argv[1:]
lines = ['.section .gpu_fw,"a"']
for src, name in ((gmu, 'gpu'), (sqe, 'sqe')):
    lines += ['.balign 4', f'.global _{name}_fw_start', f'_{name}_fw_start:']
    if src:
        path = pathlib.Path(src).resolve(strict=True)
        size = path.stat().st_size
        assert path.is_file() and 4 < size <= 0x100000 and size % 4 == 0, 'invalid firmware size'
        if name == 'sqe':
            assert size - 4 <= 0x20000, 'SQE exceeds reserved buffer'
        lines.append('.incbin ' + json.dumps(str(path)))
    lines += [f'.global _{name}_fw_end', f'_{name}_fw_end:']
pathlib.Path(dst).write_text('\n'.join(lines) + '\n')
FWPY
# incbin avoids C absolute pointers in an Image loaded at an arbitrary PA.
printf '.section .payload,"a"\n.incbin "%s"\n' "$payload" > "$out/payload.S"
flags=(--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib
       -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror -DFUXI=1 -DFUXI_XNU_LOADER=1 -I..)
for source in start.S loader.c ../console.c ../fuxi-probe/probe.c; do
 name=$(basename "${source%.*}")
 "$CLANG" "${flags[@]}" -c "$source" -o "$out/$name.o"
 if [[ $source == *.c ]] && "$READELF" -r "$out/$name.o" | grep -q R_AARCH64_ABS; then
  echo "Absolute C relocation in $source" >&2; exit 1
 fi
done
"$CLANG" "${flags[@]}" -c "$out/payload.S" -o "$out/payload.o"
"$CLANG" "${flags[@]}" -c "$out/gpu-fw.S" -o "$out/gpu-fw.o"
"$LLD" -T loader.ld --build-id=none "$out/start.o" "$out/loader.o" "$out/console.o" "$out/probe.o" "$out/gpu-fw.o" "$out/payload.o" -o "$out/loader.elf"
"$OBJCOPY" -O binary "$out/loader.elf" "$out/Image"
python3 - "$out/Image" <<'PY'
import struct,sys
from pathlib import Path
b=Path(sys.argv[1]).read_bytes()
assert struct.unpack_from('<Q',b,16)[0]==len(b), 'Image size must cover embedded payload'
assert b[56:60]==b'ARM\x64'
PY
python3 "$MKBOOTIMG" --header_version 4 --kernel "$out/Image" \
 --os_version 16.0.0 --os_patch_level 2026-08 -o "$out/fuxi-xnu-boot.img"
ls -lh "$out/Image" "$out/fuxi-xnu-boot.img"
