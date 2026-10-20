# A740 in XNU: milestone 6 implementation and acceptance

Milestone 6 is implemented in `xnu/patches/0096-fuxi-adreno.patch` and built
natively against the integration XNU 8796 tree (0093, 0095 and 0092 included).
**XNU hardware acceptance is pending.** Loader milestones 1–5 have passed on
the coordinator's device; that evidence does not prove the XNU mappings,
cache transitions, ring reuse or failure isolation. This replaces the earlier
migration proposal that predated the successful PAS/secure-exit device result.

## Ownership and physical memory

`tools/mkadt-fuxi.py --gpu-reserve` changes ADT `/chosen/dram-size` to
`0x30000000` and adds `fuxi-gpu-memory=<0x8f0000000,0x02000000>`.
The payload builder uses that ADT geometry; the loader copies it into both
`boot_args.memSize` and `memSizeActual`. Thus VM sees
`[0x8c0000000,0x8f0000000)` (768 MiB). This deliberately gives up the entire
last 256 MiB of the previous 1 GiB window; only its first 32 MiB is GPU scratch.
No reliance on a Linux reserved-memory binding or late removal of free pages.
An ordinary, non-reserved payload remains supported and GPU defaults off.

The loader checks segment/ADT/boot-argument bounds and reservation overlap with
its image and live FDT before staging anything. It initializes **no GPU** at
handoff. Its existing stage9/start latch still refuses request49 after a probe
has started GMU; the monitor's existing launch gate stops GPU requests once
XNU launches. Cold preconditions and SID ownership checks also reject a partly
initialized probe. Never run loader stage8+ before an XNU GPU test.

| Physical address/range | Retained use |
| --- | --- |
| `0x8f0000000..0x8f0100000` | GMU page-table pool |
| `0x8f0100000..0x8f10fc000` | GMU instruction backing |
| `0x8f1100000..0x8f1111000` | GMU data/log/HFI/debug |
| `0x8f1200000..0x8f1300000` | GPU page-table pool; source L3s at pool+`0x10000..0x18000` |
| `0x8f1300000..0x8f132a000` | SQE, 32 KiB ring, shadows and fences |
| `0x8f1400000..0x8f1542000` | Scratch clear/copy self-test and guards, including gaps |
| `0x8f1600000..0x8f1602000` | Dedicated contiguous PAS metadata, never reused after init |
| `0x8f1800000..0x8f1804000` | Versioned cold firmware handoff |
| `0x8f1804000`, `0x8f1904000`, `0x8f1a04000` | GMU/SQE/zap slots, each at most 1 MiB |
| `0x9b09a000..0x9b09c000` | Existing Fuxi TZ zap reservation, outside XNU RAM |
| `0xb8000000..0xb89e3400` | Existing live 1080×2400 scanout, pitch4320 |

XNU requires the ADT reservation, verifies `gPhysBase/gPhysSize`, validates the
handoff and each firmware size/FNV1a checksum, then maps the reservation with
`ml_io_map` (Device/uncached). It creates no cached alias. FNV detects transfer
corruption; it is not authentication. Reserved-payload builds SHA256-pin the
three exact device-verified firmware files. No firmware bytes are tracked.
The existing framebuffer mapping is reused by the CPU; GPU writes use its PA,
without a second CPU scanout mapping.

## Shared bring-up and the kernel boundary

`shim/fuxi-probe/gpu.c`, `gpu-a740.h`, `gpu-zap.h`, and `gpu-handoff.h` are the
canonical shared source. Patch0096 contains exact snapshots so it applies to
an external XNU tree without repository-relative build dependencies.
`tools/sync-fuxi-adreno-shared.py --check` enforces equality, including the
canonical adapter/API in `xnu/fuxi-adreno`. Run the script without `--check`
after changing those files, then rebuild both targets.

The shared code retains clocks/GDSC → GMU SMMU → firmware/HFI → GX → GPU SMMU
→ SQE bases **before RUN** → CP init/fence → one-shot PAS13 →
`CP_SET_SECURE_MODE 0` → scratch solid clear and patterned copy, including
pixel/source/guard validation. PAS query booleans remain advisory; operational
transport and service errors fail. XNU calls the same SMC sequence from EL1,
using the physical uncached metadata address. Stage15's panel test is not run
inside XNU: panel writes begin only with real presents after readiness.

`AppleCLCD::start`, after registration, starts a kernel thread which sleeps
two seconds before bring-up. CPU presents continue during initialization.
The release/acquire readiness flag publishes all initialized state. There is
one display workloop submitter; no raw PM4/user-client API or competing GPU
owner, interrupts, runtime collapse, suspend/resume or retry path.

All register accesses use mapped apertures. A private aligned32/64-bit assembly
routine joins XNU's copyio recovery table. A narrow FUXI exception-handler check
also recovers precise external aborts **only within that routine**, since generic
copyio recovers VM/alignment faults but otherwise panics. Scratch accesses are
ordinary volatile uncached accesses to the verified reservation. Bring-up
failures log stage/result codes and permanently select CPU copying; memory
and power votes remain retained until reset.

