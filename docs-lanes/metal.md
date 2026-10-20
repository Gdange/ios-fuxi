# Metal lane, milestone 1: NativeAdreno userspace ABI

2026-10-20, branch `codex-metal1`, based on `fuxi-bringup` `8dae9f4`.
**Native build and offline verification pass. No device execution is claimed.**
No phone access, push, firmware redistribution or Apple binaries in git.
`AGENTS.md`, `NOTES.md`, `FUXI.md`, patch 0097 and `xnu/fuxi-perf` are unchanged.
The only Mac build tree used was `ci-lane-metal`, cloned from `ci-lane-gpu6`.
The 27 native source inputs match the clean 0097b + 0098 patch result;
`native-inputs.sha256` and `native-inputs-check.log` accompany the kernel.

## Implemented contract

Apply `0098-fuxi-native-adreno-abi.patch` after 0096; 0097 is unchanged.
Canonical sources are in `xnu/fuxi-adreno`. `sync-fuxi-metal.py` generates
0098, and `sync-fuxi-adreno-shared.py --check` checks the combined snapshots.
0096 remains independently applicable. The default-off driver behavior and
loader addresses remain unchanged when `fuxigpuabi` is absent or not 1.

Both `fuxigpu=1` and `fuxigpuabi=1` are required. The service is an ordinary
`IOService` subclass, **NativeAdrenoGpuPublished**, registry name
`NativeAdrenoGpu`, accepting open type **0x4147** (also type 0 for Turnip's
direct backend). Only an administrator/root connection is admitted. There is
one live connection, recycled on close/death; no sandbox tunnel or mailbox.
For the unmodified Turnip patch use:

```sh
export TU_DARWIN_SERVICE=NativeAdrenoGpuPublished
export TU_DARWIN_OPEN_TYPE=0x4147
export TU_DEBUG=nocb
```

All 16 direct selectors and fixed struct lengths match NativeAdrenoABI v1.
Query, GetDeviceInfo, BoAlloc/Free/Map, BoCpuMap/Unmap, Submit and WaitFence
are implemented. Metadata and queue selectors are implemented as well.
GetTimestamp, GetStatus and BoImport are unsupported. Query does not advertise
HostImport or cached coherence. Queue 0 exists at open; extra queue IDs share
the same serialized hardware queue.

Limits: 64 BOs, 256 MiB per BO and 256 MiB live BO allocation budget per connection,
16 IBs per submission, one hardware job in flight. Allocation tries contiguous
wired memory first, then scattered wired pages unless contiguity is required.
It honors alignment 12..24 and constrains physical addresses to 36 bits.
Backing is rounded/zeroed to 16 KiB, while GPU mappings cover the requested
4 KiB-multiple length. BoCpuMap reports the actual CPU view length.
`clientMemoryForType` returns a stable physical-range descriptor for its token.
Both kernel and user BO mappings use Normal NC/write-combine. The descriptor
retains the prepared backing through its last CPU mapping, including forgotten
unmaps. Correct users unmap memory, release the CPU token, then free the BO.
Unreleased CPU mappings can retain memory until connection/task teardown.

The client has a separate 4 KiB SMMU root in the final 4 MiB of reserved
scratch (offset 28 MiB). Static bounds and the host model keep it beyond
the retained zap metadata, cold handoff and all three firmware slots.
Its full aperture `[0x20000000, 0x80000000)` is available; overlapping mappings
are rejected. The original scanout and four present-source windows stay in
the present root. With the ABI enabled only, runtime SQE/ring/pointer/fence
IOVAs move below the client aperture (`0x10000000` onward), identically in
both roots. TTBR switching on the existing GPU bank happens only with an idle
ring; ASID invalidation/TLBSYNC precedes a new doorbell and backing release.
No GMU bank, SID ownership or present PTE is repurposed for BO leaves.

A shared mutex serializes ring/control/PTE operations with present calls;
BO allocation and scheduler waits do not hold it. While a client owns the
ring, presentation uses its existing CPU copy. On successful client retirement
the present root is restored and GPU presents become eligible again.
Submission wraps client IBs in CP_INDIRECT_BUFFER with BR sync, context reset,
PERFCTR SRAM-init status wait, GPU cache flush/invalidate, BOTH threads and
IFPC off. Completion writes BV timestamp, waits on BR, writes the BR timestamp
with the IRQ bit, restores BOTH/IFPC and waits for idle. Context reset is emitted
on every return from the present context, including the first client submit.
RBBM interrupt delivery remains masked; no GIC/IRQ completion is claimed.

