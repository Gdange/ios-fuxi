# GPU K1: NativeAdreno kernel compatibility

K1 supplies the kernel portion of the NativeMetal migration. It follows the
MIT-licensed UniXNU NativeAdreno implementation and the Turnip Darwin KMD at
reference revision `fb6aad6ab1f50199a4d99d2da1d0ca403b067633`. Credit belongs to
the UniXNU NativeAdreno contributors, especially the user-client, timer and
status contracts. Existing license attribution is in
`xnu/fuxi-adreno/LICENSE.NativeAdreno`. This ports behavior into the Fuxi driver;
it does not vendor UniXNU or Mesa sources.

## Behavior

- Method 7, GetTimestamp: no input, an 8-byte `NativeAdrenoAbiTimestamp` reply.
  Read CP_ALWAYS_ON_COUNTER at A6XX/A7XX GPU dword indices 0x0980/0x0981 using
  high/low/high, retrying at most eight times across rollover. Return raw GPU
  ticks, independent of CPU time and submission fence sequence. Disabled GPU
  returns NotReady, MMIO failure returns IOError and contains the fault, and
  persistent rollover returns Timeout. Failure never fabricates ticks.
- Method 8, GetStatus: no input, a 16-byte `NativeAdrenoAbiStatus` reply with
  64-bit `faults` then `suspends`. Counts are boot-wide, serialized with the GPU
  ring, saturate at UINT64_MAX, and survive client close. The first permanent
  failure after ready increments faults, including kernel-present failures,
  client GPU/SMMU faults and the independent 100 ms watchdog. Startup failures
  before ready do not represent a lost context. A successful containment-reset
  register write increments suspends once; an unconfirmed AXI drain still keeps
  DMA pages quarantined. There is no normal suspend/resume or recovery yet.
  Status observes pending work and idle faults; after permanent failure it
  returns the recorded counts successfully without touching MMIO. Turnip marks
  the device lost when faults changes. Repeated polls cannot hide that change.
- The BO table grows from 64 to 1024 entries, the shared allocation/import wired
  quota from 256 to 768 MiB, and the backend mapped-import table from 64 to 512.
  The 256 MiB per-object limit is unchanged. Ordinary backing is charged after
  16 KiB rounding; imports charge their full aligned length. Quota subtraction,
  table bounds, token exhaustion and handle exhaustion are checked before
  allocation. Close still attempts every live entry, waits for DMA retirement,
  and releases prepared descriptors/maps/backing, or preserves their references
  on an unconfirmed drain. Imported cache handoff traverses all 512 slots.
- GetMetadata returns BadArgument when requested capacity is smaller than the
  stored metadata, matching NativeAdreno. No truncated success reply is issued.
- CPU unmap with no lease keeps BadArgument. NativeAdreno returns Busy there,
  but Turnip HEAD retries Busy only from deferred BoFree (and submission); any
  deferred CPU lease-release error becomes device loss. Its failed CPU-map
  rollback also does not retry. Changing this error would not enable a KMD
  retry, so K1 retains the existing invalid-lease behavior. Valid lease release
  and BoFree's Busy response while leases exist remain unchanged.

ABI version remains **2**, host-import alignment remains **16 KiB**, and all
wire sizes/selectors remain unchanged. The single-connection guard, root
admission, exec-swap diagnostics, and patch 0102's opt-in platform-backboardd
admission are preserved. Multi-client scheduling belongs to M1.

## Patch synchronization

Canonical sources live in `xnu/fuxi-adreno/`. Patch 0098 embeds new ABI/backend
files (including copies of shared headers in both BSD and IOKit directories)
and deltas over 0096's shared driver snapshots. Its user-client snapshot is
intentionally frozen; patch 0100 carries the delta to the canonical client.
Patch 0102 applies the separate admission policy afterward.

After edits run, in order:

```sh
python3 tools/sync-fuxi-metal.py
python3 tools/sync-fuxi-metal2.py
python3 tools/sync-fuxi-adreno-shared.py --check
```

`test-fuxi-metal.py` applies 0098 and 0100 with the real patch tool, checks all
embedded source bytes against canonical files, then verifies that unchanged
0102 still applies without fuzz.

## Local validation

Passed on Linux:

```sh
python3 tools/test-fuxi-gpu-k1.py
python3 tools/test-fuxi-hostimport.py
python3 tools/test-fuxi-metal.py
python3 tools/test-fuxi-adreno.py
python3 tools/test-fuxi-metal2.py
python3 tools/test-fuxi-metal2g.py
python3 tools/test-fuxi-metal2h.py
```

The host models execute extracted production methods under ASan/UBSan. Coverage
includes 1024 ordinary allocations, full-table refusal/reuse/close, 1024 imports
and close, three 256 MiB imports reaching the exact 768 MiB quota, refusal beyond
it, rounding and shared-quota checks, handle/token exhaustion, all 512 mapped
import slots with cache retirement/unmap, metadata refusal, method 7/8 lengths,
rollover/retry/error paths, idle faults, watchdog faults with successful and
failed drains, counters after loss and saturation. Existing present, retained
CPU-map, import-unwind, exec-swap, trace-cap and quarantine regressions pass.
Models do not prove real CP execution, cache coherence or kernel allocation.

The Linux `tools/xnu-cc.py` replay was attempted, but the available build log
contained no compile commands; it reported `no compile commands matched`.
A real XNU compile remains outstanding. The complete device helper also passed a C++ syntax check with minimal Mach
declarations, without SDK linking. The iOS test binary build/sign step
requires macOS and its SDK and was not run here. No phone was contacted and no
remote CI was run.

## Device-test plan (not executed)

1. Build the kernel with the synchronized patches and build/sign the updated
   raw tool using `bash tools/build-fuxi-gpu-abi-test.sh` on the normal Mac build
   host. Stage it through the existing reviewed artifact process. Use a fresh
   boot with the ABI enabled and no competing GPU client.
2. Run `fuxi-gpu-abi-test` as unsandboxed root. Expect advancing GPU timestamps,
   a readable status baseline, short-metadata refusal/full-metadata success,
   CP_MEM_WRITE and CP_BLIT readbacks, unchanged healthy counts, close and a
   second successful open/round. Existing exec-swap host-argv handling remains.
3. On another fresh boot explicitly run `fuxi-gpu-abi-test --k1-limits`. It adds
   1024 simultaneous 16 KiB BO allocations, overflow refusal and release, then
   512 simultaneous mapped 16 KiB host imports and release. Backing peaks at
   16 MiB in these separate phases. Confirm the second normal round still passes
   after close/reopen and no prepared-page leaks or mapping warnings appear.
   The exact 768 MiB accounting boundary is covered by the host model; real
   large-allocation behavior remains a separate device validation item.
4. On a separate fresh boot, explicitly run `fuxi-gpu-abi-test --watchdog`.
   Expect WaitFence Timeout and a successful GetStatus reporting increased
   faults on the still-open connection. Record the hang snapshot, drain result,
   reset count, safe BO cleanup/quarantine, and continued CPU presents. Do not
   expect the permanently disabled GPU to reopen successfully on that boot.
5. With the newer Turnip, exercise a workload that accumulates its 256 deferred
   frees, retires them, destroys the device and reopens. Confirm no old 64-BO or
   64-import exhaustion and that status detects an injected hang as device loss.
   Recheck root/backboardd admission and competing-open refusal under the same
   boot arguments as the pre-K1 baseline.
