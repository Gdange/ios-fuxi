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

## Opt-in compile boost (2026-10-24)

### OBSERVED: supplied baseline and host checks

`research/cb/use89.log` records first-use interactive translation stalls around
0.1–1.6 seconds while the governor normally votes cool. In the cold-cache boot
`research/cb/stages90.log`, the 45 timing records sum to **15,333.829 ms
compile_ms + 19,304.510 ms create_ms = 34,638.339 ms**. Their caller-visible
`total_ms` sum is **35,156.845 ms**, and process CPU sums to **31,605.325 ms**.
These are sums of per-request intervals, not elapsed boot duration. Stage reuse
already hits in this log. Both translation and Turnip Vulkan creation matter.

The scheduler is homogeneous SMP. The device tree reserves physical CPU 1 for
the monitor: physical 0,2,3,4,5,6,7 become XNU logical 0,1,2,3,4,5,6. The new
helper uses `ml_get_cpu_number(physical << 8)`, rather than binding logical 7.
Host tests exercise the actual governor and scheduler helper with MMIO,
thread-reference and runqueue stubs. They establish policy and API sequencing,
not device migration, actual clocks or a performance improvement.

### Interface and settings

Enable **both `fuxiperf=1 fuxicompileboost=1`** in the kernel command line.
Absent/off leaves the compile interface unsupported (`ENOTSUP`) and FuxiMetal
makes no compile-lease calls. FuxiMetal accepts only the exact, unique
`fuxicompileboost=1` token in `kern.bootargs`; there is no environment override.
Keep the existing GPU/client admission settings. Rebuild the kernel and
FuxiMetal; the existing adapter/translator needs no change for this feature.

`sysctlbyname("kern.fuxicompileboost", ...)` takes an exact `int32_t` new value:
**1 enters, 0 leaves** for the calling thread only. An optional old buffer
receives the 24-byte version-1 `fuxi_compile_reply` from
`xnu/fuxi-perf/fuxi_compile_abi.h` (sampled high-vote nanoseconds, physical CPU,
state and nesting depth). This is an action endpoint, not a readable knob.
`CTLFLAG_ANYBODY` permits mobile to reach the handler; the handler independently
requires `proc_selfname()==backboardd` and a platform code signature, following
patch 0102's GPU gate. Root receives no bypass. No caller-selected CPU, thread,
task, frequency, duration or user-client connection can authorize another thread.

Admission samples TSENS synchronously through the serialized governor worker,
then chooses online prime CPU 7, otherwise an online gold CPU 6..3. A failed
EPSS domain is excluded. Existing bindings and realtime threads are refused.
Binding uses `thread_bind_internal` with the same scheduler locking as
`thread_bind`, with eligibility checked under that lock; `thread_block` then
forces migration outside the governor mutex. The vote raises only the selected
fast domain to its existing high LUT row (at most 2.4 GHz); silver is unchanged.
The normal touch vote can independently raise both fast domains.

The FuxiMetal RAII scope starts after an object-cache miss and covers the bridge
compile (including synchronous vertex/fragment stage compiles), product hashing
and Vulkan pipeline creation. It ends before object-cache insertion. Runtime
variants and lazy topology pipeline builds also have scopes; nested scopes share
one lease and exceptions release it. No lock wait before acquiring the backend
mutex is boosted. The existing translation timing fields remain comparable.

### Safety limits and lifetime

- Fresh valid CPU sensor quorum is mandatory. No entry above **60.0 C**; the
  existing predictive/rise guard, **62 C** soft cut, **68 C** maximum ceiling and
  recovery hysteresis still veto high votes. Lower configured limits still win.
- Compile and touch **share the existing rolling duty budget**, **200 ms per
  1000 ms by default** (`fuxiperfbudget` retains its existing 20..500 ms bounds).
  Compile boost cannot replenish it or extend a touch burst through swaps.
  Consequently a long compile usually loses its high vote and binding before
  completion. This feature deliberately does not enlarge the thermal budget.
- Every lease has a **2-second nonrenewable deadline**, at most **16 nested
  enters**, and one global owner. Nested enters cannot restart a cut lease;
  another thread receives `EBUSY`. Initial thermal/duty denial also returns `EBUSY`: XNU's `userland_sysctl()` retries a handler's `EAGAIN` forever, and the first device boot (EAGAIN) spun backboardd in the kernel with no frames.