WaitFence sleeps in 1 ms slices and returns `kIOReturnTimeout` for a user or
hardware deadline. Zero timeout observes once. An independent delayed callback
checks every 2 ms, even when no client calls WaitFence, and declares a pending
job lost at 100 ms. Completion observed after the deadline wins if the ring is
already retired. Scheduling can delay observation beyond these intervals.
A client fault/hang permanently disables GPU submission for this boot, drains
GBIF, holds reset and faults SID0/1, allowing CPU presentation to continue.
Failed drain quarantines the wired BO references until reset. There is no
attempt to recover the CP or reset it beneath active UI work.

## Chip facts and reference provenance

References read: UniXNU `7b52e7968c76453cdc18b5aa174fc368d5293b91`,
UniXNU-sheng `8a0c9ddab92038a50abc07fffd4882ec4e301f45`, including
`NativeAdrenoABI.h`, `NativeAdrenoRing.cpp` and `GpuPowerProbe.cpp`.
MIT ABI/framing notices are retained in `LICENSE.NativeAdreno`; README credits
UniXNU. The sheng board code is BSD-2-Clause-Patent; it is a reference here.

| Field | Value and basis |
| --- | --- |
| chip_id, GMEM | `0x43050a01` / C510v2, 3 MiB, catalog entry using `a740_sqe.fw`, `gmu_gen70200.bin`, `a740_zap.mdt`; matches our previously qualified A740 firmware family. This milestone performs no fresh phone chip-ID read. |
| GMEM base / trap | `0x1000000` / `0x1fffffffff000`, exactly the UCHE values our existing bring-up programs; ABI publication checks their live readback and GMEM end `0x12fffff`. |
| HBB / swizzle / macrotile / UBWC | `16 / 6 / 1 / 4`: SM8550 platform configuration, encoder 4.0 (decoder is 4.3). Existing NC values are `0x816 / 6 / 0x26 / 0x60`; ABI init also initializes BR/BV GRAS banks. HBB UCHE bits are checked. |
| VA | start `0x20000000`, size `0x60000000`, ClientIova flag; driver policy, independently reserved in the client root. |
| Query widths | input 31, **output 36**. Recorded Fuxi SMMU ID2 is `0x5511`: OAS code 1 means 36 bits. The existing present mapper also rejects PA >= 2^36. UniXNU's Query advertises its own 40-bit allocation ceiling; we use the observed hardware limit. Turnip does not require 40. |

The pinned [Linux A740 catalog](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/gpu/drm/msm/adreno/a6xx_catalog.c)
was independently hash-checked (`ff1abc1b7735d6bd6aa07d5df776e2549be2ee70ffcc02ff24aac646dfd6a3c7`).
The same tree's [UBWC configuration](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/soc/qcom/ubwc_config.c)
and [SMMU ID decoding](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/iommu/arm/arm-smmu/arm-smmu.c)
support the remaining configuration facts. Device-specific coherency still
requires execution; the catalog's coherent-capability quirk is not proof that
our XNU BO mappings are coherent.

## Raw test, build and coordinator procedure

`tools/build-fuxi-gpu-abi-test.sh` was run on the Mac using the installed
**iPhoneOS 27.1 SDK**, targeting **arm64-apple-ios16.0**, then ad-hoc signed.
The binary's LC_BUILD_VERSION is iOS 16.0. It dynamically resolves IOKitLib
because the public iOS SDK omits its headers. The script attempts the macOS
SDK with the same iOS target only if the iOS SDK is absent; that fallback was
not used or qualified in this run. Binaries go to gitignored `tools/out`.

The normal test waits at most 60 s for the service, opens 0x4147, queries,
allocates/maps a 64 KiB BO, writes an IB with CP_MEM_WRITE plus a 16x16 BGRA
solid CP_BLIT, submits, waits and verifies `0xa740cafe` and all 256 pixels
`0xff336699`. It then unmaps/frees/closes and repeats after reopening, exercising
address reuse and the enumeration/open-close pattern. Every step prints
PASS/FAIL to stdout; final success is `PASS: fuxi-gpu-abi-test`, exit 0.

