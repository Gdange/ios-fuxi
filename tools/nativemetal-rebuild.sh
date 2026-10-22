#!/bin/bash
# Installed as REBUILD.sh in the corresponding-source archive. Mac, offline.
set -euo pipefail
cd "$(dirname "$0")"
[[ $(uname -s) == Darwin ]]
python3 - <<'PY'
import json
from pathlib import Path
root=Path.cwd()
p=root/'Cargo.toml'
s=p.read_text().splitlines()
for i,line in enumerate(s):
    if line.startswith('path = '):s[i]='path = '+json.dumps(str(root/'native/bridge/NativeMetalBridge.rs'))
    for crate in ('metal2vulkan','metal2vulkan-runtime'):
        if line.startswith(crate+' = { path = '):
            suffix=', features = ["ash"] }' if crate=='metal2vulkan' else ' }'
            s[i]=crate+' = { path = '+json.dumps(str(root/'metal2vulkan/crates'/crate))+suffix
p.write_text('\n'.join(s)+'\n')
(root/'.cargo').mkdir(exist_ok=True)
(root/'.cargo/config.toml').write_text('[source.crates-io]\nreplace-with = "included"\n[source.included]\ndirectory = "corresponding-crates"\n')
PY
export IPHONEOS_DEPLOYMENT_TARGET=16.0
export SDKROOT=$(xcrun --sdk iphoneos --show-sdk-path)
export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER=$(xcrun --sdk iphoneos --find clang)
export CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS='-C link-arg=-miphoneos-version-min=16.0 -C link-arg=-Wl,-install_name,/usr/local/lib/libnative_metal_bridge.dylib'
cargo +1.98.0 build --locked --offline --release --target aarch64-apple-ios
codesign -f -s - target/aarch64-apple-ios/release/libnative_metal_bridge.dylib
codesign --verify --strict target/aarch64-apple-ios/release/libnative_metal_bridge.dylib
