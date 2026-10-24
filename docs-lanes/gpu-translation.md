# GPU translation: first-use UI latency

2026-10-24. Original Fuxi frontend/backend and Fuxi C ABI adapter changes only.
The external metal2vulkan checkout remains clean at
`720ef1ce9ae2a2068999a8675717418b9416283e`; Mesa and NativeMetal are unchanged.
No upstream implementation is copied into this repository. The adapter remains
part of the separately built, dynamically loaded library, never statically
linked into FuxiMetal. No phone access, downloads, Apple inputs or hosted runs
were used for this work.

## OBSERVED: device log and existing code

The supplied `research/xl/use89.log` contains 90 backboardd `translation PASS`
records totaling **26,082.686 ms**, worst **1,637.933 ms**. Nine exceed one
second; 24 are below 50 ms. Interaction includes `hz=3.53 window_frames=35`.
GPU execution and the scanout copy are much smaller than the long translation
stalls. The same log also has two Vulkan pipeline timing outliers of 522.215
and 504.772 ms; Vulkan creation is usually small, but not uniformly 2–17 ms.
Those two outliers should be tracked separately during acceptance.

The boot already loads **33,385,261 bytes / 62 pipeline artifacts**. Later
snapshots contain about 32.9 MB / 49 artifacts. Thus this is a mixed warm/cold
session, and the 32 MiB byte limit is already relevant, well below the 256-entry
limit. The adapter's `translation cache MISS` describes its **eight-product
process cache**, not the persistent artifact cache. It cannot establish that
every miss retranslated AIR: an artifact load can satisfy it.

`Backend::compilePipeline` holds `Backend::mutex` across identity, translation,
product hashing and Vulkan creation. Submissions use the same mutex. Both
render-pipeline completion-handler selectors previously called the synchronous
selector inline. A compile can therefore block the caller and GPU submissions.

The original Fuxi adapter:

- Parses the metallib container at `open`; library/function handles retain the
  input. It does not reopen/reparse the metallib container on each pipeline.
- Constructs a new translator `Device` and a vertex/fragment descriptor for
  every product-cache miss, querying device descriptions each time.
- Calls `compile_render_variant` on two `StageSource::Function` values. The
  pinned library explicitly keeps no compiled-function cache. Shared vertex
  functions, and fragment functions reused with other render state, are
  specialized/lowered again for each uncached pair.
- With persistence enabled, first calls `render_variant_key` with temporary
  `StageEntries`, drops those specialized entries, then compiles again on an
  artifact miss. That repeats decode/specialization within one request.
- Formats all of the 40,360-byte `CompileOptions`, including unused constant
  slots, into a debug-text product key. Its scan reconstructs/copies that text
  for each of at most eight cached products. This is unnecessary bounded CPU
  and allocation work, not an unbounded growing pipeline list.

The backend object LRU is bounded at 32 entries / 16 MiB; adapter product LRU at
8 / 16 MiB of charged product payload; artifact LRU at 256 / 32 MiB. Their linear
scans are bounded. Product payload accounting does not represent total process
RSS (retained descriptors, source references and caller-owned pipelines also
exist). The inspected pinned library has no global growing compiled-stage list.
`StageEntries` has a linear specialized-entry list, but the old adapter creates
and discards it per key request; it cannot grow across this session.

## GUESS: why later translations take longer

The old log does not contain per-stage timings or per-request artifact outcomes.
It does **not** prove that equivalent work gets progressively slower. Later
requests can be more complex shader/constant variants, can lose persistent
artifacts through byte-cap eviction, and can retranslate shared stages. CPU
thermal/governor limits and scheduling on the smaller cores can increase the
wall cost of this repeated work. Memory pressure and allocation costs are also
possible. No claim of a translator leak or a proven thermal cause is made.

The new trace distinguishes these possibilities by recording full upstream
stage keys, artifact outcomes, stage-cache evictions, process CPU time, peak
RSS, and wall-time phases. Compare repeated *equal keys*, not just early/late
session averages. `process_cpu_ms` includes other threads of the process;
`peak_rss_kib` is a high-water mark, not current RSS or evidence of a leak.

## Settings (all absent/off by default)

| Boot argument | Equivalent process environment | Behavior |
| --- | --- | --- |
| `fuxigpustages=1` | `FUXI_MTL_STAGE_CACHE=1` | Reuse compiled functions, retain specialization within a request, and use compact product input keys. |
| `fuxigputrace=1` | `FUXI_MTL_TRANSLATION_TRACE=1` | Detailed translation timing and cache diagnostics. |
| `fuxigpuasync=1` | `FUXI_MTL_ASYNC_PIPELINES=1` | Queue the two render-pipeline completion-handler APIs on a serial worker. |

Only the exact value `1` enables an option. Explicit boot tokens override the
environment, including zero, malformed values and duplicates (all disable).
Settings are sampled once per process. Existing client admission,
`fuxibbgpu=`, `fuximetalbackend=` and `fuxigpucache=` policies still apply.
Use the Fuxi backend. Persistence remains independently controlled by
`fuxigpucache=1`; stage reuse also works with disk caching disabled/unavailable.

