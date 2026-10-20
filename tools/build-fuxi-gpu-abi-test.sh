#!/bin/bash
# Native Mac build. Output and Apple SDK inputs stay outside git.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin ]] || { echo 'Build on the Mac with Xcode' >&2; exit 1; }
mkdir -p tools/out
sdk=iphoneos
if ! sdkroot=$(xcrun --sdk "$sdk" --show-sdk-path 2>/dev/null); then
 sdk=macosx
 sdkroot=$(xcrun --sdk "$sdk" --show-sdk-path)
 echo 'No iOS SDK: using macOS headers/stubs with an iOS 16 target' >&2
fi
xcrun --sdk "$sdk" clang++ -target arm64-apple-ios16.0 -isysroot "$sdkroot" \
 -std=c++17 -O2 -Wall -Wextra -Werror tools/fuxi-gpu-abi-test.cpp \
 -o tools/out/fuxi-gpu-abi-test
codesign -f -s - tools/out/fuxi-gpu-abi-test
file tools/out/fuxi-gpu-abi-test
shasum -a 256 tools/out/fuxi-gpu-abi-test > tools/out/SHA256SUMS
