# Metal M9i: persistent Vulkan pipeline cache

2026-10-22. Source, tests, documentation and compile/static checks only. No
executable tests, GPU submissions, device access, workflow runs, deployment or
push. backboardd and SpringBoard remain **DENIED**. Client configuration and
admission logic are unchanged. No Mesa or external translator implementation
is copied into this repository; the translator pin remains
`720ef1ce9ae2a2068999a8675717418b9416283e`.

## Evidence and intended effect

Read M9g, M9h, PipelineCache.hpp and all eight commits after M9h (`1d6c117`
through `2cfb2ad`): sleep-overshoot probes, time-constraint pacing, interrupt
counters, darwin-bg clearing, foreground/QoS requests and Interactive launchd
job policy. Those pacing changes remain intact.

Supplied device observation: cumulative CA stages 0–5 PASS; stage 5 completes
60/60 1080×2400 frames at 60 Hz, late=0/59, warm wall 5.9–12.7 ms and GPU
3.58 ms. The first display frame costs 3.6 s, including two 1.4–1.7 s Turnip
pipeline builds, with seven object-cache hits and two misses. These are user
observations, not results obtained in this lane.

The existing object LRU eliminates identical requests within one process. The
VkPipelineCache now carries driver compilation products across device opens,
process exits and boots. A second process still translates and creates Vulkan
pipeline objects; a validated driver cache should remove most compilation
cost. `load=hit` means an accepted blob, not measured per-pipeline compiler hits.
The device experiment must establish the latency improvement and identical
pixels; no new performance result is claimed here.

## Lifetime, checkpoints and bounds

Backend passes the already identified executable basename to PipelineCache;
it never derives identity from argv or changes admission. Device open queries
Vulkan driver and ID properties, validates a file, then creates the driver cache
with accepted initial data. A refused seeded create retries with empty data.
Failure even with an empty cache leaves a null VkPipelineCache hint, so normal
pipeline creation can continue. An older ABI-6 bridge without the optional
identity symbol keeps memory-only behavior. The C structs/ABI remain version 6.

Every new baseline or runtime pipeline pair marks the cache dirty. Checkpoint
policy is **four new pairs (eight Vulkan pipelines)**, or a pair taking at least
**100 ms**, with at most one checkpoint attempt per **one second**. The elapsed
build time excludes serialization/fsync, which has its own log. Checkpoints
are on the pipeline-construction slow path only; there is no cache I/O, timer,
serialization, or new hook in steady submit, fence, draw, or per-frame handling.
A suppressed checkpoint remains dirty until a subsequent qualifying build or
clean close. Clean close flushes dirty data regardless of the rate limiter,
before destroying the cache/device. Uncertain GPU ownership retains M9h's
poison policy, including skipping close; there is no persistence attempt on
that path. Abrupt process death may lose work since the last checkpoint.

The old eight-pair generation retirement is removed: it would repeatedly throw
away the very compilation products being persisted. The driver size query
still runs after construction; a generation over **32 MiB of payload** (or a
failed size query) is retired and replaced empty. Existing VkPipelines remain
valid. Oversized generations are never written; an earlier valid file remains.
The byte query bounds retained serialized generations, not opaque compiler
scratch or exact driver heap use. Load/write allocation is bounded to one
32 MiB payload plus a 336-byte wrapper. File size is capped at **32 MiB + 336**.
No unbounded queue or background thread is introduced.

## Storage, security and concurrent writers

Chosen device path: `/private/var/db/fuxi-metal/<client>.cache`, one file per
client executable basename. The daemon creates `fuxi-metal` with **0700** under
the existing system database directory. It must be root-owned with exactly
0700 permissions; existing unsafe directories are refused, not repaired.
Production persistence requires effective UID 0. The basename is restricted
to 1–63 ASCII letters, digits, hyphen or underscore. Paths and shader identities
are never printed. There is no production environment override for the root.

All files are **0600**, root-owned, regular, single-link files opened relative
to the trusted directory FD with no symlink following. Reads are nonblocking
until the regular-file check, so a substituted FIFO cannot hang open. Size is
checked before allocation; exact reads, length, checksum and both identities
must succeed before any bytes reach vkCreatePipelineCache. CRC32 detects
accidental corruption; it is not authentication. Ownership and restrictive
permissions are the trust boundary against unprivileged writers. Root can
already replace the driver/framework and is outside that boundary.