Rebuild FuxiMetal **and its original Fuxi adapter** with the existing
`tools/build-fuxi-metal-translator.sh` / `tools/ci/build.sh metal` route.
No metal2vulkan API or source changes are required. The new optional ABI-8
symbol is `fuxi_m2v_translation_scope_v1`; old ABI-8 bridges remain loadable,
log `bridge=0`, and cannot enable stage reuse/internal timing. Adapter source
changes invalidate the existing persistent snapshots through the existing
source digest; allow for that first boot when comparing results.

### Stage reuse and correctness boundaries

The pinned public APIs provide `render_variant_key`, `StageEntries`,
`compile_function_from_entries`, `CompiledFunction::{to_bytes,from_bytes_for_key}`
and `StageSource::Compiled`. Fuxi uses these APIs directly. It first obtains the
**actual** lowering inputs and upstream function keys for the complete pair and
variant, compiles only missing functions, then links their compiled artifacts.
Keys include constants, entry/AIR hash, device profile and lowering facts:
vertex-fetch projection, fragment attachment/varying facts and runtime variant
inputs. A function name alone is insufficient. The original function descriptor
stays in each product for later runtime variants.

The stage LRU stores **encoded artifacts only**, up to **128 entries / 16 MiB**
of artifact bytes plus bounded key/container overhead. Hits validate/decode the
artifact against its exact function key and device. Decoded/specialized AIR is
kept only within the current request. Eviction cannot invalidate active
products. Oversized artifacts are not retained; optional cache encoding failure
does not fail a successful compile. No additional disk cache, AIR input store,
cache-size increase, stage worker threads or upstream patches are introduced.

This removes repeated lowering and the second specialization on an artifact
miss. Computing the exact keys still decodes/specializes `Function` stages on
each new pair, including cache hits; this optimization does not eliminate all
first-use CPU work. Device queries, product hashing and linking remain costs.

### Asynchronous API behavior

Both `newRenderPipelineStateWithDescriptor:completionHandler:` selectors now
optionally copy the descriptor before returning and compile on one serial GCD
queue. The copied block retains the device, descriptor/functions/libraries and
handler. A worker autorelease pool encloses compilation and callback; the
new-method state ownership is released after callback. Callers can retain the
state/error they need beyond callback. Reflection/options failures use the same
NSError path as synchronous calls. The worker does not use the main queue.

Default inline behavior remains. Synchronous methods still return complete,
usable states or errors. No deferred placeholder or skipped-draw mode is added.
Asynchrony avoids doing work on the requesting thread, but **does not guarantee
that an existing frame can submit while compilation runs**: the backend mutex
and adapter product-cache mutex remain serialized. Clients that immediately
wait for completion still wait. No device result establishes whether CA uses
these asynchronous selectors often enough to help; stage reuse is the primary
optimization for synchronous CA requests.

## Log interpretation

- `translation settings stages=1 trace=1 bridge=1`: both adapter capabilities active.
- `translation request id=... vertex=... fragment=... objects=...`: correlate a
  request with its backend timing. The short ID is diagnostic only.
- `translation timing result=... wait_ms=... identity_ms=... compile_ms=... hash_ms=... create_ms=... total_ms=... process_cpu_ms=... peak_rss_kib=...`:
  caller-visible total and backend phases. Results include object-hit,
  product-hit, miss and fail; wait is time before acquiring the backend lock.
- `translation detail phase=... ms=...`: adapter wall timings for product-key,
  product-lock, product-lookup, device-descriptor, keys-specialize, artifact-read,
  vertex/fragment-cache-read, vertex/fragment-lower, stage-cache-write, pair-link,
  pair-compile, artifact-write, product-plan and library-open. The plan phase
  includes product extraction/validation. Detailed phases are subsets of
  `compile_ms`, not extra time to add to it. Library-open precedes pipeline work.
- `translation keys vertex=FunctionCompileKey(...) fragment=...`: full stage keys.
- `translation stage=vertex|fragment hit=... entries=... bytes=... evictions=...`:
  misses report retained capacity and cumulative evictions; hits avoid lowering.
- `translation artifact hit=... rejected=...`: distinguish a real persistent
  hit from the eight-product process cache. Correlate with existing cache-period
  counters and `Vulkan pipelines PASS` / `hz=` frame windows.

Timers describe wall time, including preemption. Trace output adds overhead;
use it for attribution, then repeat the final interaction comparison with trace
off. Async callers may finish their API call before worker timings appear.

## Host measurements and tests

Linux host, authored `metal-m7-control.ll`, external pinned translator, lavapipe,
ASan/UBSan on C++, no Apple libraries/shaders. The new
`TranslationTests.cpp` / `test-fuxi-metal-translation.py` makes 104 distinct
pipeline requests in fresh processes, A/B/B/A. The first 96 vary render state
while reusing functions; the last eight vary constants, vertex offsets and
strides. Every product hash (executable bytes, resources, specialization,
blobs/layout and render state) must match baseline. The runner also requires
stage hits, misses, and 104 backend misses: an object/pair cache hit cannot
masquerade as a stage-cache win. Timing thresholds are not test assertions.

