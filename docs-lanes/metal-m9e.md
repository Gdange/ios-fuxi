# Metal M9e: NativeMetal CA frontend port and ordered destination reads

2026-10-22. Source and documentation lane for **iOS 16.7 / arm64**. No device
access, test-executable runs, GPU submissions, deployment, workflow dispatch or
push. The first three supplied CA logs describe preceding builds; they are not
M9e results. **backboardd and SpringBoard remain DENIED**, including explicit
allow entries and census mode. The isolated `fuxi-ca-smoke` remains the only CA
client. The 528-byte `_MTLDevice` guard, NativeAdreno ABI 2, 16 KiB HostImport
contract and SweetSurface/IOMobileFramebuffer display path are unchanged.

## Sources and file-by-file provenance

Read M8, M9b, M9c and M9d before this port. Source references use the same
revisions as M9b:

* UniXNU `b6ae99c17194e6a18fd819d3148e69fe405e91fc`.
* NativeMetal `6dcd4cbb69eb5625988706de42aa8ad3009b8ee7`, MIT,
  [source tree](https://github.com/UniXNU/NativeMetal/tree/6dcd4cbb69eb5625988706de42aa8ad3009b8ee7).
* metal2vulkan `720ef1ce9ae2a2068999a8675717418b9416283e`, external,
  dynamically loaded; no dependency source or runtime source copied into Fuxi.
* Mesa `85fda3b36405bcaea8bffc64aa4f2651dc90770d`, external Turnip.

| Fuxi file | Provenance and change |
| --- | --- |
| `tools/fuxi-metal/Frontend.mm` | MIT adaptations of `NativeBridgeMetalFrontend.mm`: NBBuffer/NBTexture resource metadata and lifetime, region-transfer/view adapters, buffer-backed textures, NBHeap ownership, NBSharedEvent listener queue forwarding, NBFence/encoder adapters, NBQueue completion routing, NBCommandBuffer scheduled/completed callbacks and event adapters, resource groups/cache hints, function/library aliases. Manual retain/release, bounded validation and Fuxi synchronous submission replace their ARC/runtime handles. Attribution extends the existing notice; the full MIT license remains in `LICENSE.NativeMetal` and the signed bundle. |
| `tools/fuxi-metal/Backend.hpp` | Original Fuxi Vulkan consumer of the external compiler's documented graphics/binding plans. Adds actual feature enablement, ordered input-attachment passes/pipelines/descriptors, no-copy host buffers, checked linear images over buffers, smaller texture extents and empty render passes. Their `NativeMetalObjects.rs` delegates these operations to metal2vulkan-runtime; its framebuffer-fetch/render-target/pipeline contracts were studied, not vendored. |
| `tools/fuxi-metal/Translator.hpp`, `tools/fuxi-metal2vulkan-bridge.rs` | Original C/Rust adapter extended to ABI 5: device enable and fetch-plan fields, exact color-0 render-pass dependency admission and attachment writes, plus format-derived out-of-bounds-alpha specialization. Existing typed constants, linked stages, draw plans and specialization/variant checks remain authoritative. |
| `FramebufferFetch.hpp`, `Timeline.hpp` | Original SDK-independent validation and process-local timeline used by production. The timeline provides the CPU event semantics behind the MIT-derived frontend adapters. |
| `TestBackend.hpp`, `CAFrontendTests.mm`, `TimelineTests.cpp`, `PipelineFrontendTests.mm` | Original tests/model for the ported Objective-C adapters, texture regions/views, heap/resource lifetime, events/fences, callback routing and function aliases. |
| `tools/shaders/metal-m7-control.ll`, `tools/test-fuxi-metal-m7.cpp` | Original authored AIR destination-read function and pixel oracle; overlapping fragments within one draw and across passes, cache reuse and CPU ownership checks. No extracted Apple shader content. Smaller clear-only passes are also covered. |
| `tools/test-fuxi-metal-m9b.cpp`, `.py` | Fetch-contract negatives and non-submitting ordered-pipeline priming/reopen/cache-retirement controls. Adds compile-only mode. |
| `tools/test-fuxi-metal-frontend.py` | Includes the new native suite and timeline controls, preserving `-Wall -Wextra -Werror` and ASan/UBSan. Adds compile-only mode. |
| `tools/test-fuxi-metal-m9a.cpp`, `tools/test-fuxi-metal-m6.py`, `tools/test-fuxi-metal-defaults.py`, `tools/build-fuxi-metal-translator.sh`, `VertexProfile.hpp` | ABI layout assertion, modeled ABI export, native-test/help text and source-package replacement notice updated for ABI 5. Vertex rate restrictions stay explicit. |

`HostMetalCommandServer.mm` and `Offline*Wire.h` belong to NativeMetal's separate
host/offline transport. Fuxi calls Vulkan and the external translator directly;
none of that transport, its object handles or its private registration code is
imported. `NativeMetalObjects.rs` is the API/runtime mapping reference, not a
replacement for the NativeAdreno kernel contract. `quartzcore_blur_host.mm`
exercises actual blur/compute workloads beyond the five smoke scenes; its
success does not qualify those workloads here. No NativeMobileDisplay,
NativeAdreno or AirNir implementation was copied. Their licenses and private
ABIs are not interchangeable with NativeMetal's frontend license.

## Complete CA-facing surface map

The inventory follows actual NativeMetal method bodies (its introductory heap
comment is stale). “Bounded” below means a real implementation with explicit
refusal outside the stated profile; no zero-return forwarding is counted as
support. The relevant iOS 16 calls and layouts remain described in M8/M9d.

| Family | NativeMetal surface | Fuxi M9e result / remaining difference |
| --- | --- | --- |
| Device / discovery | Accelerator-port registration, identity, limits and runtime-derived selector answers; forced Apple1–3/Common1–2 families | Retains iOS 16 discovery and 528-byte base. No iOS 17 offsets, 536-byte guard, family waivers, display mutations or registration tables. Native-half query remains true; families, ROG, tile, memoryless, SIMD and YCbCr remain false. Ordered color fetch is internal Vulkan functionality, not general Metal raster-order-group support. |
| Queue | `newCommandQueue`, max-count, descriptor/unretained command-buffer aliases; submission/completion queues, GPU priorities | Existing aliases retained. Submission stays on caller; completion queue is retained and routes completed handlers. Priority requests return NO. Owner identity returns KERN_NOT_SUPPORTED (46); fine-grained background visibility is advisory. |
| Command buffer | Encoder factories, enqueue/commit, scheduled/completed handlers, waits, `commitAndWaitUntilSubmitted`, protection/timing/debug SPI, event wait/signal, cache hints | Synchronous GPU completion retained. Scheduled notification follows actual successful `vkQueueSubmit`; user handlers execute outside the backend mutex after its wait returns, possibly with Completed/Error status. Completion routing and callback-group waits added, including own-callback wait avoidance. Protection only zero; timings zero. Rejected/unsubmitted work gets completion/error, never an invented scheduled callback. |
| Render bindings | Stage buffers, ranges, offsets, copied bytes, textures/samplers/ranges, combined fragment texture/sampler SPI | M9a–d bindings retained; four buffers per stage, four fragment textures/samplers, eight vertex attributes/four layouts. Owners remain retained in immutable draw snapshots. Vertex texture/sampler bindings and larger slots still refuse. |
| Render state / draws | Pipeline/depth-stencil, viewport/scissor, raster/depth bias/blend/stencil state, visibility, primitive/indexed variants, resource hints, fences | Existing triangle/list-strip, UInt16 index and single-instance paths; neutral depth/stencil/raster/query state. Adds resource-use retention and fences. No stencil attachments/tests, arbitrary culling/fill, true instancing, UInt32 indices, lines/points, indirect/base-instance variants or tile API. Empty render encoders now execute load/clear/store semantics. |
| Blit | Buffer fill/copy, image/image and buffer/image transfers, mipmaps, synchronization, fences | Mip-0/slice-0 rectangles at actual dimensions up to 256, padded buffer rows, checked offsets/strides/bounds and CPU region methods. Fences and synchronization hints retain owners. Mipmap generation accepts only the already single-level texture (no work); multilevel/array/MSAA blits still refuse. |
| Compute | Compute descriptor/function factories and reflection options; buffer/texture/sampler adapters, threadgroup memory, thread/group dispatch, barriers, fences | Remains refused. The solid/opacity/rounded-clip/image/transform smoke has no blur or compute scene. A CA path that requests compute remains a failed diagnostic. NativeMetal's compute support is a remaining port, not a selector stub here. |
| Buffers | Shared/private allocation, copied/no-copy host data, resource metadata/owner/purgeability, buffer-backed textures | Shared copied buffers retained. Adds aligned same-memory HostImport no-copy buffers and owned deallocator blocks; failures never take the caller's deallocator. GPU-uncertain ownership keeps pages/deallocator alive. Private/managed/untracked/write-combine buffers remain refused. |
| Textures | Descriptor/IOSurface imports, CPU region read/replace, linear buffer textures, views, resource metadata | Adds dimensions 1–256, shared/private descriptors, padded CPU rows, partial CPU regions, identity-format full-range views with retained parent, checked linear buffer texture adapters and backing-owner retention. IOSurface stays single BGRA plane at 256 square. Formats remain BGRA8/RGBA8/RGBA16Float/R8/RG8 plus sampled A8. sRGB, reinterpret views, mip/array/MSAA/protected/multiplane imports remain refused. |
| Raw-memory textures | Device no-copy-texture and texture-layout SPI | NativeMetal explicitly refuses both: Apple's raw texture layout is unknown. Fuxi continues to refuse these too. Buffer-backed linear images require a validated Vulkan layout; no guessed Apple layout. |
| Heaps | Automatic/placement heap, budget/allocation queries, buffers/textures, purgeability/metadata | Adds automatic shared heaps as a bounded allocation budget. Resources retain heap and return actual allocation bytes on destruction. Underlying Vulkan allocations are independent/nonaliasable. No placement, sparse, private heaps, aliasing, or size/alignment estimator API. Allocation can fail when driver requirements exceed the remaining budget. |
| Samplers / depth-stencil | Runtime sampler/state mapping and labels, sampler pixel-format SPI | Adds sampler's neutral Invalid pixel-format answer. Existing nearest/linear clamp-to-edge sampler and Always/no-write/no-stencil state retained. Broader sampler/depth/stencil descriptors still refuse. |
| Functions / libraries | Data/file/URL/default-bundle loading, named/constants/descriptor functions and async completion, names/types | Adds file/default-bundle aliases, executable library type, function options/descriptor reconstruction and synchronous completion-handler adapter. Keeps public-setter constant snapshots (128/stage, including uchar3), precise errors and no precision-name substitution. Kernel lookup, function-name enumeration and reflection remain absent. Library bound stays 4 MiB, appropriate to this iOS 16 resource, not NativeMetal's larger iOS 17 library. |
| Pipelines / specialization | Render/compute descriptors/options, reflection and archives | Render options alias accepts None and clears reflection; other options fail explicitly. Real linked AIR + dynamic binding plans, specialization bytes and variants are consumed; details below. Compute/reflection/archive options remain refused. The optional private Apple pipeline-library probe continues returning nil/error without latching a rendering failure, allowing descriptor-based CA compilation. |
| Argument buffers | No argument-encoder frontend in this NativeMetal revision; runtime can support broader resource tables | Neither frontend claims argument encoders. Ordinary CA “arg” buffer-pool bindings are supported within slot limits. Unconsumed synthetic tables, heap descriptors and runtime plans remain refused. |
| Events / fences | Process-local events/shared events/listeners and encoder fences; shared handle refused | Adds local monotonic CPU timelines, listener-queue delivery and command wait/signal points. GPU work before a signal completes first; waits precede subsequent submits. Waits are capped at two seconds. Fences rely on the serialized backend stream's full operation dependencies. No cross-process/shared-event handle or IOSurfaceSharedEvent integration. |
| Drawables / presentation | Explicitly absent in NativeMetal frontend; server produces completed IOSurfaces | Remains absent. No CAMetalLayer/MTLDrawable, presentation timestamps or display-driver replacement. The smoke renders to IOSurface directly. |
| Archives / caches / priming | Runtime binary archives, persistent specialized products/Vulkan records, device priming, QuartzCore recipe priming, exported UI packs | Keeps process-only translation and driver caches; extends non-submitting priming tests to ordered recipes. Persistent archive/pack and runtime-device priming remain differences; details below. |

Debug groups/signposts and cache hints are advisory. Resource groups actually
retain their member resources. Large standalone resources track purgeability;
volatile resources are kept resident, shared buffer Empty zeros its bytes, and
texture Empty preserves texels. Heap resources and texture views answer
NonVolatile. These operations do not claim a kernel purge implementation.

## Ordered fetch: frontend through bridge to Vulkan

CA's fragment function/constants choose whether a pipeline fetches the target;
the frontend does not infer that from fixed-function blending being off.
Backend enumerates the device extension and queries
`VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT`. It enables
only color access, together with the EXT extension, at `vkCreateDevice`.
The pinned Turnip `tu_device.cc` advertises the EXT extension and all three
attachment-order feature booleans; depth/stencil are deliberately left disabled.
See the [pinned Turnip source](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/vulkan/tu_device.cc).
This is source evidence; runtime queries still decide, and no A740 result is
claimed by this lane.

**ABI 5 requires updating FuxiMetal and libmetal2vulkan together.** CompileOptions
appends `orderedColorAccess` (size 40100; constants still start at offset 160).
Translation appends `fetchMask` and the actual enabled ordering contract. The
adapter's DeviceDescription enables exactly the corresponding Vulkan feature
and extension; it does not enable the whole supported/recommended profile.
The enable is part of the process translation-cache key through CompileOptions.
An old dylib is refused by the ABI guard before compilation.

The bridge admits only `FramebufferFetch { color: 0 } / RenderPassContract`
with that enable, a single color-0/input-0 use, single sample, and ordered input
attachments. It emits descriptor type INPUT_ATTACHMENT from the plan's
`WriteSource::Attachment`, including format and fragment-stage identity.
Backend checks mask/enable agreement, layout and draw descriptor identity,
then writes the actual target view, null sampler and GENERAL image layout.
Storage/interlock plans and unordered fetch continue to fail. No dependency,
SPIR-V ordering requirement or descriptor source is silently discarded.

All render targets on the enabled device take INPUT_ATTACHMENT usage, including
same-memory IOSurface imports and linear buffer-backed targets. External-image
format/import checks include that usage. A target lacking it refuses before
submission. Every graphics pipeline and compatible subpass uses the ordering
flags, including shaders that do not fetch. Input and color references name the
same attachment in GENERAL. Load/store variants keep this same contract.
External subpass dependencies include color output and fragment/input reads;
the existing full memory dependencies between operations order stored results
before the next pass. The extension provides ordering among overlapping
fragments within one draw. A sampled snapshot or pre-draw copy cannot do that.

Imports for which the extra usage is unavailable currently refuse: the external
runtime's scratch-image fallback and interlocked storage-image role splitting
are not ported. The current pinned Turnip source supports the preferred ordering
form, but imported-image compatibility still needs device qualification.
Ordinary sampling feedback remains refused, including views sharing Vulkan
memory with the target. Shader buffers aliasing the target also refuse.

The existing specialization map/data is installed on both stages. Binding-plan
changes still select or compile bounded variants and then revalidate the draw;
OutOfBoundsAlpha/Specialization now follows the bound format at fragment texture
slots 0–3 through this same mechanism. No fixed default substitutes for that
format fact. Other dependencies, constant blobs, extra sets, shader resource
writes and unsupported graphics state still refuse.

## Cache and priming comparison

NativeMetal's `bridge/quartzcore_cache.mm` builds actual library-derived ordinary
vertex/fragment pairs plus blur recipes, populates a runtime binary archive,
and verifies producer/consumer, UUID-miss and empty-driver-cache behavior.
Those recipes target its larger iOS 17 library, float-color layout and optional
multiple render targets. They must not be copied as iOS 16 CA specializations:
M9d established the iOS 16 buffer-1/stride-48/Half4 layout, while fixed CA
functions require their actual typed constants.

Fuxi reuses the eight-entry/16 MiB translation cache and per-device Vulkan
pipeline cache. The driver cache retires after eight pipeline pairs or an
oversize/query failure. M9b's non-submitting test now primes/reuses 20 ordinary
and 20 ordered-fetch pipeline pairs over device reopen and cache retirement.
It is the reusable descriptor-replay technique from QuartzCore cache priming,
using authored inputs and our real consumer. It does not create a persistent
QuartzCore UI archive or eliminate first-use CA compilation across boots.
`cache=1` reports a supplied VkPipelineCache handle, not a proved driver hit.

No persistent pack parser/exporter, binary archive, runtime startup priming,
Turnip builtin pack, or external compiler patch 0001–0005 is shipped. Their
serialization/runtime contracts are absent from Fuxi's bounded bridge. A future
persistent cache needs exact library/typed constants/layout, adapter enable
profile, translator revision/options, device/driver UUID and atomic private
storage. Apple-derived cache products must remain outside git. Cold compiler
latency and the existing CA alarms remain material risks.

## Verification and remaining qualification

Completed here, **without running test executables**:

* `test-fuxi-metal-frontend.py --compile-only` builds Linux constant-hook,
  vertex-profile and timeline controls with warnings as errors and ASan/UBSan;
  native Foundation/Metal suites are skipped on Linux.
* `test-fuxi-metal-m9b.py --compile-only` builds cache/fetch-contract controls
  and the actual Backend pipeline-priming program, and assembles authored AIR.
* M7 and M9a C++ translation units compile with `-Wall -Wextra -Werror` and
  ASan/UBSan; authored M7/fetch AIR assembles with LLVM 18.
* `cargo +1.98.0 check --offline --tests` checks the bridge and Rust tests
  against the clean external translator pin, including exact dependency scope
  and ABI-5 layout checks. Assertions have been compiled, not executed.
* Python syntax, shell syntax, `git diff --check`, and `metal --dry-run` pass.

The new native CA suite checks real Objective-C wrappers over a CPU model:
scheduled-before-completed delivery, callback resource creation, completion queue
routing/waits, local event listeners, fences, heap-budget/lifetime release,
resource groups/hints, padded texture rows, partial regions, shared views and
clear-only encoders, purgeability transitions and no-copy deallocator refusal. Existing tests retain valid descriptor indices below 31;
no test asks Mac Metal to install an out-of-range bufferIndex. Native Mac builds
and test executions are unperformed. No new iOS `metal` build result is claimed;
the CI target, framework dependencies and iOS deployment target remain intact.

M7's authored fetch oracle draws two coincident quads in one indexed draw,
then repeats in a LOAD pass. Each fragment computes destination/2 plus
(1/4, 1/8, 1/16, 1/4). Four visits must produce exact BGRA (30,60,120,120)
at all 65536 pixels, also visible in the imported CPU pages. It repeats after
cache reuse and refuses DontCare destination reads. A separate 32×24 target
checks clear-only/load-only/discard behavior, padded partial uploads/readbacks
and rectangular image copies with zero initialization of unspecified texels. These are **new unrun regressions**,
not pixel results. Existing M7 blend/format/vertex/bounds controls remain.

Remaining risks include real CA's selected shader/runtime plans and slots,
nonneutral stencil/compute paths, IOSurface INPUT_ATTACHMENT compatibility,
linear row/alignment compatibility on Turnip, native MRC/block lifetimes and callbacks on a serial completion queue, cold
compilation, CPU/GPU ownership after faults, and numerical agreement. Event
points split synchronous submissions; a later event timeout can follow already
completed earlier work. General concurrent submission/encoding, GPU timeline
semaphores, cross-process producer synchronization, display-size images,
full Metal capability tables and persistent CA cache priming remain outside
this implementation. A refusal or abort still fails the isolated diagnostic;
there is no compositor transaction replay or new system-process admission.

## Exact next device test (separate operation)

Build the existing CI **metal** target normally, then stage the matching signed
bundle, **ABI-5** translator, source archive/notices and existing smoke files
using M9a's established image recipe. Keep the existing NativeAdreno-ABI-2
kernel/Turnip pair and display path. This lane performs none of those operations.

For the first boot set only `ca-smoke.conf` to `0`. Keep `metal-clients.conf`
restricted to `fuxi-metal-smoke` and `fuxi-ca-smoke`. Use the existing
`sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-ca-smoke` and its `sweetnull`
entry, preserving the other boot arguments. Do not exec-swap either compositor.

Require these initialization observations:

```text
FUXI MTL: gate deny backboardd
FUXI MTL: gate deny SpringBoard
FUXI CA: jetsam ... set=1024MB rc=0 ...
FUXI MTL: gate allow fuxi-ca-smoke
FUXI MTL: ordered framebuffer fetch enabled (input attachments)
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
```

Pipeline work should use `fixed_frag_lph_cpf` (or another actual lph variant),
matching lph vertex functions and M9d's buffer-1/stride-48 descriptor. Require
`translation PASS`, `Vulkan pipelines PASS` and
`framebuffer fetch plan mask=1 ordered=1` for a destination-reading pipeline.
`mask=0 ordered=1` is valid where specialization prunes the read. Function
ordering, cache hits and pipeline counts are not fixed expected values.
A missing enable, ABI mismatch, descriptor/dependency refusal, failed import,
alarm or abort fails the diagnostic; do not proceed by changing capabilities,
pixel tolerances or process admission.

After stage 0 passes, repeat maximum stages 1, 2, 3, 4, each cumulative scene
with the same gates and unchanged pixel thresholds:

| Maximum stage | New required line (each preceding stage must also pass) |
| --- | --- |
| 0 | `FUXI CA: stage 0 solid PASS pixels=65536 ... gpu_draws=...` |
| 1 | `FUXI CA: stage 1 opacity PASS pixels=65536 ... gpu_draws=...` |
| 2 | `FUXI CA: stage 2 rounded-clip PASS pixels=65536 ... gpu_draws=...` |
| 3 | `FUXI CA: stage 3 image PASS pixels=65536 ... gpu_draws=...` |
| 4 | `FUXI CA: stage 4 transform PASS pixels=65536 ... gpu_draws=...` |

Every gpu_draws value must be positive, frontend errors zero, and each boot must
end `FUXI CA: done PASS`. Record translation/driver timing and the first refusal
in ignored logs. Separately repeat the preexisting `fuxi-metal-smoke` maximum
stage 6 regression. Neither successful host controls nor these isolated scenes
authorize backboardd/SpringBoard admission.
