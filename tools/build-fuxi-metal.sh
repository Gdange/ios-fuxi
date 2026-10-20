#!/bin/bash
# Native Mac, external Vulkan headers; outputs and SDK inputs stay ignored.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin ]] || { echo 'requires a Mac with Xcode' >&2; exit 1; }
headers=${1:?Usage: build-fuxi-metal.sh EXTERNAL_VULKAN_INCLUDE [OUT]}
out=${2:-tools/out}
[[ -f $headers/vulkan/vulkan.h ]]
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
cxx=$(xcrun --sdk iphoneos --find clang++)
flags=(-target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -Wextra -Werror -Werror=unguarded-availability-new -fstack-protector-strong -fno-objc-arc -fblocks)
bundle=$out/FuxiMetal.bundle
mkdir -p "$bundle"
cp tools/fuxi-metal/Info.plist "$bundle/Info.plist"
"$cxx" "${flags[@]}" -std=c++17 -I"$headers" -dynamiclib tools/fuxi-metal/Frontend.mm \
 -install_name /System/Library/Extensions/FuxiMetal.bundle/FuxiMetal \
 -framework Foundation -framework Metal -o "$bundle/FuxiMetal"
"$cc" "${flags[@]}" tools/fuxi-metal-smoke.m -framework Foundation -framework Metal -o "$out/fuxi-metal-smoke"
cp tools/metal-smoke.conf "$out/metal-smoke.conf"
for binary in "$bundle" "$out/fuxi-metal-smoke"; do
 codesign -f -s - "$binary"
 codesign --verify --strict "$binary"
done
python3 tools/audit-fuxi-metal.py "$bundle/FuxiMetal" "$out/fuxi-metal-smoke"
shasum -a 256 "$bundle/FuxiMetal" "$bundle/Info.plist" "$bundle/_CodeSignature/CodeResources" \
 "$out/fuxi-metal-smoke" "$out/metal-smoke.conf" > "$out/M4-SHA256SUMS"
cat "$out/M4-SHA256SUMS"
