# Metal M9a: isolated native offscreen CA diagnostic

2026-10-21. Source and Linux host checks only. The reported A740 Metal smoke
stages 0–6 PASS are the starting point, not a result repeated by this lane.
No device access, deployment, CI dispatch or push occurred. Native iOS
compilation is wired into the existing Mac CI `metal` target; it has not been
executed here. That target builds from `main`, so this source commit must be
integrated there before its artifacts can include M9a.

## Function constants

The frontend observer, immutable snapshots, pipeline input and separate Rust
bridge now accept **128 constants per stage**, rather than 32. Scalar and
2/3/4-lane float, half, int, uint, short, ushort, char, uchar and bool values
use public MTLDataType ordinals. Three-lane values carry only their component
bytes to the translator; range setters advance by the padded four-lane Metal
array stride. In particular uchar3 carries three bytes, with a four-byte range
stride. Unsupported types, bad names, unobserved allocations, hook coverage
changes and excess counts still invalidate the entire snapshot. Copies/reset
retain the existing provenance rules.

The input layout change requires **bridge ABI 4**. An old ABI-3 dylib is refused,
so the plugin and translator must be replaced together. The separate limits on
Vulkan runtime specialization entries, resource slots, formats and framebuffer
fetch remain unchanged. Function constants are specialized before those runtime
plans; a larger input array does not enable unsupported plans. The M8 offline
probe uses the new count/type handling too; M8's recorded numbers remain a
historical ABI-3 result.

No metal2vulkan or Mesa sources or patches were added or changed. They remain
external dependencies at the existing pins. Further renderer/API/translation
gaps from M8 still need qualification; this diagnostic does not claim to close
them or to admit the compositor.

## Offscreen process and acceptance

`fuxi-ca-smoke` ignores all daemon argv. It reads the required root-image config
`/usr/local/share/fuxi/ca-smoke.conf`, whose single digit selects maximum stage
0–4, inclusive. The shipped value is `4`. Missing, malformed or oversized input
fails; there is no implicit enable/default stage. Root is required. Before
Metal discovery it raises active and inactive jetsam limits to 1024 MB,
non-fatal attributes zero, using `memorystatus_control` command **7**, and
refuses to proceed if that call fails. This addresses the exec-swapped host
daemon's inherited 6 MB limit; command 6 is not a substitute.

