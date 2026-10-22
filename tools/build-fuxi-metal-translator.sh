#!/bin/bash
# External source cache only. Never link this library into FuxiMetal or XNU.
set -euo pipefail
cd "$(dirname "$0")/.."
pin=720ef1ce9ae2a2068999a8675717418b9416283e
cache=${FUXI_M2V_CACHE:-deps/metal2vulkan}
bridge=${FUXI_M2V_BRIDGE:-deps/metal2vulkan-bridge}
out=${1:-tools/out}
export PATH="$HOME/.cargo/bin:$PATH"
[[ $(uname -s) == Darwin ]] || { echo 'requires Mac Xcode + Rust 1.98.0' >&2; exit 1; }
mkdir -p "$out" "$bridge/src"
if [[ ! -d $cache/.git ]]; then git init "$cache";git -C "$cache" remote add origin https://github.com/steelbrain/metal2vulkan.git;git -C "$cache" fetch --depth 1 origin "$pin"; fi
git -C "$cache" diff --quiet
git -C "$cache" diff --cached --quiet
git -C "$cache" checkout --detach "$pin"
[[ $(git -C "$cache" rev-parse HEAD) == "$pin" ]]
source=$(cd "$cache" && pwd)
cp tools/fuxi-metal2vulkan-bridge.rs "$bridge/src/lib.rs"
cat > "$bridge/Cargo.toml" <<TOML
[package]
name = "fuxi-metal2vulkan-bridge"
version = "0.1.0"
edition = "2021"
[lib]
name = "metal2vulkan"
crate-type = ["cdylib"]
[dependencies]
metal2vulkan = { path = "$source/crates/metal2vulkan", features = ["ash"] }
ash = { version = "0.38", default-features = false, features = ["std"] }
sha2="0.10"
[profile.release]
debug = false
strip = "debuginfo"
TOML
rustup toolchain install 1.98.0 --profile minimal
rustup +1.98.0 target add aarch64-apple-ios
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
export IPHONEOS_DEPLOYMENT_TARGET=16.0
export SDKROOT="$sdk"
export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER
CARGO_TARGET_AARCH64_APPLE_IOS_LINKER=$(xcrun --sdk iphoneos --find clang)
export CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS='-C link-arg=-miphoneos-version-min=16.0 -C link-arg=-Wl,-install_name,/usr/local/lib/libmetal2vulkan.dylib'
cargo +1.98.0 build --manifest-path "$bridge/Cargo.toml" --release --target aarch64-apple-ios
cp "$bridge/target/aarch64-apple-ios/release/libmetal2vulkan.dylib" "$out/libmetal2vulkan.dylib"
codesign -f -s - "$out/libmetal2vulkan.dylib"
codesign --verify --strict "$out/libmetal2vulkan.dylib"
# Keep source, lockfile, build recipe, all transitive crate sources and licence texts
# alongside the library. No Apple SDK/toolchain or generated AIR belongs in this archive.
cargo +1.98.0 vendor --manifest-path "$bridge/Cargo.toml" "$bridge/corresponding-crates" > "$bridge/vendor-config.toml"
cp tools/build-fuxi-metal-translator.sh "$bridge/BUILD.sh"
cp "$cache"/LICENSE* "$bridge/"
rustdocs=$(rustc +1.98.0 --print sysroot)/share/doc/rust
mkdir -p "$bridge/rust-notices"
cp -R "$rustdocs/licenses" "$rustdocs/COPYRIGHT-library.html" "$rustdocs/COPYRIGHT.html" "$bridge/rust-notices/"
cat > "$bridge/NOTICE" <<NOTICE
metal2vulkan https://github.com/steelbrain/metal2vulkan
Revision $pin; licence choice LGPL-3.0-or-later.
Original Fuxi C ABI adapter: see src/lib.rs and the Fuxi repository licence.
Replace /usr/local/lib/libmetal2vulkan.dylib with an ABI-v7 compatible library;
ad-hoc sign it and stage it through the same offline root-image procedure.
No translator objects are linked into FuxiMetal. Reverse engineering to debug
modifications to this library is permitted; preserve LGPL/GPL texts and notices.
Corresponding source and build inputs accompany this separately replaceable library.
Build: see BUILD.sh and docs-lanes/metal.md; Mac Xcode and Rust 1.98.0 required.
NOTICE
cp README.md "$bridge/README.Fuxi.md"
printf '%s\n' "$(basename "$cache")" > "$bridge/source-directory"
cat > "$bridge/REBUILD.sh" <<'REBUILD'
#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
source=$(cd "../$(cat source-directory)" && pwd)
# Rewrite only the original adapter manifest's external source-cache path.
python3 - "$source" <<'PYCODE'
from pathlib import Path
import re,sys
p=Path('Cargo.toml')
p.write_text(re.sub(r'path = "[^"]+/crates/metal2vulkan"', 'path = "'+sys.argv[1]+'/crates/metal2vulkan"',p.read_text()))
PYCODE
mkdir -p .cargo
# Point Cargo at the accompanying registry sources; no network required.
printf '[source.crates-io]\nreplace-with = "fuxi-sources"\n[source.fuxi-sources]\ndirectory = "corresponding-crates"\n' > .cargo/config.toml
export IPHONEOS_DEPLOYMENT_TARGET=16.0
export SDKROOT=$(xcrun --sdk iphoneos --show-sdk-path)
export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER=$(xcrun --sdk iphoneos --find clang)
export CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS='-C link-arg=-miphoneos-version-min=16.0 -C link-arg=-Wl,-install_name,/usr/local/lib/libmetal2vulkan.dylib'
cargo +1.98.0 build --locked --offline --release --target aarch64-apple-ios
codesign -f -s - target/aarch64-apple-ios/release/libmetal2vulkan.dylib
REBUILD
chmod +x "$bridge/REBUILD.sh"
# Archive from ignored caches only. Tracked tree never contains third-party source.
tar --exclude target --exclude .git --exclude fixtures --exclude golden --exclude corpus --exclude '*.air' --exclude '*.metallib' --exclude '*.mtlb' --exclude '*.spv' -czf "$out/metal2vulkan-source.tar.gz" -C "$(dirname "$cache")" "$(basename "$cache")" \
 -C "$(cd "$(dirname "$bridge")" && pwd)" "$(basename "$bridge")"
shasum -a 256 "$out/libmetal2vulkan.dylib" "$out/metal2vulkan-source.tar.gz"
