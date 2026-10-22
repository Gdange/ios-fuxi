# Metal M9o: shared linear texture shadows and lazy line pipelines

2026-10-23. Source and Linux lavapipe qualification only. No phone access,
deployment, CI dispatch or push. FuxiMetal remains the default lane over the
external metal2vulkan pin `720ef1ce9ae2a2068999a8675717418b9416283e`.
The authored bridge adapter must be rebuilt: ABI 8's layout is unchanged, but
its draw planner now accepts nonzero counts below three for lines. No external
translator, Mesa or NativeMetal sources, binaries or shaders are added.

## Survey evidence and allocation diagnostics

The supplied read-only excerpt reports `survey summary distinct=17 refusals=29`
and zero completed frames. Its A8/1 and RG8/30 shared buffer textures pass the
frontend and fail exact Vulkan linear binding; line-list draws (Metal type 1,
start 0, counts 64 through 598) fail frontend topology validation.

The former generic `linear texture Vulkan memory/layout mismatch` now appends
**every failed condition**, with actual and required values:

- `alignment offset=... required=...`
- `memory type buffer=... allowedBits=...`
- `size required=... available=...` (remaining Vulkan buffer allocation)
- `layout.offset Vulkan=... Metal=0`
- `rowPitch Vulkan=... Metal=...`

A successful fallback prints these reasons in an informational
`FUXI MTL: linear texture shadow format=... WIDTHxHEIGHT offset=... row=... reason=...`
line. It does not call the failure/survey path. Missing linear format features or
external-image alias support can also select the fallback; genuine Vulkan
allocation/device errors still follow the existing failure policy.

## Read-only shadow and coherence

An exactly compatible linear image continues to borrow the buffer allocation.
Otherwise, **only exact ShaderRead usage (0x1)** can allocate an ordinary
optimal-tiled sampled image. ShaderWrite descriptors remain refused; mismatched
render-target/combined usages do not get a fallback. Existing compatible linear
render-target aliases and IOSurface import contracts remain unchanged.
GPU uploads, texture-copy destinations, clears/render targets and mip generation
on a shadow are explicitly refused, so no image write can silently diverge from
its backing buffer.

The buffer offset must be texel aligned, the row must contain the width and be
a whole number of texels, and all used bytes must fit the logical Metal buffer.
Bounds use `(height - 1) * bytesPerRow + width * texelBytes`; padding after the
last row need not exist. The existing dimension, single-resource and aggregate
byte limits remain in force. Vulkan copies use the buffer's original offset,
`bufferRowLength = bytesPerRow / texelBytes`, and the full image extent. Vulkan
specifies row length in texels and only accesses the used row regions.
See [VkBufferImageCopy](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferImageCopy.html)
and [vkCmdCopyBufferToImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBufferToImage.html).

Before **each operation that reads a shadow**, outside any render pass, the
backend records `vkCmdCopyBufferToImage`. Repeated slots for one texture share a
refresh within that operation. Refreshing again before later operations also
observes earlier GPU buffer fills/copies in the command buffer. Host writes are
flushed with the existing coherent-memory policy and HOST-to-device barrier;
the image transitions to TRANSFER_DST and then to the sampled/read layout with
transfer-write visibility. Existing operation barriers order buffer writes and
later refreshes. Readback and source-image blits also refresh. There is no extra
queue submission or synchronous readback in the refresh path.

CPU writes to shared buffer contents **before commit are visible** to that
submission, including writes after texture creation or after a previous
completion. The CPU must not write the participating bytes during GPU execution.
This is the shared-storage coherence model for textures created from buffers;
there is no creation-time snapshot or persistent dirty-bit assumption.
`replaceRegion`/`getBytes` on buffer textures and their views now access the
actual shared CPU rows under the backend mutex and `requireLive`, preserving
partial regions, buffer offset and row padding.

A shadow's Vulkan allocation is charged separately from its buffer, through the
ordinary texture allocator and 256 MiB aggregate budget. Exact aliases add no
memory charge. Backend textures retain their backing Buffer as well as the
frontend texture retaining its buffer wrapper; views and command resources keep
that chain alive through completion. Destruction releases image/view caches,
owned image memory and the backing reference in order. Failed creation rolls
back its charge; pre-submit failure restores layout/defined-state bookkeeping.
Poison and admission/failure latches still refuse allocation/submission, and
uncertain GPU ownership preserves Vulkan allocations rather than destroying them.

## Topology and pipeline caching

Operation records now use a topology enum for triangle list, triangle strip,
line list and line strip. Frontend and backend admit lines, preserve bounded
starts/counts/index ranges, and remove a trailing line-list vertex/index before
translation, CPU fetch validation and drawing. Zero/one-vertex line requests
emit no primitive. Line strips need two vertices to emit a primitive and use
UINT16 primitive restart, as existing triangle strips do. Triangle count and
fetch checks remain intact. Points remain refused: their shader PointSize
contract needs separate qualification.

Only the existing two triangle pipelines are built up front. Each pipeline
retains a recipe whose SPIR-V/entry pointers belong to its retained translation
product and whose inline specialization/blend/vertex data is copied. First use
of LINE_LIST or LINE_STRIP lazily builds and caches that topology under the
backend mutex, after runtime shader variant selection and before recording the
render pass. Later draws reuse its handle. All variants use line width 1 and
the same descriptor/push layouts, specialization, vertex attributes, MRT/blend
state and render-pass compatibility. The recipe is included in object-cache
retained bytes; line pipeline handles are destroyed with their owning pipeline,
including runtime variants. Missing/null base pipelines still drop safely.