Save writes a unique PID/sequence temporary file using exclusive creation,
writes the full wrapper and payload, fsyncs the file, atomically renames over
the client file and fsyncs the directory. Ordinary failure removes the temp;
a crash may leave a private orphan temp, which is never considered for load.
No stale lock can block future opens. Concurrent writers are **last complete
writer wins**: readers see an old or new complete inode, never an in-progress
write. Independent processes' different pipeline sets are not merged; some
acceleration may be lost, never correctness. Automatic cache reclamation is
limited to replacing this client's file; there is no directory sweep.

Sandbox denial, missing/unwritable storage, allocation failure, unsupported
bridge identity and save failure all leave rendering memory-only without a
frontend error or admission change. A failed save disables further attempts
for that device lifetime. The required diagnostic/counters describe fallback;
there are no user prompts, environment workarounds or sandbox changes.
Host tests alone accept a temporary current-user-owned 0700 directory through
`FUXI_MTL_TEST_CACHE_DIR`; that branch is compiled out of the shipping backend.

## File format and validation

All wrapper integers are explicit little-endian uint32 values; no native
struct padding, pointers or C++ serialization is used.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | ASCII magic `FXMTLC01` |
| 8 | 4 | Format version 1 |
| 12 | 4 | Wrapper length 336 |
| 16 | 4 | Exact payload length, 32 through 33554432 |
| 20 | 4 | IEEE CRC32 of the entire payload (polynomial 0xedb88320) |
| 24 | 4 | Current Vulkan driverVersion |
| 28 | 4 | Current Vulkan driverID |
| 32 | 16 | Current driverUUID |
| 48 | 256 | Zero-initialized Vulkan driverInfo field |
| 304 | 32 | Translator/adapter identity SHA-256 |
| 336 | variable | Unmodified vkGetPipelineCacheData bytes |

The optional `fuxi_m2v_cache_identity` export hashes a domain/version string,
the full pinned metal2vulkan revision and our adapter source as compiled. Both
lib.rs and the inventory harness's bridge.rs placement are supported. It does
not hash a filename, expose source text, or change translator struct layouts.
Stage the updated bridge with the frontend to enable persistence.

The payload must contain an exact 32-byte VkPipelineCacheHeaderVersionOne
header, version ONE, matching vendorID, deviceID and pipelineCacheUUID. Each
field is decoded without unaligned pointer casts. Any mismatch, changed driver
or translator, truncated/extra data, oversized file or checksum error discards
the whole blob and creates an empty cache. Driver rejection after validation
also discards and retries empty. Corrupt files can be replaced by a subsequent
successful save. Repeated load rejection never prevents pipeline creation.

The inspected pinned Turnip `tu_device_get_cache_uuid` incorporates its build
identifier, chip ID and relevant driver settings; thus the Vulkan UUID is also
the build-identity check. driverVersion, driverID, driverUUID and driverInfo
provide additional separation. A changed driver/UUID must invalidate rather
than bypass validation, including across boots. Cached products are private
runtime data, never build resources, distributed artifacts or tracked files.

## Mesa disk shader cache finding (no Mesa change)

`tools/build-fuxi-turnip.py` explicitly supplies `-Dshader-cache=disabled` to
the Darwin Turnip Meson build. In the pinned external Mesa source, meson.build
adds ENABLE_SHADER_CACHE only when that option is enabled; src/util/disk_cache.h
otherwise supplies a disk_cache_create stub returning NULL. Turnip's
src/freedreno/vulkan/tu_device.cc calls that function for its disk cache.
src/util/disk_cache_os.c reads MESA_SHADER_CACHE_DIR only in the enabled
implementation. Consequently **the recipe build has no Mesa disk shader cache;
MESA_SHADER_CACHE_DIR cannot enable it**. This is a source/build-recipe finding,
not an inspection of a new device binary. No Mesa option is changed here.

The application Vulkan pipeline cache is separate. Turnip's tu_pipeline.cc
uses vk_pipeline_cache lookup/add operations and shader/NIR serialization even
without the optional disk cache. Persisting vkGetPipelineCacheData therefore
does not depend on enabling Mesa's disk cache.

## Diagnostics and checks

The existing object-cache hits/misses remain unchanged. New uint64 process
exports use the prefix `fuxi_metal_pipeline_cache_`:

| Suffix | Meaning |
| --- | --- |
| load_result | Last open: 0 empty/unavailable, 1 hit, 2 discard |
| load_bytes | Last open's accepted payload bytes; zero for empty/discard |
| save_result | Last open/save: 0 none, 1 saved, 2 unavailable, 3 save error |
| save_bytes | Last successful save's payload bytes since last open |
| save_count | Process-total successful saves |
| save_failures | Process-total failed save attempts |

CA stage 5 reads optional exports after rendering, next to its cold-frame
records, printing load/save outcome and bytes. They add no pacing-loop work.
The native Mac CAFrontendTests cover forwarding all six exports; TestBackend
mirrors the new constructor and statics with the existing -Werror harness.

