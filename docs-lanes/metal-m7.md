# Metal M7: bounded layer renderer

2026-10-21. Source and Linux/lavapipe validation only. No device access,
deployment, workflow execution or push. The supplied **device observation** is
M6b stages 0–5 PASS on A740, including translation (276 ms cold / 0.3 ms cached)
and the IOSurface same-memory textured quad. This is the starting qualification,
not evidence that M7 ran on device. See [M6b](metal-m6b.md) and [M6](metal-m6.md).

The shipped `tools/metal-smoke.conf` is **5**. Stage 6 requires an explicit
configuration value of 6. The existing missing-file fallback remains stage 3.
The client allowlist and daemon restrictions are unchanged; this does not
enable QuartzCore/CoreAnimation in backboardd or SpringBoard.

## Pinned translator inventory and accepted subset

The external checkout was clean at
`720ef1ce9ae2a2068999a8675717418b9416283e`; the build recipe retains that pin.
Inventory used that checkout's public API and implementation, particularly
[`pipeline`](https://github.com/steelbrain/metal2vulkan/tree/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan/src/pipeline),
[`bind`](https://github.com/steelbrain/metal2vulkan/tree/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan/src/bind),
and [`metal/format`](https://github.com/steelbrain/metal2vulkan/tree/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan/src/metal/format).
Translator capability alone does not qualify Fuxi's consumer or Turnip.

| Requested family | What the pin provides | M7 works / refuses |
| --- | --- | --- |
| Blending, write masks | Metal factors/operations/masks in `ColorAttachmentDescriptor`; linked Vulkan blend state in `GraphicsState` | Source-over premultiplied `(one, one-minus-source-alpha)`, fixed-function factors zero/one, source/destination color/alpha and their complements; add/subtract/reverse/min/max when the linked plan fits. Linux pixel tests cover source-over, copy, plus, destination-over, source/destination in/out/atop and a Red-only mask. One attachment only. Constant/dual-source factors, framebuffer fetch and artistic CA modes needing destination reads remain refused. Other accepted factor/operation combinations are not pixel-qualified here. |
| Vertex descriptors / indices / strips | `VertexDescriptor`, linked `vertex_input`, planned `vertex_buffers`; indexed/primitives `DrawCall`, instantiated buffer offsets/draw command | Attributes 0–7: Float2, Float4, UChar4Normalized. Buffers 0–3, fixed per-vertex strides 1–256, offsets at most 252. Uint16 indices, triangle lists/strips, up to 65,536 vertices/indices, one instance. CPU checks every fixed-function fetch/index before submission. GPU-written draw buffers require a separate completed submission. Instancing, base vertex/instance, UInt32, dynamic strides and other topologies/formats refuse. |
| Function constants | `FunctionDescriptor.constants`, values by index/name, upstream AIR specialization; ordinary binding-plan Vulkan specialization | Immutable snapshots of observed public `MTLFunctionConstantValues` setter calls, at most 32 scalar bool/char/uchar/half/short/ushort/float/int/uint values per stage. Upstream specializes the AIR; Fuxi does not invent function-constant SpecIds. Plan specialization entries/data still reach Vulkan. True/false index/name controls change pixels. Unobserved objects, overridden setters, unsupported types/options, missing required or mistyped constants refuse. |
| Pixel formats | Device-aware format mappings, swizzles, variants and shader conversions; some require extra resources/features | BGRA8Unorm, RGBA8Unorm, RGBA16Float, R8Unorm, RG8Unorm sample/render roundtrips pass on lavapipe. A8Unorm samples through the planned R8 `(0,0,0,R)` view; A8 attachments refuse. BGRA8Unorm_sRGB is explicitly deferred/refused: sRGB probes reached unsupported planned descriptor sources. No substitute descriptors are synthesized. Format/usage and blend features must be available at runtime; Turnip remains unvalidated. IOSurface import stays BGRA8 only. |
| Multiple resources / bytes | Stage tables, descriptor writes, buffer ranges, residual-offset push bytes, combined image/sampler views | Four fragment textures/samplers (0–3), vertex/fragment buffers (0–3), shared buffers up to 16 MiB; copied `setVertexBytes`/`setFragmentBytes` up to 4096 bytes. Four independently bound images/samplers, offset-16 uniforms and missing slot-3 refusals pass. Argument buffers, writable/synthetic/neutral resources, vertex textures, separate image/sampler descriptors and slots outside these bounds refuse. |
| Viewport/scissor / actions / passes | Graphics dynamic-state requirements; draw plans are independent of encoder render-pass scheduling | Finite contained 256-square viewport/scissor; clear/load/dontCare load and store/dontCare store; multiple draws/passes per command buffer. Clear+load, inset viewport/scissor and discard refusals pass. DontCare load/store conservatively makes content undefined; later load/sample/readback refuses until a clear/upload defines it. Empty passes, resolve/MSAA, depth/stencil and additional attachments refuse. |

All textures remain 256×256, 2D, one mip/layer/sample. Private texture allocation
checks format usage features; imported linear targets additionally need linear
attachment blend support. Unsupported states return nil/error or reject the
whole command before submission. Precision notes from the translator remain
visible; these tests do not establish all Metal rounding, NaN or HDR semantics.

## Bridge, frontend and backend

The separately loaded adapter is now **ABI 3**. Compile options and function
constant values participate in the process cache key. The linked plan supplies
vertex formats/bindings/strides, blend state, descriptor layouts and both stage
push ranges. Before every draw, the bridge calls `plan` and `instantiate_draw`
with actual buffer lengths/offsets, texture formats and samplers. Their outputs
supply descriptor sources/views/ranges, push bytes, vertex offsets and the
indexed/non-indexed Vulkan draw command. The backend pads buffer allocations to
16 bytes as required by the planned descriptor-range contract and validates
those ranges against that allocation. It never fabricates ColorWriteRule or
residual-offset words.

Runtime format/buffer-form changes can select a newly compiled translator
variant or specialization; at most eight variants are retained per pipeline.
The process compile cache retains at most eight entries / 16 MiB of SPIR-V.
Unsupported dependencies, resource writes, holes, generated tables, constant
blobs, descriptor arrays or other unconsumed commands refuse. Pre-submit errors
restore image layout/defined state; uncertain submissions retain the existing
poisoned-ownership behavior and two-second fence bound.

Metal exposes setters but no public constant-value enumeration. In an allowed
client, the plugin observes public init/copy/reset/index/name/range setters
using Objective-C method replacements and associated snapshots. It does not
inspect Apple's ivars. Objects created before observation and unobserved
subclass overrides fail closed. Setter hooks are not installed for denied or
census clients. This runtime integration requires Apple-host/device validation;
the Linux translator tests bypass it and supply the same ABI records directly.

Each recorded draw retains its pipeline, textures, samplers, index buffer and
stage buffers. Byte setters copy immediately. Multiple draws preserve the
encoder's load/store semantics by using internal compatible render passes;
only the last draw applies the requested store action. The conservative
undefined-content rule deliberately does not infer full coverage after a
dontCare load.

## Stage 6 and CI

Stage 6 imports two original BGRA8 layer IOSurfaces and a target IOSurface.
The first pass clears and draws an opaque asymmetric quadrant layer; the second
loads and draws a premultiplied layer with half tint inside an inset scissor.
It uses Float2 position/UV, normalized uchar4 color, a vertex buffer, Uint16
index buffer, a boolean function constant, vertex bytes and fragment bytes.
The caller overwrites the temporary byte arrays after setting them to expose
missing copies. Pipeline creation is repeated to exercise the compile cache.

All 65,536 pixels are compared byte-for-byte with an independent integer CPU
source-over reference. Untouched image edges and readback prefix/suffix bytes
are sentinels. After imported textures/encoder references are released, the
target IOSurface is locked and its rows must equal readback from the same memory.

Original `fuxi_layer_vertex`, `fuxi_layer_fragment` and four-slot control MSL
functions were added to `tools/shaders/metal-smoke.metal`. The existing CI
`metal` target invokes `build-fuxi-metal.sh`, which already compiles this whole
file with Xcode Metal and links the existing `metal-smoke.metallib` artifact.
No new artifact names or Apple build inputs are checked in. The new MSL has
**not** been compiled here; Linux's hand-authored AIR is a translator-side
control, not a substitute for that CI build.

## Linux validation

`tools/test-fuxi-metal-m7.py` takes external `--source`, `--build`, `--metallib`,
`--sums`, `--headers`, optional `--icd` and `--llvm-as` arguments. It first
verifies the clean external revision and the CI metallib hash, builds/tests our
adapter, and runs the existing M6b contract regression. It then assembles the
original `metal-m7-control.ll` and runs the actual translator/backend under
ASan/UBSan with lavapipe. Add `--ci-m7` after supplying the newly CI-built
metallib and matching sums to test its new functions too.

Verified existing CI metallib SHA-256:
`22197e0949eb365a5024391266af332e529507facde575ecd4381df5f9f84683`.
The metallib/manifest remain ignored; no Apple binary, metallib, generated
SPIR-V, translator source or Mesa source is committed.

Passed:

- Rust dependency scoping, one-line errors, inline-offset/size and sRGB gates.
- M6b actual CI AIR, cache hit, descriptor set/binding and push64:4=0x2222
  interception on every draw; nearest/linear, same-memory source/target,
  65,536 pixels and sentinels.
- M7 two-layer integer oracle, cache, float2/float4/uchar4normalized inputs,
  nonzero vertex/index offsets, indexed lists/strips, function constants,
  inline/ordinary offset uniforms, four independent textures/samplers,
  direct formats and A8 swizzle, mask/viewport/scissor and Porter-Duff controls.
- Missing resources, out-of-range vertex/index inputs (including overflow),
  feedback, GPU-written index input, undefined/discarded reads, unsupported
  constants/dual-source/A8 rendering/sRGB refuse without poisoning valid state.
- Existing backend fill/copy/cache/fence fault controls; M6 transfer/import/GLSL
  sampling controls; CI AIR triangle/cache/reopen and SPIR-V Vulkan 1.2
  validation with timeout/device-loss/corruption fault controls; client policy.
- Python syntax, shell syntax and whitespace checks.

Mac Objective-C tests were updated for the new gates and recording behavior
but were **not run**. Native iOS compilation, MSL compilation, signing, staging
of real signed artifacts, A740/Turnip execution and IOSurface coherence for
M7 are unvalidated. General CA shader compatibility, cross-process producers,
shared events/fences, drawables/presentation and daemon enablement remain out
of scope. This is a bounded offscreen profile, not CA qualification.

## Redeploy and expected output

After integration into main, build through the existing CI `metal` target.
Redeploy the complete newly signed **FuxiMetal.bundle**, matching **ABI-3
libmetal2vulkan.dylib** and corresponding-source archive, the rebuilt
**fuxi-metal-smoke** and **metal-smoke.metallib**, and the smoke configuration.
ABI-2/3 components cannot be mixed. This increment requires no new kernel/ICD.
Retain stage 5 for existing deployments; explicitly select 6 for M7 testing.
No deployment was performed here.

Expected stage-6 additions after stages 0–5 pass (not observed on A740):

```text
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: translation cache MISS
FUXI MTL: translation PASS ...
FUXI MTL: translation cache HIT
FUXI MTL: translation PASS ...
FUXI MTL: stage 6 layers PASS IOSurface BGRA8 source-over + vertex/index buffers + function constant + fragment bytes + two passes + scissor 65536 pixels + same-memory target + sentinel edges
FUXI MTL: done PASS
```

Follow-up: compile/run the CI M7 AIR with `--ci-m7`, exercise Apple constant
hooks and recorded bytes on the native host/device, qualify the stage-6 line
on A740, then implement the missing sRGB plan resources before admitting that
format. Broader CA blend shaders, resource layouts and synchronization need
separate qualification.
