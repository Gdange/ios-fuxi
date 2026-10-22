# Metal M9g: display-size isolated CA composition

2026-10-22. Source, tests and compile/static checks only. No test-executable
runs, GPU submissions, device operations, deployment, workflow dispatch or
push. The supplied passing isolated smoke established stages 0–4 on A740
through FuxiMetal → metal2vulkan → Turnip. This change has **no new device
result**. backboardd and SpringBoard remain **DENIED**, including census and
explicit allow entries; the only admitted CA client remains fuxi-ca-smoke.

## Starting evidence

Read M9b–f and every commit after M9f (`b117955` through `fe67332`). Those
follow-ups fixed CA's linear-alignment queries, shadowed unimportable no-copy
buffers, flushed layer commits, established top-left reference geometry and
bottom-up CARenderer image contents, and added the one-pixel edge comparison.
The supplied passing diagnostics also expose repeated identical descriptors:
translation reuse still leaves expensive Vulkan pipeline construction. The
existing VkPipelineCache handle is a driver compilation hint, not an object
identity cache. Device traces and Apple-derived products remain ignored.

## Shared pipeline objects

Backend now owns a mutex-protected access-order LRU. Independent Objective-C
pipeline wrappers retain the same intrusive-refcounted backend object; labels
remain independently copied on each wrapper. A hit takes one ref, updates LRU
order and skips translation, blob uploads, layout/render-pass creation and both
list/strip pipeline builds. Commands keep their existing wrapper/resource
retains until synchronous completion. Eviction drops only the cache's ref;
active wrappers and commands survive it. Teardown drops cache refs before the
VkDevice. Existing poison handling retains Vulkan allocations on uncertain
GPU ownership.

Each entry has an input SHA-256 alias and a compound input/product SHA-256 key.
Input identity includes both library content hashes, length-delimited function
names, every accepted attachment format/blend factor/operation/write mask,
vertex attribute/stride, typed function-constant payload and the actual ordered
fetch enable. One sample, one color, no depth/stencil, per-vertex stepping and
the remaining invariant render state are validated before lookup; the key has
an explicit single-sample schema marker. Invalid sample counts never enter the
cache. Device/driver identity is scoped by the owning VkDevice, with no shared
entries between devices or persisted data. No pointer-valued library identity
or object label is a key.

The product digest also includes both translated SPIR-V streams/entry names,
the variant, descriptors, complete layout including immutable samplers, vertex
bindings/attributes, specialization map/data, blob SHA-256 values, fetch plan
and render state. The alias avoids even the 1–6 ms translation-cache path for
identical requests. The compound product key prevents sharing a runtime plan
across different source identities. No shader bytes or identities are printed.

Retention is at most 32 baseline entries and 16 MiB of accounted shader words,
blob payloads and Pipeline structures. Each baseline owns at most eight runtime
variants. Variant growth reweighs its LRU entry and evicts until within budget;
shared baseline wrappers reuse these variants too. Opaque driver allocations
and the external compiler's retained graph are not measurable by this byte
counter. The finite entry/variant count bounds retained objects (at most 288
pipeline pairs); externally retained live objects and transient compiler memory
are outside the LRU retention bound. Constant-upload allocations are separately
charged to the resource budget. First builds continue using the existing
per-device VkPipelineCache and its eight-pair/16 MiB generation policy.

Successful descriptor requests log exactly one of:

```text
FUXI MTL: pipeline cache MISS <ms> ms
FUXI MTL: pipeline cache HIT <ms> ms
```

A HIT must have no new `Vulkan pipelines PASS` line for that request. MISS
includes translation and driver construction; HIT measures identity lookup and
ref acquisition. First-use compilation remains potentially expensive.

The C layout stays **ABI 6**, with two newly required symbols:
`fuxi_m2v_identity` and `fuxi_m2v_product_hash`. An older ABI-6 translator lacking
these symbols fails discovery at symbol loading. Stage the matching new bundle
and translator together. The external translator pin remains
`720ef1ce9ae2a2068999a8675717418b9416283e`; no translator or Mesa implementation
is vendored or modified.

## Dimensions, memory and validation

The effective 2D dimension is `min(Vulkan maxImageDimension2D, 4096)`. Actual
allocation/import uses that bound; device selectors report it (zero in a
non-rendering census device). Formats, usage, single-plane BGRA imports,
identity views, mip-0/slice-0, single sample and existing feature refusals stay
bounded. IOSurface dimensions must equal the descriptor. Viewport and scissor
are checked against the bound target in both frontend and Backend, using
subtraction rather than unchecked endpoint sums. Default state uses that
actual target size.