Each stage creates a fresh 256×256, one-plane, BGRA8Unorm IOSurface texture and
CARenderer, using the Fuxi Metal device and an explicit Metal command queue.
The renderer factory and required selectors are checked dynamically, since
some iPhone SDKs omit the CARenderer declaration. Exported renderer option keys
are resolved rather than guessing their string values. The API follows Apple's
[CARenderer factory](https://developer.apple.com/documentation/quartzcore/carenderer/init%28mtltexture%3Aoptions%3A%29-1cr0b?language=objc)
and [Metal command queue option](https://developer.apple.com/documentation/quartzcore/kcarenderermetalcommandqueue?language=objc).
Only a standalone CALayer tree and IOSurface are constructed; no window,
CAMetalLayer, drawable, display surface, screen context or presentation is used.

Stages are cumulative:

| Stage | Name | Scene |
| --- | --- | --- |
| 0 | solid | Opaque non-gray background |
| 1 | opacity | Offset colored sublayer at opacity 0.5 |
| 2 | rounded-clip | cornerRadius plus masksToBounds and an overflowing nested sublayer |
| 3 | image | Asymmetric BGRA CGImage contents on a separate layer, nearest filtering |
| 4 | transform | Image layer with a 90-degree affine rotation and translation |

The CPU reference renders the same tree using `CALayer renderInContext:` into
an sRGB, premultiplied BGRA CGBitmapContext. No automatic row flip, channel swap
or fitted tolerance is permitted. Every pixel/channel participates in metrics.
Acceptance requires maximum channel error ≤64, at most 256 pixels with any
channel error >8, and mean absolute channel error ≤1.0 over 262144 channels.
Thus narrow rasterization-edge differences can pass, while a missing layer,
wrong orientation, stale sentinel target or broad color mismatch fails.

The explicit queue receives a completion fence after `endFrame`. The frontend
currently submits synchronously. A diagnostic export counts completed render
operations; every stage must increase it during CARenderer rendering. Pixel
agreement without a Metal draw cannot pass. Renderer/texture autoreleases are
drained before locking the IOSurface for the CPU consumer.

A second export counts frontend failures even beyond the log cap. Unsupported
selectors, descriptor checks, constant observation and library/translation or
submission failures log the first **64** compact records per process. CA
constant-snapshot diagnostics share this cap; the older detailed eight-record
observer diagnostics remain available to other clients. Any such failure in
`fuxi-ca-smoke` latches rejection of later GPU submissions, including valid
prefixes after a swallowed refusal. The smoke requires a zero failure count
through comparison and cleanup. Initialization has a 30-second alarm; each
stage has a 60-second alarm, including compilation, render and readback. Timeout
exits 124, SIGABRT exits 134 with a stage FAIL line, and ordinary errors or
exceptions exit 1. These are isolated test wall limits, not display deadlines.

`backboardd` and `SpringBoard` are **permanently DENIED** by the shipped client
parser, including census entries and explicit allow entries. The staging tool
also refuses configurations containing either name. The shipped allowlist has
only `fuxi-metal-smoke` and `fuxi-ca-smoke`. This does not implement compositor
recovery, shadow presentation or an active CA enablement gate.

## Expected device lines, not observed results

Successful full qualification would include the following prefixes; the actual
numeric metrics must be printed, with nonzero gpu_draws and the limits above:

```text
FUXI CA: jetsam before=... get=... set=1024MB rc=0 errno=0
FUXI MTL: gate allow fuxi-ca-smoke
FUXI CA: stage 0 solid PASS pixels=65536 max=... mean=... bad=... gpu_draws=... total_draws=...
FUXI CA: stage 1 opacity PASS pixels=65536 max=... mean=... bad=... gpu_draws=... total_draws=...
FUXI CA: stage 2 rounded-clip PASS pixels=65536 max=... mean=... bad=... gpu_draws=... total_draws=...
FUXI CA: stage 3 image PASS pixels=65536 max=... mean=... bad=... gpu_draws=... total_draws=...
FUXI CA: stage 4 transform PASS pixels=65536 max=... mean=... bad=... gpu_draws=... total_draws=...
FUXI CA: done PASS
```

An unsupported renderer request may instead stop at stage 0 or a later stage.
That is a diagnostic result, not success or permission to admit backboardd.
Typical failure forms are `FUXI MTL: unsupported <class> <selector>`,
`FUXI MTL: <scope> reject <field> <value>`,
`FUXI MTL: constant reject type=... bytes=... records=...`,
`FUXI MTL: pipeline/library FAIL <reason>`, followed by
`FUXI CA: stage N <name> FAIL frontend_errors=... gpu_draws=...`.
A fatal CA assertion produces the SIGABRT line; a hang produces
`FUXI CA: stage N <name> FAIL watchdog=60s`. No later stage or done PASS is valid
after any failure. M8 identified additional queue/encoder SPI, plan dependencies
and destination-read gaps; there is no prediction that this first build passes
all native stages.

## Redeploy list and config

Use artifacts from the normal `metal` target after integration into `main`.
The target compiles, ad-hoc signs, verifies and audits the new executable, and
includes it and its config in the existing explicit artifact/hash lists. No
Apple CA metallib is packaged: CARenderer uses the installed system resources.

Stage the matching set into an already mounted writable offline iOS root:

```sh
python3 tools/stage-fuxi-metal.py "$IOS_ROOT" \
  "$ICD" "$METAL_OUT/FuxiMetal.bundle" "$METAL_OUT/fuxi-metal-smoke" \
  --translator "$METAL_OUT/libmetal2vulkan.dylib" \
  --metallib "$METAL_OUT/metal-smoke.metallib" \
  --source "$METAL_OUT/metal2vulkan-source.tar.gz" \
  --clients "$METAL_OUT/metal-clients.conf" \
  --conf "$METAL_OUT/metal-smoke.conf" \
  --ca-smoke "$METAL_OUT/fuxi-ca-smoke" --ca-conf "$METAL_OUT/ca-smoke.conf"
```

The staging operation requires root for root:wheel ownership. It checks both
executables, both configs and all other inputs before copying. Installed files:

| Destination | Mode / required contents |
| --- | --- |
| /System/Library/Extensions/FuxiMetal.bundle/FuxiMetal | 0755, rebuilt ABI-4 consumer |
| /System/Library/Extensions/FuxiMetal.bundle/Info.plist | 0644, matching signed bundle |
| /System/Library/Extensions/FuxiMetal.bundle/_CodeSignature/CodeResources | 0644, matching signature |
| /usr/local/lib/libmetal2vulkan.dylib | 0755, rebuilt ABI 4 |
| /usr/local/lib/libvulkan_freedreno.dylib | 0755, existing compatible pinned Turnip ICD |
| /usr/local/bin/fuxi-ca-smoke | 0755, new signed executable |
| /usr/local/bin/fuxi-metal-smoke | 0755, matching build's existing smoke |
| /usr/local/share/fuxi/ca-smoke.conf | 0644, `4` plus newline; `0` for first-stage-only qualification |
| /usr/local/share/fuxi/metal-clients.conf | 0644, lines `fuxi-metal-smoke` and `fuxi-ca-smoke` only |
| /usr/local/share/fuxi/metal-smoke.conf | 0644, shipped `5`; use `6` to repeat all existing Metal stages separately |
| /usr/local/share/fuxi/metal-smoke.metallib | 0644, existing owned-source smoke shaders |
| /usr/local/share/fuxi/metal2vulkan-source.tar.gz | 0644, matching external translator corresponding source/licences |

Do not mix an old translator with the new plugin. For a later isolated boot,
replace the existing test mapping with
`sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-ca-smoke`, preserving the
other boot arguments. Include `fuxi-ca-smoke` in the existing test's `sweetnull`
list for its console logging, preserving its other names. Do not exec-swap
backboardd or SpringBoard or add either to Metal policy. No new kernel/ICD
change is required by this source lane. No boot image was generated here.

## Host validation

```sh
python3 tools/test-fuxi-metal-m9a.py --source "$M2V_SOURCE" \
  --build research/metal-m9a/bridge --spirv-val "$SPIRV_VAL"
python3 tools/test-fuxi-metal-frontend.py
bash tools/ci/build.sh --plan none true false false
bash tools/ci/build.sh metal --dry-run
```

The M9a test verifies the clean existing translator pin, builds/tests our bridge
offline, and generates an original synthetic AIR control only in temporary
output. It exercises 58 used bool/uchar/uchar3 constants through the linked
C/Rust compile path, named/indexed entries, cache reuse, changed vector value,
count/type/size rejection, ABI structure sizes and pixel tolerance boundaries.
It queries lavapipe but submits no GPU command buffers. Generated SPIR-V is
validated for Vulkan 1.2. Four Rust bridge tests and the C++ integration pass.
ASan/UBSan report no errors. Linux staging format fixtures exercise both
executables, all CA config stages, malformed configs, hard denial of both
compositors, unsigned/nonroot refusal and symlink containment; these fixtures
are not genuine native signatures. Constant-hook runtime model tests pass.

Native observer regressions were extended for 58 named uchar3 values,
four-lane range strides, 128-entry boundary/update/overflow, vector snapshots
and unsupported matrices. Native frontend tests also cover the CA failure
latch and continuing counters after the log cap. These Foundation/Metal tests
require a Mac and were skipped on Linux. The production iOS build compiles
Frontend.mm and fuxi-ca-smoke.m with the existing iOS 16 target and warnings as
errors when the normal CI target is next built. No native build or device
pixel result is asserted by this document.
