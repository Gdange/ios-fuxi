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
bash tools/build-fuxi-metal-translator.sh "$out"
if [[ -n ${FUXI_METAL_TOOLS:-} ]]; then
 metal=$FUXI_METAL_TOOLS/metal;metallib=$FUXI_METAL_TOOLS/metallib
else
 metal=$(xcrun --sdk iphoneos --find metal);metallib=$(xcrun --sdk iphoneos --find metallib)
fi
# Compile the whole original source, including the opt-in stage-6 layer controls.
"$metal" -isysroot "$sdk" -std=ios-metal2.4 -mios-version-min=16.0 -fno-fast-math -c tools/shaders/metal-smoke.metal -o "$out/metal-smoke.air"
"$metallib" "$out/metal-smoke.air" -o "$out/metal-smoke.metallib"
bundle=$out/FuxiMetal.bundle
mkdir -p "$bundle"
# Audit reports are not bundle resources and must never enter its signature.
rm -f "$bundle/FuxiMetal.undefined.txt"
cp tools/fuxi-metal/Info.plist "$bundle/Info.plist"
cp tools/fuxi-metal/LICENSE.NativeMetal "$bundle/LICENSE.NativeMetal"
"$cxx" "${flags[@]}" -std=c++17 -I"$headers" -dynamiclib tools/fuxi-metal/Frontend.mm \
 -install_name /System/Library/Extensions/FuxiMetal.bundle/FuxiMetal \
 -framework Foundation -framework Metal -framework IOSurface -o "$bundle/FuxiMetal"
"$cc" "${flags[@]}" tools/fuxi-metal-smoke.m -framework Foundation -framework Metal -framework IOSurface -o "$out/fuxi-metal-smoke"
"$cc" "${flags[@]}" tools/fuxi-ca-smoke.m -framework Foundation -framework Metal -framework IOSurface \
 -framework QuartzCore -framework CoreGraphics -o "$out/fuxi-ca-smoke"
cp tools/ca-smoke.conf "$out/ca-smoke.conf"
cp tools/metal-clients.conf "$out/metal-clients.conf"
cp tools/metal-smoke.conf "$out/metal-smoke.conf"
for binary in "$bundle" "$out/fuxi-metal-smoke" "$out/fuxi-ca-smoke"; do
 codesign -f -s - "$binary"
 codesign --verify --strict "$binary"
done
python3 tools/audit-fuxi-metal.py "$bundle/FuxiMetal" "$out/fuxi-metal-smoke" "$out/fuxi-ca-smoke" "$out/libmetal2vulkan.dylib"
codesign --verify --strict "$bundle"
shasum -a 256 "$bundle/FuxiMetal" "$bundle/Info.plist" "$bundle/LICENSE.NativeMetal" "$bundle/_CodeSignature/CodeResources" \
 "$out/libmetal2vulkan.dylib" "$out/metal-smoke.metallib" "$out/metal2vulkan-source.tar.gz" \
 "$out/fuxi-ca-smoke" "$out/ca-smoke.conf" "$out/fuxi-metal-smoke" "$out/metal-smoke.conf" "$out/metal-clients.conf" > "$out/M5b-SHA256SUMS"
cat "$out/M5b-SHA256SUMS"