Every Vulkan buffer/image allocation or host-memory import reserves its actual
allocation size against a **256 MiB aggregate resource budget**, with a
**128 MiB individual allocation limit**. Buffers are allowed up to 128 MiB so
large float textures can use the same checked transfer path. A buffer-backed
linear image borrows already charged backing memory and adds no duplicate
charge. Failed allocations and ordinary destruction return their charges.
Import/linear paths validate dimensions before multiplication, full row span,
row divisibility/minimum pitch, offset subtraction, allocation extent, Vulkan
memory type/alignment, subresource offset, exact Vulkan row pitch and import
capabilities. An IOSurface/Vulkan layout mismatch is a failure, never a copy
fallback or a guessed stride. Resource budget admission does not bound driver
scratch, shader graphs or Objective-C/CG allocations.

1080×2400 BGRA pixels are 10,368,000 bytes (9.89 MiB) tightly packed. A 4352-byte
row uses 10,444,800 bytes before page rounding. One target, a full-size
intermediate and a transfer buffer therefore cost about 30 MiB; each extra
full-size BGRA intermediate adds about 10 MiB, and RGBA16Float about 20 MiB.
At contentsScale 3 a rasterized logical backing can cost nine times its area;
CA's intermediate choices and the three rounded clips remain material risks.
The 256 MiB admission limit provides room for several intermediates while
refusing runaway residency. It does not promise the scene will fit every CA
allocation choice. The smoke retains the preexisting **1024 MB** jetsam setup;
there is no further jetsam raise. Kernel import/driver scratch limits can still
refuse an otherwise valid frontend descriptor.

## Stage 5 and performance oracle

`ca-smoke.conf` now selects maximum 5; parser and staging accept 0–5. Maxima
0–4 keep their existing cumulative scenes, retries, pixel thresholds and
65536-pixel PASS lines. Maximum 5 first requires all of those to pass, then:

* Imports one 1080×2400 IOSurface, taking its real padded row from IOSurface.
* Uses a full-size root with contentsScale 3 and 30 total layers: 24 colored
  panels, three rounded/masked overflow children, two pictures and the root.
  Panels include opacity; pictures are 512×320 and 128×128, with a rotation.
* Renders exactly 60 consecutive frames on one CARenderer/target, updating one
  panel's position each frame with implicit actions disabled and a flush.
  Unlike old-stage no-draw diagnostics, a missing draw on any display frame
  fails immediately. Each frame keeps the existing 60-second watchdog ceiling.
* Checks the last position (frame 59) against a separately constructed CPU tree.
  CPU geometry is top-left; image rows are reversed for the established
  CARenderer bottom-up contents convention. CPU ownership follows renderer and
  imported-texture teardown, then an IOSurface read lock.

`fuxiCACompareSize` extends the same 3×3 reference-neighborhood comparison to
checked arbitrary dimensions and padded GPU rows. `fuxiCACompare` and
`fuxiCAAccept` preserve the 256 API exactly. For display size, the max error
stays 64, the exact-position mean stays at most 1, and the count of pixels with
neighborhood error above 8 is limited to the same density, 1/256 (10125 of
2592000 pixels). This does not tolerate a frame flip or systematic image swap.

Production Vulkan TOP/BOTTOM timestamp queries are read only after successful
fence completion. Period conversion and the queue's valid-bit wrap mask produce
process counters for total GPU nanoseconds and successful timestamp samples.
A display frame sums all submissions, including CA offscreen passes and the
queue completion fence; GPU time excludes CPU encoding, cold compilation and
queue/host wait overhead. Unsupported timestamps fail the display diagnostic,
not silently fall back to CPU timing. Existing Metal timing selectors are not
repurposed. Query resources follow normal cleanup/poison ownership.

All 60 per-frame GPU/wall samples are printed after rendering to avoid console
transport in the paced loop. Min/median/max GPU times include the cold frame.
Frame 0's cold wall latency is printed; frames 1–59 are paced at 60 Hz, and
**every warm frame must finish layer mutation, flush, rendering and synchronous
completion and per-frame resource retirement within 16.667 ms**, also meeting
the absolute paced completion deadline. Late warm frames fail even if pixels match.
The final CPU reference and readback are outside the frame timing window.
Cache HIT/MISS deltas cover stage 5; at least one HIT, positive draws on every
frame and zero frontend failures are required. These are isolated offscreen
composition measurements, not physical scanout/vsync or system-compositor
qualification. The current synchronous queue and per-draw Vulkan descriptor
pools/framebuffers may remain bottlenecks after compilation reuse.

## Tests and checks

Completed here without executing test binaries or submitting GPU work:

* `python3 tools/test-fuxi-metal-frontend.py --compile-only`: Linux constant,
  vertex, timeline and new resource/LRU/large-compare controls compile with
  `-Wall -Wextra -Werror` and ASan/UBSan. Native Mac suites are skipped.
* `test-fuxi-metal-m9g.py --compile-only` builds the production Backend with
  warnings as errors and ASan/UBSan, and assembles the existing authored M7 AIR.
