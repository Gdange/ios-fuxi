#!/bin/bash
# Build patched XNU 8796 on native Apple Silicon with already staged dependencies.
# Usage: XNU_IIG_CACHE=/path/cache tools/build-fuxi-native.sh /path/patched/xnu
# Requires ../fakeroot headers + arm64 libfirehose_kernel.a; see FUXI.md.
set -euo pipefail
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || { echo 'Native Apple Silicon required' >&2; exit 1; }
tools_dir=$(cd "$(dirname "$0")" && pwd)
source_dir=$(cd "${1:?Usage: build-fuxi-native.sh PATCHED_XNU}" && pwd)
: "${XNU_IIG_CACHE:?Point XNU_IIG_CACHE at the matching upstream IIG cache}"
[[ -f $source_dir/config/MASTER.arm64.fuxi && -f $XNU_IIG_CACHE/manifest.json ]] || exit 1
root=$(cd "$source_dir/.." && pwd)
[[ -f $root/fakeroot/usr/local/lib/kernel/libfirehose_kernel.a ]] || { echo 'Missing native firehose archive' >&2; exit 1; }
mkdir -p "$root/native-bin"
# /usr/bin/python3 is an xcrun shim: a symlink named python changes its lookup.
printf '#!/bin/sh\nexec /usr/bin/python3 "$@"\n' > "$root/native-bin/python"
chmod +x "$root/native-bin/python"
export PATH="$root/native-bin:/opt/homebrew/bin:$PATH"
cd "$source_dir"
make -j8 VERBOSE=YES SDKROOT=macosx TARGET_CONFIGS='DEVELOPMENT ARM64 FUXI' \
 CONCISE=0 LOGCOLORS=n BUILD_WERROR=0 BUILD_LTO=0 \
 SRCROOT="$PWD" OBJROOT="$root/native-obj" SYMROOT="$root/native-sym" \
 DSTROOT="$root/fakeroot" FAKEROOT_DIR="$root/fakeroot" \
 IIG="$tools_dir/xnu-iig-cache.py" MIG="$(xcrun -find mig)" MIGCOM="$(xcrun -find migcom)" \
 CTFDUMP=/usr/bin/true CTFCONVERT=/usr/bin/true CTFMERGE=/usr/bin/true CTFINSERT=/usr/bin/true \
 RC_DARWIN_KERNEL_VERSION=22.6.0 RC_ProjectSourceVersion=8796.141.3 \
 TIGHTBEAMC=tightbeamc-not-supported MEMORY_SIZE="$(sysctl -n hw.memsize)" \
 SYSCTL_HW_PHYSICALCPU="$(sysctl -n hw.physicalcpu)" SYSCTL_HW_LOGICALCPU="$(sysctl -n hw.logicalcpu)" \
 KERNEL_BUILDS_IN_PARALLEL=1 CWARNFLAGS=-w CXXWARNFLAGS=-w