The least invasive launch method is a dedicated root one-shot job in the
existing unsecure launchd cache, using the same plist editing mechanism as
`sweet-root-tweaks.py`. On an **already mounted writable root image**, the
coordinator can stage the tool offline:

```sh
sudo python3 tools/stage-fuxi-gpu-abi-test.py "$IOS_ROOT" tools/out/fuxi-gpu-abi-test
# After qualification, remove only this binary/job:
sudo python3 tools/stage-fuxi-gpu-abi-test.py "$IOS_ROOT" --remove
```

This copies to `/usr/local/bin/fuxi-gpu-abi-test`, writes
`/Library/LaunchDaemons/org.fuxi.gpu-abi-test.plist` and adds that job to
`/System/Library/xpc/launchd.plist`. It preserves other jobs, uses root without
a sandbox profile, RunAtLoad=true, KeepAlive=false, and stdout/stderr
`/dev/console`. No existing daemon or SpringBoard injection is needed. Keep
the existing root-image signing/AMFI policy; the tool is ad-hoc signed and no
new trust-policy bypass is added by this lane. The helper never mounts an image,
accesses USB or installs anything on a phone.

Coordinator acceptance, serially:

1. Install/stage through the existing root-image workflow and use
   `shim/out/fuxi-metal1-boot.img` through the coordinator's normal temporary
   boot procedure. Look for `FUXI GPU ABI: NativeAdrenoGpuPublished v1`.
2. Capture the complete PASS/FAIL output, exit status if available, GPU logs
   and present statistics. Require both normal rounds, reopening and readbacks
   to pass; exercise touch/app switching while presentation remains alive.
3. On a separate boot, stage with `--watchdog`. This deliberately submits an
   unsatisfied CP_WAIT_REG_MEM. Require `watchdog returns timeout`, a
   `client fence deadline (100 ms)` / GBIF containment log, continuing UI/CPU
   presents, and refusal of later client work. Failed drain should quarantine
   BOs and may make the free check fail; report it rather than treating it as
   safe reclamation. A reboot is needed to restore GPU submission.
4. Remove the diagnostic job afterwards. Test default-off by omitting
   `fuxigpuabi=1` from a separately packed image: no ABI service should appear
   and the existing present path should remain usable.

To reproduce the image, build the patched XNU in `ci-lane-metal` with
`tools/build-fuxi-native.sh` and the matching IIG cache, copy stripped/unstripped
kernels into `xnu/out-fuxi-metal1`, then supply the existing private toolchain
and GMU/SQE/zap environment variables:

```sh
bash tools/build-fuxi-metal-images.sh "$BASE_0097B_ARTIFACTS"
python3 tools/test-fuxi-metal.py
python3 tools/test-fuxi-adreno.py
python3 tools/test-fuxi-memory.py
python3 tools/test-fuxi-scm.py --kernel xnu/out-fuxi-metal1/kernel.development.fuxi.unstripped
python3 tools/test-fuxi-metal-artifacts.py --base "$BASE_0097B_ARTIFACTS" \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
sha256sum -c xnu/out-fuxi-metal1/SHA256SUMS
```

Offline coverage includes fixed layouts/selectors and overflow/flag checks;
an independent PM4 decoder checking reset/SRAM wait, parity, IFPC and BV/BR
ordering; the actual C backend compiled under ASan/UBSan with modeled register,
SMMU and scheduler interfaces; present exclusion and late fences; watchdog with
no WaitFence calls, faults and failed drain; clean patch application; existing
pixel/cache/lifetime regression models; native recovery/SCM instructions;
boot-v4/Image/payload/full-RAM bounds, unchanged 0097b ADT/root/CPU identity and
exact command line plus ` fuxigpuabi=1`; exact external firmware blobs; signed
raw Mach-O target. These are host models/build checks, not GPU execution.

## iOS 16.7 Metal discovery, re-derived

Facts from the supplied iPhone10,3 / 20H392 arm64 shared cache, inspected with
`ipsw dyld disass`, `macho --symbols`, `macho --objc` and `dump`. Main cache
SHA256: `76874e8375252ee643011ff8063f0cce12355f05cf5f9897a34e5679667a65aa`.
Addresses below are unslid and specific to this cache.

