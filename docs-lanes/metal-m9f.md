# Metal M9f: offline QuartzCore pipeline qualification

2026-10-22. Linux source, static analysis and offline Vulkan work. No device,
image staging, CI dispatch, push or native Mac test execution. **backboardd and
SpringBoard remain DENIED.** The supplied run 4 was M9d, before M9e; its two
`M6 stage subgroup=None flags=0 blob=true` refusals explain its CA abort, not a
new M9e device result.

## Cause and implementation

A stage's `constant_blob` is shader data, not an unsupported executable form.
The bridge rejected it after successfully compiling and planning the pipeline.
NativeMetal delegates this to the pinned external runtime: `render.rs` collects
per-stage blobs and `binding.rs` handles `WriteSource::ConstantBlob`, uploads
16-byte-padded data and binds that buffer. The validation consumer's
`candidate/platform/plan_resources.rs` independently follows the same contract.
These APIs were followed; no compiler/runtime/Mesa implementation was vendored.
The external pin remains `720ef1ce9ae2a2068999a8675717418b9416283e`.

**Bridge ABI 6 requires the frontend bundle and translator to be updated
together.** CompileOptions stays 40100 bytes. Descriptor is 64 bytes and carries
set, binding, exact Vulkan descriptor type/stages and an explicit source kind.
Translation is 2232 bytes; its two owned blob records expose bytes, length and
SHA-256 while its handle lives. The Product maps each plan ConstantBlob write to
its actual stage's data; missing, duplicate, oversized or mismatched bindings
refuse. Both uniform and storage descriptor types are honored, without guessing
binding numbers. Backend creates pipeline-owned host-visible buffers, zeros
padding, copies once, flushes before shader use and binds the planned range.
They are never exposed as mutable Metal buffers. Variants and cached products
own independent uploads; normal teardown destroys them, while uncertain GPU
ownership retains Vulkan allocations under the existing poison rule.

The process translation-cache identity includes the blob SHA-256 values in
addition to both library/function hashes, names, typed constants, descriptor,
enables and device/driver identity. Its byte accounting includes blob payloads.
There is still no persistent Apple-derived cache export. SHA-256 uses the same
cached sha2 dependency family already required by the external compiler; build,
source-archive and Linux bridge manifests include it.

Further fixes found by exercising real functions:

* Sixteen buffer bindings per stage and sixteen fragment textures/samplers now
  travel through frontend setters/ranges/offsets, immutable draw snapshots, ABI,
  runtime shape, variant keys and Backend descriptors. This includes CA's second
  image at texture slot 4 and auxiliary uniform slots. Vertex descriptor layouts
  remain four, attributes eight, per-vertex only; resource slots and vertex
  descriptor layout indices are separate limits.
* A planned neutral zero buffer is allocated, padded, flushed and retained until
  submission completion, including the unused generic chroma-matrix binding.
  It is not represented by a null Vulkan descriptor or a fabricated caller slot.
* Standalone sampled-image and sampler descriptor writes join combined sampling.
  All use the exact plan's slot, view format/swizzle/layout and sampler identity.
  Image writes and unsupported views still refuse.
* ConstexprSampled/Variant is consumed through the real planner and its identity
  address-mode maps. Immutable samplers use the plan's filters, mip mode, address
  modes, LODs and border color through a separate ABI-6 sampler query. Backend
  owns their VkSampler handles and installs them in the set layout. No runtime
  sampler is substituted. Optional anisotropy, comparison, reduction/swizzle and
  address modes requiring additional enables remain explicit refusals.
* Grouped draw/layout errors now name the requirement/count or binding/type,
  rather than reporting only `unsupported draw resources`. Dependency errors
  include the relevant stage/class/slot. Push bounds use subtraction to avoid
  wrapping an offset plus size.

## Static solid recipes

The read-only iOS 16.7 QuartzCore analysis is the same build/UUID as M9d.
`create_fragment_shader`, `get_pipeline_spec`, `get_config`, `needs_attachment`,
`dest_count` and `derive_vertex_spec` establish the following public-value
recipes. The constant dictionary independently identifies `sov=3`, `cpy=1` and
`non=54`; zero is CLEAR, not source-over. There was sufficient resolved local
analysis to follow these paths without another headless decompilation.

