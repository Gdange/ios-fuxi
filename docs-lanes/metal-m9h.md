# Metal M9h: display-rate CPU cost

2026-10-22. Source, tests, documentation and compile/static checks only. No
executable tests, GPU submissions, device access, deployment, workflow runs or
push. No new device performance or pixel result is claimed. backboardd and
SpringBoard remain **DENIED**; client admission/gating is unchanged. No kernel,
Mesa or external translator source changes are required by this work.

## Evidence and scope

Read M9g and all five subsequent commits: `a06a153` (render area), `d7a0b2e`
(stored DontCare-load draws define their targets), `f3ce834` (failing Vulkan
line and live memory objects), `a4ea716` (one descriptor pool per submit),
`64f2a0c` (submit CPU breakdown). These behaviors remain in place.

The supplied private stage-5 baseline has 60/60 correct 1080×2400 frames, pixel
PASS, GPU 3.58 ms/frame, warm wall median 18.4 ms (9.8–23.6). Warm per-frame
means: submits 2.00, prepare 6.780, wait 6.174, retire 2.642, other 1.358 ms.
CA itself is about 1.4 ms. These are supplied observations, not measurements
from this commit. Live VkDeviceMemory counts do not include driver-owned BOs
for pools, query storage or command streams. Recreating those objects can hit
the NativeAdreno per-client 64-BO quota even with modest explicit allocations.

The target remains **every warm frame below 16.667 ms**, with headroom, the
same pixels, positive draws/timestamps and zero frontend errors. Resource
reuse and eliminating the smoke's empty completion submission should reduce
prepare, retirement and duplicated kernel handoff costs. The actual gain is
unmeasured; the next device run decides whether this meets the target.

## Synchronous reuse and lifetime

Backend retains one command pool and primary command buffer, one fence, one
2-slot timestamp query pool and one descriptor pool. The existing backend
mutex serializes creation, recording, submission, completion and resource
retirement. There is no additional in-flight work. A subsequent nonempty
submit resets the command pool and fence only after the previous fence
succeeded. Timestamp queries reset in the recorded command buffer before the
TOP/BOTTOM writes; result conversion and successful-sample accounting remain
unchanged. Reset uses flags 0, allowing command-stream backing to be reused.

Descriptor demand is counted across the whole submit before descriptor
allocation. Sets and each descriptor type grow to powers of two, retaining
previous type capacities. Admission is bounded to 4096 sets and 65536
descriptors per type. Reset frees the previous sets while retaining pool
storage. Growth destroys the quiescent old pool before creating its replacement
to avoid a temporary second pool BO. Failure during growth clears all submit
handles and capacities. Descriptor writes still happen for each draw; descriptor
sets are never reused across resets. Neutral scratch uploads remain per submit.

Framebuffer identity is `(backend texture, VkRenderPass)`; the render pass
encodes format/load/store, and texture dimensions/view are immutable. Sampled
view identity is `(backend texture, VkFormat, four component swizzles)` with
the supported fixed mip-0/slice-0 range. Each cache retains at most 128 entries.
When full, new objects belong to the current submit and are destroyed after
its fence, so an object already referenced by a recording is never evicted.
Texture destruction removes all its cached framebuffers/views before its main
view/image/memory. Render-pass destruction removes matching framebuffers,
including runtime variants and pipeline-cache eviction. Keys cannot survive
texture or render-pass handle reuse. Cache entries do not retain texture or
pipeline ownership; the existing command/wrapper retains still provide it.

Pre-submit failure restores saved layout/defined state and destroys the
command pool/buffer, fence, query pool and descriptor pool. The next submit
recreates them, avoiding reuse of partly recorded/reset state. Texture state
snapshots use one record per texture so allocation failure cannot split layout
and defined snapshots. Valid cached framebuffers/views may survive this
rollback. A failure after fence success keeps the committed texture state but
also discards submit objects. Any failure after attempting QueueSubmit,
including timeout or a throwing scheduled callback, retains the existing
poison policy: no reset, teardown or further submission with uncertain GPU
ownership. Process death/KMD containment owns reclamation. Ordinary close
clears submit objects and caches before pipelines and VkDevice.

An empty operation vector takes the same mutex and live-device check, invokes
the internal scheduled callback once and returns without allocating, recording,
querying or submitting Vulkan work. GPU samples and actual-submit counters do
not increase. Thus the smoke's `finish()` command still completes after earlier
synchronous work, with frontend status, scheduled/completed handlers, completion
queue dispatch and event signal/wait ordering intact. Event-only commands keep
the existing frontend event loop. Handlers still run outside the backend mutex
and can create resources or commit another command. Empty work on a poisoned
device still fails.

## Import cache investigation and safe userspace change

The checked-in kernel code establishes the expensive behavior directly:

* `xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp`, `importHost`: pins the full
  `NativeAdrenoAbiBoImport.bytes`, prepares a persistent IOMemoryDescriptor and
  wires a DefaultCache kernel alias. BoMap passes that alias and full `b->bytes`
  to `fuxi_gpu_abi_map_import`.
