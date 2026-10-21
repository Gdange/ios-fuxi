# Metal M9b: lessons from UniXNU's GPU compositor

2026-10-21. Source port and Linux host checks only. No device access, deployment,
workflow dispatch or push. The supplied Fuxi A740 smoke stages 0–6 PASS remain
the starting observation. **backboardd and SpringBoard stay DENIED**, including
census and explicit allow entries. This lane does not enable system CA Metal.

## Evidence and revisions

UniXNU reports a working GPU-composited desktop and pixel-identical GPU copies
into scanout on its A740 platform. These are **upstream observations**, not a
Fuxi result. Its [driver report](https://unixnu.com/drivers/native-adreno)
also records substantial cold compilation and an incomplete precompiled UI pack.
The source inspected in the ignored read-only checkout is pinned here:

| Component | Revision | Relevant source / license |
| --- | --- | --- |
| UniXNU | `b6ae99c17194e6a18fd819d3148e69fe405e91fc` | [build inputs and patches](https://github.com/UniXNU/UniXNU/tree/b6ae99c17194e6a18fd819d3148e69fe405e91fc/scripts) |
| NativeMetal | `6dcd4cbb69eb5625988706de42aa8ad3009b8ee7` | [frontend, plugin, bridge and caches](https://github.com/UniXNU/NativeMetal/tree/6dcd4cbb69eb5625988706de42aa8ad3009b8ee7), MIT |
| NativeAdreno | `805c7e4bfb7451eb6923f4a14dea8c657b9be7d2` | [KMD](https://github.com/UniXNU/NativeAdreno/tree/805c7e4bfb7451eb6923f4a14dea8c657b9be7d2), MIT |
| NativeMobileDisplay | `eb9c949164826c2e52f2771eac19e4b8bd8a3dae` | [swap/retirement](https://github.com/UniXNU/NativeMobileDisplay/tree/eb9c949164826c2e52f2771eac19e4b8bd8a3dae), BSD-2-Clause-Patent |
| AirNir | `032a867b5dc3121164bbd706e4e81e3b9fcd2b8f` | [offline compiler](https://github.com/UniXNU/AirNir/tree/032a867b5dc3121164bbd706e4e81e3b9fcd2b8f), MIT |

Both projects use metal2vulkan
`720ef1ce9ae2a2068999a8675717418b9416283e`. The important difference is that
NativeMetal uses **metal2vulkan-runtime**, with five recorded external compiler/
runtime patches. Fuxi uses its bounded C/Rust compile-and-plan adapter and its
own Vulkan backend. Sharing the translator pin does not give us their runtime.
No external compiler/Mesa sources or patches were vendored or repinned here.

## Side-by-side

| Boundary | UniXNU source | Fuxi, including this commit |
| --- | --- | --- |
| Discovery | IOAcceleratorES → MetalPluginName → principal class `initWithAcceleratorPort:`; published mode returns a runtime device for system Metal registration (`MTLAddDevice` path). Without publication it tests then returns nil. | Same discovery family; Fuxi dynamically subclasses the installed `_MTLDevice`, with its 528-byte iOS 16 base guard and inherited-method interception. A root-owned process allowlist precedes backend construction. |
| Selection | `NativeAdrenoMetal.publish` admits backboardd; a `.clients` file admits other names or `*`. Each admitted process has its own device/connection/cache. NativeAdreno reserves 1 GiB for the render server and 256 MiB each for up to two clients. | No publish marker, wildcard or compositor admission. Only the two smoke clients ship as allowed. Per-process objects exist; the kernel backend remains a single privileged diagnostic client, not independent per-process GPU windows or a PM4 security boundary. |
| Private ABI | iPadOS 17 target checks a 536-byte base and private protocol conformance. Some display-format adjustments read/write target-specific QuartzCore state. | 528-byte base and installed iOS 16 defaults. No private object offsets, address-based patches, family waivers or sandbox changes copied. |
| Device queries | Runtime answers selector tables; Apple1–3 and Common1–2 also return true despite journaled feature-row waivers. XR10 withheld to keep display surfaces readable. | Families remain false; bounded formats and size limits unchanged. Raising families is a separate path-selection experiment, not proof of the corresponding feature table. |
| Queue / commands | Descriptor and unretained aliases, scheduled/completed handlers, completion-queue routing, submission-queue SPI, priority refusal, protection checks, events/fences, async runtime submission and waits. | Synchronous submit/completion and existing handlers. Added descriptor/unretained aliases, caller-thread submission-queue SPI and explicit priority-not-applied answers. Scheduled handlers, completion-queue routing and protection/event APIs remain gaps. |
| Encoders | Render/compute/blit; stage buffer/offset/byte adapters, ranges, combined fragment texture/sampler SPI, raster/depth/stencil state, fences and resource-owner tracking. Unsupported tile calls fail. | Added vertex/fragment buffer ranges and offset updates, fragment texture/sampler ranges, combined texture/sampler SPI, nil unbind and neutral depth/stencil. Still four resource slots, triangle/list-strip, bounded render/blit only; unsupported calls poison commands. |
| IOSurface | Imports the process mapping as host pages, creates a texture over a buffer with the surface stride, retains surface/resource owners through GPU completion and flushes CPU caches at ownership transitions. | NativeAdreno ABI-2 HostImport pins the same memory with validated linear layout; still one-plane 256×256 BGRA. No display-size, multiplane, protected or cross-process completion contract added. |
| Drawable | Frontend explicitly does not implement drawables/presentation. CA produces completed IOSurfaces for the display driver. | Offscreen CARenderer texture path only. CAMetalLayer/MTLDrawable are not prerequisites for this test and are not implemented. |
| Shared events | CPU values, waits and listener callbacks forwarded to the listener queue; command-buffer signal/wait and encoder fences. `newSharedEventHandle` explicitly returns nil: no cross-process kernel event. | Not implemented. Shared IOSurface memory alone does not imply shared event synchronization. |
| Archives / caches | Metal binary archives backed by runtime pipeline descriptors and Vulkan caches, priming cache, incremental `m2vpack`, Turnip builtin pack. | Existing eight-entry/16 MiB process translation cache plus new process/device Vulkan pipeline cache. No persistent pack, binary archive, device-priming or builtin-pack consumer. |
| Display | NativeMobileDisplay offers a validated finished surface, user GPU client copies to mapped scanout, sequence-checked completion retires it; failed/abandoned copies use the display's CPU copy. | Our IOMobileFramebuffer/SweetSurface swap/present and cache-lock ownership remain. The existing kernel GPU-copy path has its own deadline/poison/quarantine rules. NativeGOP selectors and original display-family layouts are not our ABI. |
| Fallback | Nil discovery keeps SW; missing selectors/refused commands report errors. Display-copy failure has a CPU-copy fallback. No compositor transaction replay was found in the inspected plugin/display path. | Deny before selection; isolated CA failure counter latches refusal of later submissions. Two-second backend fence ceiling; poisoned resources are retained on uncertain ownership. CA smoke adds 30-second initialization and 60-second stage alarms. |

The display fallback copies **already composed pixels**. It cannot repair a CA
shader that failed or a render server that aborted. Likewise the upstream
one-second `waitSwap` maximum is a wait-for-offer timeout, not a watchdog on an
already delivered GPU frame. Client teardown abandons an outstanding offer;
the inspected offer path does not establish an independent compositor deadline.
Its telemetry thread is observational, not a recovery supervisor.

The working NativeMetal frontend's concrete API surface includes the following;
the wrappers still validate descriptors in the runtime, and some advisory calls
are explicitly no-ops:

| Object | Implemented entry points relevant to CA |
| --- | --- |
| Device / resources | `newCommandQueue`, max-count alias, `newBufferWithLength:options:`, copied/no-copy bytes, `newTextureWithDescriptor:` and IOSurface variant, no-copy texture SPI, linear-row/texture-layout helpers, heaps, samplers, depth/stencil, `newEvent`, `newSharedEvent`, `newFence`. |
| Libraries / pipelines | Data/file/URL/default-bundle libraries, named and constant-specialized functions, render/compute descriptors and reflection options, binary archive add/serialize. These are AIR/runtime paths, not execution of Apple's archived GPU machine code. |
| Queue / command buffer | Descriptor/unretained aliases, submission/completion queues, priority refusal, render/compute/blit encoders, scheduled/completed handlers, commit/schedule/completion waits, `commitAndWaitUntilSubmitted`, event signal/wait, default protection options and timing queries. |
| Render / resource state | Pipeline/depth/stencil, buffer/offset/range/bytes, texture/range/sampler/combined fragment SPI, viewport/scissor, raster state, primitive/indexed draws and fences. Resource-use hints are no-ops. |
| Blit / compute | Buffer and image copies, fills, mipmaps; compute pipeline, threadgroup memory, thread/group dispatch and fences. Compute resource-use and `memoryBarrierWithScope:` hints are no-ops in this frontend. |
| CPU / IOSurface ownership | Buffer contents, texture region transfer/views, no-copy deallocator ownership, retained IOSurface mappings and completion-time cache maintenance. |
| Shared event | `signaledValue`, setter, `waitUntilSignaledValue:timeoutMS:`, `notifyListener:atValue:block:`; cross-process `newSharedEventHandle` refused. |
| Explicit gaps | No MTLDrawable/presentation; tile setters refuse despite the external runtime having broader capabilities. Unknown selectors and unsupported descriptors report refusal. |

The file's introductory comment still lists heaps among gaps; the inspected
revision actually contains NBHeap and device heap methods. The table follows
those method bodies rather than treating that older comment as the API inventory.

## Framebuffer fetch, programmable blending and tiles

M8's 181/592 fetch refusals came from the bare Vulkan profile. Our bridge still
uses `with_enabled(EnableSet::new())` and rejects input/storage attachments and
rasterization-order state. That is an intentionally unconsumed runtime plan,
not proof that A740 cannot run the shaders. NativeMetal does not replace CA with
a software shader interpreter: it drives the external runtime, which enables
the supported Vulkan feature set and consumes the compiler's selected form.

The pinned external runtime's `framebuffer_fetch.rs`, `render_target.rs`,
`pipeline/graphics.rs` and `render.rs` implement two ordered alternatives:

* Rasterization-order color attachment access: `INPUT_ATTACHMENT` usage, input
  attachment bindings at the color index, `GENERAL` layout, compatible subpass
  and color-blend rasterization-order flags. All pipelines sharing that subpass
  must use compatible ordering, including pipelines that do not fetch.
* Ordered fragment pixel interlock: fetched color attachments become storage
  images and are excluded from that draw's fixed-function color attachments.
  Changes of attachment role split render-pass instances. Barriers also order
  storage writes between draws; an interlock alone is insufficient there.
  Scratch images bridge imports without the required usage. sRGB uses integer
  storage views and explicit code conversion; multisample forms have additional
  sample-plane or multisampled-storage requirements.

This is the external runtime's source capability, **not a captured trace naming
which form the upstream A740 desktop used**. The report plus this implementation
supports testing the ordered feature path here. It does not license an unordered
attachment read, sampled snapshot substitute or `AcceptUnordered` policy.
Programmable blending must preserve reads after earlier fragments in the same
draw as well as across draws; an ordinary pre-draw image copy cannot do that.

The current NativeMetal frontend offers no tile shaders (explicit refusals) and
the five forced families stop at Apple3. CA can evidently compose the reported
desktop without requiring a full Apple tile API. Do not turn M8's four tile
entry inventory records into a mandatory first-frame implementation task.
Determine whether iOS 16 CA actually requests them with our false capabilities.
The external runtime has broader imageblock emulation, but that does not make
it reachable through NativeMetal's refused tile entry points.

## Constants, cold cost and the three cache layers

NativeMetal snapshots private `newIndexedConstantArray`/`newNamedConstantArray`
accessors only after exact Objective-C method-encoding checks. It gathers a
dynamic vector of constants, journals each specialization and forwards it to
the runtime. Its inspected frontend refuses three-lane vectors; this is not
evidence that the iOS 16 uchar3 cases work there. Fuxi M9a already handles 128
constants/stage, including packed vector3 values and padded range strides,
through observed public setters. Keep that implementation and ABI 4.

The upstream report records 77 pipelines in about 43 seconds on one cold boot;
another records 103 Metal pipelines totaling 40.1 seconds, maximum 1.40 seconds,
and 94 Vulkan pipelines totaling 23.3 seconds. These totals are not frame times
and host compiler timings cannot predict Fuxi device deadlines. Runtime shaders
are still compiled on demand; there is no complete precompiled UI pack.

The external patch `0004` folds scalar/vector constant-global fields, alternates
folding with SCCP and prunes unused entries before inlining. Its rationale
describes an uber fragment shrinking from roughly 46,000 SPIR-V instructions
to a few hundred. Patch `0005` retains four decoded modules above 32 KiB with
ledger accounting and caches recent structural device-profile fingerprints.
Those changes explain why just increasing our constants limit is insufficient
for cold latency. They were studied, not copied into our repository or applied
to our clean external checkout in this lane. Patches `0001`/`0002` also address
optional constant outputs and resource-wait storage dependencies; `0003` caches
runtime device entry-point priming.

The caches solve distinct costs:

| Cache | What it reuses | Port decision |
| --- | --- | --- |
| Translation/runtime UI cache and `m2vpack` | Specialized shader products and driver pipeline records from earlier boots. MIT `CacheExport.h` exports changed files after quiescence and imports a streamed compressed pack before runtime device open. Packs can grow across boots; a single export is bounded. | No kernel log exporter or pack parser ported. Persisting a future pack requires exact translator/options/device/driver identity and private ignored storage; Apple-derived products never enter git. |
| Priming cache | Runtime startup's internally generated entry-point pipelines; UUID mutation and missing-cache host controls distinguish a real hit from fallback. | Our backend does not use that runtime startup path. Porting its cache blindly would cache work we never issue. |
| Turnip builtin pack | Driver-owned clear, blit and empty shader variants keyed to exact compiler source/profile. Independent of UI variant caches. | Requires an explicit external Mesa build change and matching generated data. No Mesa patch or builtin binary imported. |
| M9b Vulkan cache | Our triangle-list/strip VkPipeline compilation across repeated descriptor creation, separately from existing SPIR-V translation reuse. | Implemented locally using core Vulkan cache APIs, in memory per device; no serialized data import/export. It does not eliminate first-use UI specialization. |

## Source port and limits

The MIT adaptations in `tools/fuxi-metal/Frontend.mm` come from NativeMetal's
`NativeBridgeMetalFrontend.mm`: NBRenderEncoder's buffer-offset and range
adapters, combined texture/sampler setter, NBEncoder's nil resource semantics,
NBQueue aliases/submission/priority methods, and NBDeviceMethods' depth/stencil
object mapping. Fuxi names and manual retain/release are used throughout. Our
four-slot limits, class/device checks, retained owners, descriptor validation
and command poison remain authoritative. A malformed range is rejected before
walking arrays. Buffer-offset updates reject unbound slots, offsets at/past
length and inline-byte bindings. Draws retain their previous state snapshots.

Depth/stencil support is deliberately **neutral only**: Always compare, no depth
writes, and nil or Always/Keep stencil faces. Nonneutral states still fail at
creation; foreign states fail the encoder. Render passes still reject actual
depth/stencil attachments. The queue descriptor alias accepts only default error
options; unretained requests retain owners for safety. Submission remains on the
caller thread, and priority setters explicitly report NO. These adaptations do
not implement scheduled/completion queue semantics or pretend all CA SPI works.

`CAState.hpp` contains SDK-independent production bounds/state predicates.
`PipelineCache.hpp` and the changes to `Backend.hpp` are original Fuxi code
informed by the upstream cache separation. One empty cache is created lazily per
VkDevice and passed to both list/strip creations under the backend mutex. After
eight creation attempts, a queried size above 16 MiB, or a query failure, its
generation is destroyed between creations. The size limit is a post-creation
retention limit, not a bound on driver scratch/peak memory. Cache allocation
failure uses a null cache and ordinary pipeline creation; it grants no rendering
success. Existing pipelines outlive cache retirement. Teardown follows existing
poison rules. A separate Vulkan-pipeline timing line distinguishes driver work
from the existing translation timing; `cache=1` means a handle was supplied,
**not a verified driver hit or measured device speedup**.

The complete MIT notice is in `tools/fuxi-metal/LICENSE.NativeMetal`, copied
into the bundle before signing, hashed and required by staging as a regular
file. README Credits is updated. No NativeMobileDisplay, NativeAdreno or AirNir
source was copied in this commit; the existing NativeAdreno ABI notice stays.
No Apple bytes, extracted shader/IR/disassembly, metal2vulkan or Mesa code was
added. Native build/signing inputs and the existing CI target remain intact.

## iOS 17 versus 16.7

Public Metal range/offset setters, neutral depth/stencil objects, IOSurface
ownership, AIR specialization and the Vulkan ordering/cache mechanisms are
plausibly portable and do not inherently need iOS 17. The combined setter and
queue SPI need native iOS 16 qualification; M8 already found relevant selector
references, but a reference is not a working call trace. Our implementations
use SDK types available at the existing iOS 16 deployment target.

The 536-byte `_MTLDevice`, private constant accessor encodings, display object
offsets, deep/compressed framebuffer state addresses, original sandbox grant
patch, original display-family request/vtable layouts, target-specific GPU
family decisions and four-KiB native SMMU setup are **not portable ABI facts**.
Fuxi retains its 528-byte guard, public-setter snapshots, kernel page/import
contract and SweetSurface ownership. BGRA-only policy is useful; upstream
address-based display mutations are not a usable iOS 16 solution.

## Revised M9 sequence

1. **Run the existing isolated CA test next.** With these adapters and M9a's
   constant support, stage 0 determines the next actual missing request. Advance
   through stages 1–4 only after a zero-error, nonzero-draw, pixel-qualified
   stage. Keep SW system composition and both system processes denied. No full
   SW transaction-replay integration is needed to do this isolated experiment.
2. **Implement the consumed destination-read path when requested.** Prefer the
   runtime's ordered input-attachment design if the actual Turnip device enables
   that feature. Extend the backend device feature chain, bridge enable profile
   and cache identity together; add input-attachment usage/import validation,
   descriptor writes, subpass/blend flags and synchronization as one change.
   Otherwise port the interlock/storage design with role splitting and scratch
   ownership. Validate authored overlapping fragments within/across draws,
   load/store and CPU handoff before trying CA pixels. Keep unsupported tile,
   sRGB, MSAA and other runtime plans refused until consumed correctly.
3. **Qualify the observed working set and cold budget.** Collect compact request
   and timing counters from the isolated client, not tracked shader dumps.
   Add only requested queue scheduling/SPI, slots/formats, archive fallback and
   producer synchronization. If uber compilation dominates, qualify upstream's
   compiler optimizations in the external replaceable translator build with
   provenance and corresponding-source packaging. Then consider private cache
   persistence; priming and builtin caches are separate decisions.
4. **Scale an isolated scene before touching presentation.** Move from 256×256
   to display-size surfaces with row/plane/overflow/ownership checks and measure
   scenes with text, opacity, images, clipping and effects against CPU reference.
   Keep our IOMobileFramebuffer path. A native MTLDrawable is not required just
   to produce an IOSurface that this path can later consume.
5. **Design compositor admission separately, after evidence.** M8's mandatory
   full SW replay before any useful CA work was too broad. Upstream demonstrates
   a workable smaller rendering path; it does not prove runtime failure recovery.
   A future lane can choose explicit shadow/replay, or a supervised bounded
   canary with durable disable-before-restart and last-completed scanout, stating
   the interruption it permits. Test pipeline refusal, abort, timeout/device
   loss, stale completion and supervisor failure before system admission. A
   display-copy CPU fallback alone is insufficient. This lane ships neither
   choice and cannot admit backboardd/SpringBoard.

## Validation and exact next device test

Linux reproduction, with variables naming existing external inputs:

```sh
python3 tools/test-fuxi-metal-m9b.py --source "$M2V_SOURCE" \
  --build research/metal-m9b/bridge --headers "$VULKAN_HEADERS" \
  --spirv-val "$SPIRV_VAL"
python3 tools/test-fuxi-metal-frontend.py
bash tools/ci/build.sh --plan none true false false
bash tools/ci/build.sh metal --dry-run
```

The M9b test runs M9a's clean-pin bridge/58-constant/cache/SPIR-V/staging controls,
production CA bounds and cache failure/lifetime controls, and 20 actual Vulkan
pipeline pairs across cache retirement/device reopen using original synthetic
AIR and lavapipe. It issues no command buffers. Native frontend tests add actual
adapter snapshot, nil-unbind, bad range/offset, neutral-state and queue descriptor
cases; these require a Mac. Neither a native iOS build nor CI run is claimed.

Passed on Linux: M9b (including four Rust bridge tests and sanitizer controls),
existing constant-hook tests, NativeAdreno ABI/backend controls, 32 backend
fill/copy submissions with loss/timeout/corruption injection, M6 same-memory
image/transfer controls, M6b's hash-verified existing CI AIR sampled-quad pixels,
and M7's original AIR controls (all formats, blend pairs, indexed strips,
constants, load/store, same-memory pixels and refusal cases). The new Xcode
stage-6 MSL was not rebuilt or qualified. CI matrix plan and `metal --dry-run`,
Python/shell syntax and whitespace checks pass. Logs/products stay ignored.

After integration and a normal Mac `metal` build, use the matching bundle,
translator, ICD and both smoke/config files exactly as listed in M9a's staging
recipe. The bundle now also requires `LICENSE.NativeMetal` (0644) and its matching
signature. Bridge ABI remains 4. For the **first device boot**, set only
`ca-smoke.conf` to `0` and keep `metal-clients.conf` containing only
`fuxi-metal-smoke` and `fuxi-ca-smoke`. Use
`sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-ca-smoke` and include
`fuxi-ca-smoke` in the existing `sweetnull` list; preserve other boot arguments.
Do not exec-swap either compositor.

Require `gate deny backboardd`, `gate deny SpringBoard`, the 1024 MB jetsam call
succeeding, `gate allow fuxi-ca-smoke`, and
`FUXI CA: stage 0 solid PASS ... gpu_draws=...` followed by `done PASS`.
gpu_draws must be positive, frontend errors zero, and the existing comparison
limits must pass without tolerance changes. Record translation versus Vulkan
pipeline times and the first refusal if it fails. Only then repeat maximum
stages 1, 2, 3, 4 with the same gates and require each cumulative scene to pass.
Separately repeat `fuxi-metal-smoke` with max stage `6` for the preexisting GPU
regression. An alarm/refusal/abort is a failed diagnostic, never authorization
to publish a system Metal device.