`tools/test-fuxi-metal-m9i.py` builds our bridge with
`cargo +1.98.0 build --offline` when given `--source`, requiring a clean external
checkout at the pin. It uses only tracked authored M7 AIR, compiles the
production backend with -Wall -Wextra -Werror and ASan/UBSan, and supports the
M9g/M9h --translator/--icd/--headers/--llvm-as/--compile-only interface.
Its executable assertions cover first-process save, second-process hit and
exact pixel equality; truncation, flipped byte, wrong UUID with recomputed CRC,
oversized sparse files; atomic concurrent saves and a validating third process;
and an unwritable directory with unchanged pixels. Deterministic fake-driver
controls additionally assert no corrupt seed reaches the driver, wrapper and
all Vulkan header fields are checked, checkpoint count/rate limiting, close,
seeded-create retry, file modes and symlink refusal. They are **compiled only,
not executed in this lane**.

Checks completed here: M9i bridge offline build and sanitizer/-Werror compile;
M9g and M9h compile-only regressions; frontend Linux controls compile-only
(native Objective-C tests skipped on Linux); production Backend syntax check;
offline CA backend translation-unit compile; Python syntax, shell syntax,
git diff --check and `bash tools/ci/build.sh metal --dry-run`. Native Mac/iOS
compilation and all executable assertions remain pending. The CI target metal,
its recipe, framework list, deployment target and pins are unchanged.

## Exact next device test (separate operation)

1. Before device qualification, run the M9i Linux runner without --compile-only
   against a clean external pinned checkout (`--source`) and lavapipe, then M9g
   and M9h regressions. Run the native Mac frontend harness with -Werror.
2. Build the existing **metal** CI target normally, outside this lane. Stage the
   matching signed frontend, updated bridge, source/notices, CA smoke/config
   and Metal smoke using the established procedure and unchanged ABI-2
   kernel/Turnip pair. Keep the existing smoke-only client configuration and
   backboardd/SpringBoard denied. Preserve the observed Interactive launchd
   job policy and isolated smoke exec-swap/sweetnull setup.
3. **Boot A:** keep the new cache directory absent for the initial experiment,
   or remove only the isolated smoke's old cache during authorized preparation.
   Select cumulative CA maximum 5. Require all stages, 60 frame records,
   pixel PASS, positive finite GPU timings/draws, no frontend errors and
   late=0/59. Expect load=empty followed by successful save(s) with positive
   bytes. Preserve the writable data volume and this file when preparing Boot B.
4. **Boot B:** boot the same staged binaries/config again without replacing or
   clearing the cache. This must be a fresh process/device, with no inherited
   pipeline LRU. Require `pipeline cache load fuxi-ca-smoke ... result=hit` and
   `FUXI CA: display pipeline cache load=hit bytes=N`. Compare frame-0 wall_ms
   and `Vulkan pipelines PASS` durations with Boot A: expect approximately warm
   rendering plus translation and Turnip cache-hit/object-creation cost, not
   the observed two long compilation stalls. Require identical pixel oracles,
   60 frames, no frontend failures and late=0/59 again. A hit log alone is
   insufficient performance evidence. Unexpected discard must be investigated,
   never relaxed into acceptance. Separately rerun Metal smoke maximum 6.
5. If two boots are impractical, two fresh smoke processes within one boot,
   each normally closed and with the same persistent file, qualify process
   persistence first; the two-boot experiment is still needed for boot reuse.

Expected lines (numbers/order vary):

```text
FUXI MTL: pipeline cache load fuxi-ca-smoke bytes=0 result=empty
FUXI MTL: pipeline cache save bytes=N ms=T result=saved
FUXI MTL: pipeline cache load fuxi-ca-smoke bytes=N result=hit
FUXI MTL: Vulkan pipelines PASS <cache-hit-cost> ms cache=1
FUXI CA: display frame=0 gpu_ms=... wall_ms=...
FUXI CA: display pipeline cache load=hit bytes=N save=saved saved_bytes=N
FUXI CA: display warm wall min/median/max=... ms late=0/59 target_hz=60
FUXI CA: stage 5 display PASS pixels=2592000 frames=60 ...
FUXI CA: done PASS
```

A fast hit process may have `save=none saved_bytes=0` at stage 5, then save at
clean close. Corruption examples are `bytes=0 result=discard(checksum)` or
`discard(vk-uuid)`; storage denial prints load=empty and save=unavailable in CA.
No blob contents, Apple libraries/metallibs, extracted shaders, IR, disassembly,
device dumps or workstation/device identities belong in tracked results.