* `xnu/fuxi-adreno/fuxi_gpu_abi_backend.inc`, `fuxi_gpu_abi_map_import`: records
  source, full bytes and IOVA in the 64-entry mapped-import table. Removal
  occurs on GPU unmap, not per submit. BO permissions are not CPU-dirty flags.
* `fg_abi_import_cache(0)`, called before every client doorbell: barriers around
  a 64-byte loop over **every mapped import**, using `dc civac` (clean and
  invalidate). It does not consult IB references, CPU writes, dirty pages or
  draw rectangles. Indirect PM4 references explain the conservative coverage.
* `fg_abi_observe`, before fence success: calls `fg_abi_import_cache(1)`, which
  loops the full table with `dc ivac`. The handoff flag avoids duplicate
  retirement invalidations, not the next submit's handoff. Drained fault
  isolation also performs the retirement handoff.

Consequently the entire imported display allocation (padded rows and page
rounding included, roughly 10 MB) is maintained for **each submit**, even an
empty command, even if the host never wrote/read that frame. It is not just a
Vulkan FlushMappedMemoryRanges cost, and cache maintenance can be charged to
QueueSubmit/wait by the synchronous driver/kernel path. This establishes
coverage, not a measured duration for each kernel loop.

Userspace now omits Vulkan flush/invalidate calls only when the selected memory
type advertises HOST_COHERENT. Buffers, imported textures and borrowed linear
textures preserve that memory-type identity. Noncoherent memory still gets
whole-allocation calls because shared pointers, IOSurface producers and aliasing
provide no reliable CPU dirty-range tracking. Host/GPU barriers remain. This
omission follows Vulkan's coherence contract and does **not** disable the
kernel's physical cache maintenance. The inspected external Turnip functions
`tu_FlushMappedMemoryRanges` / `tu_InvalidateMappedMemoryRanges` route through
`sync_cache`; no Mesa code is copied or changed. Empty-submit elimination is
the direct safe reduction in the number of kernel import sweeps.

Future kernel proposal, **not implemented and not a dependency**: a separately
reviewed versioned ABI could transfer CPU/GPU ownership explicitly per import,
with checked, cache-line/page-aligned dirty ranges. CPU release would clean only
CPU-written ranges before GPU access; explicit CPU acquire would invalidate
GPU-written ranges only before host reads. It must conservatively handle
unannounced shared/IOSurface writers, aliases, indirect shader accesses, partial
cache lines, fence/watchdog/fault isolation, unmap and process exit. Treat
imports as fully dirty/host-readable unless ownership is explicitly established;
never trust a draw rectangle as a memory-access bound. Current BoImport has
only address/bytes and cannot express this protocol. Suppressing its kernel
sweeps from userspace, changing cache attributes or guessing that the surface
has no CPU access would violate the current contract. Any future kernel patch
must be separately committed and tested with the userspace fallback intact.

## Counters and expected measurements

The existing line and its fields are preserved:

```text
FUXI CA: display warm per-frame submits=... prepare=... wait=... retire=... other=... wall=... ms
FUXI CA: display warm detail objects=... descriptors=... record=... shadow=... flush=... invalidate=... ms flush_calls=... invalidate_calls=... empty=... creates=... pool_creates=... frame_creates=... view_creates=...
```

Both report warm per-frame means (frames 1–59). The second line uses optional
new process-counter exports and is absent with an older plugin. Objects times
pool creation/reset, descriptor capacity planning/growth, framebuffer/view
lookups/creation and neutral allocation. Descriptors times set allocation and
Vulkan descriptor updates. Record is Begin through EndCommandBuffer, excluding
measured object/descriptor intervals. Flush is the pre-record mapped-range
phase; invalidate is the post-fence mapped-range phase (part of retire).
Shadow is frontend no-copy shadow refresh before Backend::submit, counted in
other, not prepare. Lock acquisition, validation, draw planning/runtime pipeline
builds and remaining bookkeeping explain prepare's unaccounted remainder.
Timer/atomic overhead remains included in wall time. These buckets are not all
additive subdivisions of prepare; failed diagnostics can leave partial counters.

Creates counts successful reusable-object creation API calls. Pool_creates
counts command pool, command buffer, fence, query pool and descriptor pool;
frame_creates and view_creates count cache misses, including uncached overflow.
They exclude ordinary texture/buffer allocation and runtime pipeline creation.
A steady resource set should have pool_creates=0.00 after warm-up and no new
frame/view objects. CA texture churn may produce frame/view creation and should
be attributed, not mislabeled as pool growth. If finish() was the only extra
submit, expect submits near 1.00 and empty near 1.00 per frame; additional CA
passes may legitimately produce more submits. GPU samples now describe only
real GPU work. Flush/invalidate calls may be zero on coherent memory while
kernel import handoffs remain. Do not infer a performance PASS from these
expected trends.