- Leave at depth zero drops the vote synchronously and unbinds. Leave without
  a matching entry succeeds with `FC_NONE`. Copyout failure rolls back an enter.
  A thread reference prevents owner-address reuse; worker checks remove dead
  owners and release references. Timeout releases even a living hung caller.
- Thermal/duty cuts and offline/failed domains unbind immediately at the worker
  sample and retain a bounded reply until leave, death or the lease deadline.
  Queued threads are removed and reinserted under scheduler locks when unbound.
  Active high votes use the existing fast sampling period (20 ms default),
  shortened for remaining duty/deadline. Cleanup without high runs within the
  normal 100 ms period. These are timer deadlines, subject to scheduler latency,
  not hard realtime guarantees. Hardware thermal protection remains in control.

### Trace and acceptance experiment (device unverified)

With `fuxigputrace=1`, FuxiMetal adds:

```
FUXI MTL: compile boost phase=pipeline enter_errno=0 leave_errno=0 cpu=7 state=4 scope_ms=... voted_ms=...
```

`phase` is pipeline, variant or topology. `scope_ms` measures the compile scope,
including entry/migration overhead; `voted_ms` is the kernel's sampled high-vote
wall interval, **not measured frequency or thread CPU time**. A refused entry or
expired/discarded reply gives `voted_ms=-1` (unknown), never an invented full-scope
boost. `cpu=-1` means admission failed. Other trace phase times still describe
all work, including the portion after a cut. Compare them with the unboosted run;
a single boosted run contains no counterfactual baseline.

Kernel lines are `FUXI PERF: compile enter cpu=... cap_ms=2000 duty_ms=.../1000
state=...` and `FUXI PERF: compile end cpu=... reason=... voted_us=...`.
States/reasons: 0 none, 1 active, 2 leave, 3 thermal, 4 duty, 5 timeout, 6 dead,
7 offline/domain failure. Existing governor stats now include the union of
compile and touch high intervals. The kernel end line retains the reason/time
when a subsequent userspace leave can no longer retrieve the reply.

For device acceptance, use the **same newly built kernel, FuxiMetal and adapter**
in both runs, with stages and trace enabled and async explicitly disabled.
Keep GPU level, CPU/thermal settings, display, workload and cache state equal:

1. A: `fuxiperf=1 fuxicompileboost=0 fuxigpustages=1 fuxigputrace=1`.
2. B: identical except `fuxicompileboost=1`. Begin with equivalent cold empty
   translation and driver caches and a cooled device. Repeat in reversed order.
3. For cold boot, report pipeline counts, sums of `compile_ms + create_ms`,
   `total_ms`, process CPU, first-frame time, cache outcomes and scope/voted
   times. Compare against the **34,638.339 ms / 45** stages90 baseline as well
   as A; changes in shaders, cache hits or stage hits invalidate a direct ratio.
4. Repeat the use89 interaction sequence with comparable touch contacts; report
   median/p95/worst first-use `total_ms` and `wait_ms`, translation/Vulkan phases,
   stalls above 100/500/1000 ms, immediate repeat-screen behavior, frame Hz and
   failures. Separate admission refusals and duty/thermal-cut compiles.
5. Record hottest CPU TSENS, slope/cuts, time above the soft threshold, duty
   utilization and actual EPSS votes/clocks. Check selected physical core,
   migration, gold fallback and that leave/death/timeout clear binding and votes.
   At 58–60 C the existing guards may deny nearly every entry; explicitly report
   that result. Check visual correctness and run long enough to expose heating.

**GUESS:** moving CPU-bound compile work from silver to a fast core and its high
LUT row can shorten first-use stalls. The short shared duty budget and existing
thermal refusal can sharply limit that benefit; worker threads inside an
external compiler would not inherit a caller's binding. No device result yet
establishes migration, sandbox permission for mobile's sysctl, high clocks,
thermal safety under this workload, Turnip worker behavior or any speedup.
Full Mac kernel/FuxiMetal builds and native Objective-C tests remain unverified
on this Linux host; CI build routes were checked with `--dry-run` only.

Host validation for this change: 15 kernel/governor suites and 12 FuxiMetal
suites passed, including `tools/test-fuxi-perf.py` (which runs the scheduler
helper model), frontend policy/scope tests, software Vulkan backend, cache and
M9g–M9o controls. The authored-AIR translation test also verified 104 identical
pipeline product hashes when compile boost is requested against an unsupported
host kernel, with one refusal trace per pipeline and no redundant topology
scope. Patch snapshots match; the scheduler hunks apply in order through the
later idle hooks. Both modified kernel units pass an ARM64 syntax check against
cached XNU headers. These host checks do not replace the device experiment.

