# Metal lane M5b: external shader pipeline

## M5b external translator and offscreen pipeline

2026-10-21. **Built and host-verified; device verification pending.** No new
M5a device observation was supplied in this task; its gate/census device report
will be recorded when available. M4's reported device PASS remains the baseline.
No phone, mounted production root, kernel build/change or push was involved.
The boot image remains **metal4**, SHA256
`e10f981a0bae32ad5c892d978a786eabe4cc21acda6e35ed064f1ad68a53c4b6`.

The original source `tools/shaders/metal-smoke.metal` defines `fuxi_vertex`
(`vertex_id`, three positions) and `fuxi_fragment` (constant opaque red).
Xcode compiles this source to AIR/metallib **at build time** on the Mac.
No Apple shaders/metallibs, CA inputs, translated replacement blobs, copied
Apple implementation or firmware are tracked or used for this test.

The smoke's stages 0–3 retain their existing fill/copy/reuse flow. Stage 4,
**pipeline**, exercises both `newLibraryWithURL:error:` and
`newLibraryWithData:error:`, `newFunctionWithName:` and
`newRenderPipelineStateWithDescriptor:error:`. It renders one triangle over
opaque black into a **private 256x256 BGRA8Unorm** texture, then blits the whole
image into a shared buffer at offset 256. CPU verification checks all 65,536
pixels, six inside/outside samples, colour counts (8,192 ±128 red) and 256-byte
sentinels at both ends. A failed API, command, translation or verification
prints FAIL and terminates; there is no replacement shader/success fallback.

The original frontend owns Metal library/function/pipeline/texture/render
objects and retains command resources through submission. Its independent C
ABI v1 wrapper calls the external translator's library and render-pipeline
APIs. The translator queries the actual Vulkan physical device, restricted to
Vulkan **1.2** and the empty enable set used at `vkCreateDevice`; it supplies
SPIR-V and its planned fragment `ColorWriteRule` push bytes. Fuxi creates
Turnip shader modules, a pipeline/layout/render pass, framebuffer, render and
image-to-buffer copy commands. Negative viewport height preserves Metal's
upper-left viewport convention. The existing two-second fence wait and
poisoned-device/quarantine behavior also apply to stage 4.

This is deliberately bounded: one sample, one BGRA8 attachment, no blending,
depth/stencil, vertex attributes, shader resource bindings, specialization,
linked functions, additional draws or partial texture copies. Other descriptor
shapes/encoders fail explicitly. Only a four-byte fragment push write from the
translator's own draw planner is accepted. The gate still defaults to only
`fuxi-metal-smoke`; census mode still stops at a nil command queue before any
GPU/translator construction. Normal clients still require unsandboxed root.
The shipped `metal-smoke.conf` is **4**; accepted maximum stages are 0–4.
Missing config retains the earlier stage-3 default. `metal-clients.conf` is
byte-identical to M5a's default.

### External build and licence boundary

Source: [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan),
revision **720ef1ce9ae2a2068999a8675717418b9416283e**. All workspace crates
state **GPL-2.0-or-later OR LGPL-3.0-or-later**. The approved choice for this
separate dynamic library is **LGPL-3.0-or-later**. Fuxi's own code keeps its
existing repository terms. No translator/Mesa source or objects enter the
tracked tree, kernel or FuxiMetal binary. The original adapter source is ours;
Cargo builds it and the translator into their own `libmetal2vulkan.dylib` in
ignored `deps/` caches. FuxiMetal resolves its versioned C symbols with `dlopen`
and does not link against Rust/translator archives. The dylib has only a system
`libSystem` load; FuxiMetal/smoke retain system framework/library loads.

On the assigned Mac tree, with external Vulkan headers and Xcode installed:

```sh
# Requires Rust/rustup and the Xcode Metal compiler component.
xcodebuild -downloadComponent MetalToolchain
# If Xcode's default component mapping is stale, select/install the matching
# component explicitly and set FUXI_METAL_TOOLS to its usr/bin directory.
# This build used Metal component 27A266a, compiler 32023.921.6, iPhoneOS 27.1 SDK.
xcodebuild -downloadComponent MetalToolchain -buildVersion 27A266a
bash tools/build-fuxi-metal.sh "$VULKAN_HEADERS"
```

`build-fuxi-metal-translator.sh` fetches/checks the pin into ignored
`deps/metal2vulkan`, creates the adapter crate in ignored
`deps/metal2vulkan-bridge`, installs **Rust 1.98.0** and target
`aarch64-apple-ios`, and runs:

```sh
export IPHONEOS_DEPLOYMENT_TARGET=16.0
export SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)"
export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER="$(xcrun --sdk iphoneos --find clang)"
export CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS='-C link-arg=-miphoneos-version-min=16.0 -C link-arg=-Wl,-install_name,/usr/local/lib/libmetal2vulkan.dylib'
cargo +1.98.0 build --manifest-path deps/metal2vulkan-bridge/Cargo.toml \
  --release --target aarch64-apple-ios
# The main build then compiles our shader and frontend:
"$METAL" -isysroot "$SDKROOT" -std=ios-metal2.4 -mios-version-min=16.0 \
  -fno-fast-math -c tools/shaders/metal-smoke.metal -o tools/out/metal-smoke.air
"$METALLIB" tools/out/metal-smoke.air -o tools/out/metal-smoke.metallib
```

All three Mach-O artifacts target plain arm64 **iOS 16.0**, are ad-hoc signed,
strict-signature verified and audited for system imports/load paths and no
EL0 `CTR_EL0` reads. All **210** strong imports across these artifacts were
found in the supplied iOS 16.7 cache (same SHA256 as the M5a analysis).
`libSystem`'s explicit reexports include `libdispatch` and `libcommonCrypto`;
the cache audit now recognizes those actual providers. Translation is measured at pipeline creation with
`steady_clock`; every call prints elapsed milliseconds and PASS/FAIL.
No persistent shader or pipeline cache is implemented. The translator's optional
runtime/cache crate is not built. No independent AIR subset alternative was
needed: the pin accepted our compiler's actual AIR.

The source archive distributed **beside** the dylib contains the library source,
original adapter, exact Cargo lockfile, registry dependency sources/notices,
Rust notices, upstream LGPL-3.0/GPL-3.0 texts and notice, build script and
`metal2vulkan-bridge/REBUILD.sh`. It omits upstream shader fixtures/corpus,
build outputs, SDKs/toolchains and git history. Extract it in an ignored cache
and run that rebuild script on a Mac with the stated toolchain; it rewrites the
external source path and uses the included registry sources with
`--locked --offline`. Ad-hoc sign the modified library and replace the same
installed dylib through the coordinator's offline root-image procedure.
The packaged source was extracted into a fresh ignored directory and successfully
rebuilt with `--locked --offline` on the Mac. This checks rebuildability, not
bit-for-bit reproducibility across source paths. No FuxiMetal relink or kernel
change is required for an ABI-compatible replacement.

Distribution must retain the LGPL/GPL texts and prominent library/use notices,
provide corresponding library source and modifications (including this adapter,
lockfile and build inputs), preserve dependency notices, and permit library
replacement/relinking and reverse engineering for debugging those modifications.
Supply installation/replacement information where required; do not impose terms
that remove these rights. This lane supplies source and the replacement recipe
instead of relying solely on a download link or on dynamic loading as an
exemption. No upstream translator code was modified.

### Validation and coordinator staging

Host evidence: real external translator + lavapipe, under frontend/backend
ASan/UBSan checks, produced **8,192 red / 57,344 clear pixels** in four renders
(two reuse rounds across two device opens). Each emitted module passes
`spirv-val --target-env vulkan1.2`. Invalid library header, missing function,
fence timeout, device loss and last-pixel corruption fail. Measured Linux-host
translation calls in the recorded run were **9.547 ms and 5.140 ms**; these are
not iOS/A740 timings. Native Mac frontend tests retain M5a gate/census coverage
and add descriptor/encoder/ownership refusal checks; the CPU model never
submits a render. Native smoke tests retain stages 0–3 and verify stage-4 failure
when its required library is unavailable. Staging tests use the real signed
artifacts on a temporary root and check ownership/modes, idempotence, stage-4
config and linked/invalid destination refusal.

```sh
python3 tools/test-fuxi-metal-frontend.py                 # Mac
python3 tools/test-fuxi-metal-stage.py                    # Mac, temporary root
python3 tools/test-fuxi-metal-backend.py --headers "$VULKAN_HEADERS"  # Linux
python3 tools/test-fuxi-metal-pipeline.py --headers "$VULKAN_HEADERS" \
  --translator "$HOST_M2V_DYLIB" --metallib tools/out/metal-smoke.metallib
# Coordinator only, on an already mounted writable iOS root:
python3 tools/stage-fuxi-metal.py "$IOS_ROOT" \
  tools/out/libvulkan_freedreno.dylib tools/out/FuxiMetal.bundle \
  tools/out/fuxi-metal-smoke
```

All destinations are **root:wheel (0:0)**; new directories are 0755.

| Artifact under `tools/out/` | iOS destination | Mode |
| --- | --- | --- |
| `libvulkan_freedreno.dylib` (unchanged) | `/usr/local/lib/libvulkan_freedreno.dylib` | 0755 |
| `libmetal2vulkan.dylib` | `/usr/local/lib/libmetal2vulkan.dylib` | 0755 |
| `fuxi-metal-smoke` | `/usr/local/bin/fuxi-metal-smoke` | 0755 |
| `metal-smoke.metallib` (our compiled source) | `/usr/local/share/fuxi/metal-smoke.metallib` | 0644 |
| `metal2vulkan-source.tar.gz` | `/usr/local/share/fuxi/metal2vulkan-source.tar.gz` | 0644 |
| `metal-smoke.conf` (`4`) | `/usr/local/share/fuxi/metal-smoke.conf` | 0644 |
| `metal-clients.conf` (unchanged) | `/usr/local/share/fuxi/metal-clients.conf` | 0644 |
| `FuxiMetal.bundle/FuxiMetal` | `/System/Library/Extensions/FuxiMetal.bundle/FuxiMetal` | 0755 |
| `FuxiMetal.bundle/Info.plist` | `/System/Library/Extensions/FuxiMetal.bundle/Info.plist` | 0644 |
| `FuxiMetal.bundle/_CodeSignature/CodeResources` | `/System/Library/Extensions/FuxiMetal.bundle/_CodeSignature/CodeResources` | 0644 |

Expected additional lines on a successful **future device run** (timing and
boundary coverage counts may vary within the verified bounds):

```text
FUXI MTL: translation PASS <milliseconds> ms (no cache)
FUXI MTL: stage 4 pipeline PASS BGRA8 256x256 inside/outside 6 red 8192 clear 57344 + sentinel edges
FUXI MTL: done PASS
```

Risks: the actual iOS `dlopen`/AIR translation/Turnip stage-4 submission and
pixels remain unobserved; host/software-driver success is not device success.
The in-process Rust translator has its own standard 20-second cooperative budget
and the diagnostic's 60-second stage alarm, not a separate hard memory-isolated
worker. Alpha translator correctness, cold RSS/latency and driver compilation
on A740 still need measurement. This does not enable CA, IOSurface import,
sampling, presentation or additional processes. Keep the gate/census stop.

Complete signed output manifest: `tools/out/M5b-SHA256SUMS`.

| M5b artifact | SHA256 |
| --- | --- |
| `tools/out/FuxiMetal.bundle/FuxiMetal` | `bb071e27f1a3b7e4f4e6b5fc369a475df826ad0e1590bdc4502da0de199edb2c` |
| `tools/out/FuxiMetal.bundle/Info.plist` | `2578f53d73942a55e0163fa5c80c29beb00839bfb8db477a4060e0a81ab5fd06` |
| `tools/out/FuxiMetal.bundle/_CodeSignature/CodeResources` | `9fcd9223ba2b40aee792c83c8edf32c7c1847f01369fbb0f230e1108a493451b` |
| `tools/out/libmetal2vulkan.dylib` | `015d536f2617827cb7c83fd4a58682d370881c59a9bb719f19b00908bae4bd88` |
| `tools/out/metal-smoke.metallib` | `c1da69afc209598f775022dae5325894c1128935334efa024d378a179c6c6464` |
| `tools/out/metal2vulkan-source.tar.gz` | `9eb4d8ab39fcb53cf169ec0b450e71fe3fcdbb88bf88641f68b631a2f4d8770b` |
| `tools/out/fuxi-metal-smoke` | `87e652a1915c5830b7ebda274f6f4ef1f8a63b0936a01a74f9a0a293c8b9e1a4` |
| `tools/out/metal-smoke.conf` | `7de1555df0c2700329e815b93b32c571c3ea54dc967b89e81ab73b9972b72d1d` |
| `tools/out/metal-clients.conf` | `8d5b163a382ff4eca7579803ec5dd9e2f24f8c0a4e3d65fc6965c539b3e7e6cc` |
| `tools/out/libvulkan_freedreno.dylib` (unchanged) | `5661a054b5e4be57fe54bc38f3cccf6d264ae20562473caf72e50d2adc158a8c` |


## M5a process safety gate

2026-10-20. The coordinator reports **M4 observed PASS**: native metaclass
matching, system Metal discovery (528-byte base and subclass), 64 KiB fill,
completion handlers, copy with sentinel edges, 16 reuse rounds and done PASS;
no panic, with the home screen continuing on the CPU path. backboardd and
SpringBoard also loaded the M4 bundle. They must not receive a transfer-only
device during normal UI operation.

The plugin now checks `/usr/local/share/fuxi/metal-clients.conf` before any
IOKit lookup or Turnip construction. The installed default contains only
`fuxi-metal-smoke`. It compares the actual dyld executable basename, independent
of exec-swap's preserved daemon argv. Missing, unreadable, linked, oversized,
untrusted or writable-by-others files deny access. Names match exactly;
comments/blank lines are allowed. Policy is cached until process exit and logs
one `FUXI MTL: gate allow <proc>` or `FUXI MTL: gate deny <proc>` per process.
Staging installs the policy root:wheel **0:0 0644**.

Nil-init verification, same iOS 16.7 cache SHA256 documented below:
`_MTLRegisterDevices` calls `initWithAcceleratorPort:` at `0x181b14580`;
its nil branch at `0x181b14584` goes directly to `0x181b14594`, releases the
service at `0x181b14598`, and advances the iterator at `0x181b145a0`.
`_MTLAddDevice` at `0x181b1458c` is skipped. This is an explicit supported
absence path, not a call through nil into registration. The default-device
array initializes once; no initializer-failure retry loop is present there.

Native Mac gate tests passed under ASan/UBSan: 64 denied initialization
attempts return nil with one policy read/log, before IOKit/backend access;
exact names, malformed input, NULs, conflicts, missing/untrusted/writable files
and symlinks fail closed. Existing frontend and smoke tests passed. The arm64
iOS 16.0 plugin and smoke were built, ad-hoc signed, signature-verified and
audited. No kernel changes, phone access, mounted-root edit or push.

## M5a census and the CA stop point

Add `census backboardd` to the same policy file for an **explicit diagnostic
run**, then restart the process/reboot; the shipped file does not enable this.
A census entry grants a device even to a nonroot process, but **never constructs
Turnip, opens a native GPU client, allocates GPU memory or submits work**.
It uses registryID 0, conservative M4 capabilities and associated state with no
backend. Normal allow entries retain M4's root requirement and transfers.

Every implemented Metal API entry logs its first call. Unsupported messages on
the device and resource classes enter Objective-C forwarding with encodings from
the private superclass/public protocols; private protocols loaded in the process
also supply signatures. NSInvocation returns zero bytes for the actual return
size, covering nil, integer, float, structures (including indirect returns), and
void no-op. Inherited private device implementations cannot execute against the
uninitialized base. A mutex protects a fixed **256 (class, selector)** table
shared across all frontend objects; the logger allocates nothing and duplicate
calls produce no further lines, and reaching capacity prints one limit line. Lifetime and
forwarding machinery itself is excluded from API logging.

