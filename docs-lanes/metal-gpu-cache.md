# GPU-CACHE: persistent UI shader products

2026-10-23. Offline implementation and Linux host qualification. No phone,
root-image edit, hosted workflow, CI run or push. The supplied 9.5 s first-frame
and subsequent late-frame observations are motivation, not measurements made
here. This option targets compilation cost; it does not change GPU/CPU clocks,
submission, presentation or the old readiness line mentioning 220 MHz.

## Existing path and reference finding

`tools/fuxi-metal/Frontend.mm` creates a Backend with the verified process
basename. Its render pipeline method calls `Backend::compilePipeline`.
`Backend::createPipeline` builds triangle/list-strip objects through
`buildTopologyPipeline`; line/list-strip objects are lazy through
`topologyPipeline`. All call `vkCreateGraphicsPipelines` with the Backend's
`VkPipelineCache`. Runtime translation/specialization goes through
`fuxi_m2v_variant` and the same Vulkan creation path. There is no compute
pipeline creation in this bounded FuxiMetal frontend.

[M9i](metal-m9i.md) already persisted driver blobs; later client-storage changes
placed them under private `<client>-<euid>` directories. Saving depended on
four newly built pairs, one slow pair, or clean teardown. It had no periodic
idle checkpoint, translator artifact persistence, or actual driver-hit metrics.
The C++ pipeline-object LRU and Rust eight-product LRU were process-local.

UniXNU superproject `fb6aad6ab1f50199a4d99d2da1d0ca403b067633`, NativeMetal
gitlink `a96feceda55e8fed1e20c0a2998736cef7aab006`, was read as untrusted text
only. `docs/devices/sheng.md` describes reuse of exported pipeline caches on
later boots. `drivers/NativeMetal/bridge/NativeMetalObjects.rs` configures the
external runtime's shader-cache directory, covering translated artifacts and
driver records. `NativeAdrenoMetalPlugin.mm` tries the user's persistent
Library/Caches before temp directories (boot cleanup had erased the temp
cache), and imports an optional image pack before opening the device.
`CacheExport.h` exports settled cache files through the boot log because its
RAM Data disk loses them. That export/import transport is unnecessary on Fuxi's
writable persistent root. Fuxi instead checkpoints both layers directly there.

This is an original adaptation of that approach; no reference code was copied.
NativeMetal's inspected files declare MIT, independently of UniXNU's
BSD-2-Clause-Patent superproject licence. Existing NativeMetal MIT notices and
the single UniXNU README Credits entry are retained. No Mesa, translator,
NativeMetal source tree, firmware, Apple input or derived shader is vendored.

## Options and compatibility

| Boot argument | Behavior for the Fuxi backend |
| --- | --- |
| Absent | Existing M9i `.cache` persistence and process caches; no new worker, feedback extension or translation snapshot. |
| `fuxigpucache=1` | Two persistent layers, five-second maintenance/report worker, idle/periodic checkpoints, actual driver feedback when supported. |
| `fuxigpucache=0` | No shader-cache file reads/writes; memory caches remain available. Existing files remain intact. |

Malformed or duplicate `fuxigpucache=` tokens act as zero. This option does not
admit clients. Keep the existing `fuxibbgpu=1` and root allow-list admission,
backboardd boot-scoped crash gate, native HostImport and kernel GPU ABI policy.
It applies to `fuximetalbackend=fuxi` (the default); the separately selected
NativeMetal helper keeps its own runtime cache policy.

The bridge stays dynamically loaded, separately replaceable, pinned at
`720ef1ce9ae2a2068999a8675717418b9416283e`, with ABI 8. Rebuild/stage it with
FuxiMetal to get the optional `fuxi_m2v_artifact_scope_v1` capability. Older ABI-8
bridges retain driver caching and log translation-cache unavailability.
Callbacks are synchronous and thread-local, cleared by RAII after compile or
submit, with each Backend owning its store under its mutex. No library/AIR
input, pointer, Vulkan handle or Rust object layout is serialized.

## Storage, identity and failure policy

For the target's root backboardd job:

```text
/private/var/db/fuxi-metal/                         root:wheel 0755
  backboardd-0/                                   root:wheel 0700
    backboardd.pipelines                          root:wheel 0600
    backboardd.translations                       root:wheel 0600
    cache-io.lock                                 root:wheel 0600
```

The existing `gate.lock`/`gate.state` live alongside these files. If launchd
runs backboardd under a nonroot account, the root-owned 0755 parent remains
fixed and the `backboardd-<uid>` leaf/files belong to that actual UID (0700/0600).
Resolve it from the target launchd plist/passwd, never the host account database.
Nonroot clients cannot create the parent. Root-only clients use the same
private per-client layout; no production environment path override exists.

Offline provisioning, on an already mounted test root with matching signed
artifacts and corresponding source, is available through:

```sh
sudo python3 tools/stage-fuxi-metal.py "$IOS_ROOT" "$ICD" "$BUNDLE" "$SMOKE" \
  --translator "$TRANSLATOR" --source "$TRANSLATOR_SOURCE" \
  --prepare-pipeline-cache
```