Initial triangle-pair build logs retain their existing count/format. Lazy line
build logs append internal `topology=2/3` for line list/line strip respectively,
only on first use in each translated pipeline variant. The unchanged ABI-8
`DrawInput.strip` field remains a restart flag at the adapter boundary; operation
validation and Vulkan topology selection use the enum, not that legacy field.

## Local validation

The runners compile with `-Wall -Wextra -Werror` and ASan/UBSan:

- `python3 tools/test-fuxi-metal-m9o.py`: production backend diagnostics for all
  five mismatch conditions; A8/RG8 shadow readback with nonzero offsets, padded
  rows, tight final rows, unchanged padding/buffer sentinels, repeated CPU and
  GPU buffer changes, pre-submit rollback, write/descriptor refusal, exact-alias
  ownership, separate shadow charging, budget exhaustion/rollback, and isolated
  device-loss poison/requireLive/retained-allocation controls. It also compares
  every texel for the five supplied survey dimensions/rows/tight byte counts.
- Add `--translator "$BRIDGE"` for the tree's hand-authored AIR fixture:
  actual two-vertex line and line-strip pixels, odd-count and unused trailing
  index handling, strip restart, empty draws, invalid bounds/topology, laziness
  and handle reuse, and actual A8 shadow sampling before/after CPU buffer writes.
- The backend fill/copy/cache-transition/reopen and injected timeout/loss/
  corruption controls pass. M6, M7, M9g, M9h, M9i, M9k, M9l, M9m and M9n
  regressions pass. These include
  triangle pixels, exact linear render aliases, imported surfaces, object/cache
  reuse and persistence, mip/1D sampling, MRT and ordered framebuffer fetch.
  M7's stale full-coverage DontCare refusal also fails against the unchanged
  baseline backend; it now checks the stored opaque pixels. Fetch from DontCare
  remains a refusal control.
- `python3 tools/test-fuxi-metal-frontend.py`: portable policy/gate/survey/startup,
  staging, constants, vertex and timeline controls pass. Native suites are
  explicitly skipped on Linux. TestBackend C++ syntax also passes with explicit
  `-Wmissing-field-initializers -Werror`.

All seven Rust adapter unit tests pass. The external checkout is clean at the unchanged pin; the rebuilt adapter matches
this tree. The host Rust toolchain uses the existing offline dependency cache
and `--ignore-rust-version` for that checkout's declared toolchain minimum.

`LinearFrontendTests.mm` is included in both native frontend runner modes. It
covers real selector dispatch, A8/RG8 descriptor queries, partial CPU writes and
reads through views, buffer retention after parent releases, recorded line/list/
strip/indexed topology/counts, ignored trailing vertices, empty draws and invalid
bounds. Concrete objects assigned to Metal protocol properties/arguments have
explicit casts. The maintainer must run the normal Mac harness and
`--compile-only` on current Xcode with warnings as errors; no Mac SDK compilation
or native execution is claimed here.

## Separately authorized next-boot plan

1. Run both Mac frontend modes and build the FuxiMetal bundle with the matching
   rebuilt ABI-8 bridge. Retain M9j's two-key/private-storage admission, M9m's
   compression setup and Turnip `sysmem,nocb` defaults. Default backboardd and
   every SpringBoard configuration must remain denied.
2. On a fresh boot UUID use survey mode first (`fuxibbgpu=2`). The A8/RG8
   `texture reject linearAllocation ... memory/layout mismatch` lines and
   `invalid draw bounds/topology type=1 ...` lines should disappear, along with
   their consequent selector/refused-command-buffer reports. Layout mismatch
   evidence should instead appear in successful `linear texture shadow ...`
   diagnostics; lazy line compilation should log `topology=2` (and 3 if used).
3. Require actual increasing stored-surface completions, positive GPU timing,
   and visually correct text, gradients, wallpaper, translucency and animation.
   Exercise repeated glyph/atlas buffer updates to detect stale shadow contents.
   Compare memory accounting over repeated allocations/releases and watch for
   budget failures, corruption, hangs or recurring pipeline compilation.
4. Observe at least 20 seconds of rendering. If no refusal occurs, there is no
   survey summary: the timer starts only on the first refusal. Do not predict
   `distinct=0`, or assume the prior 17/29 will simply subtract to a known total.
   XR/private formats and other previously documented limits remain unresolved
   if requested. Any genuine refusal must still collect the bounded survey and
   print `next-respawn=CPU`; verify CPU recovery and stop ordinary qualification.
5. Only after a zero-refusal survey, qualify normal admission on another fresh
   boot (`fuxibbgpu=1`). Expected progress is
   `backboardd first_frame_ms=... gpu_ms=...`, increasing frames with `failures=0`,
   and `backboardd gate good frames=120`. A controlled genuine fault must retain
   `backboardd gate runtime-failure next-respawn=CPU persisted=1` and the next
   respawn's strike denial with working CPU rendering. Device success remains
   unproven until the maintainer performs that run.
