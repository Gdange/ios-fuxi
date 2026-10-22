#!/bin/bash
# Source-only external checkouts, never submodules/vendor source in this tree.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin ]] || { echo 'requires Mac Xcode + Rust 1.98.0' >&2; exit 1; }
headers=${1:?Usage: build-fuxi-nativemetal.sh EXTERNAL_VULKAN_INCLUDE [OUT]}
out=${2:-tools/out/nativemetal}
export PATH="$HOME/.cargo/bin:$PATH"
# The legacy standalone translator recipe can perform a full fetch. Pre-create
# its clean sparse source cache so even a standalone N1 build avoids binary fixtures.
python3 tools/build-fuxi-nativemetal.py --fetch-fallback-only "${FUXI_M2V_CACHE:-deps/metal2vulkan}"
# Build the same gate + fallback, smoke tools and separate fallback translator.
bash tools/build-fuxi-metal.sh "$headers" "$out"
python3 tools/build-fuxi-nativemetal.py --headers "$headers" --out "$out" \
 --native "${FUXI_NATIVEMETAL_SOURCE:-deps/nativemetal}" \
 --metal2vulkan "${FUXI_NATIVE_M2V_SOURCE:-deps/nativemetal-metal2vulkan}" \
 --build "${FUXI_NATIVE_BUILD:-deps/nativemetal-build}"
