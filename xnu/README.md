# XNU for sweet and fuxi

Built on macOS (Apple silicon) with
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) at a pinned commit.

- `MACOS_VERSION=13.5` -> xnu-8796 (same generation as iOS 16.6/16.7).
- `patches/*.patch` (and the `*.sh` staging scripts) are copied into the build script's patch dir and applied to the xnu tree with `git apply`, in numeric order.
- `tools/build-fuxi-native.sh` does the same natively; `tools/xnu-cc.py` compiles single files for quick checks.
- Output: `kernel.development.<config>` plus the unstripped kernel used for symbolication.