| Observation | Evidence |
| --- | --- |
| `_MTLRegisterDevices` at `0x181b144c8` matches `IOAcceleratorES` | String/matching at `0x181b14514..0x181b1452c`; iteration calls `_getMetalPluginClassForService`, `initWithAcceleratorPort:` (`0x181b14580`), then `_MTLAddDevice` (`0x181b1458c`) for a non-nil result. |
| Plugin properties and bundle lookup | `_getMetalPluginClassForService` at `0x181a3c7c8` reads `MetalPluginName`, builds `/System/Library/Extensions/<name>.bundle`, reads `MetalPluginClassName` or falls back to principalClass; checks `isSubclassOfClass:_MTLDevice` at `0x181a3c8d0`. |
| `_MTLDevice` instance size is **528**, not 536 | Class symbol `0x1e1d88a68`, decoded class data at `0x1d59e51a8`; class_ro fields flags=4, instanceStart=8, instanceSize=`0x210`. This must also be checked with `class_getInstanceSize` before constructing a runtime subclass. |
| Stock IOGPU open type | `_IOGPUDeviceCreateWithAPIProperty` at `0x1ece1ce38` constructs `1 | (api << 16)` at `0x1ece1ce68..0x1ece1ce80`, then calls IOServiceOpen. Our 0x4147 ABI is a different connection contract. |
| Kernel family availability | The linked metal1 kernel has the new native service metaclass and ABI symbols, and no `IOAcceleratorES` or `IOGPUFamily` implementation/metaclass symbols. This standalone payload does not include Apple's IOGPUFamily. Publishing an ordinary service alone cannot create an MTLDevice. |

Reproduce disassembly without copying any Apple code into git:

```sh
"$IPSW" dyld disass "$DSC" --symbol _MTLRegisterDevices --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _getMetalPluginClassForService --symbol-image Metal
"$IPSW" dyld macho "$DSC" Metal --symbols
"$IPSW" dyld dump "$DSC" 0x1d59e51a8 --size 24
"$IPSW" dyld disass "$DSC" --symbol _IOGPUDeviceCreateWithAPIProperty --symbol-image IOGPU
```

**Unverified:** iOS 16 sandbox selector/class allowances, private IOGPU object
vtable layouts, root-image plugin signing/loading and CoreAnimation's complete
requirements. Do not import UniXNU's iOS 17 IOGPU size 736 or `_MTLDevice` size
536 into this kernel. Its published plugin returns nil; its raw IB success does
not establish Turnip or Metal success. Its documented Turnip IBs still hang CP.

## Remaining milestones and acceptance gates

1. **M1 device qualification:** normal raw IB/readback/reopen, present coexistence
   and watchdog containment above. Current status: built and offline-tested,
   awaiting coordinator execution.
2. **M2 Turnip cross-build for iOS 16:** follow UniXNU
   `scripts/build-turnip-darwin.py` and `scripts/data/adreno-compiler-inputs.json`.
   The actual build pin is Mesa **85fda3b36405bcaea8bffc64aa4f2651dc90770d**;
   `gpu-metal-vulkan-inputs.json` also records **c823a83e...** as a reference
   survey tree, not the revision the build script checks. Apply, in manifest
   order: `freedreno-tools-nir.patch`, `ir3-darwin-build-id.patch`,
   `ir3-portable-asm-roundtrip.patch`, `offline-adreno-compiler-probe.patch`,
   `turnip-darwin-kmd.patch`. Retain MIT notices; keep source/build caches out
   of git. Change the cross target from ios17.0 to **arm64-apple-ios16.0**.
   Use iPhoneOS SDK, Meson `freedreno-kmds=darwin`,
   `tu-darwin-abi-dir` pointing at our ABI header, `vulkan-drivers=freedreno`,
   `platforms=macos`, no Gallium/GL/EGL/GBM/LLVM, and native glslang/build tools.
   Audit iOS availability, Mach-O load commands/dependencies, cache-line helpers
   and absence of EL0 CTR_EL0 instructions; the supplied patch now substitutes
   Darwin's 64-byte cache-line value. Require exported vk_icdGetInstanceProcAddr
   and a root-daemon load/enumeration before submitting shaders.