An entirely untyped private selector uses the conventional object/word nil
signature and is logged. Objective-C supplies no ABI for such a selector:
arbitrary unknown floating/structure returns cannot be guaranteed by guessing.
The typed Metal/public and discovered private protocol cases are covered by the
host tests; this is not a universal substitute for an unknown private ABI.
Unimplemented textures, libraries and pipelines return nil; census does not
fabricate successful resources. No texture/render/compute object is vended yet;
forwarding applies to such protocol methods when a diagnostic object exposes
them. M4 buffer/queue/command-buffer/blit classes have the same instrumentation.

**Safety takes priority over depth:** census `newCommandQueue` returns nil.
The cache's `new_metal_context` handles this by returning null
(`0x188487ef0..0x188488108`). Allowing a queue would advance into loading
QuartzCore's metallib; returning nil there **aborts the client**, rather than
selecting software. Consequently M5a learns the safe first blocker dynamically;
the deeper census below is static cache evidence, not an observed backboardd
trace or a claim that all CA shaders form a small fixed set. Advance the stop
point only as each real requirement is implemented and independently tested.
The generic forwarding tests can exercise queue/command/encoder/texture protocol
messages without opening a GPU; these do not qualify CA runtime behavior.

Expected new lines (examples, not an M5a phone observation):

```text
FUXI MTL: gate allow fuxi-metal-smoke
FUXI MTL: gate deny backboardd
FUXI MTL: gate deny SpringBoard
```

With an explicitly staged census entry, replace backboardd's deny with:

```text
FUXI MTL: gate allow backboardd
FUXI MTL: census enabled backboardd (no GPU)
FUXI MTL: census backboardd FuxiMTLDevice initWithAcceleratorPort: implemented
FUXI MTL: census backboardd FuxiMTLDevice newCommandQueue implemented
```

The queue line occurs only if CA actually asks for one. Registration may log
other selectors first. The existing smoke stages should still end `done PASS`
for the default allowlisted executable. No new on-device run was performed.

## Repeated matching investigation

The supplied M4 report shows many **matching1** requests. In the existing XNU
source that diagnostic is in `internal_io_service_get_matching_service`, i.e.
**IOServiceGetMatchingService**, not notification delivery or the iterator used
by Metal registration. `IOTaskRegistryCompatibilityMatching` adds
`IOServiceNotificationUser=true` to ordinary requests unconditionally; that key
is not proof of a notification re-arm. The service publishes once and does not
re-register/change matching properties after publication.

In this cache, Metal's `_MTLRegisterDevices` uses
**IOServiceGetMatchingServices**, enumerates once and is reached through the
device-array `dispatch_once` (`_MTLCreateSystemDefaultDevice`,
`0x181a37770..0x181a37828`). Initializer failure does not schedule a retry.
QuartzCore's accelerator wait (`0x18853e664..0x18853e6d4`) retries only while
**no service exists**, in 100 ms steps with a bounded counter; finding our
service exits that wait. It cannot explain repeated successful matching1 calls.

OpenGLES `_eagl_init` performs a single-service IOAcceleratorES probe at
`0x1d3f525d4..0x1d3f525e4`; `_EAGLMemoryNotificationRecycling` does another at
`0x1d3f528b4..0x1d3f528c4`. These are concrete alternative call sites for the
observed request shape. EAGL init can fail to establish its GL renderer and be
called again by clients; this service implements no GL renderer. Neither lookup
contains a property-wait or notification retry loop. The report has no caller
stacks, so attributing all requests to one function would be speculation.
**No kernel/property change is justified.** Do not add fictitious Apple/GL
plugin properties to suppress legitimate failed GL probes. The safety gate
removes our Metal device from those processes without changing service matching.

## CoreAnimation requirements from the iOS 16.7 cache

Same iPhone10,3 / 20H392 arm64 cache and SHA256 as below. Addresses are unslid.
These are call-site findings; branches depend on renderer flags, device limits
and workload, and are not evidence that every listed path runs on fuxi.

| Requirement | Cache evidence and consequence |
| --- | --- |
| Device/queue selection | `CAMetalContextCreate` (`0x18838421c`) calls the system default-device API. `CA::OGL::new_metal_context` (`0x188487e28`) creates a queue, checks queue/device identity, labels it, and conditionally sets GPU/background priorities and submission/completion queues. Queue failure returns null; a non-nil MTLDevice is not proof of CA compatibility. |
| Init state | `0x188488078..0x18848809c`: render-pass and depth/stencil descriptor/state creation. `0x18848822c..0x188488364`: `supportsMemorylessRenderTargets`, optional `CA_ENABLE_MEMORYLESS`, and `minConstantBufferAlignmentBytes`. These private queries need honest values, not inherited Apple implementations. |
| Feature/family/limits | `0x1884886d4..0x188488adc`: `supportsFamily:` includes Apple1/2/3/5 (1001/1002/1003/1005), vendorName, maxFragmentTextures/Samplers, maxTextureWidth2D/Height2D, maxColorAttachments, supportsRasterOrderGroups, supportsYCBCRFormats12/packed variants, supportsNativeHardwareFP16, supportsTileShaders, supportsSIMDGroup and iosurfaceReadOnlyTextureAlignmentBytes. `supportsFeatureSet:` gates offline pipeline variants at `0x1883758b8/0x1883758e0` and `0x18847eb30..0x18847ebac`. `readWriteTextureSupport` has **no direct call found** in this QuartzCore image; retain its conservative zero answer and census it if another framework asks. |
| Its own libraries | `0x1884883e0..0x188488424`: NSBundle for com.apple.QuartzCore resolves **default.metallib**, then calls **newLibraryWithURL:error:**. No direct `newLibraryWithData:` or `newDefaultLibrary` call was found here; supporting those alone would miss this entry. The frontend ultimately needs a URL-to-metallib/AIR path with checked lengths and errors, without invoking Apple's GPU compiler. |
| Function specialization | `load_shader_function` (`0x188482774`), especially `0x1884829bc..0x1884829e8`, uses MTLFunctionDescriptor, name, constant values and `newFunctionWithDescriptor:error:`. The vertex path at `0x1884862b0` uses precision suffixes and constants including fc_generic, fc_vertex_layout, fc_texcoord_count and fc_base_color; the fragment path starts at `0x18848566c`. Translation must include specialization and binding/vertex-layout metadata. |
| Pipelines | `create_pipeline_state` (`0x188380b94`) configures vertex/fragment functions, attachments, formats and blend/write state, then calls **newRenderPipelineStateWithDescriptor:error:** at `0x188381b9c/0x188381bf4`. Tile pipelines at `0x18848214c` and compute pipelines at `0x188482e20` are additional workload paths. Optional **default.pipelinelib**, newPipelineLibraryWithFilePath:error: (`0x188488534`), and lookup by pipeline name (`0x188375908`) are Apple precompiled-pipeline optimizations, not portable Vulkan binaries. |
| Pixel formats | `four_cc_to_mtl_format` (`0x1882d69f4..0x1882d7084`) converts IOSurface FourCCs using MetalLimits. Concrete cases include BGRA -> 80/BGRA8Unorm (`0x1882d6c1c..0x1882d6c3c`), A8 -> 1, L8 -> 10/R8Unorm, L16 -> 20/R16Unorm. Other paths include wide/half-float and YCbCr formats gated by limits. Unsupported combinations return format 0. First target BGRA8Unorm, then sampling/sRGB/plane formats actually requested; do not advertise the whole conversion table. |
| IOSurface textures | `create_surface_from_iosurface` (`0x188366dd0`): validates dimensions/format/limits, makes a texture descriptor with usage 5 (shader read + render target), applies protection options, calls **newTextureWithDescriptor:iosurface:plane:** at `0x18836702c` (plane 0 in this path), and cleans up if texture creation returns nil. Other multi-plane/import paths exist at `0x18830afc8`, `0x18831ee5c`, `0x1883471a4`. Real import requires HostImport/page lifetime/coherence and synchronization, absent from the current ABI. |
| Events/fences | CAMetalDrawable's **sharedEvent** is consumed at `0x18829e0c8`; `-[CAMetalDrawable sharedEvent]` is at `0x18829e240`. No direct **newSharedEvent** factory call was found in this QuartzCore image. This distinguishes drawable-supplied event consumption from a device-init requirement. Event ownership, wait/signal and cross-process lifetime still need census/implementation before CAMetalLayer presentation. |
| Software fallback | `CA::CG::AccelRenderer` selection at `0x188507960..0x188507af8`: nil device, disabled Metal route, or null Metal-context result goes to software creation when the caller's surface/backend policy permits it; another branch returns null. The window-server get_ctx block (`0x18853bfe8`) skips Metal-context construction for nil device. These branches do not establish universal fallback for every CA client. |
| Fatal failures after selection | Missing bundle/metallib URL hits assertions; nil library follows `0x188488424 -> 0x188488c80..0x188488d68` into **abort_with_payload**. Render-pipeline failure has a similar fatal diagnostic. Returning nil for every unsupported API is ABI-safe forwarding but is **not** sufficient to keep an already-selected CA Metal renderer alive. Deny by default, and retain the early census queue stop. |

Minimal shader candidates identified **statically**, not from a live census:
`std_vert0`/`std_vert1` in the vertex type table (`0x1d5b48050`), their
`_lph`/`_lpf` precision variants, and the fallback fixed vertex/fragment families
(`0x18848634c`, `0x188485724`), including `fixed_vert_lpf_spc`,
`fixed_vert_lph_spc`, `fixed_frag_lpf_cpf` and `fixed_frag_lph_cph`. Many blur/path/image/compositing variants and
function constants also exist. Do not infer a small complete shader set from
a first solid-background test.

Reproduce using external tools/Apple inputs, saving all output only in an
ignored analysis directory; no disassembly, cache, metallib or Apple source
belongs in git:

```sh
"$IPSW" dyld disass "$DSC" --symbol _MTLRegisterDevices --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _MTLCreateSystemDefaultDevice --symbol-image Metal
"$IPSW" dyld macho "$DSC" QuartzCore --symbols
"$IPSW" dyld disass "$DSC" --image QuartzCore > "$ANALYSIS/QuartzCore.disass"
"$IPSW" dyld disass "$DSC" --vaddr 0x188487e28 --count 1100
"$IPSW" dyld disass "$DSC" --vaddr 0x188380b94 --count 1150
"$IPSW" dyld disass "$DSC" --vaddr 0x188366dd0 --count 200
"$IPSW" dyld disass "$DSC" --vaddr 0x188507950 --count 110
"$IPSW" dyld dump "$DSC" 0x1d5b48050 --count 54 --addr
"$IPSW" dyld disass "$DSC" --symbol _eagl_init --symbol-image OpenGLES
"$IPSW" dyld disass "$DSC" --symbol _EAGLMemoryNotificationRecycling --symbol-image OpenGLES
```

## Shader translation decision

**The user approved (a) for M5b: an external dynamically loaded metal2vulkan
adapter.** M5a itself links none of it. M5b starts with our own constant-colour
MSL, not the CA shader candidates in this earlier survey. Cache design below
is future work; M5b has no shader/pipeline cache. Reviewed the actual pin
`720ef1ce9ae2a2068999a8675717418b9416283e`, including Cargo manifests, API,
AIR front end and cache documentation. The repository identifies itself as
alpha; this is a candidate, not an established CA/A740 translator. No CA
metallib was translated or benchmarked in M5a.

Effort below is an engineering estimate, excluding IOSurface/kernel work:

| Option | Concrete integration, effort and legal boundary | Device load cost and caching | First target |
| --- | --- | --- | --- |
| (a) external metal2vulkan | Medium: days for a pinned AIR parse/translation probe and narrow versioned C ABI adapter, weeks or more for specialization/binding/format correctness and iOS integration. The pin exposes Rust crates, not a ready stable C ABI/cdylib; build the adapter in the **external tree**, dlopen it from our original frontend, and pass bounded bytes/descriptors, results and free callbacks. Its AIR front end already understands metallib/bitcode, metadata, intrinsics and constants and emits SPIR-V plus pipeline/binding plans. All workspace crates declare **GPL-2.0-or-later OR LGPL-3.0-or-later**. Select/comply with LGPL for a separately replaceable dynamic library: notices/license texts, corresponding library source and modifications, and required relinking/replacement/installation rights/information. Dynamic loading alone does not waive those obligations. No static incorporation or vendoring here. Apple inputs and derived shader outputs stay external/private, outside repository/distribution artifacts. | Parse at library load; specialize/translate requested entry points at pipeline creation, then Turnip SPIR-V -> NIR/ir3 compilation. The library requires no llvm-as subprocess for binary AIR. The translator supplies content/profile/version keys and artifact encodings; it does **not** itself persist a cache. Its optional runtime has disk/memory and driver pipeline caches, but defaults (25 GiB disk/256 MiB memory) are unsuitable here. Use an explicit small bounded cache, atomic writes and corruption-as-miss. No A740 timing claim. | One std_vert0_lpf variant (or fixed_vert_lpf_spc) plus a fixed_frag_lpf_cpf constant-color specialization from CA's private default.metallib, BGRA8Unorm, one sample, no texture, no destination read/tile/compute. Get exact entry/constants/bindings from the external metallib; cache names alone do not establish their AIR ABI. |
| (b) independent LLVM AIR -> SPIR-V | High: weeks for a narrow prototype, months for robust graphics. LLVM reading bitcode is only the front end: independently lower AIR metadata, address spaces, resource binding, sampling/interpolation, derivatives, function constants and graphics stage ABI into Vulkan Shader-capability SPIR-V. Khronos SPIRV-LLVM-Translator explicitly targets **OpenCL Kernel** SPIR-V; it is not a drop-in Vulkan vertex/fragment compiler. LLVM version compatibility and bitcode dialect remain checks. Observe LLVM/translator licenses and notices for the exact selected versions; using metal2vulkan's AIR crate/parser would retain its GPL/LGPL boundary rather than creating an independent permissive path. Never copy Apple's compiler implementation. | Larger LLVM dependency, startup/RSS and optimization cost on cold loads; still needs Turnip compilation. Translate only requested specializations, with finite budgets and the same content/ABI/driver cache keys. Host-assisted translation may speed experiments but is not the required on-device runtime. | The same constant-color pair, first proving metadata/resource/vertex IO lowering and spirv-val Vulkan validation. A generic llvm-spirv invocation is not this milestone. |
| (c) original hand-ported replacements | Low for one offscreen constant-color test (days); medium/high and open-ended for CA compatibility (weeks/months as variants grow). Reimplement the observed interface and mathematically specified transform/color behavior in original GLSL/SPIR-V. No copied/decompiled Apple shader bodies, embedded Apple metallibs or instruction-by-instruction transcription; keep provenance for each implementation. Map only understood entry name + specialization + layout tuples and refuse every other variant. A small live working set has **not** been established. | Ship only our own compiled SPIR-V; no AIR translation at load. Still pay Vulkan pipeline/ir3 compilation once, cache by replacement version + specialization + render state + driver identity. Lower startup/RSS than an LLVM path but a growing compatibility table. | Original transform + solid premultiplied-color pair, one BGRA8Unorm target. Useful as a separately named validation control for (a); not evidence that it replaces CA's shader library. |