This pre-creates backboardd storage without editing admission or enabling the
boot argument. The existing `--enable-backboardd` also prepares this storage
while explicitly adding admission. Unsafe existing owners/modes or symlinks
are refused. The writable backing filesystem must survive reboot, be mounted
before backboardd opens its device, and retain these files through root-image
updates/boot cleanup. A RAM root without a persistent backing store cannot do
so. Do not stage caches into build artifacts or commit runtime products.

UID/modes do not bypass a sandbox. backboardd must be allowed to traverse/read
the root-owned parent and read/write/create/rename/unlink files, query metadata
and take a file lock inside its one private directory. Both fsync(file) and
fsync(directory) must work. The pre-existing crash gate already needs access to
this directory. The stager grants POSIX ownership/modes only; it does not edit
Seatbelt profiles or entitlements. Verify that existing research sandbox policy
admits this subpath; if denied, persistence logs unavailable/memory-only and
rendering continues. Do not grant broad filesystem writes to solve a denial.

Driver snapshot v2 uses a 352-byte little-endian wrapper and at most 32 MiB of
Vulkan data. It compares adapter/translator SHA-256, driver version/ID/UUID/info,
and device UUID, then checks the standard Vulkan header's vendor/device and
pipelineCacheUUID, length and CRC32 **before** giving data to the driver. A
seeded-cache refusal retries empty. Turnip's pipeline UUID includes its build
identity/chip/settings (see M9i), so a different driver build is rejected. Legacy
`.cache` format/files are separate and unchanged. The serialized-size query
still retires an over-limit generation; existing live pipelines remain valid.

Translation snapshots have a 128-byte wrapper plus at most 32 MiB of records,
at most 256 pipelines, at most 8 MiB per artifact, evicted least recently used.
The wrapper includes adapter source SHA-256, the external compiler's
`SOURCE_DIGEST`, semantics/codec versions (in that hash), Vulkan API/driver
versions, driver ID and pipeline/driver/device UUIDs. Every artifact is keyed by
the external compiler's `render_variant_key`, which covers functions, constants,
descriptor, variant and profile. Its own versioned codec checks the expected
pipeline key, profile, SHA-256 and module sanity. Decode failure is a miss and
recompile; all Fuxi binding-plan/resource checks still run on loaded artifacts.
Encoding uses a bounded writer. Snapshot decoding never trusts record counts
or lengths before bounding them; duplicate keys and trailing data are refused.

Files must be regular, single-link, 0600 and owned by the effective client UID.
Directory FDs and `openat(O_NOFOLLOW|O_NONBLOCK)` prevent symlink/FIFO tricks.
A nonblocking, validated writer lock serializes checkpoints across processes;
contention defers to a later period. Each store writes a fixed `.tmp` sibling,
fsyncs it, atomically renames, then fsyncs the directory. A later writer removes
that store's interrupted temp under the lock. Stable new-mode snapshots total
at most 64 MiB plus 480 bytes per client. The two fixed temp names (including
crash leftovers) add at most another 64 MiB plus 480 bytes. Legacy files, gate
files and the tiny lock are additional. Concurrent writers may replace each other's learned entries;
they cannot expose partial snapshots. backboardd's existing gate lease admits
one process. No unbounded history or shader journal is created.

The worker runs every five seconds, reports deltas, and saves dirty caches after
at least 250 ms without a compile/submit, or after 30 seconds during continuous
activity. Clean teardown checkpoints and joins the worker before device
destruction. New-mode compilation does not fsync on the pipeline build path.
Driver extraction and bounded IO remain serialized with the Backend; a forced
checkpoint can delay a frame and needs device timing qualification. The existing
mutex/GPU wait can also delay a worker, so 30 seconds is a scheduling target,
not a real-time guarantee. Abrupt death loses work since the last checkpoint;
the host test uses `_exit` after an idle save. Poisoned/failed contexts skip
worker cache operations and join the worker before retaining uncertain GPU
allocations. Cache IO/allocation errors fall back to memory, never fake hits.

## Exact log formats

First cold device open, then a successful checkpoint (N/B/M/T vary):

```text
FUXI MTL: pipeline cache load backboardd bytes=0 result=empty
FUXI MTL: translation cache loaded 0 bytes / 0 pipelines result=ready
FUXI MTL: persistent caches enabled period=5s driver_feedback=1
FUXI MTL: pipeline cache save bytes=N ms=T result=saved
FUXI MTL: translation cache saved B bytes / M pipelines result=saved
FUXI MTL: cache period translation hits=H misses=K rejected=R
FUXI MTL: cache period driver hits=H misses=K unknown=U objects hits=O misses=P
```

Next boot with matching identities:

```text
FUXI MTL: pipeline cache load backboardd bytes=N result=hit
FUXI MTL: translation cache loaded B bytes / M pipelines result=ready
FUXI MTL: persistent caches enabled period=5s driver_feedback=1
```