### Compile boost device results

- OBSERVED, first boot (refusals returned `EAGAIN`): no frames. XNU's
  `userland_sysctl()` retries `EAGAIN`, so backboardd spun in the kernel.
- OBSERVED, second boot (`EBUSY`): renders, first frame 7.3 s, but all 49
  enters were refused with `cpu=-1`. The governor was ready and below 60 C.
- GUESS: the compiling thread is CoreAnimation's realtime render thread, and
  `fuxi_compile_bind()` refuses realtime threads. Such a caller now gets an
  unbound lease (`cpu=-1`, `state=1`) that votes high on gold and prime without
  migrating the thread. Refusals log
  `FUXI PERF: compile refused reason=N` (1 busy, 2 not ready, 3 thermal,
  4 duty, 5 no online fast core).

## Frame time breakdown

### OBSERVED: supplied interaction and synchronous submission

The supplied `research/ft/use89.log` ends at 502 completed surface frames,
255 late frames (50.8%), and zero recorded failures. Interaction windows include
`hz=4.84 gpu_ms=4.180`, `hz=11.41 gpu_ms=5.516`, and
`hz=22.98 gpu_ms=6.650`. These are completion-window averages, not per-frame
percentiles. Low GPU execution time alone does not establish where the CPU
spent time. The user reports very laggy swiping.

`Frontend.mm` commits synchronously: it acquires surface writer leases, copies
no-copy buffer shadows, submits operation segments around process-local events,
then publishes Completed and delivers handlers. `Backend::submit` holds its
mutex through preparation, queue submission, fence completion and invalidation.
A pipeline compilation or cache checkpoint can contend for that same mutex.

The blocking/cost audit below refers to the implementation in this change;
all paths are conditional unless described as every nonempty submit. Vulkan
object creation and cache maintenance are synchronous CPU calls, not additional
explicit GPU completion waits.

| Location | Blocking wait or synchronous work |
| --- | --- |
| `tools/fuxi-metal/Backend.hpp:758` | Backend mutex, held through the fence wait; shared with resource creation, compilation and cache maintenance. |
| `tools/fuxi-metal/Backend.hpp:1100` | `vkQueueSubmit`; driver CPU work can itself block. No async worker is introduced. |
| `tools/fuxi-metal/Backend.hpp:1104` and `:1106` | The **only explicit Vulkan completion wait**: `vkWaitForFences`, once per nonempty operation segment, production timeout 2 seconds. Host-only macro can extend it. A Metal command with events may have several segments/waits. |
| `tools/fuxi-metal/Backend.hpp:1110` | Timestamp `vkGetQueryPoolResults` after the successful fence wait, without `VK_QUERY_RESULT_WAIT_BIT`; no extra wait requested. |
| `tools/fuxi-metal/Backend.hpp:896`–`:897`, `:1116`–`:1117` | Synchronous whole-allocation flush before queue submission and invalidate after completion for each noncoherent referenced buffer/mapped image. HOST_COHERENT skips these calls. No per-frame map for ordinary persistent resources. |
| `tools/fuxi-metal/Backend.hpp:882`–`:892`, `:322`–`:323` | Alias-copy snapshot and translator neutral buffers call `allocateUnlocked`: buffer creation, `vkAllocateMemory`, bind, `vkMapMemory`, CPU zeroing. Default: transient allocation each applicable submit, destroyed during cleanup. Opt-in scratch reuse covers only the bounded alias buffer. Neutral buffers remain transient. |
| `tools/fuxi-metal/Backend.hpp:900`–`:906` | Command pool/buffer, fence and timestamp query pool are **already reused**. Created on first use or after recovery; reset on subsequent submits, after the prior fence completed. No default per-frame command-buffer creation. |
| `tools/fuxi-metal/Backend.hpp:923`–`:939`, `:990` | Descriptor pool is **already reused**, geometrically grown for new type/set capacity, reset per submit. Descriptor sets are still allocated and updated per draw. |
| `tools/fuxi-metal/Backend.hpp:981`, `:1017`, `:1040` | Framebuffers and sampled image views have bounded caches. Cache misses/overflow create objects; partial empty passes create transient render-pass/framebuffer objects. |
| `tools/fuxi-metal/Backend.hpp:944`, `:1056`, `:1067`, `:1074`, `:1078` | GPU staging: sampled linear shadows refresh buffer-to-image; aliases snapshot image-to-buffer then buffer-to-image; explicit texture uploads/readbacks copy to/from buffers. These commands share the segment's single fence wait. |
| `tools/fuxi-metal/Backend.hpp:857`, `:858`, `:862`, `:735` | Submission can synchronously translate/create runtime pipeline variants and lazy topology pipelines. Not unconditional per-frame pipeline creation. |