| Constant | Solid source-over | Solid NONE/copy-output alternative |
| --- | ---: | ---: |
| fc_generic | 0 specialized; 1 generic | same |
| fc_vertex_layout | 0 (Surface) | 0 |
| fc_framebuffer_fetch | 1 | 1 |
| fc_framebuffer_sample | 0 | 0 |
| fc_attachment_count | 1 | 1 |
| fc_has_attachment_0/1/2 | 0 (fetch path) | 0 |
| fc_texture_function | 0 (color) | 0 |
| fc_blend_function | 3 (source-over) | 54 (NONE) |
| fc_dest_index / fc_dest_count | 0 / 1 | 0 / 0 |
| fc_memoryless_index0/1 | 255 / 255 | 255 / 255 |
| All other declared constants | 0 | 0 |

This reconstructs solid recipes from static state rules; it is **not a captured
live tuple**, nor proof which blend simplification the first device draw will
select. Both alternatives are qualified. Generic vertex fc_base_color is zero
(the Surface half-color representation). Generic buffers use the metadata's
four-byte VertexShaderConfig and 52-byte FragmentShaderConfig, with layout,
image records and typed constant fields populated explicitly. The config's
has_vertex_color flag is true for the authored solid quad.

The specialized descriptor is exactly M9d's buffer 1, stride 48, Float4 at 0,
Float2 at 16 and 24, Half4 at 32. Generic vertex functions receive **no** vertex
descriptor and load buffer 1 manually. Both get an identity MVP in slot 2, one
BGRA8 attachment, no fixed-function blending, and an indexed six-index quad.
Solid input color is RGBA (1/2, 1/4, 1/8, 1). Every solid pixel must read back
BGRA (32,64,128,255). Zero/toggle/image recipes report draw completion and pixel
counts/hashes; arbitrary toggles do not claim the solid oracle.

## Reproducing the qualifier

Provide local inputs via arguments, never tracked fixtures:

```sh
python3 tools/test-fuxi-metal-ca-offline.py "$CA_LIBRARY" \
  --source "$M2V_SOURCE" --icd "$LAVAPIPE_SO" \
  --output research/metal-m9f/offline
```

The driver verifies the external revision and clean tracked source, builds our
actual cdylib and production Backend with `-Wall -Wextra -Werror`, and requires a
lavapipe CPU device. No translator CLI is substituted for our bridge. The helper
reads only function names/constant metadata. Each pair runs the two solid
recipes, an ordinary image, a two-image multiply recipe (including slot 4),
all-zero constants, and each declared constant independently set to one over
zero. For uchar3 this toggles its first packed component. `--pairs`,
`--pair-limit`, `--toggle-limit`, `--sweep none` and `--timeout` bound replay cost.
Each subprocess isolates compiler faults/timeouts. `--ca-pool` additionally uses
256 KiB buffers with nonzero binding offsets. It compiles an identical
cached pipeline, verifies independent equal blob uploads, destroys the first
pipeline, then draws with the cached product. This checks blob ownership after
producer retirement, not merely a translation-cache log message.

A metallib or a pipelinelib `functions` container/directory is accepted.
Embedded MTLB containers stay in ignored output. Cross-container pairs are
supported; for duplicate function names the largest constant declaration set
wins, first container on ties. Baked catalog entries are candidates, **not a
reconstruction of Apple's archive recipe selection**; the default metallib is
the authoritative input for the explicit specialization sweep. Missing pairs
are a qualification failure. Errors not on the bounded diagnostic allowlist
become an error kind plus hash, never an IR/source excerpt.

Only counts, pass/fail labels and hashes are printed or saved as results. Input
containers remain ignored; no AIR, SPIR-V, shader bytes, disassembly or derived
cache is exported by the qualifier. Mesa may maintain its own private driver
cache. The host runner uses `GALLIVM_PERF=nopt` and at most two lavapipe workers.
A host-only compile define allows a 120-second fence wait for generic shader
JIT; production retains its two-second fence ceiling. A host timeout is still a
failure, not permission to relax a device deadline.

## Offline results

Input default.metallib SHA-256:
`11c74cc42ce5137b73d44d427f75764f886656d4ddb374422d56b609ed2762d9`.
The solid fragment blob SHA-256 is
`ef99cfd192ee2fe43a68cef2af40c85c2c215759f491c1b3fa09ed0f794f9201`.
Solid pixel FNV-1a64 is `b75d410ec94a2325` (diagnostic only; cache identities use
SHA-256). The specialized solid blob is set 0 / binding 3 / descriptor type
7 (STORAGE_BUFFER), fragment stage 16, padded range 16 bytes. The lavapipe
shared-library SHA-256 is
`9d69cae2004b16572acca424741109be37c4ffc759c2461af69d75f7d1c6c139`.
Detailed final counts and remaining refusals follow below.