The translation byte count includes record framing, excluding its wrapper;
M counts compiled artifact records, including variants. Vulkan cache data is
opaque: it has no portable pipeline-count query. A loaded driver blob alone
does **not** prove any hit. Driver hits/misses count successful creation calls
with `VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT`; hits additionally require
`APPLICATION_PIPELINE_CACHE_HIT_BIT`. Missing feedback increments `unknown`;
unsupported devices log `driver_feedback=0`. Object counters are Backend-local
LRU deltas, distinct from compiler/driver hits. Translation hits require a
successful artifact decode; rejected artifacts increment both misses/rejected.
Reports are per five-second maintenance period (plus final teardown).

An identity mismatch logs `result=discard(identity)` and recompiles. Other
rejections name the fixed reason (`checksum`, `length`, `permissions`, etc.).
Cache-write failure logs `result=memory-only`. Missing storage logs
`FUXI MTL: translation cache unavailable (storage)`; an old bridge logs
`FUXI MTL: translation cache unavailable (bridge capability)`. Explicit zero
logs `FUXI MTL: disk caches disabled fuxigpucache=0`; no new-mode worker runs.
Existing `backboardd first_frame_ms=... gpu_ms=...` and five-second frame/late/
failure logs remain the device performance evidence.

## Host checks and limits

```sh
python3 tools/test-fuxi-metal-cache.py --source "$M2V_SOURCE" --build "$CACHE_BUILD"
python3 tools/test-fuxi-metal-m9i.py --translator "$CACHE_BUILD/target/debug/libmetal2vulkan.so"
python3 tools/test-fuxi-metal-m9o.py --translator "$CACHE_BUILD/target/debug/libmetal2vulkan.so"
python3 tools/test-fuxi-metal-backend.py --headers "$VULKAN_HEADERS"
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-stage.py --policy-storage-only
python3 tools/test-fuxi-metal.py
python3 tools/sync-fuxi-metal.py --check
python3 tools/sync-fuxi-metal2.py --check
python3 tools/sync-fuxi-adreno-shared.py --check
```

The new test verifies clean external pin, builds/tests the separate Rust bridge
offline, compiles production C++ under ASan/UBSan/-Werror, and renders authored
AIR through real lavapipe. It checks idle save before abrupt exit, fresh-process
reload with identical pixels, actual artifact hits, malformed/checksum/identity/
permission/link rejection, cap/eviction/orphan replacement, disable and storage
fallback. Existing M9i covers seeded-driver rejection, corrupt Vulkan headers,
query/size caps, concurrent atomic saves and loss/timeout gate behavior. No
kernel or Mesa source/patch changes are needed; existing sync checks must pass.
The commands above passed, as did M9g/M9h and the backend fault harness with
`FUXI_MTL_TEST_BOOTARGS=fuxigpucache=1` and an unavailable translator (including
worker shutdown after timeout/device loss). The seven Rust adapter unit tests,
Python compilation, shell syntax and whitespace checks passed. Native
Objective-C tests were skipped on Linux, as the frontend harness reports.

Lavapipe can return a header-only driver blob and does not establish reusable
Turnip machine code or A740 application-cache hits. Mac/iOS compile/link/sign,
Seatbelt access, persistent HFS+ fsync, real reboot reload, driver UUID behavior,
first-frame speedup, frame stalls/power and UI pixels remain unverified.

## Device plan — not executed

1. Later, build/sign matching FuxiMetal and external ABI-8 translator with the
   existing recipes; retain corresponding sources/notices. Stage into a separate
   mounted test root using `--prepare-pipeline-cache` (or the already authorized
   `--enable-backboardd`). Check target account/modes, sandbox access and writable
   persistent backing. Keep kernel, Turnip, admission, clocks and all other args
   identical between runs; do not enable unrelated clients.
2. Control boot with `fuxibbgpu=1 fuxigpucache=0`: record first-frame time and
   several minutes of frame/GPU/late/failure counters for a repeatable Settings,
   home-screen, scroll, lock/wake workload. Require the disable line.
3. Cold-cache boot with `fuxigpucache=1`: require the cold-open lines above,
   successful compilation/pixels, then save lines after idle (or periodic
   checkpoint during animation). Record bytes/pipelines and driver misses. Wait
   for a successful save before reboot; retain that root's cache files.
4. Reboot the same image/root with the same argument and repeat exactly that UI
   sequence. Require nonzero loaded bytes/artifact count, translation hits and
   actual driver application-cache hits. Known shaders should have fewer misses;
   new UI variants may still compile. Compare first-frame and first-minute late
   ratios against control/cold boot, including checkpoint timing. Reject stale
   pixels, black/incorrect composition, faults, failures or a new frame stall.
5. After a translator/driver rebuild, require a safe identity discard and
   rebuilding, followed by reuse on the next identical boot. Test corruption and
   abrupt mid-write faults on the host only. Roll back by using
   `fuxigpucache=0`; removing the argument restores the legacy cache policy.