The smoke now checks the deadline for frame 59 even when the breakdown is
available; the previous `else if` skipped that frame. No pixel threshold,
deadline, watchdog, jetsam or process admission is relaxed.

## Checks completed and executable tests pending

Completed without running test executables or submitting GPU work:

* M9h and M9g runners with `--compile-only`, `-Wall -Wextra -Werror` and
  ASan/UBSan; authored M7 AIR assembled from tracked source in temporary output.
* M9b compile-only cache/backend controls and the M7 production-backend C++
  translation unit compiled with warnings as errors and ASan/UBSan.
* Production Backend syntax check without host-test switches, warnings as errors.
* Frontend runner `--compile-only`: Linux constant/vertex/timeline/resource-cache
  controls compiled; native Mac Objective-C suites skipped on Linux.
* Offline CA production-backend C++ translation unit compiled with warnings as
  errors and ASan/UBSan; no Apple inputs consumed.
* Our bridge built with `cargo +1.98.0 build --offline` against a clean external
  checkout at pin `720ef1ce9ae2a2068999a8675717418b9416283e`. Outputs stay outside
  tracked files. The external compiler and Mesa remain unvendored.
* Python syntax, shell syntax, `git diff --check` and CI `metal --dry-run`.
  The metal recipe, deployment target, frameworks and pin are unchanged.

`tools/test-fuxi-metal-m9h.py` follows the M9g interface (`--translator`, `--icd`,
`--headers`, `--llvm-as`, `--compile-only`). Its authored-AIR executable compares
all pixels against the same recorder with submit reuse disabled in a host-test
mode. Assertions cover 256 steady submits with unchanged creation counts and
resource accounting, 64 submits after descriptor growth, descriptor-limit
refusal and recovery, recording/pre-queue rollback and subsequent recreation,
140-texture cache overflow/retirement and pixels, destruction invalidation,
render-pass eviction, mutex-ordered empty completion, 256 empty completions
without submissions/samples/objects, and poison refusal after a queue-attempt
failure. Explicit memory counts exclude opaque driver storage; creation counts
and cache bounds complement them. None of these executable assertions ran here.

Native Mac `CAFrontendTests.mm` adds actual wrapper checks for zero submits on
empty completion, scheduled-before-completed handlers, an earlier buffer write,
event signal/wait ordering, and nested empty commit from a completion handler.
The existing FrontendTests enqueue-probe count now excludes its empty GPU submit.
Only valid Metal enums/descriptor indices are installed, avoiding Mac Metal's
out-of-range setter assertions. These additions are uncompiled/unrun here;
Linux checks are not Mac/iOS build evidence.

## Exact next device test (separate authorized operation)

1. On Linux, run the new M9h runner without `--compile-only` using the matching
   pinned bridge, external Vulkan headers and lavapipe ICD. Run M9g/M9b/M7
   regressions, and run the frontend harness on native Mac with its existing
   `-Werror` flags. This lane intentionally performs none of these runs.
2. Build the existing CI target **metal** normally (no dispatch in this lane).
   Stage the matching signed bundle/translator, notices/source archive, both
   smokes and configs using the established M9 procedure and unchanged ABI-2
   kernel/Turnip pair. No kernel proposal is needed.
3. Keep the existing smoke-only client file; backboardd/SpringBoard stay denied.
   Use the established isolated `fuxi-ca-smoke` exec-swap entry and matching
   sweetnull entry, preserving other boot arguments. First select cumulative
   maximum **4** and require all five existing stage/pixel oracles.
4. Select cumulative maximum **5**, using the same imported 1080×2400 target,
   30-layer scene, 60 frames and 1024 MB jetsam setting. Save complete diagnostics
   privately, including both warm breakdown lines. Stop at a refusal, timeout,
   abort, corruption or deadline failure. Separately repeat Metal smoke maximum
   **6**. Do not enable compositor clients to collect these results.

Expected stage-5 lines, with variable values/request order:

```text
FUXI MTL: gate deny backboardd
FUXI MTL: gate deny SpringBoard
FUXI MTL: gate allow fuxi-ca-smoke
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
FUXI CA: display warm per-frame submits=... prepare=... wait=... retire=... other=... wall=... ms
FUXI CA: display warm detail objects=... descriptors=... record=... shadow=... flush=... invalidate=... ms flush_calls=... invalidate_calls=... empty=... creates=... pool_creates=... frame_creates=... view_creates=...
FUXI CA: stage 5 display PASS pixels=2592000 frames=60 ... gpu_draws=...
FUXI CA: done PASS
```

Require all 60 frame records, positive finite GPU times/draws/cache hits, zero
frontend failures, identical existing pixel oracles and **late=0/59**, with
warm wall comfortably below 16.667 ms. Compare the new measured mean breakdown
against the supplied baseline; a pixel PASS with a deadline miss remains a
performance failure. Persist only source/tests/docs, never device dumps, Apple
products, extracted shader/IR/disassembly or workstation/device identifiers.