3. **M3 Turnip smoke:** adapt NativeTurnipSmoke's direct-library tests, without
   a Metal frontend. With TU_DEBUG=nocb, run Vulkan buffer fill, a small compute
   shader, then an offscreen triangle; map and compare every result, repeat
   BO/IOVA reuse and process reopen, capture the failing fence/CP/SMMU state.
   Stop and use CPU UI after any loss. Real cache/TTBR/coherency and A7xx reset
   sequencing remain hypotheses until these pass; do not infer success from
   a successful cross-build or CP_MEM_WRITE alone.
4. **M4 Metal discovery/frontend:** use the iOS 16 findings above and a runtime
   size guard. Decide between a small native IOAcceleratorES compatibility
   service and bringing in an appropriate family after auditing real kernel
   dependencies; the current kernel cannot inherit an absent metaclass.
   Verify bundle loading, signing and unsandboxed MTLCreateSystemDefaultDevice
   returning a real device first. Then derive iOS 16 sandbox access and actual
   selectors. Do not add a catch-all selector tunnel as a substitute for that
   audit. Implement device capabilities, resources, queues and encoders against
   the tested Vulkan path; enumerate unsupported features truthfully.
5. **M5 Translation and licensing:** keep **metal2vulkan external** at pinned
   `720ef1ce9ae2a2068999a8675717418b9416283e`; its declared choice is
   GPL-2.0+ OR LGPL-3.0+. Do not vendor it or statically fold it into this repo's
   kernel/plugin artifacts. Distribution needs a separate license/relinking
   plan before shipping a combined frontend. Validate AIR/metallib translation
   and shader caching separately. Imported host/IOSurface BOs, page lifetime
   and coherence are additional kernel work; M1 advertises no HostImport.
6. **M6 presentation/CoreAnimation:** implement IOSurface texture import,
   CAMetalLayer drawables, producer/consumer fences and buffer lifetime, pixel
   format/stride/orientation handling, and CA's required render/compute paths.
   Preserve the existing CPU-present fallback through frontend/plugin failure.
   Require actual GPU-rendered app and CA content, UI correctness, latency,
   thermal and memory measurements. Neither UniXNU's nil-returning plugin nor
   its partial frontend supplies this integration today.

## Risks and delivery

Real idle TTBR switching, CP context reset and BR/BV fences remain unbooted.
The raw solid fill is deliberately small; success will not prove shader or
Turnip execution. Raw PM4 is privileged and not validated as a security
boundary; only an unsandboxed root diagnostic client is supported. Arbitrary
root IBs can access privileged GPU state. Failed bus drain leaks/quarantines
memory until reboot; wedged interconnect/MMIO can exceed software containment.
There is no CP recovery, preemption, fair scheduling, host import, IRQ completion,
MTLDevice, drawables or accelerated CoreAnimation in M1.

Artifacts and their exact hashes are in `xnu/out-fuxi-metal1/SHA256SUMS` and
`tools/out/SHA256SUMS`; all binaries/firmware/Apple analysis output are ignored.
The command line is the supplied 0097b line plus exactly ` fuxigpuabi=1`.
The ADT remains byte-identical, including seven CPUs, pinned UFS root and
5088 MiB of XNU RAM with the existing 32 MiB GPU reservation.

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal1/kernel.development.fuxi` | `40662b2881e536fbcada825e92768718d92aa35999afa52d13c5dc8211b80088` |
| `xnu/out-fuxi-metal1/kernel.development.fuxi.unstripped` | `4376b4eefb3c66f0c1eac928459df7b7d3fab7d9473837ba889c7e45e7fc0b22` |
| `xnu/out-fuxi-metal1/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-metal1-payload.bin` | `781e358637e37205e6bc1140406d7965801293516ad74c9453a733273969eeb2` |
| `shim/out/fuxi-metal1-boot.img` | `c430735773e9b97f0fa60f37244da085ab219ad97c46224dbf2f297834b59851` |
| `tools/out/fuxi-gpu-abi-test` | `fc04e8157061a21d6f713197625010006e28b77b56c01cddf1d38ffd5cd85ae1` |