There are **no `vkQueueWaitIdle` or `vkDeviceWaitIdle` calls** in this backend.
Resource creation outside submit can also allocate/map memory (buffer import,
image import and ordinary texture allocation); it is not an unconditional
per-submit operation. Frontend IOSurface locks (`Frontend.mm:1230`), shadow
`memcpy` (`Frontend.mm:1237`), and explicit process-local event waits
(`Frontend.mm:1242`, up to 2 seconds) are additional possible stalls.
`waitUntilCompleted` (`Frontend.mm:1280`) can wait for a configured completion
queue's handlers; that wait is outside the measured commit interval.

### Opt-in diagnostics and log format

`fuxigpuframetrace=1` enables the additional line from the existing five-second
backboardd reporter. Default is off, with no environment-variable fallback.
Both new options use `translationOption`: only one exact `=1` token enables;
`=0`, malformed values and duplicate occurrences disable, including duplicate
`=1` tokens. Whitespace tokenization matches the existing translation options.
No diagnostic thread, per-frame heap storage, or per-frame printing is added.

Example **format only**, not a device measurement:

```text
FUXI MTL: frame trace n=100 samples=100 wall=12.000/25.000/80.000 ms cpu_before_commit=4.000/9.000 shadow_copy=0.100/0.300 lease_lock=0.200/0.800 backend_submit=2.000/8.000 gpu_wait=4.000/7.000 gpu_exec=3.000/6.000 draws/frame=8.00 render_passes/frame=8.00 pipelines/frame=0.10 descriptors/frame=8.00 allocations/frame=0.20 cpu_hist=0:10,1:10,2:20,3:20,4:10,5:10,6:20,7:0,unknown:0 encode_counts_unknown=0 gpu_exec_unavailable=0
```

- `n`: successfully completed command buffers storing a defined IOSurface
  render target, matching the existing gate's frame definition. It is a
  **window** count; a command is not necessarily a display refresh. Failures,
  non-surface commands and native-backend commands have no frame-trace sample.
  The existing `FUXI MTL: backboardd frames=...` line is preserved.
- All slash pairs are p50/p90 in milliseconds; `wall` adds the maximum.
  Host phases use `steady_clock` elapsed time and include descheduling.
  `wall` runs from commit to successful synchronous completion, including
  lease release, before user handlers and gate persistence/reporting work.
- `cpu_before_commit`: command-buffer creation to commit, the elapsed CA encode
  interval. It includes scheduling gaps and CPU waits; it is not CPU usage.
  Compilation before command-buffer creation and intervals between commands
  are outside it.
- `shadow_copy`: the frontend resource scan and shadow `memcpy` loop.
  `lease_lock`: surface lease acquisition plus backend mutex acquisition over
  all operation segments. Surface unlock and post-fence retirement contribute
  to wall but are not included in this pair.
- `backend_submit`: after acquiring the backend mutex through return from
  `vkQueueSubmit`, including validation, flush, allocation, translation,
  recording and descriptor work. `gpu_wait`: time inside `vkWaitForFences`.
  Explicit Metal event waits and post-fence invalidation/cleanup contribute to
  wall, outside these pairs.
- `gpu_exec`: existing Vulkan top/bottom timestamps, converted with the device
  timestamp period and valid-bit mask, summed across segments. This is GPU
  elapsed time, **overlaps** the CPU fence wait, and must not be added to it.
  `gpu_exec_unavailable` counts frames without timestamp samples; their stored
  value is zero. It is not an engine utilization counter.
- Draw/pass counts are actual recorded Vulkan draws/render passes. A Metal
  encoder with many draws currently becomes many Vulkan passes; full-image
  clear-only operations can instead use `vkCmdClearColorImage` (zero passes).
  Count fields are window arithmetic means per completed surface command.