| Vertex / fragment | Solid oracle | Image / two-image / zero draws | Single-constant toggles |
| --- | --- | --- | --- |
| lph_spc / lph_cpf | 2/2 PASS, 65536 exact pixels each | 3/3 PASS | 54 PASS / 4 refused |
| lph_spc / lph_cph | 2/2 PASS, 65536 exact pixels each | 3/3 PASS | 54 PASS / 4 refused |
| lph_gen / lph_cpf | 2/2 PASS, 65536 exact pixels each | 3/3 PASS | 56 PASS / 3 refused |
| lph_gen / lph_cph | 2/2 PASS, 65536 exact pixels each | 3/3 PASS | 56 PASS / 3 refused |

Default-library total: **240/254 PASS; 14 compile refusals; no draw failures or
timeouts.** Every successful case uses a real pipeline and submission, including
cache reuse and first-pipeline destruction. The specialized five-recipe baseline
also passes 10/10 with 256 KiB pools and 256-byte offsets (`--ca-pool`), rather
than just compact vertex/uniform allocations.

The refused constant sets are completely specified by the sweep rule: all
declared constants zero, with only the named constant set to one in every stage
that declares it. These are not the static solid tuples above.

| Constant set | Pairs affected | Exact refusal / implication |
| --- | --- | --- |
| fc_generic=1 | Both spc pairs (2 cases) | InvalidDescriptor / VertexAttribute. Toggling generic without switching the vertex function/descriptor makes an inconsistent recipe. The actual gen pair and config are separately qualified. |
| fc_has_color_cube0=1 | All four pairs | Plan diagnostics count=1: a cube/3D LUT expects a view outside the bound 2D profile. |
| fc_has_color_cube1=1 | All four pairs | Same incompatible 2D-versus-LUT plan diagnostic. |
| fc_has_lut_tex=1 | All four pairs | Same incompatible LUT view diagnostic. |

For the last three, the diagnostic SHA-256 is
`e6cefa04f1afa09824ffba6893e941dcc0474f713d8e273eda2006a87f8c9fe8`
(the bounded line `M6 plan diagnostics count=1`). The generic-toggle diagnostic
hash is `627f7785bc18515db49b90f861baa138d547d6c3b25c63a9d5fa903ebe1c1864`.
No graphics state, subgroup, set flags, handle table, execution-status or color
write-table refusal is hidden behind the blob gate for these solid/image cases.
This is not an exhaustive Cartesian product of constants.
The specialized replay result digest is
`2791a68db123d5ce3280126a0e79e9504ddc8d26017f21254529022e70e88638`;
the generic replay result digest is
`61d1e498c4d01d5acfea2ab999f53afd27597a157efe926a05a7fce16f88e0ef`.

A separate pipelinelib-path control carves 451 containers and probes the first
selected baked spc/cpf pair with five recipes: 5/5 refuse at compiler linking
with UnmatchedVarying (diagnostic SHA-256
`d35edf7f4fc1c4a1d682a8d218c7a774bda8aadf4906753fa1ba8f755591be1b`).
This exposes the need for the archive's matching baked recipe, rather than
silently pairing unrelated specialized stages or claiming an archive success.
It does not invalidate the default-library pipeline/draw results.

Additional checks pass: six Rust unit/ABI/dependency controls; 58 typed
bool/uchar/uchar3 specialization and cache controls with SPIR-V validation;
client/staging denial controls; `-Werror` Linux frontend and M9b compile controls;
ASan/UBSan M6 sampling/import/transfer regressions; and authored M7 actual AIR
rendering, including overlapping ordered fetch within/across passes, exact
format/blend/vertex pixels and neutral-uniform readback. Shell/Python syntax,
whitespace checks and `metal --dry-run` pass. The formerly unrun M9d
half/multiple-buffer control also resets list topology/count explicitly: it
previously inherited strip state after restoring triangle-list indices, so its
pixel failure was a test setup defect. Pixel expectations are unchanged. Native Mac/iOS builds and native
Mac test execution remain unperformed.

## Refusal audit and scope