## GPU domain and each present

SID5/GMU and SID0/1/GPU selection is inherited from the probe's snapshot,
masked-SID collision and live ownership checks; the observed CB1/CB2 and ASID1/2
are not forced. CB0, SID2 and apps-SMMU remain untouched.

The GPU domain maps only its SQE/ring/shadow/fences, scratch test surfaces,
the scanout's rounded4KiB span at IOVA`0x41000000`, and transient source pages
inside IOVA`0x50000000..0x51000000`. The source is read-only, destination RW;
all are Normal NC in the GPU domain. Eight fixed L3 pages cover the source
aperture. Only4KiB pages intersecting copied rows get leaves, so padding-only
pages and unrelated surfaces stay unmapped. Page sharing within4KiB is inherent.
No identity mapping of XNU RAM is installed.

For an eligible linear BGRA IOBuffer surface, the caller retains and prepares
the descriptor. XNU obtains actual physical pages from its wired kernel VA
with16KiB `pmap_find_phys` pages and4KiB offsets; physical contiguity of a swap
surface is not assumed. It cleans the CPU-written source span to PoC, publishes
uncached PTEs and synchronizes the selected GPU ASID. Unsupported surfaces,
formats or bounds use the existing CPU path without disabling the GPU.

Every row uses its own64-byte-aligned source/destination base and X phase.
Neither4320-byte pitch is truncated to64-byte units. At most64 rows are
submitted per batch, with the shared BR/CCU setup, resolve-clean, CACHE_CLEAN
fence and WFI. A batch is smaller than the8192-dword ring; modulo stores and
WPTR permit wrap, and the preceding fence/rptr/wptr/idle gate proves the ring
retired before any reuse. Full2400-row frames take38 batches. A50ms frame
counter deadline and5000-iteration/1us per-batch polls bound completion waits.
After the final fence, source leaves are cleared and TLBSYNC finishes before
`complete`/release. Successful fences include GPU cache writeback.

A present failure permanently disables submissions. The failure path requests
GBIF client then AXI-arbiter halt with bounded ACK waits, holds GPU reset and
faults the two owned GPU streams. CPU copying resumes. If draining fails, the
prepared source descriptor/reference is deliberately leaked until reset so DMA
cannot reach freed VM pages. **Missing drain ACK still leaves a hardware risk
of late scanout writes; isolation is not device-proven.** There is no same-boot
recovery attempt or release of scratch. Frame statistics report GPU and CPU
present counts and average wall times every600 frames; mixed frames count in
both groups; no-copy frames count in neither. GPU time includes pin/map/cache/submit/wait work; CPU time covers
its copy. The existing aggregate logger retains full present time/max/drops.

## Acceptance and remaining boundaries

Use the companion CPU, stage13, stage16 and full-present images documented in
`gpu.md`. Fresh boot for every step; the loader probe must stay cold.

1. GPU off: confirm768MiB RAM, unchanged GUI/touch/root operation, CPU statistics
   and allocation stress. Separately, old1GiB payload + GPU off must remain usable.
2. `fuxigpu=1 fuxigpu_stage=13`: require through13 PASS, valid fence, no defined
   CP/SMMU faults, then the intentional diagnostic stop and CPU-only presents.
3. `fuxigpu=1 fuxigpu_stage=16`: require PAS transport/service0, advisory support
   false accepted, TRUST0 and scratch clear/copy PASS, then diagnostic stop.
4. `fuxigpu=1`: require ready, visible full-panel/cropped updates and increasing
   GPU counts for600+ frames. Compare copy times and thermal behavior with CPU.
   Check ring wrap, multiple surfaces/layers, display off/on and long idle.
5. Fresh diagnostic boots: `fuxigpu_fail=1` stops after scratch validation;
   `fuxigpu_fail=2` injects failure immediately after the first present doorbell.
   Require permanent fallback, drain ACK/reset/stream-fault logs, continued CPU
   updates and no further GPU submissions. Do not resume a failed GPU.

Offline tests cover real emitted commands, page permissions/translation,
noncontiguous sources, full-panel/cropped row phases,1000 frames/ring wrap,
cache-clean invocation, malformed bounds, missing pages/TLBSYNC/fence/fault/
deadline/access failures, failed-drain quarantine and no retry. They do not
execute real SQE, SMMU, cache hardware or secure firmware.

A synchronous SMC has no cancellable EL1 timeout: the implementation calls each
service once with no retry/WAITQ continuation, logs entry/return, and handles
returned errors. A firmware call that never returns, interconnect lockup or
asynchronous SError cannot be made recoverable by these polling bounds. XNU
power/performance/thermal behavior, bus isolation and repeated ring use require
device evidence. This offloads framebuffer copies only; it is not AGX, Metal,
GLES or general CoreAnimation rendering acceleration.