- `pipelines`, `descriptors`, `allocations`: successful
  `vkCreateGraphicsPipelines` pipeline objects, `vkAllocateDescriptorSets` sets
  (not descriptor writes/pool creation), and `vkAllocateMemory` objects.
  Thread-local counters cover creation-to-completion on the committing thread.
  They exclude work on other threads. Overlapping command-buffer lifetimes can
  attribute the same intervening thread activity to multiple commands. If
  creation and commit occur on different threads, only commit-time counts are
  available and `encode_counts_unknown` increments. These counts are diagnostic
  attribution, not process totals or host malloc counts.
- Percentiles retain the latest 1,024 samples per reporting window in a fixed
  array; `samples` exposes truncation. Maximum wall, means and CPU histogram
  cover **all** `n` samples. Reporting sorts a fixed scratch array, then resets
  window counters, including idle windows (`n=0`).

**CPU convention verified against visible XNU sources:**
`osfmk/arm64/machine_machdep.h:35`–`:43` specifies the logical CPU in the low
12 bits of **TPIDR_EL0**, mask `0xfff`. `libsyscall/os/tsd.h:70`–`:79`
implements `_os_cpu_number()` by reading that register. TPIDRRO_EL0 holds the
cthread pointer in those sources; using its low bits would produce a misleading
histogram. The instrumentation reads TPIDR_EL0 once at commit entry on arm64
Darwin; unsupported/host-test builds report unknown. This is a hint only:
threads can migrate immediately before/after the read. Logical 0–1 are A510,
2–5 A715/A710, 6 X3 in the supplied topology; the histogram neither pins threads
nor establishes residency throughout encode/commit.

### Opt-in conservative speedup and host evidence

`fuxigpupipelined=1` currently enables **alias-copy scratch reuse only**. It
retains one buffer per Backend, up to 1 MiB of requested payload, charged to
existing resource limits (driver alignment can add padding). Larger requests
use the original transient path. Growth destroys the quiescent smaller buffer
before allocation; clean close reclaims it. The existing mutex and fence wait
prove that previous GPU use has finished before reuse or growth. Every copy
still records the full required snapshot and existing barriers. Pre-submit
failure leaves reusable scratch; uncertain queue/fence ownership poisons the
backend and retains resources until process death as before.

This deliberately does not make commit asynchronous: status publication,
completion handlers, CPU readback, event ordering and IOSurface ownership still
follow the original synchronous contract. Retained scratch can consume quota
between alias copies. With the flag absent, invalid or duplicated, allocation
and completion behavior follow the default transient path.

`tools/test-fuxi-metal-frametrace.py` uses the production Backend on lavapipe
with ASan/UBSan. Twelve 128x128 overlapping same-image snapshots with changing
CPU input allocate **12 objects by default versus 1 with reuse**, and full-image
readback matches CPU snapshot references. Controls cover
growth, >1 MiB transient fallback, pre-submit failure/retry, empty submission,
partial empty render passes, scheduled callback after submission, clean close
accounting, and refusal after an injected uncertain queue attempt. Duplicate and
malformed pipeline flags select the default path. `FrameTraceTests.cpp` checks
percentiles, reset, overflow, full-window maximum, log fields, the CPU bit mask,
and nested/thread-local attribution. Traced M9o authored shader controls also
verify pipeline, descriptor, draw and pass counters with unchanged pixels.
Timing output is informational; no noisy wall-time threshold is a correctness
assertion.

Host validation passed 20 script suites: `test-fuxi-metal{,2,2g,2h}`, backend,
frontend (its Linux controls), M6, M9a/M9b, M9g/M9h/M9i/M9k/M9l/M9m/M9n/M9o,
cache, translation, and the new frametrace suite. M9a ran through M9b, including
eight offline Rust adapter unit tests and SPIR-V validation. Available C++
controls use ASan/UBSan. Native macOS frontend/defaults builds, real signed
artifact staging/certification, and Apple-library suites were not run here.

### GUESS / unverified

Running encode/commit on A510 cores, pipeline compilation, shared-mutex
contention, surface locking, shadow/staging copies, or CPU scheduling gaps could
explain low completion Hz despite short GPU execution. The supplied log does
not distinguish them, and this change makes no scheduler/affinity adjustment.
The new trace must be collected during equivalent interaction to rank them.
Alias reuse helps only commands using that path; its frequency and benefit to
backboardd are unmeasured. No asynchronous commit change is justified by the
host evidence alone. macOS Objective-C++ build, target register values, Turnip
performance, actual frame percentiles and perceived UI improvement remain
unverified; no device or hosted execution was performed.