* M9b compile-only controls; M7 and offline CA C++ translation units compile
  with warnings as errors and sanitizer instrumentation.
* `cargo +1.98.0 check --offline --tests` checks our bridge against the clean,
  pinned external compiler. ABI layouts remain unchanged.
* Modified Python syntax, build-shell syntax, `git diff --check`, and
  `bash tools/ci/build.sh metal --dry-run`. The existing metal build recipe,
  CI target, deployment target and framework set are unchanged.

New native Mac CAFrontendTests exercise real wrappers over the CPU model:
1080×2400 descriptors, target-relative viewport/scissor acceptance/refusal,
4097-width and sample-count refusal, separately labeled wrappers sharing one
backend refcount, survival after wrapper release and changed write-mask miss.
The portable production LRU tests cover access-order and byte eviction,
oversized bypass and lifetime after cache retirement. Test descriptors use
valid Metal enum ordinals and indices; no out-of-range descriptor-array value
is installed that would assert inside Mac Metal.

The new Linux runner accepts `--translator`, `--icd`, `--headers`, `--llvm-as`
and `--compile-only`. With the matching separately built bridge and lavapipe,
run it without `--compile-only` to check actual pipeline identity, render-state
and typed-constant hash differences, producer-ref retirement and actual LRU eviction survival, 2592000 sampled
pixels clipped by a large scissor, sentinel rows, target-bound refusals, large
padded linear targets, same-memory imports and GPU timestamps. Its fixtures are
entirely authored AIR. The old offline blob test now expects shared immutable
uploads with independent caller refs, and M9b/M7 cache expectations reflect
object hits. **All executable assertions remain unrun in this lane.** Native
Mac/iOS compilation is also unperformed; Linux compilation is no substitute.

## Exact next device test (separate authorized operation)

1. Build the existing **metal** CI target normally; this lane dispatches no
   workflow. Stage the matching signed bundle, new ABI-6 translator, source
   archive/notices and both smoke executables/configs using the established
   M9 recipe and unchanged NativeAdreno ABI-2 kernel/Turnip pair.
2. Keep the client file restricted to the existing two smoke names. Keep
   backboardd and SpringBoard denied. Use only the established
   `sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-ca-smoke` diagnostic and
   corresponding sweetnull entry; preserve the other boot arguments.
3. First run cumulative maximum **4** to reconfirm the known five-stage oracle
   with the new cache/limits. Then select **5** in the staged CA config and run
   the same isolated smoke. Maximum 5 is also the new tracked default. Record
   complete diagnostics privately; stop at the first refusal, abort or timeout.
4. Separately repeat the existing Metal smoke maximum **6** regression. Run
   native Mac frontend tests and the Linux M9g lavapipe runner before treating
   their compiled assertions as passed.

Expected device lines (numbers and request ordering vary):

```text
FUXI MTL: gate deny backboardd
FUXI MTL: gate deny SpringBoard
FUXI CA: jetsam ... set=1024MB rc=0 ...
FUXI MTL: gate allow fuxi-ca-smoke
FUXI MTL: texture limits dimension=4096 budget=256 MiB
FUXI MTL: ordered framebuffer fetch enabled (input attachments)
FUXI MTL: pipeline cache MISS <ms> ms
FUXI MTL: pipeline cache HIT <ms> ms
FUXI CA: stage 0 solid PASS pixels=65536 ...
FUXI CA: stage 1 opacity PASS pixels=65536 ...
FUXI CA: stage 2 rounded-clip PASS pixels=65536 ...
FUXI CA: stage 3 image PASS pixels=65536 ...
FUXI CA: stage 4 transform PASS pixels=65536 ...
FUXI MTL: IOSurface import PASS BGRA8 1080x2400 same-memory
FUXI CA: display frame=0 gpu_ms=... wall_ms=...
FUXI CA: display frame=59 gpu_ms=... wall_ms=...
FUXI CA: display GPU min/median/max=... ms cold=... ms frames=60 cache_hits=... cache_misses=...
FUXI CA: display warm wall min/median/max=... ms late=0/59 target_hz=60
FUXI CA: stage 5 display PASS pixels=2592000 frames=60 ... gpu_draws=...
FUXI CA: done PASS
```

Require all 60 frame records, finite positive GPU statistics, positive cache
hits, no new driver pipeline creation on an identical descriptor HIT, zero
frontend errors, and every old/new pixel oracle. A pixel success with late
frames is a performance failure. Large import layout, budget, timestamp,
watchdog or plan failures remain genuine failures; do not relax the oracle,
process admission or kernel containment to obtain a PASS. No Apple binary,
metallib, extracted shader, IR, disassembly, device dump, local workspace
identity or persistent pipeline cache is shipped by this change.