Optimized host-translator results (fresh-process A/B/B/A after the other lane
checks finished, host milliseconds):

| Path | Backend request totals, runs 1 / 2 | Sum of backend `compile_ms` | Worst backend request, runs 1 / 2 |
| --- | --- | --- | --- |
| Default | 2,978 / 2,890 | 1,237 / 1,189 | 54.443 / 56.700 |
| Stage reuse | 2,257 / 2,324 | 479 / 502 | 43.723 / 42.131 |

Each optimized run recorded **201 stage hits / 7 misses**, across 208 stage
requests. Translation time fell about **60%**, backend request time about
**22%**. Pair compile alone totaled 666 / 645 ms in the reference; exact-key
specialization plus remaining stage lowering and linking totaled 261 / 269 ms
with reuse. Product-key plus product-lookup fell from 385 / 374 ms to 6 / 6 ms.
Total harness time was 3,299 / 3,202 ms versus 2,578 / 2,654 ms (about 20% less);
it includes the test's second product-hash calculation, outside backend timing.
Separate phases must not be summed into `compile_ms` a second time. Earlier
runs under concurrent host checks had overlapping worst-request ranges: this
small fixture does not establish a reliable phone worst-stall improvement.
An earlier debug build also passed the exact-hash comparison and showed lower
compile cost. No phone FPS or millisecond device target is inferred.

Commands, using already available external inputs (no downloads):

```sh
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-cache.py --source "$M2V_SOURCE" --build "$BRIDGE_BUILD"
python3 tools/test-fuxi-metal-translation.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-cache.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9g.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9h.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9k.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9l.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9o.py --translator "$HOST_TRANSLATOR"
bash tools/ci/build.sh metal --dry-run
```

The adapter's eight Rust unit tests include LRU recency, replacement, entry/byte
caps and oversize rejection. C++ policy tests cover exact/duplicate/malformed
boot tokens and environment precedence. Existing persistence tests exercise
fresh-process pixels, idle checkpoint, corruption, identity mismatch and
unavailable storage with stages off/on. Pixel/control suites cover constants,
MRT/framebuffer fetch, mip/format variants and topology; the backend loss,
timeout and corruption controls remain applicable.

`AsyncPipelineFrontendTests.mm` exercises the actual frontend with its CPU model:
a suspended worker proves return-before-work, then descriptor mutation and
input/device releases test snapshot/ownership. Success/error callbacks fire
once and retained states survive worker pool drainage. It is wired into the
frontend runner and the `metal` CI build. **Mac Foundation/Metal/iOS SDK builds
and Objective-C execution remain unverified on this Linux host**, including this
new async test. A full Mac `tools/ci/build.sh metal` run is required; only its
Linux dry-run was executed here.

## Device acceptance experiment (not executed here)

Use the same newly built adapter for both A and B so its source digest is equal.
Keep CPU/GPU/thermal settings, display, client admission and the interaction
sequence fixed. Record temperature/governor samples and 27 comparable touch
contacts. Use fresh process state and equivalent empty translation/driver cache
state for the cold A/B comparison; keep a separate warm-cache comparison. Do not
compare an old warm snapshot against a newly invalidated adapter snapshot.

1. A: `fuxigpucache=1 fuxigputrace=1`, stages and async explicitly zero.
2. B: same, plus `fuxigpustages=1`; keep async zero to isolate translation reuse.
3. Optional C: B plus `fuxigpuasync=1`; identify which pipeline API CA calls and
   whether callback users submit or immediately wait. Keep this result separate.
4. Repeat the same unlock/screen/gesture sequence, with the same measured
   interaction interval in each log. Record initial first-use and immediate
   repeat-screen behavior separately. Repeat A/B in reversed order to expose
   thermal/order effects, then repeat the chosen configuration with trace off.

For the **interaction interval only**, report the number and sum of
`translation PASS ... ms`, worst such stall, the sum/max of `compile_ms`, max
`total_ms` (includes lock/Vulkan/hash costs), per-stage hit/miss/eviction counts,
artifact hit/miss/rejection counts, and the `hz=... window_frames=...` windows.
Report Vulkan creation outliers, failure/dropped-draw counts and visual
correctness as well. Do not average boot-idle `hz` windows into interaction FPS;
if combining interaction windows, weight by each window's elapsed duration.

Expected effect: new pair/state combinations sharing function keys should avoid
repeated lowering, with lower translation totals and worst stalls, and higher
interaction Hz. Truly new functions still compile; key specialization may remain
expensive. If keys-specialize dominates or stage evictions thrash, the trace
identifies that next limit. If wall time rises for equal keys while process CPU
time stays flat, investigate scheduling/contention; if both rise, examine CPU
frequency and shader/cache phase cost. This work does not change the governor,
cluster scheduling or upstream compiler algorithms.