Sources: pinned [manifest/license](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/Cargo.toml),
[API and caching](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/docs/HOWTO.md),
and Khronos [translator scope](https://github.com/KhronosGroup/SPIRV-LLVM-Translator#llvmspir-v-bi-directional-translator).

M5b's concrete gate: parse the user's own external QuartzCore metallib, list
entry points/constant IDs/binding metadata, choose the pair above, translate
under a finite budget, validate Vulkan SPIR-V, create a bounded offscreen
256x256 BGRA target and compare every pixel for an identity-transform solid
quad plus edge sentinels. Record cold translation/pipeline time, warm cache
load time and RSS on device; restart and test cache invalidation after a
translator/profile/driver change. Validate color order, alpha, clipping and
vertex layout before attempting texture sampling. Keep backboardd denied:
passing that test does not supply the IOSurface/shared-event/presentation paths.

## M5a build, verification and coordinator staging

No XNU source/patch change. `build-fuxi-metal5a-images.sh` copies the M4 stripped
and unstripped kernels, ADT and **exact command-line file**, then packs the new
image. The resulting boot image is byte-identical to M4. The userspace bundle
and policy must be staged for the gate to take effect; booting the renamed
image alone does not install them.

```sh
# Native Mac, same external pinned Vulkan headers:
bash tools/build-fuxi-metal.sh "$VULKAN_HEADERS"
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-stage.py
# Linux actual backend with software Vulkan and injected faults:
python3 tools/test-fuxi-metal-backend.py --headers "$VULKAN_HEADERS"
# External absolute firmware/tool paths, same as M4:
export FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG
bash tools/build-fuxi-metal5a-images.sh "$METAL4_ARTIFACTS"
python3 tools/test-fuxi-metal-artifacts.py --lane metal5a \
  --base "$METAL4_ARTIFACTS" --out xnu/out-fuxi-metal5a \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
# Coordinator only, on an already mounted writable root, as root:
python3 tools/stage-fuxi-metal.py "$IOS_ROOT" \
  tools/out/libvulkan_freedreno.dylib tools/out/FuxiMetal.bundle \
  tools/out/fuxi-metal-smoke --conf tools/out/metal-smoke.conf \
  --clients tools/out/metal-clients.conf
```

Passed: Mac ASan/UBSan frontend/gate/census and actual smoke control-flow tests;
Linux software-Vulkan backend fill/copy/reopen plus timeout/loss/corruption;
temporary-root staging and **codesign verification of the staged bundle** on
Mac; existing Metal ABI/containment tests; artifact geometry, firmware sections,
M4 byte equality and hash manifests. Plain arm64 iOS 16.0 artifacts were ad-hoc
signed and signature-verified; load-command/CTR_EL0 audit and all **140** strong
imports against the supplied iOS 16.7 cache passed. Audit reports now live
outside the signed bundle and stale reports are removed before signing, so
staging the three intended bundle members preserves its resource signature.
These checks qualify host behavior/packaging, not M5a phone execution.

Install all files below, **root:wheel (0:0)**. The default policy is exactly
`fuxi-metal-smoke` plus newline, with no system UI allow/census entries. No
launchd job, mounts, phone access, push, shader translator or firmware staging
is part of this helper.

| Artifact under `tools/out/` | iOS destination | Mode |
| --- | --- | --- |
| `libvulkan_freedreno.dylib` (unchanged M4) | `/usr/local/lib/libvulkan_freedreno.dylib` | 0755 |
| `fuxi-metal-smoke` (unchanged M4) | `/usr/local/bin/fuxi-metal-smoke` | 0755 |
| `metal-smoke.conf` | `/usr/local/share/fuxi/metal-smoke.conf` | 0644 |
| **`metal-clients.conf`** | **`/usr/local/share/fuxi/metal-clients.conf`** | **0644** |
| `FuxiMetal.bundle/FuxiMetal` | `/System/Library/Extensions/FuxiMetal.bundle/FuxiMetal` | 0755 |
| `FuxiMetal.bundle/Info.plist` | `/System/Library/Extensions/FuxiMetal.bundle/Info.plist` | 0644 |
| `FuxiMetal.bundle/_CodeSignature/CodeResources` | `/System/Library/Extensions/FuxiMetal.bundle/_CodeSignature/CodeResources` | 0644 |

| M5a artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal5a/kernel.development.fuxi` | `8bfeae158e8250e906e3b3ffbcbddb8f5d63404eedd9807cf8e2838cf7a1b5d9` |
| `xnu/out-fuxi-metal5a/kernel.development.fuxi.unstripped` | `858e6cfe0b636530f7f8d097f23ca84a5f7efd0516093b74c5aba90431d353a8` |
| `xnu/out-fuxi-metal5a/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-metal5a-payload.bin` | `99ac95954ae0c743dac817d8be460933bcc99e7b05dff98925b4cda14ad7bcd8` |
| `shim/out/fuxi-metal5a-boot.img` | `e10f981a0bae32ad5c892d978a786eabe4cc21acda6e35ed064f1ad68a53c4b6` |
| `shim/out/fuxi-metal5a-loader.elf` | `0e5ae80e22a6a88d5e880a97d8b66a7c41750bae804728d8c99f76d9e23c07e9` |
| `tools/out/libvulkan_freedreno.dylib` | `5661a054b5e4be57fe54bc38f3cccf6d264ae20562473caf72e50d2adc158a8c` |
| `tools/out/FuxiMetal.bundle/FuxiMetal` | `1c4c8ab83cff014d45cb6dd8cb55802ff1ce7d8b693ca219967d0cfc0e6da37c` |
| `tools/out/FuxiMetal.bundle/Info.plist` | `2578f53d73942a55e0163fa5c80c29beb00839bfb8db477a4060e0a81ab5fd06` |
| `tools/out/FuxiMetal.bundle/_CodeSignature/CodeResources` | `9fcd9223ba2b40aee792c83c8edf32c7c1847f01369fbb0f230e1108a493451b` |
| `tools/out/fuxi-metal-smoke` | `64e747d6a7079fee0a675b2f757752c1569b20c19484259d5122fcc63467aec7` |
| `tools/out/metal-smoke.conf` | `1121cfccd5913f0a63fec40a6ffd44ea64f9dc135c66634ba001d10bcf4302a2` |
| `tools/out/metal-clients.conf` | `8d5b163a382ff4eca7579803ec5dd9e2f24f8c0a4e3d65fc6965c539b3e7e6cc` |

Complete manifests and analysis/test output are ignored. The supplied M4
device input remains untracked. Only the original source, tools and this
summary are committed; no external dependency, Apple code/binary, device log,
firmware, machine-specific path or address is included.

---

# Metal lane M4: native discovery and transfer-only frontend

2026-10-20. **Observed PASS on device, coordinator report above.** M3
passed the coordinator's metal3 device run (report below). M4 adds a native
`IOAcceleratorES` discovery metaclass, the `FuxiMetal.bundle` Objective-C++
frontend and the root `fuxi-metal-smoke` executable. The executable calls the
system **MTLCreateSystemDefaultDevice()**: no interposition, direct constructor,
Apple family/user-client ABI, shader compiler or MSL translation is used.
No phone access, mounted-root edit or push was performed.

## M4 route decision and evidence

**Route (a)** is small enough in this kernel. An original empty
`IOAcceleratorES : IOService` class is defined in the existing native client
translation unit; `NativeAdrenoGpuPublished` now derives from it. The published
service keeps its name, root-only open types and NativeAdrenoABI contract, and
adds string properties `MetalPluginName=FuxiMetal` and
`MetalPluginClassName=FuxiMTLDevice`. It checks `metaCast("IOAcceleratorES")`
before publication and logs PASS/FAIL. No IOGPUFamily implementation is added.
The canonical source and regenerated 0100 delta remain byte-checked together.

Re-derived from the same iOS 16.7 cache identified below:

| Check | Evidence and implication |
| --- | --- |
| Metal lookup | `_MTLRegisterDevices`, `0x181b14514..0x181b1452c`, passes `IOAcceleratorES` to IOServiceMatching/GetMatchingServices. It allocates the returned plugin class and calls `initWithAcceleratorPort:` at `0x181b14578..0x181b14580`. |
| Exact dictionary semantics | `_IOServiceMatching`, `0x18e556804..0x18e556810`, constructs the **IOProviderClass** string property. In the cached XNU 8796 sources, `IOService::copyExistingServices` calls `OSMetaClass::applyToInstancesOfClassName`; the instance set recursively includes subclass sets. `matchInternal` uses `metaCast` when the class test was not already completed. This matches the **service's metaclass ancestry**, not its provider object's class, registry name, or an IOClass property. Defining the missing base metaclass is sufficient for this lookup. |
| Bundle loading | `_getMetalPluginClassForService`, `0x181a3c7c8..0x181a3c8f4`, constructs `/System/Library/Extensions/FuxiMetal.bundle`, tries `classNamed:`, falls back to `principalClass`, checks the result is an `_MTLDevice` subclass, then returns it. |
| Runtime class ownership | `-[NSBundle classNamed:]`, `0x1811e2b5c`, loads the bundle but requires `bundleForClass:` to equal that bundle. A runtime-created class has no image ownership. Therefore the plugin deliberately relies on the documented **principalClass fallback**, with `NSPrincipalClass=FuxiMTLDevice`. `__NSBundleGetPrincipalClassFromInfoDict`, `0x1811f17c0`, resolves that value with NSClassFromString without the ownership test. |
| Registration hooks | `_MTLAddDevice`, `0x181a1b3b0..0x181a1b3f0`, checks MTLDevice and MTLDeviceSPI conformance, then calls `initLimits` and `initFeatureQueries`. The frontend implements those registration hooks; capability queries use its own conservative overrides rather than Apple's family tables. The default-device block at `0x181a37050` returns the sole registered device. |

Reproduce these checks with external Apple inputs; keep the output ignored:

```sh
"$IPSW" dyld disass "$DSC" --symbol _MTLRegisterDevices --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _getMetalPluginClassForService --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _IOServiceMatching \
  --symbol-image /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
"$IPSW" dyld disass "$DSC" --symbol '-[NSBundle classNamed:]' --symbol-image Foundation
"$IPSW" dyld disass "$DSC" --symbol __NSBundleGetPrincipalClassFromInfoDict --symbol-image Foundation
"$IPSW" dyld disass "$DSC" --symbol _MTLAddDevice --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol ___MTLCreateSystemDefaultDevice_block_invoke --symbol-image Metal
```

This establishes the lookup contract by disassembly/source review and native
linking. The coordinator subsequently observed the boot-time metaclass check and
actual bundle/Metal registration PASS, as recorded in M5a above.
M4a direct construction was unnecessary once this small route was established.

## M4 frontend contract

`tools/fuxi-metal/Frontend.mm` registers `FuxiMTLDevice` with
objc_allocateClassPair only if **class_getInstanceSize(_MTLDevice)==528**.
It adds no private-layout ivars: its instance size is also 528. State lives in
an associated owner object. The private superclass initializer/deallocator
is bypassed; inherited private operations are replaced with forwarding, and
NSObject lifetime/introspection behavior is supplied explicitly. Runtime
protocol signatures govern unsupported calls: object results log and return
nil; unsupported scalar/struct/void calls log and raise, never report success.
The native ABI remains root-only, and plugin initialization also requires root.
The registryID comes from IORegistryEntryGetRegistryEntryID on the actual
accelerator port; the cache defines that dynamically resolved IOKit symbol.

Supported: name/registryID, a command queue, retained command buffers, shared
buffers (with or without initial bytes), contents/length, blit fill/copy,
endEncoding, commit/wait/status/error and completion handlers before or after
completion. Creation accepts default shared options **0** and lengths
**1..16 MiB**. Transfer offsets/lengths must be in bounds and **4-byte aligned**;
overlapping copies and unsupported options fail loudly. The frontend advertises
no Metal GPU family/feature set, shader/texture sampling, ray tracing, dynamic
libraries or render/compute thread capacity. This diagnostic subset is not a
complete Metal family. Inherited ABI limits (64 BOs, 256 MiB live wired budget,
one client and one hardware job) still apply; allocation failure returns nil.

`Backend.hpp` directly dlopens `/usr/local/lib/libvulkan_freedreno.dylib` after
setting the same service/open/debug environment as M3: NativeAdrenoGpuPublished,
open type 0, TU_DEBUG=nocb and MESA_LOG_LEVEL=debug. It requires the observed
Qualcomm device identity. Operations use vkCmdFillBuffer/vkCmdCopyBuffer, explicit
host/transfer and transfer/transfer barriers, mapped-memory flush/invalidate,
and a finite **2 s fence wait**. Commit is synchronous; the owner mutex
serializes Vulkan work/allocation/destruction. Encoded resources stay retained
through completion. Unknown ownership after submit/fence failure poisons the
backend and skips ordinary driver/resource teardown; process death and the
existing kernel containment path own cleanup. There is no recovery claim.

The smoke ignores every argv value and reads a prefix stage **0..3** from
`/usr/local/share/fuxi/metal-smoke.conf` (absent defaults to 3). Malformed or
unreadable config fails at stage 0. Stdout/stderr are unbuffered. Discovery and
fill/copy have 30 s process alarms, reuse 60 s, cleanup 10 s. Failure exits
without driver teardown. The tool enforces the FuxiMTLDevice class and size,
not merely any non-nil device. Expected output, **not device observations**:

```text
FUXI MTL: stage 0 discovery PASS name Fuxi Turnip Adreno 740 (M4 transfers only) class FuxiMTLDevice base-size 528 instance-size 528 registryID <actual ID>
FUXI MTL: stage 1 fill PASS verified 65536 bytes + completion handlers
FUXI MTL: stage 2 copy PASS verified CPU-write/GPU-copy/readback + sentinel edges
FUXI MTL: stage 3 reuse PASS verified 16 fill/copy rounds
FUXI MTL: done PASS
```

The kernel should additionally report `FUXI MTL: IOAcceleratorES metaclass match
PASS plugin FuxiMetal/FuxiMTLDevice`; plugin stderr reports runtime class sizes.
Require all selected stages, no GPU loss/SMMU fault/panic, and CPU-present
fallback continuity. Bundle trust/loading, sandboxed clients and competition
for the single native connection remain unqualified.

## M4 rebuild, staging and verification

On the assigned native Mac tree, using the external pinned M3 Vulkan headers:

```sh
bash tools/build-fuxi-metal.sh "$VULKAN_HEADERS"
python3 tools/test-fuxi-metal-frontend.py
# Linux: actual backend on the external software Vulkan ICD, with ASan/UBSan
python3 tools/test-fuxi-metal-backend.py --headers "$VULKAN_HEADERS"
```

The build targets plain arm64-apple-ios16.0 with the iPhoneOS 27.1 SDK and
ad-hoc signs/verifies the bundle and executable. Load-command audits passed:
iOS 16.0 minimum, system dependencies only, no RPATH/strong dynamic lookup and
no CTR_EL0 reads. All **126** combined strong imports were checked against the
supplied iOS 16.7 cache. No Mesa, metal2vulkan, firmware or Apple binary/source
is vendored. M5's external licensing/relinking work remains separate.

Host validation: actual Vulkan fill/copy/barriers/readback with sentinel edges,
32 submissions, buffer reuse and full backend reopen under ASan/UBSan; injected
fence timeout, loss and corruption stop execution. Native Mac Objective-C tests
use a synthetic 528-byte superclass and CPU backend, exercising size rejection,
protocols, inherited unsupported calls, ownership, handlers, errors, bounds,
alignment and overlap. The actual smoke source also passed all stage prefixes,
default/invalid configs, host argv, nil/nonroot/failure/corruption and alarm
checks with that explicit host adapter. Those tests do not qualify the private
iOS base class or real discovery. Existing ABI/backend/watchdog/quarantine,
managed-BO lifetime, NC mapping, exec-swap and patch-chain tests passed.

Native XNU was rebuilt with the standard native/IIG setup. All 32 native input
hashes match the M3 manifest except the intended canonical client change.
Outputs are in `xnu/out-fuxi-metal4/`. Its ADT is byte-identical to metal3.
The command line changes the exec-swap target to
`sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-metal-smoke` and appends
fuxi-metal-smoke to metal3's sweetnull list; all other arguments are preserved.
Pack with all firmware/tool variables naming existing **absolute external files**:

```sh
export FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG
bash tools/build-fuxi-metal4-images.sh "$METAL3_ARTIFACTS"
python3 tools/test-fuxi-metal-artifacts.py --lane metal4 \
  --base "$METAL3_ARTIFACTS" --out xnu/out-fuxi-metal4 \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
python3 tools/test-fuxi-metal-stage.py
```

Packaging tests passed native service/metaclass and recovery symbols, exact
command line/ADT, boot-v4/Image/payload geometry and RAM bounds, allocated exact
pinned GMU/SQE/zap bytes in the final boot Image, signatures/minimum OS and
SHA256SUMS. Firmware stays external and ignored.

For the coordinator's later offline root edit, as root on an already mounted
writable iOS root:

```sh
python3 tools/stage-fuxi-metal.py "$IOS_ROOT" \
  tools/out/libvulkan_freedreno.dylib tools/out/FuxiMetal.bundle \
  tools/out/fuxi-metal-smoke --conf tools/out/metal-smoke.conf
```

The helper validates every source/destination before copying, refuses unsigned
or wrong-target artifacts and escaping symlinks, installs no launchd job, and
sets newly created directories to root:wheel 0755. Temporary-root tests passed
idempotence, file/directory modes/ownership and malformed input/symlink rejection.
Stage these files; all owners are **root:wheel (0:0)**:

| Local artifact | iOS destination | Mode |
| --- | --- | --- |
| `tools/out/libvulkan_freedreno.dylib` | `/usr/local/lib/libvulkan_freedreno.dylib` | 0755 |
| `tools/out/fuxi-metal-smoke` | `/usr/local/bin/fuxi-metal-smoke` | 0755 |
| `tools/out/metal-smoke.conf` | `/usr/local/share/fuxi/metal-smoke.conf` | 0644 |
| `tools/out/FuxiMetal.bundle/FuxiMetal` | `/System/Library/Extensions/FuxiMetal.bundle/FuxiMetal` | 0755 |
| `tools/out/FuxiMetal.bundle/Info.plist` | `/System/Library/Extensions/FuxiMetal.bundle/Info.plist` | 0644 |
| `tools/out/FuxiMetal.bundle/_CodeSignature/CodeResources` | `/System/Library/Extensions/FuxiMetal.bundle/_CodeSignature/CodeResources` | 0644 |

Boot artifact: `shim/out/fuxi-metal4-boot.img`. The following hashes describe
this build; complete manifests also live in the ignored output directories.

| M4 artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal4/kernel.development.fuxi` | `8bfeae158e8250e906e3b3ffbcbddb8f5d63404eedd9807cf8e2838cf7a1b5d9` |
| `xnu/out-fuxi-metal4/kernel.development.fuxi.unstripped` | `858e6cfe0b636530f7f8d097f23ca84a5f7efd0516093b74c5aba90431d353a8` |
| `shim/out/fuxi-metal4-boot.img` | `e10f981a0bae32ad5c892d978a786eabe4cc21acda6e35ed064f1ad68a53c4b6` |
| `tools/out/libvulkan_freedreno.dylib` | `5661a054b5e4be57fe54bc38f3cccf6d264ae20562473caf72e50d2adc158a8c` |
| `tools/out/FuxiMetal.bundle/FuxiMetal` | `9535102237abd309c94978b3ab52a1435f124b4f022ab6bdfeab0b39f60dc603` |
| `tools/out/FuxiMetal.bundle/Info.plist` | `2578f53d73942a55e0163fa5c80c29beb00839bfb8db477a4060e0a81ab5fd06` |
| `tools/out/FuxiMetal.bundle/_CodeSignature/CodeResources` | `26aaa98e843bdbffb42ecaefb507ff685bdf749a45ceb1d0c68a0c15d58e903c` |
| `tools/out/fuxi-metal-smoke` | `64e747d6a7079fee0a675b2f757752c1569b20c19484259d5122fcc63467aec7` |
| `tools/out/metal-smoke.conf` | `1121cfccd5913f0a63fec40a6ffd44ea64f9dc135c66634ba001d10bcf4302a2` |


M6 still needs IOSurface/texture import, CAMetalLayer drawables, producer/consumer
fences and lifetime/coherency, pixel format/stride/orientation, and the render
and compute paths actually used by CoreAnimation. M4's discovery and buffer
transfers do not supply any of those. The existing kernel CPU-present fallback
is unchanged; accelerated app/CA correctness, latency, thermal and memory
measurements remain acceptance gates after M5 translation/licensing.

---

# Metal lane M3: staged Turnip submission smoke

2026-10-20. **M2 observed on metal2h**, reported by the coordinator: direct ICD
load, instance creation, one Qualcomm physical device (`0x5143` / `0x43050a01`,
Turnip Adreno 740, pinned Mesa 26.2.0-devel), 1.5 GiB device-local heap,
logical device/queue creation and destruction all passed. Three internal BOs
were allocated, GPU-mapped, CPU-mapped/unmapped and freed before close. No panic
and no managed-page physical-descriptor warning occurred. This confirms the
metal2h mapping fix and the M2 lifetime gate; no GPU work was submitted.

**M3 observed PASS on metal3 (`5fd2350`), coordinator report 2026-10-20:**
root smoke exec-swapped into peakpowermanagerd completed setup, fill (16384
words, fence 1.2 ms), compute (16384 words), triangle (six probes, red 8192 /
black 57344), 16 fill reuse rounds, device/instance destruction and reopen,
then fill/compute again, ending `done PASS`. No hang, SMMU fault or panic was
reported. This qualifies the M3 Turnip workload, not Metal or CoreAnimation.

**M3 build work:** No phone access, root-image
edit or push was performed. The new kernel changes only add a read-only,
once-per-loss register snapshot before existing isolation. The native tree
also includes the inherited WLAN RC SID initialization fix at this branch tip;
`fuxiwlan` remains absent/off. No speculative CP initialization change is made.

`tools/fuxi-vk-smoke.cpp` ignores every argv value, including the executable
name, because exec-swap supplies the host daemon's argv. It reads a maximum
stage **1..4** from `/usr/local/share/fuxi/vk-smoke.conf`; absent config defaults
to **4**, malformed/unreadable config fails before loading the ICD. The staged
config contains `4` and a newline. Stages run as a prefix, stop at the first
failure and print unbuffered stdout:

| Stage | Work and acceptance |
| --- | --- |
| 1 `fill` | Recreated 64 KiB host-visible buffer; `vkCmdFillBuffer`, transfer-to-host barrier, submit/fence, invalidate mapped range, compare all 16384 words against `0xa740cafe`. |
| 2 `compute` | 256 workgroups of 64 invocations; storage buffer receives `(i * 1664525 + 1013904223) ^ 0xa740cafe` with uint32 arithmetic. Compare all 16384 words. |
| 3 `triangle` | Offscreen 256x256 RGBA8 UNORM render pass, opaque red triangle on opaque black, transfer to host buffer. Validate all pixels are red/black, six known inside/outside samples, and red count 8192 +/-128. |
| 4 `reuse-reopen` | Repeat fill 16 times with destruction/recreation and varying patterns; destroy device **and instance**, create both again in the same process (closing/reopening the global Darwin connection), then repeat fill and compute. |

The three GLSL files live in `tools/shaders/`. `glslangValidator -V
--target-env vulkan1.1` generates `tools/out/vk-smoke-spv.h` at build time; C
arrays are embedded in the signed executable. No runtime shader compiler or
shader sidecar is staged. Each submit prints BEGIN and fence rc/elapsed-ms.
The smoke forces `TU_DEBUG=nocb`, direct service/open type 0, and
`MESA_LOG_LEVEL=debug` **before dlopen**. No sysmem/gmem/flushall/syncdraw override
changes the submission path. The ICD overlay logs accepted IB count and seqno.

Fence waits request **2 s**. On timeout the tool prints FAIL and elapsed time,
then attempts `vkDeviceWaitIdle` once under a **1 s SIGALRM wall bound**; either
return or alarm exits nonzero without driver teardown. Setup/stages/teardown
also have 30/30/10 s wall bounds (60 s for stage 4), covering a stuck submit or
cleanup. Other failures exit immediately and leave connection cleanup to
process death and the independent kernel watchdog. These software bounds do
not make wedged MMIO/interconnect access recoverable.

Rebuild with the existing M2 external pins/cache and the same
`tools/build-fuxi-turnip.py` command below. It now builds/signs both diagnostics,
applies `tools/patches/mesa-fuxi-smoke.patch` **after** the iOS16 overlay, and
checks the exact patched tree, including an already patched M3 cache. The M3
overlay SHA256 is
`6a2f66e50170887d26fb30a3ed67c1aabe3a5a49d4fdfbc6de0f17b1c77f2c13`.
The historical M2 artifact hashes below describe the observed no-submit build.

Native XNU was rebuilt in the assigned `ci-lane-metal2` tree with all 32 native
inputs checked against the branch/canonical sources and unchanged base inputs.
Use the standard native build/IIG setup, copy stripped/unstripped kernels to
`xnu/out-fuxi-metal3/`, then pack/check:

```sh
export FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG
# Every variable above must name an existing absolute external file.
bash tools/build-fuxi-metal3-images.sh "$METAL2H_ARTIFACTS"
python3 tools/test-fuxi-metal-artifacts.py --lane metal3 \
  --base "$METAL2H_ARTIFACTS" --out xnu/out-fuxi-metal3 \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
```

The ADT is byte-identical to metal2h. The command line changes only the
exec-swap target to
`sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-vk-smoke` and adds
`sweetnull=backboardd,SpringBoard,fuxi-vk-smoke,peakpowermanagerd`.
Firmware remains external, hash-pinned and present in the boot Image.

For a later root edit, as the mounted-root editor:

```sh
python3 tools/stage-fuxi-vulkan.py "$IOS_ROOT" \
  tools/out/libvulkan_freedreno.dylib tools/out/fuxi-vk-probe \
  --smoke tools/out/fuxi-vk-smoke --conf tools/out/vk-smoke.conf
```

This retains the probe and stages the ICD and both binaries as **0:0 0755**,
and the config at `/usr/local/share/fuxi/vk-smoke.conf` as **0:0 0644**. No job
is installed. Boot `shim/out/fuxi-metal3-boot.img` through the coordinator's
usual temporary-boot workflow only after those files are staged.

Expected acceptance lines (not device observations):

```text
FUXI VK: stage 0 setup PASS instance/device/graphics+compute queue
FUXI VK: stage 1 fill PASS verified readback
FUXI VK: stage 2 compute PASS verified readback
FUXI VK: stage 3 triangle PASS verified readback
FUXI VK: stage 4 reuse-reopen PASS verified 16 recreations + device/instance reopen + fill/compute
FUXI VK: done PASS
```

## M3 submit/fence review and failure triage

Reviewed the pinned `tu_knl_darwin.cc`, Turnip's `tu_queue.cc`,
`tu_cmd_buffer.cc`, A6XX/A7XX XML registers, and this tree's ABI client,
`fuxi_abi_wire.h`, backend and ring bring-up. Findings:

- **Timeouts:** relative ABI nanoseconds, zero polls once, kernel client slices
  at 1 ms and caps any call at 1 s. Independent hardware ownership deadline is
  100 ms. Darwin waits use monotonic absolute deadlines and 1 ms ABI slices.
  The overlay caps the final slice to remaining time, saturates relative-to-
  absolute addition, and propagates device loss in wait-any instead of looping.
  A watchdog loss is reported as ABI Timeout, so an infinite Vulkan wait could
  keep polling it; this tool always uses finite waits and a process alarm.
- **Seqnos:** global uint32 monotonic fence with per-connection first/last
  bounds; kernel refuses wrap. KMD's uint32 fence storage matches this limit;
  a fresh connection resets KMD tracking but kernel seqnos continue. Queue 0
  exists at open; hardware accepts one outstanding job. Darwin's eight-entry
  inflight history is bookkeeping, not eight supported Fuxi hardware slots.
- **IBs/BOs:** 1..16 top-level IBs; each handle must be live and GPU-mapped,
  4-byte address/length alignment, nonempty, wholly inside that BO, at most
  `0xfffff` dwords (4194300 bytes). Queue/reserved/wait-fence fields and exact
  struct sizes are checked before submission. There is no separate residency
  list: all live mapped BOs are already in the client's root, including shader,
  descriptor and nested IB BOs. No PM4 opcode/nested-IB validation or security
  boundary is claimed. Freed mappings are invalidated before release; busy
  work prevents unmap. The KMD previously split >16 entries into separate
  submits, but this kernel resets context on each submit: the overlay rejects
  such lists **before any partial submission** with `Fuxi IB list N exceeds
  atomic submit limit 16`. It does not truncate or insert extra doorbells.
- **Caches:** KMD BoMap requests Normal NC (`writeback=0`, outer-shareable 2),
  matching CPU write-combine BO views. The kernel publishes CPU stores with
  `dsb sy`, flushes/invalidates GPU caches in the frame and switches TTBR only
  while idle with ASID invalidation/TLBSYNC. Completion uses BR/BV timestamps,
  idle/ring checks and restored root. The tool flushes initialized mappings,
  emits shader/transfer-to-host barriers, and invalidates before verification,
  including noncoherent memory. No cached-coherent or HostImport promise was
  added; actual shader/UCHE/CCU coherence remains a device gate.
- **Ring contract:** cold init programs SQE/ring/RPTR/BV RPTR bases, privileged
  access/protection, HBB/UBWC NC and UCHE GMEM/trap ranges. CP_ME_INIT omits the
  unavailable IFPC restore-list ordinal; GX stays held, IFPC/preemption are
  disabled, no preemption records or separate BV/LPAC ring are invented. The
  per-submit frame synchronizes BR, resets BOTH threads/context, waits for
  PERFCTR SRAM initialization, invalidates caches, runs IBs and retires BV then
  BR with idle. Turnip marks `has_preemption=false`; its command buffers emit
  static register initialization, CP_SET_DRAW_STATE, CP_SET_MARKER render modes,
  BR/BV offsets/ambles/thread controls and CCU/GMEM configuration themselves.
  Thus missing marker/draw state or preemption records cannot be asserted as
  the cause of UniXNU's reported CP hang. The later M3 run above qualified this
  subset for its tested Turnip workload; raw CP_MEM_WRITE/BLIT alone was weaker
  evidence. No speculative framing change was justified by the code review.

Historical pre-run hypotheses (the reported M3 run subsequently passed):

1. **CP/thread/context initialization mismatch.** Stage-1 accepted submit then
   no retirement; `CP_RB_RPTR/WPTR`, `CP_IB1_BASE_*`, `CP_IB1_REM_SIZE`,
   `CP_STATUS_1`, `CP_HW_FAULT`, `CP_INTERRUPT_STATUS`, `CP_PROTECT_STATUS` and
   `RBBM_STATUS/STATUS3` distinguish fetch/protection/error evidence from a
   busy BR/BV wait. An unchanged IB remainder with no faults suggests a stalled
   packet/wait, not proof of which packet. Preserve all snapshot lines.
2. **Client SMMU/nested resource mapping.** `SMMU GPU CB` logs TTBR, FSR, FAR,
   FSYNR0/1 and GFSR. Nonzero fault plus FAR into an IB/shader/descriptor range
   points to translation/access or lifetime; clean FSR alone does not prove
   all mappings or cache attributes correct.
3. **Cache visibility/flush ordering.** Fence rc 0 followed by `fill ... word`
   or `compute word ... got ... want ...` fails data rather than retirement.
   Failures first appearing in stage 4 implicate reuse/alias/lifetime/TLBI.
4. **Shader or graphics state/GMEM/UBWC setup.** Fill PASS then compute FAIL
   narrows to shader/SP/descriptor execution; compute PASS then triangle FAIL
   narrows to graphics/CCU/image copy. `triangle pixel`, `triangle sample` or
   `triangle count` distinguishes invalid bytes, geometry and coverage. Kernel
   GMEM range initialization is not a complete render-pipeline setup.
5. **ABI/quota/IB limits before CP.** Tool API `rc`, KMD `submit failed: 0x...`
   or explicit atomic IB-limit error, with no accepted `FUXI KMD: submit IBs N
   fence F`, distinguishes rejected work. BO limit is 64 and live wired quota
   256 MiB despite the advertised 1.5 GiB heap. On reopen failure, check close,
   open and `stage 4 reuse-reopen` setup failure before blaming the shader.

On a pending fault/deadline the kernel prints **16 bounded `FUXI GPU HANG:`
lines once per boot-wide loss, before reset**, including all registers above
and MMIO-access-failed state. The independent 2 ms watchdog observes the 100 ms
job deadline even without WaitFence calls. It permanently disables submission,
drains GBIF (bounded polling), holds reset and faults SID0/1; `GBIF drain ACK`
permits release, `GBIF drain FAILED ... source quarantined until reset` retains
DMA backing. CPU presentation remains the existing fallback; no CP restart or
recovery is attempted. Timing depends on scheduler/MMIO progress.

Validation: real smoke commands/readbacks on the software ICD under ASan/UBSan
passed all four stages, exact 8192/57344 pixel counts and complete reopen.
Injected final-word corruption, device loss, timeout, hung idle, malformed
config and host argv passed stop/bound checks. Extracted actual Darwin KMD
functions passed finite/poll/overflow waits, wait-any loss propagation, 17-IB
rejection before submit, 16-IB and empty-submit fence tests. Existing no-submit
probe, ABI/backend/framing/patch-chain, exec-swap, NC/cache/mapping/lifetime,
present quarantine, memory and native SCM checks passed. Mach-O audit/signature
verification passed (plain arm64, iOS 16 minimum, system dependencies, no
CTR_EL0 reads); the eight additional strong imports were found in the supplied
iOS 16 cache. These checks do not qualify Turnip GPU execution or Metal.

| M3 artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal3/kernel.development.fuxi` | `194e4222564b1b27152ca43b992d39ca7e38bc8f7bfc118748cadf300375a9e0` |
| `xnu/out-fuxi-metal3/kernel.development.fuxi.unstripped` | `6efaf3446b989820b056cb96ca1214bde44165d2975d399d94e548359db45482` |
| `shim/out/fuxi-metal3-boot.img` | `61240f263a112d6f5d2bcd51aa959ccfafaa0c8dae59a8b8374d2b5c778fe2b5` |
| `tools/out/libvulkan_freedreno.dylib` | `5661a054b5e4be57fe54bc38f3cccf6d264ae20562473caf72e50d2adc158a8c` |
| `tools/out/fuxi-vk-smoke` | `1d7fda3c9cd94827a18b77a4111a058eb75644535c66623cc1498c1dab0edc34` |
| `tools/out/vk-smoke.conf` | `7de1555df0c2700329e815b93b32c571c3ea54dc967b89e81ab73b9972b72d1d` |

---

# Metal lane M2 / metal2h: managed BO CPU mapping cleanup

2026-10-20. **M1 qualified on device**, as reported by the coordinator for
metal2g (`5c0b2da`): root pid 78, hosted by peakpowermanagerd, completed
open/query/device-info/alloc/map/cpu-token/cpu-map/submit/wait with rc 0,
`mem 0xa740cafe bad-pixels 0 rounds 1/2`, close/reopen, the same result for
`rounds 2/2`, then free/close with no panic. This is observed raw PM4 success,
not Turnip/shader/Metal qualification. One nonfatal managed-page physical
IOMemoryDescriptor warning occurred in cpu-map.

**metal2h change:** `FuxiAdrenoBoView` now derives from
`IOSubMemoryDescriptor`, using the original IOBufferMemoryDescriptor as its
parent for the entire allocated BO. It no longer builds physical-address
ranges over managed RAM. `clientMemoryForType` still returns the same retained
view for map/unmap, with Normal NC/write-combine flags. The view keeps the
matched prepare and buffer reference until its final mapping is destroyed;
failed DMA drain still quarantines all references. Reserved carve-out NC
mappings and pmap cache synchronization are unchanged.

**Validation:** the real view/bind/cpu-map/drop methods run under ASan/UBSan,
covering retained CPU maps after BO close, same-descriptor unmap identity,
missing/expired tokens, failed bind and failed drain. ABI/backend, patch-chain,
execswap/quota, cache-sync/NC range tests pass. Native build succeeded in the
assigned `ci-lane-metal2` tree. Packaging checks verify metal2g's byte-identical
ADT and command line, external pinned GMU/SQE/zap bytes, recovery entries,
service symbols and artifact hashes. No phone was accessed. Warning removal and the M2 device lifetime gate were subsequently observed on
metal2h; see the M3 report above.

Build with the same native XNU/IIG setup as metal2g, copy stripped/unstripped
outputs into `xnu/out-fuxi-metal2h/`, then:

```sh
# All firmware/tool variables must name existing absolute external files.
export FUXI_GMU_FIRMWARE FUXI_SQE_FIRMWARE FUXI_GPU_ZAP_MBN MKBOOTIMG
bash tools/build-fuxi-metal2-images.sh "$METAL2G_ARTIFACTS" metal2h
python3 tools/test-fuxi-metal2h.py
python3 tools/test-fuxi-metal-artifacts.py --lane metal2h \
  --base "$METAL2G_ARTIFACTS" --out xnu/out-fuxi-metal2h \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
```

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal2h/kernel.development.fuxi` | `e375ac6962acd8856f5d00219ca2e999b43734867ebc6786f8197e753fd5c848` |
| `xnu/out-fuxi-metal2h/kernel.development.fuxi.unstripped` | `75d52a0c6f876f2f957bfb0b67d4098aba7fa486d2a76b3a9f86ced4e4f0e785` |
| `shim/out/fuxi-metal2h-boot.img` | `3212f853c3787b2c128162d7d9cbb5ab3ee88c73f9a9e950b06c3184567510b0` |

metal2h intentionally still exec-swaps the raw ABI test. Expect the same two
successful rounds as metal2g, without `IOMemoryDescriptor physical with managed
page`. The M2 Vulkan probe needs a separate coordinator-selected invocation.

## M2: iOS 16 Turnip cross-build (offline complete)

**M2 lifetime gate subsequently observed on metal2h** (coordinator report; see
M3 above). `tools/build-fuxi-turnip.py` builds/signs the real
Turnip ICD against this tree's `xnu/fuxi-adreno/NativeAdrenoABI.h`, then builds
`tools/fuxi-vk-probe.cpp` using Mesa's Vulkan headers. The probe directly dlopens
the ICD, creates an instance, enumerates physical properties/heaps, creates one
logical device/queue, and destroys them. It never resolves or calls a submit
entrypoint, allocates application GPU resources, creates a surface, or presents.
Source review of the pinned CreateDevice/queue-init path found BO/command-stream
initialization but no hardware submission. The M2 report above qualifies enumeration/device lifetime only.

### Exact inputs and patch order

- Mesa commit `85fda3b36405bcaea8bffc64aa4f2651dc90770d`, base tree
  `b72ff1eb2138026ed6aba524770899faabf7d7c8` (Mesa `26.2.0-devel`).
- UniXNU commit `7b52e7968c76453cdc18b5aa174fc368d5293b91`,
  [build recipe](https://github.com/UniXNU/UniXNU/blob/7b52e7968c76453cdc18b5aa174fc368d5293b91/scripts/build-turnip-darwin.py)
  and its `scripts/data/adreno-compiler-inputs.json`. Keep its MIT notices in
  the external patched source. No upstream checkout or large patch is vendored.
- NativeAdrenoABI.h SHA256
  `c37a965ce521f57bd201b61ee0578d72b809b3d1c3697fd572ee1f3753af2e89`.
  Direct unsandboxed root service: `NativeAdrenoGpuPublished`, open type **0**;
  ABI v1, 31-bit IOVA / 36-bit PA, 256 MiB per-client wired limit, no HostImport.

Apply the following in order (the build script verifies upstream patch hashes,
then checks the entire patched worktree against an independently built Git index):

| Patch | SHA256 |
| --- | --- |
| `freedreno-tools-nir.patch` | `77e966b6e9a725892d10889a4d8deeefb03a32ae3a007904868132e56350191f` |
| `ir3-darwin-build-id.patch` | `be3ca7840a16cb7696d8d7e67195c8d097cf0e38a3b5011edc674a5aed9c6975` |
| `ir3-portable-asm-roundtrip.patch` | `bfc5552bc2bd6e1668d75773cce7bf62fd19065e6281bf7da49f9ddfc2e79339` |
| `offline-adreno-compiler-probe.patch` | `b7d7a113beb5ef99edea24858d906194d4b4755c987520545ad2f7f3136bb4fb` |
| `turnip-darwin-kmd.patch` | `dcf6a88890995243516f5fd06a41b4ff91117f25ca08e7dc4fc7b90836b11e37` |
| `tools/patches/mesa-fuxi-ios16.patch` (local, last) | `e7879df64e66442c015ca25f6484bad6ba57b56bed9bb10ac357aa46e1b40306` |

The local patch adds the missing `tu_physical_device::has_host_import` boolean:
the upstream patch references/sets it but omits its declaration. This is a
reproduced compile failure, independent of target OS. It stays false because our
Query flags are zero. No HostImport support is added.

**iOS 16 derivation:** C/C++/ObjC compile and link flags all use
`-target arm64-apple-ios16.0`, the real iPhoneOS SDK and
`-Werror=unguarded-availability-new`. No macOS SDK fallback. The upstream Darwin
condition-variable workaround and 64-byte cache-line substitution apply to iOS
16 as well. None of the iOS 17 IOGPU/Metal object-size assumptions are used.
`platforms=macos` selects Mesa's Darwin/Metal WSI sources; it does not change the
Mach-O platform to macOS. The probe enables no WSI extensions. The WSI path that
calls `MTLCreateSystemDefaultDevice` is reached when creating a swapchain, which
this probe never does; this does not provide a Metal frontend or presentation.

### Build procedure

Observed toolchain: Apple clang `21.0.0 (clang-2100.3.34.2)`, iPhoneOS SDK
**27.1**, Meson **1.12.1**, Ninja **1.13.2**, native glslangValidator **16.6.0**,
Python **3.14.8**, Bison **3.8.2**. The installed newer SDK was used with a 16.0
minimum; required imports were also checked against the supplied iOS 16.7 cache.
Apple Bison 2.3 fails IR3 parser generation (`-Wcounterexamples` unsupported).
The pinned GNU tarball SHA256 is
`9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2`.

Use absolute environment paths for an external/ignored cache, checkouts and
build directory. Only the assigned Mac lane and its own dependency cache were
written. Example setup, with native Ninja/glslang already on PATH:

```sh
mkdir -p "$CACHE"
python3 -m venv "$CACHE/venv"
"$CACHE/venv/bin/pip" install meson==1.12.1 mako==1.4.3 MarkupSafe==3.0.4 \
  pyyaml==6.0.3 packaging==26.3
curl -fL https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.xz -o "$CACHE/bison-3.8.2.tar.xz"
# Verify the SHA256 above before unpacking/building.
tar -C "$CACHE" -xf "$CACHE/bison-3.8.2.tar.xz"
(cd "$CACHE/bison-3.8.2" && ./configure --prefix="$CACHE/native" && make -j6 && make install)
export PATH="$CACHE/native/bin:$CACHE/venv/bin:$PATH"

git init "$MESA_SRC"
git -C "$MESA_SRC" remote add origin https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa
git -C "$MESA_SRC" fetch --depth 1 origin 85fda3b36405bcaea8bffc64aa4f2651dc90770d
git -C "$MESA_SRC" checkout --detach FETCH_HEAD
git init "$UNIXNU_SRC"
git -C "$UNIXNU_SRC" remote add origin https://github.com/UniXNU/UniXNU.git
git -C "$UNIXNU_SRC" fetch --depth 1 origin 7b52e7968c76453cdc18b5aa174fc368d5293b91
git -C "$UNIXNU_SRC" checkout --detach FETCH_HEAD
python3 tools/build-fuxi-turnip.py --mesa "$MESA_SRC" \
  --patches "$UNIXNU_SRC/patches/mesa" --build "$MESA_BUILD"
```

The script uses the manifest's options: `vulkan-drivers=freedreno`,
`freedreno-kmds=darwin`, `tu-darwin-abi-dir` set to our ABI directory,
`platforms=macos`; Gallium, GL/GLX/GLES/EGL/GBM, LLVM, shared-glapi, XML config,
shader cache, tools/tests/codecs/layers, unwind/sensors/valgrind/expat/zstd are
disabled; `--wrap-mode=nodownload`, native glslang/build generators. It writes a
cross file to the ignored build directory. If resuming a cache configured with
the wrong native Bison, use a fresh build directory. `--verify-inputs-only`
checks source/patch state without configuring or rebuilding.

The ICD install ID becomes `/usr/local/lib/libvulkan_freedreno.dylib` before
`codesign -f -s -`, exactly the raw test's ad-hoc signing method, with no added
entitlements. `codesign --verify --strict` passes for both outputs.

### Artifact/load-command audit

| Artifact | SHA256 |
| --- | --- |
| `tools/out/libvulkan_freedreno.dylib` | `fe2bb259cb173762aa902754f5ec0ee9b567fb5ce97351d9a8c2ba32339c2928` |
| `tools/out/fuxi-vk-probe` | `ca7851eb7f0051ccb57927971d67570c34b4b2f07695f8209c8bbfd564124a52` |

Both are thin plain arm64 (not arm64e), `LC_BUILD_VERSION platform IOS`,
`minos 16.0`, `sdk 27.1`, embedded ad-hoc signatures, no LC_RPATH. The ICD exports
`vk_icdGetInstanceProcAddr` (also `vkGetInstanceProcAddr`). Instruction-word scans
of every executable section and independent LLVM disassembly found **no
`MRS CTR_EL0`**. The upstream Darwin helper substitutes 64 bytes.

Exact ICD `LC_LOAD_DYLIB` paths:

```text
/usr/lib/libSystem.B.dylib
/usr/lib/libz.1.dylib
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics
/System/Library/Frameworks/QuartzCore.framework/QuartzCore
/System/Library/Frameworks/Metal.framework/Metal
/usr/lib/libc++.1.dylib
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/usr/lib/libobjc.A.dylib
```

The probe loads only libc++ and libSystem. These are iOS 16 system libraries;
the IOKit versioned path is present in the supplied cache. All **262 distinct
strong imports** (251 ICD, 29 probe, with overlap) were found defined in their
expected system providers/reexports in the supplied 20H392 cache (hash recorded
in the discovery section below). No required import is a dynamic lookup. The
ICD has **5571 weak dynamic references** from generated optional Turnip/WSI
entrypoint tables. The upstream `-undefined dynamic_lookup` is retained; ld
warns that it is deprecated on iOS, plus a duplicate `-lz` warning. These weak
references outside the exercised M2 path remain unqualified; the root
load/device-lifetime gate subsequently passed on metal2h.

Reproduce audit on the Mac, then use the supplied cache offline:

```sh
python3 tools/audit-fuxi-vulkan.py tools/out/libvulkan_freedreno.dylib tools/out/fuxi-vk-probe
"$IPSW" dyld symaddr "$DSC" --in tools/out/M2-required-symbols.json \
  --output tools/out/M2-ios16-symbols.json
python3 tools/audit-fuxi-vulkan.py --cache-symbols tools/out/M2-ios16-symbols.json \
  --required tools/out/M2-required-symbol-libs.json
python3 tools/test-fuxi-vulkan.py --headers "$MESA_SRC/include"
```

Host tests run the actual probe against a fake ICD that rejects unexpected
entrypoint lookups, including submit calls; they cover successful lifetime,
cleanup after enumeration/device-creation failure, exec-swap host argv, and
idempotent stage modes/ownership. They do not execute Turnip or an iOS dyld.

### Later root edit and device acceptance

Run `tools/stage-fuxi-vulkan.py ROOT tools/out/libvulkan_freedreno.dylib
tools/out/fuxi-vk-probe` as the root-image editor, against an already mounted
writable iOS root. It copies only:

| Root path | Owner | Mode |
| --- | --- | --- |
| `/usr/local/lib/libvulkan_freedreno.dylib` | `0:0` (root:wheel on iOS) | `0755` |
| `/usr/local/bin/fuxi-vk-probe` | `0:0` | `0755` |

No launchd job is installed and no root was edited in this lane. No Vulkan
loader or ICD JSON is required because the probe dlopens the ICD directly.
Invoke once as unsandboxed root, after the raw test has closed its exclusive
connection. The probe forces `TU_DARWIN_SERVICE=NativeAdrenoGpuPublished` and
`TU_DARWIN_OPEN_TYPE=0`. Direct invocation optionally accepts an absolute ICD
path; an exec-swap host basename causes all host arguments to be ignored.
metal2h's unchanged command line still selects the raw test; a coordinator must
choose a separate root invocation or a later probe-specific exec-swap image.

M2 acceptance output (the coordinator subsequently reported this lifetime gate
passing on metal2h; no submission was part of that run) includes:

```text
FUXI VK: create-instance rc 0
FUXI VK: physical-device-count rc 0
FUXI VK: physical-devices rc 0
FUXI VK: physical-devices 1
FUXI VK: device 0 vendor 0x5143 device 0x43050a01 name Turnip Adreno (TM) 740... driver turnip Mesa driver (Mesa 26.2.0-devel...)
FUXI VK: heap 0 bytes N flags 0x1
FUXI VK: create-device rc 0
FUXI VK: queue family 0 index 0 PASS
FUXI VK: destroy-device PASS; no work submitted
FUXI VK: done PASS
```

`N` is queried, not hardcoded: this pin uses a RAM heuristic capped by the
`0x60000000` (1.5 GiB) IOVA window. This is larger than our 256 MiB wired quota;
it must not be interpreted as a tested allocation budget. The device name may
include `(raytracing disabled)`; no features/extensions are enabled by the probe.
Expected kernel calls include open/query/device-info/close for enumeration,
reopen/query/device-info, internal BO alloc/map/cpu-map/free and close for device
lifetime. **No Submit/WaitFence calls or GPU results are expected.**

Remaining risks after the observed M2 gate: weak optional entrypoints outside
the exercised path, advertised heap vs
kernel quota, single-client/global backend state, and existing A7xx
cache/TTBR/submit assumptions. The no-submit gate passed without panic; M3 separately requires observed
fill/compute/triangle/readback. No shader submission,
Metal discovery/frontend, IOSurface import or CoreAnimation integration is
qualified by this build.

---

# Metal lane, milestone 2g: full-width cache sync and bounded NC access

2026-10-20, based on metal2e and Wi-Fi M6. No device execution in this lane.

**Observed metal2f:** exec-swap as peakpowermanagerd reached open, query and
DeviceInfo successfully, then panicked in `phystokv_range` with PA
`0xc4f40000`, below the managed RAM base `0x8c0000000`. No backtrace was
available. IOMFB_FDR_Loader previously passed `--launchd_boot`, which made the
old test exit with usage before opening the GPU.

**Root cause identified in the BO allocation cache path:**
`FuxiAdrenoUserClient::allocate` requests `kIOMapWriteCombineCache` from
`IOBufferMemoryDescriptor`. Changing wired pages from copyback to NC runs
`pmap_sync_wimg` -> `pmap_sync_page_attributes_phys` -> `flush_dcache` ->
`phystokv_range`. Upstream `osfmk/arm/pmap/pmap.c:6884` shifts the 32-bit
`ppnum_t` before casting: `(vm_offset_t)(pp << PAGE_SHIFT)`. The metal2f
kernel actually emits `lsl w0, w0, #14`. For example, managed PA
`0x8c4f40000` becomes the exact reported illegal PA `0xc4f40000`. The log
alone does not identify which original high RAM page was used; it does not
establish that the reported low address was a real carve-out allocation.

Patch 0101 widens before shifting. The final native kernel emits
`ubfiz x19, x0, #14, #32`, preserving all 32 PFN bits. For reserved pages it
looks up the entire page in the published GPU kernel_map NC windows and
flushes through that VA, with `phys=FALSE`. Unknown non-RAM/non-window PAs
are rejected before any physmap helper. Managed BO pages keep their full PA.

**Access audit:** the ABI backend, present mapper, user client and shared
GPU helpers contain no calls to physical zero/copy/read/write or physmap
conversion functions. The user client accesses BOs through their descriptor
KVAs. ABI/present `pmap_find_phys` calls translate wired source KVAs to PAs;
`pmap_enter_options_addr` installs the persistent NC mappings and
`pmap_remove` unwinds failed installation. These are not carve-out data
accesses through physmap. Table walks, zeroing, ring storage, firmware and
handoff reads now validate their full byte range through `G_PTR_RANGE` and
`fuxi_gpu_nc_pointer`, with overflow and window-edge checks. MMIO validates
the actual 4/8-byte width. There is no physmap fallback for NC windows.

The test ignores host arguments whenever argv[0]'s basename differs from
`fuxi-gpu-abi-test`. Only direct self invocation accepts `--watchdog`; host
arguments can never select the hanging IB. Direct invalid arguments still
return usage status 2.

**Validation:** ASan/UBSan tests execute the actual NC lookup, scratch zero,
patched pmap helper and argument parser. The original pmap helper fails the
strict physmap trap with the reported low PA. All mapped apertures, unknown
PAs, unpublished mappings, zero lengths, crossing ranges and overflow are
covered. Existing ABI, execswap, mapper, present, shared probe, memory, WLAN
M6 and final native SCM checks pass. Artifact checks verify firmware bytes,
geometry, recovery table, service symbols, signed iOS arm64 tool and hashes.

The Mac tree was synchronized to the current worktree, including replacing
0099 with Wi-Fi M6, before building. The kernel and ad-hoc signed test were
built on the Mac; the loader was packed with the existing host toolchain.
The metal2g pack lane requires absolute external firmware/tool paths and
copies metal2f's ADT and command line exactly: peakpowermanagerd exec-swap,
bluetoothd/fairplayd.H2 blocked, and no `fuxiwlan` (Wi-Fi off).

Artifacts: `xnu/out-fuxi-metal2g/`, `shim/out/fuxi-metal2g-boot.img`, and
`tools/out/fuxi-gpu-abi-test`. SHA256s:

- Kernel: `c476cba84af5e795d93e165c029df669b4127bd22d9de368b22f2a1c7942e217`
- Unstripped: `dc80e71de18e10753eff3598fdc3ca248dbf13bfd0491dcc68f50e43b7fab7a3`
- Boot: `aceb71bdf42b0741db826d636e6bbbbbe348181ff9497d77a3dcc91e76110556`
- Signed test: `1114d442a5918ca05794acec6b0a4472ec92f62a5a1357a7886840682f252cb7`

The rebuilt test still needs offline root restaging. Expected next device
lines (not observed here), after the existing open/query/device-info lines:

```text
SWEET EXECSWAP: peakpowermanagerd -> /usr/local/bin/fuxi-gpu-abi-test pid N
FUXI ABI: alloc pid N rc 0x00000000
FUXI ABI: map pid N rc 0x00000000
FUXI ABI: submit pid N rc 0x00000000
FUXI ABI: wait pid N rc 0x00000000
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 1/2
```

Then close/reopen, another successful alloc/map/submit/wait and result
`rounds 2/2`, followed by close. A later respawn should report that the test
already passed 2/2 rounds this boot. No physmap panic is expected.

---

# Metal lane, milestone 2c: page-granular UCHE readback

2026-10-20, branch `codex-metal2c`, based on `2800fd5` (metal2b plus Wi-Fi
M5). Native build, all eleven host test scripts, shim host renderer, native
SCM audit and artifact checks pass. No device execution is claimed.

**Observed (supplied metal2b boot log):** GMEM MAX reads `0x12ff000` after
writing inclusive end `0x12fffff`. The other snapshot values match. This
explains the named init failure before the NC/aperture/table setup.

The canonical backend and regenerated patch 0098 now compare UCHE address
low words (WRITE_THRU, TRAP, GMEM MIN/MAX) through mask `0xfffff000` on both
operands. Address high words and NC mode retain exact comparisons; HBB
retains its exact extracted-field check and SMMU OAS its minimum check.
Raw snapshots and failure messages still report the unmasked values.
No programmed register value or userspace ABI changes.

**GMEM:** reconstruct the inclusive end before computing size:
`(0x12ff000 | 0xfff) - 0x1000000 + 1 = 0x300000` (3 MiB).
[Linux v6.16 A740 catalog](https://github.com/torvalds/linux/blob/v6.16/drivers/gpu/drm/msm/adreno/a6xx_catalog.c#L1370-L1376)
lists chip `0x43050a01` with `3 * SZ_1M`;
[Linux UCHE setup](https://github.com/torvalds/linux/blob/v6.16/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)
programs GMEM MAX as MIN + size - 1, with a 16 MiB base for A740.
DeviceInfo already advertises this base and 3 MiB size; it remains unchanged.
Query reports ABI/address/allocation limits, not GMEM size. Host tests now
compile and execute the actual Query/DeviceInfo branches and check these
values against the observed range, avoiding the 4095-byte underestimate.

**Follow-on audit:** `g3_gw`/`g3_write`/`probe_safe_wr32` and `fg_access`
report MMIO access faults, not exact value readback. Thus the eight NC and
BR/BV/default aperture writes have no similar equality assumption to fix.
They still fail closed with a named write failure. `g_zero`, root/L2/L3
construction and fence initialization use ordinary reserved NC memory
stores, followed by `dsb sy`, rather than hardware address registers.
Later TTBR switches do compare the 4 KiB-aligned root PA plus ASID exactly;
there is no inclusive-end offset to reconstruct or discard there. The
existing SMMU setup uses the same aligned addressing. Explicit BR/BV aperture
selection, ABI table use and userspace submissions still need device testing.

Host tests use `0x12ff000` by default, accept both rounded and inclusive MAX
models, exercise each of the 12 ignored offset bits in all four low words,
and reject page-address/high-word/HBB/NC corruption. All prior read faults,
eight write failures, publication/table refusal, one-time snapshot bounds,
PM4, mapping, watchdog, execswap, governor, root-tool and Wi-Fi M5 tests pass.

**Build:** only `ci-lane-metal2` was used. Its prior 32 native input hashes
were verified before synchronization. All tracked repository files were
synchronized; the native source overlay includes the current 0099 M5 source
and this backend change. The final 32-input manifest differs from metal2b
only in those two files; all hashes pass on the Mac. The packed command line
and ADT are byte-identical to metal2b, including the absent `fuxiwlan` gate
(Wi-Fi off). The existing signed raw tool and pinned loader inputs are
unchanged. Kernel linking has the existing toolchain/linker warnings; loader
packing completes without warnings.

Repack and verify using the same private build environment:

```sh
bash tools/build-fuxi-metal2-images.sh "$METAL2B_ARTIFACTS" metal2c
python3 tools/test-fuxi-metal-artifacts.py --lane metal2c \
  --base "$METAL2B_ARTIFACTS" --out xnu/out-fuxi-metal2c
```

Outputs, manifests and logs: `xnu/out-fuxi-metal2c/`.
Image: `shim/out/fuxi-metal2c-boot.img`, SHA256
`eca58669dd03844dbeee53d6d67e8ae54b5544c8e95a359d489b3f317129cc91`.
The complete artifact manifest is `xnu/out-fuxi-metal2c/SHA256SUMS`.

**Expected next device evidence, not an observed result:** retain the eleven
raw init lines, including `UCHE_GMEM_RANGE_MAX_LO [0xe0d]=0x12ff000`, with no
MAX mismatch. If subsequent setup succeeds:

```text
FUXI GPU ABI: NativeAdrenoGpuPublished v1
FUXI ABI: open pid N rc 0x00000000
FUXI ABI: query pid N rc 0x00000000
FUXI ABI: device-info pid N rc 0x00000000
FUXI ABI: submit pid N rc 0x00000000
FUXI ABI: wait pid N rc 0x00000000
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 1/2
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 2/2
```

Allocation/mapping/cleanup lines occur between these markers as in 2a. The
second round follows close/reopen; later diagnostic opens are refused after
two successful rounds. A named `init write ... failed`, service withheld,
nonzero rc, bad pixels, watchdog or missing round remains failure evidence.

---

# Metal lane, milestone 2b: identify ABI init failures

2026-10-20, branch `codex-metal2b`. Built in the existing `ci-lane-metal2`
native tree. All ten host test scripts, the shim host renderer and artifact
checks pass. This lane did not access a phone or boot this image.

**Observed:** the supplied `ios-0098-metal2a/boot.log` reaches GPU stage 16
PASS (including zap and clear/copy), then prints ready and
`FUXI GPU ABI: initialization failed, service withheld`. Execswap runs the
raw test as pid 84 and again as pid 241. There are no ABI open/query lines.
The old compound check does not record its failing operand. The root cause
of that refusal remains unresolved; no register-value correction is claimed.

**Source audit:** `g3_hw_init()` runs before CP startup in both ABI and normal
boots. It writes the following values using dword-indexed GPU offsets and
separate 32-bit low/high stores. `g3_sr()` instead takes a byte offset.

| Register | Offset(s) | Programmed / required value |
| --- | --- | --- |
| UCHE_WRITE_THRU_BASE | `0xe07/0xe08` | `0xfffff000 / 0x1ffff` |
| UCHE_TRAP_BASE | `0xe09/0xe0a` | `0xfffff000 / 0x1ffff` |
| UCHE_GMEM_RANGE_MIN | `0xe0b/0xe0c` | `0x1000000 / 0` |
| UCHE_GMEM_RANGE_MAX | `0xe0d/0xe0e` | `0x12fffff / 0` (3 MiB, inclusive end) |
| UCHE_MODE_CNTL | `0xe01` | `0x600000`; bits 22:21 = 3, HBB = 13+3 = 16 |
| RBBM_NC_MODE_CNTL | `0x534` | `1` (macrotile mode; HBB is in UCHE) |
| SMMU_ID2 | byte `0x28` | earlier observed `0x5511`, OAS code 1 = 36 bits |

The supplied Linux `a6xx_gpu.c` and `a6xx.xml` agree. Qualcomm's
[adreno_gen7.c](https://github.com/qualcomm-linux/kgsl/blob/2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f/adreno_gen7.c),
[register header](https://github.com/qualcomm-linux/kgsl/blob/2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f/gen7_reg.h)
and [Gen7.2 catalog](https://github.com/qualcomm-linux/kgsl/blob/2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f/adreno-gpulist.h)
also agree on the offsets, trap/write-through address, 16 MiB GMEM alignment,
3 MiB size and HBB16. Reference copies remain in ignored `out-fuxi-metal2b/refs`.
The old ABI verified WRITE_THRU, despite the DeviceInfo text discussing TRAP;
both are programmed identically. The new snapshot/checks include both pairs.
No source evidence establishes a read-as-zero mask for these registers, so
2b retains exact comparisons rather than guessing a mask or rewriting state.

`gpu_probe(16)` reaches `g5_run()`, which exits secure mode and explicitly
runs `gpu_m4(14)`. That clear/copy path already writes RB/TPL1/SP/GRAS NC
values `0x816/6/0x26/0x60`, even with the ABI enabled. No later software reset
or write to the checked UCHE registers was found between `g3_hw_init()` and
`fg_abi_init()`. `g3_hold_gx()` retains GMU OOB until reset, with no idle
collapse. Thus missing stage14 setup or a deliberate GX power release is
not supported by the code/observed stage results. This does not establish
what each UCHE register actually read back on the failing boot.

The ABI-only writes use the same GPU MMIO window and fault-contained helper
as stage14. The helper executes `dsb sy` after each access. Host aperture
`0xa00` uses PIPE bits 13:12, BR=1/BV=2, then restores zero, matching Qualcomm's
`gen7_host_aperture_set()` / GRAS setup. CP packet protection does not prohibit
these CPU AHB writes. Init finishes before ready is released to display
callers. RB/TPL1/SP were already writable during the observed clear/copy;
the explicit BR/BV GRAS selections still need device qualification.

The diagnostic change is in the canonical backend and regenerated patch
0098; patch 0100 stays unchanged. On the first init attempt, it takes one
snapshot of all ten GPU registers plus SMMU_ID2 before any comparison. It
prints eleven `FUXI GPU ABI: init regs ...` lines, then at most one named
`init check <name> got 0x... want 0x...` or
`init write <reg> [0x...]=0x... failed` line. The existing service-withheld
line follows failure. There is no retry or periodic dump. Access failures
have a separate `register_access` check; raw snapshot values following a
fault must not be treated as valid hardware facts. The first mismatch still
stops setup, and publication/table construction stay behind all checks.

Host coverage injects every register mismatch, GPU and SMMU read failures,
and each of the eight write failures into the actual C init function. It
checks the named reason, one-time dump cap, and untouched tables/unpublished
service after refusal. Existing PM4, mapping, async present, watchdog,
quarantine, execswap, governor, root-tool and SCM regressions pass. Artifact
checks verify the linked native recovery/SCM code, exact metal2a ADT and
cmdline, boot geometry, existing signed raw tool and all pinned firmware.
All 32 recorded native inputs match metal2a except the intended backend edit.
These are host tests; raw IB/readback and service publication are unproven.

Repack using the normal private loader/toolchain environment, after copying
the native kernels into `xnu/out-fuxi-metal2b`:

```sh
bash tools/build-fuxi-metal2-images.sh "$METAL2A_ARTIFACTS" metal2b
python3 tools/test-fuxi-metal-artifacts.py --lane metal2b \
  --base "$METAL2A_ARTIFACTS" --out xnu/out-fuxi-metal2b
```

Outputs and build/test logs: `xnu/out-fuxi-metal2b/`. Image:
`shim/out/fuxi-metal2b-boot.img`, SHA256
`0d0cb22d70c78380baab5fef91efb7a84f31eb5c2971a0943ad915b9be32f172`.
`SHA256SUMS` records the kernel, ADT, payload, image, loader and raw tool.

Expected next device output: eleven named raw-register lines, followed by
the precise failing check/write and service withheld if the failure repeats.
If all checks/writes pass, expect `FUXI GPU ABI: NativeAdrenoGpuPublished v1`
and then the existing bounded `FUXI ABI:` open/query/submit/wait/result trace.
Do not treat that success path as an observed 2b result.

---

# Metal lane, milestone 2a: route the existing raw test and observe it

2026-10-20, branch `codex-metal2`, base `b978770` (0098i integration plus
Wi-Fi M3/M4). Native build and all ten host test scripts pass. **This image has
not been booted by this lane.** The coordinator reports that 0098i reached
GPU stage 16 PASS, valid PERF temperatures and iOS userland, but never exec'd
the staged raw test. Stage 16 is not a userspace ABI test result.

## Patch 0100 and repeat handling

Apply `0100-sweet-execswap-gpu-abi-trace.patch` after 0099 (and 0095/0098).
0098 is unchanged; `sync-fuxi-metal2.py` maintains the canonical helper and
client delta. `sync-fuxi-adreno-shared.py --check` checks both generations.
The only Mac tree used was `ci-lane-metal2`, created with the requested APFS
clone of `ci-integ98`; 0099 and 0100 applied without fuzz. All 32 affected
native source files match a clean 0098i + 0099 + 0100 result, including the
m7c/m7d governor and default-off Wi-Fi sources. Logs/manifests are under
`xnu/out-fuxi-metal2a`. No phone/root-image access, push or binary rebuild.

`sweetexecswap=<name>:<absolute path>` substitutes the original executable
pathname after copyin and before namei, permissions, activation and signing.
It accepts one basename (1..31 bytes) and one non-root absolute target within
a 192-byte buffer; rejects truncation, lists, whitespace and extra colons.
Initialization and the eight-line boot log quota use a mutex. Without the
boot argument it changes no executable path. argv/env keep their original
userspace segment. A private unused image flag (0x10000) carries substitution
through activation, suppressing the late sweetnoexec check for this exec.
If the original name is also blocked, the routing line gains
` (wins over sweetnoexec)`. Other sweetnoexec entries retain their behavior.
No fallback to the original daemon occurs if the replacement fails.

The current signed test stays byte-identical (SHA below). For exactly
`bluetoothd:/usr/local/bin/fuxi-gpu-abi-test`, opens of type 0x4147 by the test
process (its actual exec basename, as exposed through truncated p_comm) are
marked diagnostic. After successful WaitFence, the kernel reads that client's
64 KiB BO at IOVA 0x20000000: word at +0x1000 must be 0xa740cafe, and every one
of 256 BGRA pixels at +0x2000 must be 0xff336699. It counts at most one verified
round per connection. Two successful rounds allow the existing binary's
close/reopen sequence, then refuse later diagnostic opens with kIOReturnBusy.
Other clients/types and boots without that exact swap configuration have no
round quota. This is a diagnostic signature check, not proof that stdout said
PASS or that a Metal/Vulkan client works. Later respawns exit quickly with the
existing binary's open-failure exit status; the kernel explains the refusal.

A single boot-wide budget of 64 `FUXI ABI:` lines covers open, query, device
info, alloc, GPU/CPU map, submit, wait, result, cleanup and refusal, including
pid and IOReturn. It works with bluetoothd's inherited stdout even if /dev/null.
The original independent 100 ms GPU watchdog and quarantine remain intact.

## launchd finding and future root edit

Inspected only the supplied `launchd.bin` (SHA256
`12370e5ee35bf28ae479b6c4b4455f4f4693ab803e37c5eb3032537c18cac61c`), using
strings, embedded configuration parsing and arm64 disassembly. Addresses below
are unslid and specific to this binary; Apple analysis outputs remain ignored.

* Embedded `Bootstrap.Paths` enumerates `/System/Library/LaunchDaemons`,
  LaunchAngels, AppleInternal, Nano and DeveloperMode directories. It omits
  `/Library/LaunchDaemons`. `SystemRoots` likewise omits `/Library`.
* At 0x100019c08..0x100019c40, `LaunchDaemons` is accepted as an XPC dictionary;
  0x100019f68..0x100019fc8 installs its entries into the cache. Merely adding
  a key is not itself a bootstrap request.
* The directory loader at 0x100019638..0x1000196f8 checks the configured path
  arrays and enumerates cached entries under the requested directory. Its
  callback at 0x100019e0c..0x100019e64 tests the entry's prefix before creating
  the load object. The prefix helper at 0x100016210 uses strlen/strncmp.
  Thus the old `/Library/LaunchDaemons/...` key is outside the default boot
  directory selection. This explains the observed absence of any exec attempt;
  confirmation of the corrected job still requires the next root edit/boot.
* ProgramArguments-only jobs are accepted (including embedded boot jobs).
  The supplied job has no `_LimitLoadTo*` restrictions. There is no evidence
  here that missing POSIXSpawnType or a mandatory Program key caused this skip.

`stage-fuxi-gpu-abi-test.py` now writes both the file and cache key at
`/System/Library/LaunchDaemons/org.fuxi.gpu-abi-test.plist`, explicitly sets
Program as well as ProgramArguments, and migrates/removes the legacy key/file.
It preserves unrelated jobs and validates the input before migration.
**This fix is committed for the next offline root edit; it was not staged.**

## Build, artifacts and expected device evidence

`build-fuxi-metal2-images.sh BASE_0098I_ARTIFACT_DIR` uses the same mkpayload
and loader/boot-v4 packing as 0098i, with its exact external GMU/SQE/zap blobs.
The ADT is byte-identical. The 463-byte command line is precisely 0098i's line
with `bluetoothd,` removed from sweetnoexec and
` sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test` appended.
`fuxigpuabi=1` stays on; `fuxiwlan` is absent/off, with patch 0099 still linked.
The build requires the same private toolchain/firmware environment as 0098i.

All host scripts passed: adreno-cache, adreno-mapping, adreno, gpu-probe,
memory, metal, metal2, perf, scm (including final native instruction audit),
and metal-artifacts (including firmware, raw signed Mach-O, geometry, symbols,
recovery table, exact command line/ADT and hashes). New ASan/UBSan tests compile
the actual parser/exec helper and result/mount/trace methods, checking parse
once, default-off, overlap logging, eight/64-line limits, failed pixels, two
rounds, repeat refusal, unrelated clients and stage migration/removal.
These are host checks, not device qualification.

Expected success sequence (N is the test PID, unchanged across its reopen):

```text
SWEET EXECSWAP: bluetoothd -> /usr/local/bin/fuxi-gpu-abi-test pid N
exec fuxi-gpu-abi-test
FUXI ABI: open pid N rc 0x00000000
FUXI ABI: query pid N rc 0x00000000
FUXI ABI: device-info pid N rc 0x00000000
FUXI ABI: alloc pid N rc 0x00000000
FUXI ABI: map pid N rc 0x00000000
FUXI ABI: cpu-token pid N rc 0x00000000
FUXI ABI: cpu-map pid N rc 0x00000000
FUXI ABI: submit pid N rc 0x00000000
FUXI ABI: wait pid N rc 0x00000000
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 1/2
FUXI ABI: cpu-unmap pid N rc 0x00000000
FUXI ABI: free pid N rc 0x00000000
FUXI ABI: close pid N rc 0x00000000
```

Then open/alloc/map/cpu-token/cpu-map/submit/wait/result/cleanup repeat, with
`rounds 2/2` (no second query/device-info in the current binary). A later
bluetoothd respawn, with a new PID M, should log the routing and exec again,
then:

```text
FUXI ABI: open refused pid M rc 0xe00002d5 already passed 2/2 rounds this boot
FUXI ABI: open pid M rc 0xe00002d5
```

Routing stops logging after eight substitutions; ABI tracing stops after 64
lines. Any nonzero step rc, nonzero bad-pixels, missing second round, watchdog
or fault must be retained as failure evidence. Do not infer a PASS from exec
or service publication alone.

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal2a/kernel.development.fuxi` | `247d669f7aa563e7bd03435e4d0268a3e557728b54146a6ae5f23141ec2781a1` |
| `xnu/out-fuxi-metal2a/kernel.development.fuxi.unstripped` | `71c87cfe4170c4ae5cc7ce3b20d77146cec4d15087c0086c709bc5bec886764d` |
| `xnu/out-fuxi-metal2a/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-metal2a-payload.bin` | `f281445725dfcc30a36194c071418e0155e504432378847710b4ee4f4e383dfb` |
| `shim/out/fuxi-metal2a-boot.img` | `b8c441ee639f0b2d125398d6748296d38625bac22262a9ca05faa439f6942056` |
| `shim/out/fuxi-metal2a-loader.elf` | `8385bb46c6346fa31788b48751e38ba9c2bcbe8fd70979a3fa06ceac4b9f6e25` |
| `tools/out/fuxi-gpu-abi-test` (unchanged) | `fc04e8157061a21d6f713197625010006e28b77b56c01cddf1d38ffd5cd85ae1` |

The complete artifact manifest is `xnu/out-fuxi-metal2a/SHA256SUMS`.

---

# Metal lane, milestone 1: NativeAdreno userspace ABI

2026-10-20, branch `codex-metal1`, based on `fuxi-bringup` `8dae9f4`.
**Native build and offline verification pass. No device execution is claimed.**
No phone access, push, firmware redistribution or Apple binaries in git.
`AGENTS.md`, `NOTES.md`, `FUXI.md`, patch 0097 and `xnu/fuxi-perf` are unchanged.
The only Mac build tree used was `ci-lane-metal`, cloned from `ci-lane-gpu6`.
The 27 native source inputs match the clean 0097b + 0098 patch result;
`native-inputs.sha256` and `native-inputs-check.log` accompany the kernel.

## Implemented contract

Apply `0098-fuxi-native-adreno-abi.patch` after 0096; 0097 is unchanged.
Canonical sources are in `xnu/fuxi-adreno`. `sync-fuxi-metal.py` generates
0098, and `sync-fuxi-adreno-shared.py --check` checks the combined snapshots.
0096 remains independently applicable. The default-off driver behavior and
loader addresses remain unchanged when `fuxigpuabi` is absent or not 1.

Both `fuxigpu=1` and `fuxigpuabi=1` are required. The service is an ordinary
`IOService` subclass, **NativeAdrenoGpuPublished**, registry name
`NativeAdrenoGpu`, accepting open type **0x4147** (also type 0 for Turnip's
direct backend). Only an administrator/root connection is admitted. There is
one live connection, recycled on close/death; no sandbox tunnel or mailbox.
For the unmodified Turnip patch use:

```sh
export TU_DARWIN_SERVICE=NativeAdrenoGpuPublished
export TU_DARWIN_OPEN_TYPE=0x4147
export TU_DEBUG=nocb
```

All 16 direct selectors and fixed struct lengths match NativeAdrenoABI v1.
Query, GetDeviceInfo, BoAlloc/Free/Map, BoCpuMap/Unmap, Submit and WaitFence
are implemented. Metadata and queue selectors are implemented as well.
GetTimestamp, GetStatus and BoImport are unsupported. Query does not advertise
HostImport or cached coherence. Queue 0 exists at open; extra queue IDs share
the same serialized hardware queue.

Limits: 64 BOs, 256 MiB per BO and 256 MiB live BO allocation budget per connection,
16 IBs per submission, one hardware job in flight. Allocation tries contiguous
wired memory first, then scattered wired pages unless contiguity is required.
It honors alignment 12..24 and constrains physical addresses to 36 bits.
Backing is rounded/zeroed to 16 KiB, while GPU mappings cover the requested
4 KiB-multiple length. BoCpuMap reports the actual CPU view length.
`clientMemoryForType` returns a stable physical-range descriptor for its token.
Both kernel and user BO mappings use Normal NC/write-combine. The descriptor
retains the prepared backing through its last CPU mapping, including forgotten
unmaps. Correct users unmap memory, release the CPU token, then free the BO.
Unreleased CPU mappings can retain memory until connection/task teardown.

The client has a separate 4 KiB SMMU root in the final 4 MiB of reserved
scratch (offset 28 MiB). Static bounds and the host model keep it beyond
the retained zap metadata, cold handoff and all three firmware slots.
Its full aperture `[0x20000000, 0x80000000)` is available; overlapping mappings
are rejected. The original scanout and four present-source windows stay in
the present root. With the ABI enabled only, runtime SQE/ring/pointer/fence
IOVAs move below the client aperture (`0x10000000` onward), identically in
both roots. TTBR switching on the existing GPU bank happens only with an idle
ring; ASID invalidation/TLBSYNC precedes a new doorbell and backing release.
No GMU bank, SID ownership or present PTE is repurposed for BO leaves.

A shared mutex serializes ring/control/PTE operations with present calls;
BO allocation and scheduler waits do not hold it. While a client owns the
ring, presentation uses its existing CPU copy. On successful client retirement
the present root is restored and GPU presents become eligible again.
Submission wraps client IBs in CP_INDIRECT_BUFFER with BR sync, context reset,
PERFCTR SRAM-init status wait, GPU cache flush/invalidate, BOTH threads and
IFPC off. Completion writes BV timestamp, waits on BR, writes the BR timestamp
with the IRQ bit, restores BOTH/IFPC and waits for idle. Context reset is emitted
on every return from the present context, including the first client submit.
RBBM interrupt delivery remains masked; no GIC/IRQ completion is claimed.

WaitFence sleeps in 1 ms slices and returns `kIOReturnTimeout` for a user or
hardware deadline. Zero timeout observes once. An independent delayed callback
checks every 2 ms, even when no client calls WaitFence, and declares a pending
job lost at 100 ms. Completion observed after the deadline wins if the ring is
already retired. Scheduling can delay observation beyond these intervals.
A client fault/hang permanently disables GPU submission for this boot, drains
GBIF, holds reset and faults SID0/1, allowing CPU presentation to continue.
Failed drain quarantines the wired BO references until reset. There is no
attempt to recover the CP or reset it beneath active UI work.

## Chip facts and reference provenance

References read: UniXNU `7b52e7968c76453cdc18b5aa174fc368d5293b91`,
UniXNU-sheng `8a0c9ddab92038a50abc07fffd4882ec4e301f45`, including
`NativeAdrenoABI.h`, `NativeAdrenoRing.cpp` and `GpuPowerProbe.cpp`.
MIT ABI/framing notices are retained in `LICENSE.NativeAdreno`; README credits
UniXNU. The sheng board code is BSD-2-Clause-Patent; it is a reference here.

| Field | Value and basis |
| --- | --- |
| chip_id, GMEM | `0x43050a01` / C510v2, 3 MiB, catalog entry using `a740_sqe.fw`, `gmu_gen70200.bin`, `a740_zap.mdt`; matches our previously qualified A740 firmware family. This milestone performs no fresh phone chip-ID read. |
| GMEM base / trap | `0x1000000` / `0x1fffffffff000`, exactly the UCHE values our existing bring-up programs; ABI publication checks their live readback and GMEM end `0x12fffff`. |
| HBB / swizzle / macrotile / UBWC | `16 / 6 / 1 / 4`: SM8550 platform configuration, encoder 4.0 (decoder is 4.3). Existing NC values are `0x816 / 6 / 0x26 / 0x60`; ABI init also initializes BR/BV GRAS banks. HBB UCHE bits are checked. |
| VA | start `0x20000000`, size `0x60000000`, ClientIova flag; driver policy, independently reserved in the client root. |
| Query widths | input 31, **output 36**. Recorded Fuxi SMMU ID2 is `0x5511`: OAS code 1 means 36 bits. The existing present mapper also rejects PA >= 2^36. UniXNU's Query advertises its own 40-bit allocation ceiling; we use the observed hardware limit. Turnip does not require 40. |

The pinned [Linux A740 catalog](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/gpu/drm/msm/adreno/a6xx_catalog.c)
was independently hash-checked (`ff1abc1b7735d6bd6aa07d5df776e2549be2ee70ffcc02ff24aac646dfd6a3c7`).
The same tree's [UBWC configuration](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/soc/qcom/ubwc_config.c)
and [SMMU ID decoding](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/iommu/arm/arm-smmu/arm-smmu.c)
support the remaining configuration facts. Device-specific coherency still
requires execution; the catalog's coherent-capability quirk is not proof that
our XNU BO mappings are coherent.

## Raw test, build and coordinator procedure

`tools/build-fuxi-gpu-abi-test.sh` was run on the Mac using the installed
**iPhoneOS 27.1 SDK**, targeting **arm64-apple-ios16.0**, then ad-hoc signed.
The binary's LC_BUILD_VERSION is iOS 16.0. It dynamically resolves IOKitLib
because the public iOS SDK omits its headers. The script attempts the macOS
SDK with the same iOS target only if the iOS SDK is absent; that fallback was
not used or qualified in this run. Binaries go to gitignored `tools/out`.

The normal test waits at most 60 s for the service, opens 0x4147, queries,
allocates/maps a 64 KiB BO, writes an IB with CP_MEM_WRITE plus a 16x16 BGRA
solid CP_BLIT, submits, waits and verifies `0xa740cafe` and all 256 pixels
`0xff336699`. It then unmaps/frees/closes and repeats after reopening, exercising
address reuse and the enumeration/open-close pattern. Every step prints
PASS/FAIL to stdout; final success is `PASS: fuxi-gpu-abi-test`, exit 0.

The least invasive launch method is a dedicated root one-shot job in the
existing unsecure launchd cache, using the same plist editing mechanism as
`sweet-root-tweaks.py`. On an **already mounted writable root image**, the
coordinator can stage the tool offline:

```sh
sudo python3 tools/stage-fuxi-gpu-abi-test.py "$IOS_ROOT" tools/out/fuxi-gpu-abi-test
# After qualification, remove only this binary/job:
sudo python3 tools/stage-fuxi-gpu-abi-test.py "$IOS_ROOT" --remove
```

This copies to `/usr/local/bin/fuxi-gpu-abi-test`, writes
`/Library/LaunchDaemons/org.fuxi.gpu-abi-test.plist` and adds that job to
`/System/Library/xpc/launchd.plist`. It preserves other jobs, uses root without
a sandbox profile, RunAtLoad=true, KeepAlive=false, and stdout/stderr
`/dev/console`. No existing daemon or SpringBoard injection is needed. Keep
the existing root-image signing/AMFI policy; the tool is ad-hoc signed and no
new trust-policy bypass is added by this lane. The helper never mounts an image,
accesses USB or installs anything on a phone.

Coordinator acceptance, serially:

1. Install/stage through the existing root-image workflow and use
   `shim/out/fuxi-metal1-boot.img` through the coordinator's normal temporary
   boot procedure. Look for `FUXI GPU ABI: NativeAdrenoGpuPublished v1`.
2. Capture the complete PASS/FAIL output, exit status if available, GPU logs
   and present statistics. Require both normal rounds, reopening and readbacks
   to pass; exercise touch/app switching while presentation remains alive.
3. On a separate boot, stage with `--watchdog`. This deliberately submits an
   unsatisfied CP_WAIT_REG_MEM. Require `watchdog returns timeout`, a
   `client fence deadline (100 ms)` / GBIF containment log, continuing UI/CPU
   presents, and refusal of later client work. Failed drain should quarantine
   BOs and may make the free check fail; report it rather than treating it as
   safe reclamation. A reboot is needed to restore GPU submission.
4. Remove the diagnostic job afterwards. Test default-off by omitting
   `fuxigpuabi=1` from a separately packed image: no ABI service should appear
   and the existing present path should remain usable.

To reproduce the image, build the patched XNU in `ci-lane-metal` with
`tools/build-fuxi-native.sh` and the matching IIG cache, copy stripped/unstripped
kernels into `xnu/out-fuxi-metal1`, then supply the existing private toolchain
and GMU/SQE/zap environment variables:

```sh
bash tools/build-fuxi-metal-images.sh "$BASE_0097B_ARTIFACTS"
python3 tools/test-fuxi-metal.py
python3 tools/test-fuxi-adreno.py
python3 tools/test-fuxi-memory.py
python3 tools/test-fuxi-scm.py --kernel xnu/out-fuxi-metal1/kernel.development.fuxi.unstripped
python3 tools/test-fuxi-metal-artifacts.py --base "$BASE_0097B_ARTIFACTS" \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
sha256sum -c xnu/out-fuxi-metal1/SHA256SUMS
```

Offline coverage includes fixed layouts/selectors and overflow/flag checks;
an independent PM4 decoder checking reset/SRAM wait, parity, IFPC and BV/BR
ordering; the actual C backend compiled under ASan/UBSan with modeled register,
SMMU and scheduler interfaces; present exclusion and late fences; watchdog with
no WaitFence calls, faults and failed drain; clean patch application; existing
pixel/cache/lifetime regression models; native recovery/SCM instructions;
boot-v4/Image/payload/full-RAM bounds, unchanged 0097b ADT/root/CPU identity and
exact command line plus ` fuxigpuabi=1`; exact external firmware blobs; signed
raw Mach-O target. These are host models/build checks, not GPU execution.

## iOS 16.7 Metal discovery, re-derived

Facts from the supplied iPhone10,3 / 20H392 arm64 shared cache, inspected with
`ipsw dyld disass`, `macho --symbols`, `macho --objc` and `dump`. Main cache
SHA256: `76874e8375252ee643011ff8063f0cce12355f05cf5f9897a34e5679667a65aa`.
Addresses below are unslid and specific to this cache.

| Observation | Evidence |
| --- | --- |
| `_MTLRegisterDevices` at `0x181b144c8` matches `IOAcceleratorES` | String/matching at `0x181b14514..0x181b1452c`; iteration calls `_getMetalPluginClassForService`, `initWithAcceleratorPort:` (`0x181b14580`), then `_MTLAddDevice` (`0x181b1458c`) for a non-nil result. |
| Plugin properties and bundle lookup | `_getMetalPluginClassForService` at `0x181a3c7c8` reads `MetalPluginName`, builds `/System/Library/Extensions/<name>.bundle`, reads `MetalPluginClassName` or falls back to principalClass; checks `isSubclassOfClass:_MTLDevice` at `0x181a3c8d0`. |
| `_MTLDevice` instance size is **528**, not 536 | Class symbol `0x1e1d88a68`, decoded class data at `0x1d59e51a8`; class_ro fields flags=4, instanceStart=8, instanceSize=`0x210`. This must also be checked with `class_getInstanceSize` before constructing a runtime subclass. |
| Stock IOGPU open type | `_IOGPUDeviceCreateWithAPIProperty` at `0x1ece1ce38` constructs `1 | (api << 16)` at `0x1ece1ce68..0x1ece1ce80`, then calls IOServiceOpen. Our 0x4147 ABI is a different connection contract. |
| Kernel family availability | The linked metal1 kernel has the new native service metaclass and ABI symbols, and no `IOAcceleratorES` or `IOGPUFamily` implementation/metaclass symbols. This standalone payload does not include Apple's IOGPUFamily. Publishing an ordinary service alone cannot create an MTLDevice. |

Reproduce disassembly without copying any Apple code into git:

```sh
"$IPSW" dyld disass "$DSC" --symbol _MTLRegisterDevices --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _getMetalPluginClassForService --symbol-image Metal
"$IPSW" dyld macho "$DSC" Metal --symbols
"$IPSW" dyld dump "$DSC" 0x1d59e51a8 --size 24
"$IPSW" dyld disass "$DSC" --symbol _IOGPUDeviceCreateWithAPIProperty --symbol-image IOGPU
```

**Unverified:** iOS 16 sandbox selector/class allowances, private IOGPU object
vtable layouts, root-image plugin signing/loading and CoreAnimation's complete
requirements. Do not import UniXNU's iOS 17 IOGPU size 736 or `_MTLDevice` size
536 into this kernel. Its published plugin returns nil; its raw IB success does
not establish Turnip or Metal success. Its documented Turnip IBs still hang CP.

## Remaining milestones and acceptance gates

1. **M1 device qualification:** normal raw IB/readback/reopen, present coexistence
   and watchdog containment above. Current status: raw IB/readback/reopen qualified on metal2g as recorded above;
   no new watchdog-containment or presentation qualification is claimed here.
2. **M2 Turnip cross-build for iOS 16:** offline build/audits complete;
   root load/enumeration/device lifetime passed on metal2h (see M3 report above). Follow UniXNU
   `scripts/build-turnip-darwin.py` and `scripts/data/adreno-compiler-inputs.json`.
   The actual build pin is Mesa **85fda3b36405bcaea8bffc64aa4f2651dc90770d**;
   `gpu-metal-vulkan-inputs.json` also records **c823a83e...** as a reference
   survey tree, not the revision the build script checks. Apply, in manifest
   order: `freedreno-tools-nir.patch`, `ir3-darwin-build-id.patch`,
   `ir3-portable-asm-roundtrip.patch`, `offline-adreno-compiler-probe.patch`,
   `turnip-darwin-kmd.patch`. Retain MIT notices; keep source/build caches out
   of git. Change the cross target from ios17.0 to **arm64-apple-ios16.0**.
   Use iPhoneOS SDK, Meson `freedreno-kmds=darwin`,
   `tu-darwin-abi-dir` pointing at our ABI header, `vulkan-drivers=freedreno`,
   `platforms=macos`, no Gallium/GL/EGL/GBM/LLVM, and native glslang/build tools.
   Audit iOS availability, Mach-O load commands/dependencies, cache-line helpers
   and absence of EL0 CTR_EL0 instructions; the supplied patch now substitutes
   Darwin's 64-byte cache-line value. Require exported vk_icdGetInstanceProcAddr
   and a root-daemon load/enumeration before submitting shaders.
3. **M3 Turnip smoke:** observed PASS on metal3 (see report above), including
   fill, compute, triangle, reuse and reopen with TU_DEBUG=nocb. This qualifies
   that tested workload; no Metal frontend was involved. Preserve finite waits
   and CPU UI fallback after any future loss. Broader coherence, reset/recovery
   and workloads beyond the tested subset remain separate gates.
4. **M4 Metal discovery/frontend:** native discovery class + signed transfer-only
   bundle and smoke built/host-tested (M4 section above). **Observed PASS:** coordinator reported real unsandboxed-root
   MTLCreateSystemDefaultDevice, shared-buffer blit fill, copy and 16 reuse
   rounds. M5a adds a default-deny process gate and safe census stop. A runtime size guard
   requires 528. No iOS sandbox access, Apple user-client ABI or selector tunnel
   is implemented. Stop on failure; derive any additional requirements from
   the supplied iOS 16 cache before expanding the interface.
5. **M5 Translation and licensing:** keep **metal2vulkan external** at pinned
   `720ef1ce9ae2a2068999a8675717418b9416283e`; its declared choice is
   GPL-2.0+ OR LGPL-3.0+. Do not vendor it or statically fold it into this repo's
   kernel/plugin artifacts. Distribution needs a separate license/relinking
   plan before shipping a combined frontend. Validate AIR/metallib translation
   and shader caching separately. Imported host/IOSurface BOs, page lifetime
   and coherence are additional kernel work; M1 advertises no HostImport.
6. **M6 presentation/CoreAnimation:** implement IOSurface texture import,
   CAMetalLayer drawables, producer/consumer fences and buffer lifetime, pixel
   format/stride/orientation handling, and CA's required render/compute paths.
   Preserve the existing CPU-present fallback through frontend/plugin failure.
   Require actual GPU-rendered app and CA content, UI correctness, latency,
   thermal and memory measurements. Neither UniXNU's nil-returning plugin nor
   its partial frontend supplies this integration today.

## Risks and delivery

metal2g qualified raw IB/readback/reopen; metal3 subsequently qualified the
reported Turnip fill/compute/triangle/reuse/reopen workload. These do not
qualify general coherency, CP recovery, shaders beyond that workload, or M4
Metal discovery. M4 discovery/transfers passed on device; M5a adds host-verified gating and
census. M5a device acceptance and all presentation integration remain pending. Raw PM4 is privileged and not validated as a security
boundary; only an unsandboxed root diagnostic client is supported. Arbitrary
root IBs can access privileged GPU state. Failed bus drain leaks/quarantines
memory until reboot; wedged interconnect/MMIO can exceed software containment.
There is no CP recovery, preemption, fair scheduling, host import, IRQ completion,
MTLDevice, drawables or accelerated CoreAnimation in M1.

Artifacts and their exact hashes are in `xnu/out-fuxi-metal1/SHA256SUMS` and
`tools/out/SHA256SUMS`; all binaries/firmware/Apple analysis output are ignored.
The command line is the supplied 0097b line plus exactly ` fuxigpuabi=1`.
The ADT remains byte-identical, including seven CPUs, pinned UFS root and
5088 MiB of XNU RAM with the existing 32 MiB GPU reservation.

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal1/kernel.development.fuxi` | `40662b2881e536fbcada825e92768718d92aa35999afa52d13c5dc8211b80088` |
| `xnu/out-fuxi-metal1/kernel.development.fuxi.unstripped` | `4376b4eefb3c66f0c1eac928459df7b7d3fab7d9473837ba889c7e45e7fc0b22` |
| `xnu/out-fuxi-metal1/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-metal1-payload.bin` | `781e358637e37205e6bc1140406d7965801293516ad74c9453a733273969eeb2` |
| `shim/out/fuxi-metal1-boot.img` | `c430735773e9b97f0fa60f37244da085ab219ad97c46224dbf2f297834b59851` |
| `tools/out/fuxi-gpu-abi-test` | `fc04e8157061a21d6f713197625010006e28b77b56c01cddf1d38ffd5cd85ae1` |