| Boundary | Consumer / reason for remaining refusal |
| --- | --- |
| Sets, flags, descriptor arrays | Fixed direct-slot pipelines use set 0, count 1, no update-after-bind flags. The descriptor budget remains 16 total bindings. Set 1 is the external handle heap, not CA's ordinary MetalBufferPool named “arg”; it remains refused. No binding/flag is silently discarded. |
| Ordinary/inline buffers | Exact planned ranges and stage/slot, sixteen slots; inline data remains limited to 4096 bytes and offset zero. Neutral read buffers have real zero-filled backing. Shader writes/sinks remain refused. |
| Push constants | Both stage ranges and every instantiated write, at most 128 bytes and two ranges. Address holes, root/argument/handle tables, amplification and dynamic vertex-stride writes remain precise errors. No zero-filled synthetic address/table is invented. |
| Constant blobs | Implemented for both stages with exact plan descriptors, padded immutable backing and hashed identities. |
| Handle tables | Need descriptor bytes, heap residency and possibly set 1/address support. None is required by the qualified fixed solid/image recipes; unsupported plans still refuse. |
| Color write table | External runtime uses a real read-only sRGB conversion table. BGRA8Unorm recipes emit none. sRGB attachments/sampling remain outside the existing format profile; no table requirement is ignored. |
| Execution status | Requires initialized shared status words, shader-write/host-read ordering and decoding after completion. Qualified direct-slot recipes emit none. A requested ExecutionStatus dependency/write remains refused, not a throwaway success buffer. |
| Sampler/image writes | Combined and separate read descriptors, immutable core samplers and identity-format 2D views are consumed. Cube/3D, mip/array/MSAA, compare samplers, neutral images, storage writes and unsupported format conversions remain bounded refusals. |
| Framebuffer fetch | M9e ordered color-0/input-0 plan, actual target view, GENERAL layout, attachment usage, both subpass/blend ordering flags and dependencies retained. Storage/interlock and unordered substitutes remain refused. |
| Stage/graphics state | Two stages, default subgroup/flags; one color, one sample, no depth/stencil/tessellation/layer/imageblock/mesh or resource writes. Unsupported state is named. These are observed constraints of the qualified recipes, not a claim that every fixed-function specialization meets them. |

The two-image recipe probes a likely mask/composition resource boundary; it is
not a static reconstruction or pixel qualification of the full rounded-clip
scene. Linux does not exercise Objective-C callback lifetimes, IOSurface's
A740 INPUT_ATTACHMENT import, actual CA command order, stencil/compute requests,
Turnip numerical behavior or device cold compilation time. Stage 0–4 device
qualification remains cumulative and must stop on the first refusal.

## Validation and exact next device test

Linux bridge unit/ABI/dependency tests, authored typed-constant/cache controls,
frontend SDK-independent compile controls and real Vulkan resource regressions
are recorded with the final results. Native Objective-C++ tests were **not**
run or compiled here. Resource-bound tests use valid Metal descriptor indices;
vertex descriptor layout bounds are unchanged. The `metal` CI selection,
iOS deployment target and framework dependencies are unchanged. No iOS/Mac
build success is claimed from a Linux compile check.

In a separate device operation, build the existing **metal** target normally.
Stage the matching signed FuxiMetal bundle and **ABI-6** translator plus its
corresponding source/notices with the established M9 image recipe. Keep the
existing NativeAdreno ABI-2 kernel/Turnip pair, allowlist, GPU import and display
path. First set only `ca-smoke.conf` to `0`; admit only the two existing isolated
smoke clients and use the established peakpowermanagerd exec-swap. Do not change
backboardd/SpringBoard admission or GPU-family answers.

Require both compositor DENY lines, isolated-client allow, jetsam setup,
ordered-fetch enable and same-memory BGRA IOSurface import PASS, then lph
function lookup, descriptor acceptance, translation PASS and Vulkan pipelines
PASS. `fetch mask=1 ordered=1` is required for source-over destination reads;
`mask=0 ordered=1` is valid for the pruned specialized NONE recipe. The blob
refusal must disappear. Stage 0 must report `solid PASS pixels=65536`, positive
gpu_draws, zero frontend errors and `done PASS`. Any ABI mismatch, plan refusal,
import failure, alarm, timeout or abort fails this diagnostic.

Only after stage 0 passes, repeat cumulative maxima 1 (opacity), 2 (rounded
clip), 3 (image), 4 (transform), with unchanged pixel tolerances/gates and a
positive GPU draw count at every stage. Preserve each first refusal and its
function/typed-constant identity in ignored diagnostics for offline replay.
Separately repeat the preexisting Metal smoke maximum stage 6. Linux pixels do
not authorize either compositor or predict the device's cold-compile deadline.
