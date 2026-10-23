# P-UNIXNU: driver audit and selective ports

Offline implementation, 2026-10-23. Patch **0106**, canonical sources in
`xnu/fuxi-unixnu/`. No phone access, deployment, push, hosted build or CI run.
This is an audit of source contracts, not a claim that sheng behavior works on
Fuxi. The selected changes are independently **default off**.

Read alongside [touch/display](touch-display.md), [thermal](thermal.md),
[K1](gpu-k1.md), [host import](metal-hostimport.md), [N1](nativemetal-n1.md),
[T1](turnip-t1.md) and [H1](kernel-h1.md). README's historical CPU-rendering
status does not describe every later experimental GPU lane.

## Reference and licensing

The reference superproject is `research/UniXNU`, revision
`fb6aad6ab1f50199a4d99d2da1d0ca403b067633`, targeting
**xnu-10063.141.1 / Darwin 23 / iPadOS 17.7.11**. Many driver and kit directories
in the supplied checkout were empty submodule placeholders. Their exact
superproject gitlink revisions below were retrieved as text references outside
the repository. No reference script, module, test or build was executed or
imported, and neither supplied reference checkout was modified. The sheng
board/package source was read for integration context. HackiOS code is not
ported by this patch.

UniXNU's root licence is BSD-2-Clause-Patent; licences are also per submodule
and per file. AdrenoPM4, ArmSmmu4K and most AirNir sources declare MIT;
AirNir's `FusedF32.h` declares BSD-3-Clause. NativeUfs's selected source files
have **BSD-2-Clause-Patent SPDX headers**, but its submodule `LICENSE` contains
**BSD-2-Clause**, copyright 2026 blu-sharky. We preserve that exact file in
`LICENSE.NativeUfs`, the superproject patent notice in `LICENSE.UniXNU`, and
the source SPDX headers. NativeMobileDisplay's exact `LICENSE` (including its
ArmVirtPlatform contributor credit) is retained in `LICENSE.NativeMobileDisplay`.
This records the upstream discrepancy without relabelling its notices.
The README Credits entry now covers both ports. No GPL, Mesa, NativeMetal,
metal2vulkan source, firmware, Apple binary or device dump is added here.

## Driver comparison

Value is **stability / GPU / power**, with H/M/L indicating expected value of
closing the remaining gap, rather than measured benefit. Effort estimates
include adaptation and testing: S = localized changes, M = subsystem work,
L = new transport/ABI/boot architecture. Links pin the audited source revision.

| Driver | Ours today | Theirs, source examined | Gap and value (S/G/P) | Effort and disposition |
| --- | --- | --- | --- | --- |
| [ArmCpu](https://github.com/UniXNU/ArmCpu/tree/1951cc8444b340d7d8879e9418220cf29eb711d7) | `SweetCPU.cpp` from 0018/0065/0090; seven CPUs, PSCI CPU_ON, GICv3 virtual timer PPI 27, SGI1. XNU `osfmk/arm64/cpu.c::cpu_idle` already does DSB/WFI. `xnu/fuxi-perf/` owns EPSS/TSENS touch boost (0097). | `ArmCpu.cpp`: CPU/GIC/timer, redistributor discovery, opt-in secondaries, SPI handler table, SPI-based IPI workaround, quiesce/halt diagnostics. | SPI dispatch is useful for eventual TE/touch/storage IRQs (M/M/M). **No cpufreq or ordinary idle governor exists in this driver**; its terminal WFI halt disables GIC/timer and cannot replace scheduler idle. | L for IRQ routing. Defer: our SGIs work, timer/PSCI already exist; importing sheng's SPI IPI workaround or terminal halt could break SMP/wakeup. CPU frequency already has better Fuxi thermal guards. |
| [ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform/tree/0d544705c2e89725caabc0542bb96bc8febd8c70) | 0016 `SweetPlatformExpert.cpp`, Fuxi matching from 0082, `tools/mkadt-fuxi.py`; board-specific nubs. | `ArmDTPlatform.cpp`: generic DT platform, requires real CHRP NVRAM proxy and creates compatible-matched device nubs; empty delete/exclude lists. | Generic enumeration/NVRAM proxy, not a missing display or GPU primitive (L/L/L). | M; defer replacement of a working board expert and its ADT contract. |
| [DarwinBoot](https://github.com/UniXNU/DarwinBoot/tree/d099354d0eab34df5088ca1ba89c9c88bc3260bb) | `shim/fuxi-loader/`, `tools/mkpayload.py`, `mkadt-fuxi.py`; temporary ABL Linux-image handoff and separate monitor core. | `DarwinBootPkg/Driver/DarwinBoot.c`, `BootArgs.h`, `PackedRamdisk.h`: UEFI GOP, segment loading, trust-cache and packed RAM disk handoff. | UEFI boot architecture, no runtime GPU stability fix (L/L/L). | L; defer. Darwin 23 boot_args and firmware-volume assumptions are not drop-in loader updates. |
| NativeAdreno | 0096/0098, `xnu/fuxi-adreno/`, K1 counters, HostImport, watchdog and DMA quarantine. | Existing audited K1 reference, same superproject pin; NativeAdreno submodule `cedcaa2949c795e71a47e78ae413c8a41b17d7d0`. | Already assigned/ported, GPU multi-client scheduling remains separate. | Excluded per task; preserve kernel ABI 2 and current ownership. |
| [NativeAES](https://github.com/UniXNU/NativeAES/tree/a34964b8c2fec2e83c8e4f68fb7d6fb7e4b2227b) | 0020 BearSSL MIT portable AES-CBC/ECB provider, FUXI configuration in 0082; existing AppleKeyStore software compatibility in 0038. | `NativeAES.cpp`, `OriginalAESABI.h`: software AES-CBC adapter subclassing original `IOAESAccelerator`; NIST KAT, descriptor/task overloads, raw software keys only. | Missing original-family registration, **not missing AES-CBC**. No hardware engine, SEP/device keys, XTS or GCM added by theirs (L/L/L absent a consumer). | M/L: reconstruct actual iOS 16.7 parent vtable/size first. Defer: no demonstrated 20H392 consumer blocked on this service, and the supplied ABI's 0x120 instance/virtual layout is for their original family. Cannot fix hardware keybags. |
| [NativeGeniSpi](https://github.com/UniXNU/NativeGeniSpi/tree/b5399b5646c4d7415deec6d887fd51715a97a024) | `bsd/dev/fuxi_touch.c` (0091/0093): SM8550 SE4 GPI DMA rings, FTS FIFO draining/reinit; Sweet FIFO GENI in 0060/0063. | `GeniSpi.c/.h`: callback-based FIFO register I/O, packing and bounded cancel/reset, FIFO width/depth discovery. | Shared bus abstraction/FIFO fallback, not our missing DMA transport (L/L/L). | M; defer: Fuxi relies on inherited GPI/FTS configuration, not sheng's FIFO transaction mode. |
| [NativeHidInput](https://github.com/UniXNU/NativeHidInput/tree/28f9b463b48e862f2def91d29c11b9143785715e) | 0058 IOHIDFamily sources + SweetHID events, 0091 FTS digitizer, 0092/H1 buttons; edge-origin flag already present. | `NativeHidInput.cpp`: original HID-device ABI, keyboard/consumer reports, normalized ten-finger descriptor and original-event edge flag hook. | Combined reports and original-family plumbing (M/L/L); no FTS hardware controller. | M/L; buttons explicitly belong to another lane. No iOS 17 vtable hooks copied. |
| [NativeKeyStoreEvents](https://github.com/UniXNU/NativeKeyStoreEvents/tree/e59bac4ad1ac8825919ae94da8440a64d8ab90b3) | 0038 `SweetKeyStore.cpp`: already publishes AppleKeyStore, no-passcode/unlocked state, DER device state and selector logging. | `NativeKeyStoreEvents.cpp`: matching notification service, opt-in `sheng-aks=1`, selectors 7/14/17/35 etc.; state only, no secure key operations. | Minor selector/notification differences, largely duplicate compatibility that even cites Sweet's iOS 16.7 behavior (L/L/L). | S/M if logs identify a missing selector; defer speculative successful replies. |
| NativeMetal | FuxiMetal plus external Turnip route, N1/T1 and subsequent Metal lanes. | Existing N1/T1 audit; submodule `a96feceda55e8fed1e20c0a2998736cef7aab006`. | Already assigned/ported. | Excluded per task; no sources vendored. |
| [NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay/tree/c031d882b9721d943383d11f66dc4748252ce929) | `SweetFramebuffer.cpp` (0046/0061/0079/0093/0096), `SweetSurface.cpp` (0068/0096/0098): absolute software tick, latest-frame queue, centered crop, asynchronous kernel GPU blit, one-in-flight poll, power-off black fill; 0087 TE autorefresh. | `NativeLegacyDisplay.cpp`, `NativeLegacyRequest.h`, `NativeGOPPresenter.cpp`: bounded scalar/device copy and shadow diff; client-demand software vsync; original-family retirement after actual copy; privileged userspace GPU offer/complete/abandon. `NativeMobileDisplay.cpp` itself is metadata prototype only. | Bounded copy and idle ticks (M/M/M). No actual panel TE/vblank/DSI power driver exists in this component. | S/M; **ported bounded/differential CPU copy and demand timer**, retaining Fuxi GPU path. Defer original-family request/retirement offsets and new userspace presenter ABI. |
| [NativeRamBlock](https://github.com/UniXNU/NativeRamBlock/tree/d4d22d9819b0162eda49f6402235dddc2279410d) | 0021/0022/0024 RAM-root/mockfs support; production root uses 0088 pinned-file HFS+ extents. | `NativeRamBlock.cpp`, `SparseDisk.h`, `PersistOverlay.h`, `SeedDisk.h`, `DirectDisk.h`: IOBlockStorage RAM disk, sparse seed, SHA-identified changed-page persistent overlay/store. | Alternate seeded Data filesystem architecture (M/L/L), not UFS timeout recovery. | L; defer: requires provisioned store windows/seed identity and a mount/data migration, none authorized here. |
| [NativeUfs](https://github.com/UniXNU/NativeUfs/tree/af785c1777161a62931425c2faaa536ce0bebd5b) | `sweet_ufs.c` (0028/0030/0034/0069/0084–0088): ABL host reuse, single slot/bounce, five-second timeout, optional sleep, extent-confined writes. `shim/ufs.c` is a legacy Sweet read-only MMU-off probe. | `TransferPoll.h`, `RootCommands.h`, `NativeUfs.cpp`, `SmmuTakeover.h`: OCS-confirmed completion, fatal detection, invalidate-only DMA reads, response/tag/LUN/residual validation, poisoned allocation retention; native SMMU ownership, IOStorage root/store windows. | High impact DMA lifetime gap: old timeout cleared slot then next command could overwrite descriptors/bounce before DMA ownership was proved (H/M/L). | S for policy, L for complete host replacement. **Ported completion/cache/response/quarantine policy**; no timeout retry or new SMMU/PHY/clock/partition ownership. |
| [NativeVirtioBlock](https://github.com/UniXNU/NativeVirtioBlock/tree/98909eb9198a4360d7a43be831887cb8b74d814d) | Physical UFS and RAM-root support; no virtio-mmio block frontend. | `NativeVirtioBlock.cpp`: modern split-ring MMIO block, optional VerifiedRoot authorization. | QEMU/testing transport; Fuxi firmware exposes no such device (L/L/L on hardware). | M; defer until a virtio test target is requested. |
| [NativeVirtioConsole](https://github.com/UniXNU/NativeVirtioConsole/tree/066f9bded311233ed56824e13a5628077619efda) | Loader monitor/log ring, framebuffer console and existing USB support; no virtio transport. | `NativeVirtioConsole.cpp`: modern MMIO queues, exclusive client and binary broker channel. | QEMU offline GPU broker rather than physical A740 acceleration (L/L/L). | M; defer, no Fuxi MMIO device to bind. |

## Kit comparison

| Kit | Ours today | Theirs, source examined | Gap and value (S/G/P) | Effort and disposition |
| --- | --- | --- | --- | --- |
| [AdrenoFirmware](https://github.com/UniXNU/AdrenoFirmware/tree/40fdb8ea87606612ac6f1e80bbb49955d0ce4051) | Loader `shim/fuxi-probe/gpu.c`, XNU `fuxi_adreno.c`: external GMU/SQE, HFI tables, SCM/ZAP, votes, GX/CP startup. | `AdrenoFirmware.c`, `AdrenoHfi.c`, `AdrenoHfiStartup.h/.c`, `AdrenoHfiVotes.c`: bounded staged HFI ACK sequence, explicit votes and single-attempt poison/retention contracts. BSD-2-Clause-Patent. | Reusable startup validation/ownership, board votes remain caller-supplied (M/H/M). | M/L; defer GPU power sequencing to its owner. No sheng bandwidth/ACD/IFPC vote table is assumed safe for Fuxi. No firmware copied. |
| [AdrenoPM4](https://github.com/UniXNU/AdrenoPM4/tree/445f8fe6d115738352b9d78dc70af6d61aa7f62f) | Existing PM4 framing/blits in `fuxi_adreno.c` and ABI backend; Turnip emits real graphics state. | `AdrenoPM4.h/.c`, `AdrenoShader.h`, `AdrenoVPC.h`, `AdrenoRaster.h`: structural plan/packet encoders and graphics bindings; explicitly not command authorization. MIT. | Alternative encoder abstractions, no new compositor capability by themselves (L/M/L). | M; defer redundant kernel encoding; cannot replace Turnip's state tracking. |
| [AirNir](https://github.com/UniXNU/AirNir/tree/032a867b5dc3121164bbd706e4e81e3b9fcd2b8f) | External replaceable AIR translator plus SPIR-V/Vulkan/Turnip; original Metal tests and frontend. | `AirNir.h`, `Frontend.cpp`, `MesaBackend.c`, `ConstantUploads.h`: offline AIR→NIR→IR3, constants and resource interfaces. MIT, with BSD-3-Clause `FusedF32.h`. | Different compiler architecture with Mesa coupling (L/M/L). | L; outside kernel driver lane. Do not vendor Mesa or compiler trees. |
| [ArmSmmu4K](https://github.com/UniXNU/ArmSmmu4K/tree/4d0d6598b1a36aeec3de01091893fa6660e49388) | 0096/0098 GPU native 4 KiB tables over 16 KiB XNU pages, alias/cache/import lifetime, TLB and fault containment; UFS ABL takeover. | `ArmSmmu4K*.h/.c`: table/stream/context planning, alias audits, live map/unmap with whole-graph verification and poisoned ownership. MIT. | Shared generic audits and storage isolation (M/H/L). | L; defer replacing proven GPU ABI backing, streams/context-bank ownership and quarantine. UFS isolation must first establish Fuxi stream IDs and all inherited users. |
| [NativePrelink](https://github.com/UniXNU/NativePrelink/tree/2b6ffb50117eee820b98aca9bd51b5c7dfb2970e) | Built-in XNU drivers and existing native builder; stand-ins implement required services. | `native_prelink.py`, `emit_prelink.py`, xnu-10063 sparse-index/security-init patches: original-kext relocation and prelink metadata. | Whole original-family/kernel ABI migration (L/M/L). | L; no iOS 17 binary layout, addresses or patches can be applied to 8796/20H392. No Apple binaries redistributed. |
| [VerifiedRoot](https://github.com/UniXNU/VerifiedRoot/tree/2854c61f6c4dfda870ac3d93e1f03d9181edb041) | Extent-bounded writable HFS+ image, permissive research AMFI (H1); no authenticated immutable root. | `src/verified_root.c`, `include/verified_root.h`, `adapters/xnu_crypto.c`: RSA3072/SHA256 manifest and block hashes, independent trusted anchor and generation/UUID checks. | Real owner-root integrity, separate from GPU/timeout fixes (M/L/L). | L; requires key provisioning, monotonic-generation policy and immutable-root/data design. Not silently grafted onto writable root or presented as restored Apple security. |

## What 0106 ports

### UFS: `fuxi_unixnu_ufs=1`

Adapted `drivers/NativeUfs/TransferPoll.h` and `RootCommands.h` to a small C
policy header plus our existing BSD driver; adapted `NativeUfs.cpp`'s real
64-byte-line CIVAC/IVAC maintenance. The allocation is already dedicated,
contiguous, aligned and permanent. Original SPDX/provenance lines and both
upstream licence notices ship with the patch.

Host takeover preserves inherited error evidence and refuses a fatal status before
changing the request list. Before **any descriptor write**, inspect the complete transfer doorbell and
fatal IS mask `0x71880`. Strategy and persistent-log writers also check before
touching the bounce buffer. On submission, publish descriptor/data cache lines
with real CIVAC and ordered MMIO; acknowledge only completion bit 0. While
polling, DB=0 must accompany OCS != 15 (invalid sentinel). This also accepts
fast completion where software never saw DB=1. Device-owned descriptor reads
use **IVAC only**, never clean/writeback over a potentially newer DMA result.
Successful completion invalidates descriptors/response and read data before
CPU consumption. Existing `ufswait` pacing and five-second deadline remain.

Fatal status or timeout permanently poisons the allocation; **no UTRLCLR,
reset or resubmission** occurs in this path. All subsequent commands and
strategy writes fail before buffer mutation; the allocation stays resident
until reboot. A malformed type/tag/LUN/EHS/length/sense/residual response is
also quarantined. Ordinary completed SCSI CHECK CONDITION or OCS errors remain
errors without pretending they are success. The existing three-attempt
ACTIVE/TUR startup loops can still consume completed status errors, but a
poisoned attempt immediately fails every later call without ringing again.
No new retries were added: upstream explicitly prohibits timeout retries while
DMA ownership is uncertain. The existing root extent/write/sync checks remain.

This is fail-stop robustness, **not recovery**: a genuine root storage failure
may leave iOS unable to proceed. The old default timeout/clear behavior remains
available with the option absent or zero. `shim/ufs.c` was audited, not changed;
its Sweet physical address/MMU-off assumptions do not belong in the Fuxi kernel.

### Display copies: `fuxi_unixnu_diff=1`

Ports the bounded copy-layout/plain copy/fill/differential helpers from
`drivers/NativeMobileDisplay/NativeLegacyRequest.h`. Diagnostic pixel scans,
legacy swap masks and private-family ABI declarations are not copied.

Differential copies compare 64-byte source blocks against a packed cached
shadow; changed blocks/tails get aligned volatile 64-bit stores to scanout.
The shadow is bounded to 64 MiB, allocated once, and freed with the framebuffer.
Allocation/geometry failure logs `unavailable` and keeps the ordinary path.
Only one composed **full-panel BGRA** CPU frame, with console muted using the
existing `sweetfb_noconsole=1`, is eligible. Fuxi's 1125×2436→1080×2400 centered
crop remains unchanged. Layout checks are adapted to permit the last visible
cropped row without requiring padding past the descriptor; the shadow now has
an explicit byte capacity. The plain helper handles unaligned-width tails
with scalar stores and validates every addressed row.

GPU submission stays first choice. Any accepted GPU copy invalidates the
shadow; GPU retirement still precedes display kick, next copy or black clear.
CPU partial/multilayer copies, rejected surfaces and power-off blanking also
invalidate it. After GPU use or wake, the next eligible CPU copy repopulates
the entire shadow. Default-framebuffer surfaces invalidate rather than reusing
a stale shadow. Writers must obey the existing producer/scanout ownership
contract; the option is unsuitable for other direct writers into scanout.
This does not add asynchronous producer fences or fix default SwapWait semantics.

With the option enabled, non-differential CPU copies use the bounded scalar
helper; without it, the old memcpy path remains. No new GPU service, GPU ABI,
userland client or firmware is needed. UniXNU itself notes diff was not faster
on sheng and keeps it opt-in after problematic early boots. Benchmark Fuxi;
reduced stored bytes alone is **not evidence of lower frame time or power**.

### Display scheduling: `fuxi_unixnu_idlefb=1`

Adapts the demand-driven timer/absolute-deadline policy of
`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp` to the existing
`AppleCLCD` client list and lock. No raw private-object offsets are used.
The present timer disarms when there is no pending frame, outstanding GPU
copy, required black fill or subscribed vsync client. Swap submission,
notification registration/port installation and display power requests wake
it under the same lock as its rearm decision. This avoids a lost wake when a
swap races the last callback. No new MMIO/power state is introduced.

Outstanding GPU copies continue polling at the existing cadence even on a
power-off request; black is written only after retirement. Black-screen swaps
stay pending without waking the timer once the clear is complete. Wake forces
the newest frame to redraw. Vsync subscribers retain continuous notification
behavior, including while off, to preserve the existing client contract.
Absolute deadlines skip missed periods instead of producing catch-up bursts.
The first idle/wake transition logs once each, avoiding per-frame log traffic.

This reduces **CPU timer wakeups**, not panel refresh. ABL INTF1 TE autorefresh,
DSC, physical panel power and brightness stay inherited. UniXNU's GOP display
also lacks real vblank/DSI panel-off control; its logical PM states explicitly
do not claim hardware power output. No transplanted register sequence can
supply that missing driver. Existing 60 Hz software vsync values remain the
**8796/iOS 16.7 six-word notification ABI**, not iOS 17's original-family sender.
No 120 Hz claim, physical vsync timestamp or extra boot argument is invented.

## Version adaptations and deliberate omissions

* **8796 versus 10063:** retain our built-in `SweetFramebuffer` and `SweetSurface`
  classes, iOS 16.7 selectors/0x46c record, crop and six-word notification;
  reject private original-family object offsets (for example vsync client list
  at +5592) and iOS 17 virtual-slot declarations. Do not prelink either original
  display or crypto family. The prototype `NativeMobileDisplay` is not their
  functioning legacy/GOP presenter and is not a replacement driver to register.
* **NativeUfs integration:** C/errno/BSD single-slot path replaces their C++
  templated IOReturn/IOBlockStorage plumbing. Reuse our DMA layout, 16 KiB XNU
  pages, 64-byte ARM cache lines, Fuxi boot host and pinned-file extent policy.
  No NativeRamBlock store windows, WRITE/FUA policy, SMMU bank selection,
  APFS/verified-root publication or clocks/PHY reset are copied.
* **ArmCpu:** ordinary WFI already lives in XNU 8796; frequency scaling lives
  in 0097's EPSS/TSENS policy. UniXNU `haltCPU` is terminal, and no frequency
  implementation exists there to port. Do not disable GIC/timer on ordinary
  idle or duplicate the boost governor. SPI dispatch/TE IRQ work is deferred
  until controller routing can be validated. Buttons remain another lane.
* **NativeAES:** iOS 16.7 already boots with portable AES-CBC/ECB. Theirs delegates
  to those same legacy APIs and implements only raw software CBC keys. Its
  private `IOAESAccelerator` parent ABI requires 20H392-specific verification
  and an actual blocked consumer; adding its iOS 17 layout would not provide
  hardware keys/SEP or accelerate the GPU. No AES service or crypto boot arg
  is added on speculation.
* **GPU scanout:** our 0096/0098 path already pins sources, maps/imports, submits
  blits, retires fences and falls back after containment. Theirs delegates the
  scanout copy to a privileged userspace offer/wait/complete client. Replacing
  ours would add another ABI/process lifetime without solving a demonstrated
  gap. Existing K1/N1/T1 remain the owners of GPU/Metal/Turnip work.

## Reproduction and tests

```sh
python3 tools/sync-fuxi-unixnu.py
python3 tools/sync-fuxi-unixnu.py --check
python3 tools/test-fuxi-unixnu.py
```

The sync tool replays every preceding relevant numbered patch with `git apply`,
compares canonical bytes, generates **0106-fuxi-unixnu.patch**, and checks the
result applies over that baseline. No new translation unit or build list is
needed; headers/licences are installed beside the including sources. The
normal builders enumerate the patch directory, so 0106 needs no CI option.
Number gaps 0104/0105 are intentionally unclaimed by this lane.

Host tests compile the **actual ported** UFS submit/read/write/strategy and
framebuffer present/tick/swap/rearm methods under ASan/UBSan. Stubs replace
MMIO, clock, IOKit surfaces and cache hardware, not completion/copy policy.
They cover fast/delayed/phantom completions, timeout/fatal/invalid response,
cache and sleep combinations, byte-identical DMA buffers after quarantine,
extent write refusal, and unchanged default timeout/clear. Copy tests cover
padded/cropped rows, alignment, last-pixel bounds, shadow capacity, dirty
blocks/tails and arithmetic overflow. Display tests cover GPU pending/retire/
fallback, CPU→GPU→CPU shadow invalidation, black fill/wake, off-screen swaps,
notifications and both sides of idle/wake ordering. The actual ARM cache
and copy routines are also cross-compiled with `--target=aarch64-none-elf
-mgeneral-regs-only -Wall -Wextra -Werror`.

Also passed: `test-fuxi-h1.py`, `test-fuxi-adreno.py`, `test-fuxi-metal.py` and
`test-fuxi-perf.py`, including existing GPU DMA/async-source/containment tests.

The old build log has no compile commands, but its component `.CFLAGS` and
`.CXXFLAGS` are present. With those saved flags and actual 8796 exported/kernel
headers, **both complete modified translation units passed** an arm64 Mach-O
syntax check with `-mgeneral-regs-only -Werror`. Two existing C++ designated-
initializer warnings in XNU allocation macros are suppressed; Apple-only
allocation type builtins use the same syntax-check stand-ins as `xnu-cc.py`.
Only our canonical sources and the patch-reconstructed `SweetSurface.h` plus
our `fuxi_adreno.h` are staged in a temporary directory. No reference source
or build output is modified. Reproduce this optional check with:

```sh
python3 tools/test-fuxi-unixnu.py --xnu-buildtree "$XNU_BUILDTREE"
```

These models/checks do not prove bus accessibility, cache coherence, a physical
TE, source-producer synchronization, thermal benefit, kernel linking or UI
pixels. A full macOS 8796 compile/link remains required before device testing.

## Device acceptance plan — not executed

Build 0106 through the normal FUXI kernel builder later; retain the same ADT,
RAM layout, root extent table, external firmware and baseline boot command
line. Prepare separate temporary payloads differing **only** in these options.
No root edit or new userland executable is required. Do not change the baseline
`ufsrw`, `ufswait`, `ufscache`, `fuxiperf` or GPU client policy during the A/B.

1. **Control:** all three options absent (or `=0`). Expect existing
   `SWEET FB: present up to 60 Hz (every 1 ticks)`, root extent/mount and GPU
   readiness lines; **no `FUXI UNIXNU` lines**. Unlock, scroll Settings, lock and
   wake; confirm baseline behavior. Save the usual display/GPU counters/log.
2. **UFS only:** add `fuxi_unixnu_ufs=1` to that command line, keeping the existing
   `fuxi_ufs_probe=1` needed for storage. Expect exactly:
   `FUXI UNIXNU UFS: guarded completion, fatal/timeout quarantine enabled`, then
   `FUXI UFS device ACTIVE power command result=0`,
   `FUXI UFS: root file in userdata: <N> extents, <B> blocks`, mount/launchd and
   SpringBoard. Open/read apps and Settings for 15 minutes. If baseline already
   permits root writes, create/read/remove a disposable file **inside that root
   image** and verify persistence after orderly reboot; do not enable additional
   write targets. Expect no `quarantined`, timeout, UFS response errors or panic.
   Check the same files still read correctly. A real error must produce one
   `FUXI UNIXNU UFS: quarantined reason=<preflight|fatal|timeout|response> db=... is=... ocs=...; no retry until reboot`;
   subsequent requests return errors, never new doorbells or buffer reuse.
   Do not induce a physical storage fault: timeout/fatal injection is host-only.
3. **CPU display diff only:** add `fuxi_unixnu_diff=1 sweetfb_noconsole=1` and use
   a separately prepared CPU baseline (`fuxigpu=0` in both halves of this A/B).
   Expect `FUXI UNIXNU FB: differential copy enabled (CPU full-panel BGRA only)`
   and `FUXI UNIXNU FB: diff 1 frames stored <X> of <X> bytes` on the first eligible
   frame; at 1080×2400 X is **10368000**. Scroll, pause on a clock/static Settings
   page and scroll again. After 600 eligible frames expect
   `FUXI UNIXNU FB: diff 600 frames stored <S> of 6220800000 bytes`.
   S should be lower when content is unchanged. Compare existing
   `SWEET FB: stats ... copy avg ... max ...` against control; keep disabled if
   frame time worsens, pixels stale, edges/padding corrupt or UI freezes.
   Lock/wake repeatedly; first post-wake CPU diff must refresh the full panel.
4. **Demand timer only:** add `fuxi_unixnu_idlefb=1`. Expect
   `FUXI UNIXNU FB: demand timer enabled, software 60 Hz, panel power inherited`,
   then first `FUXI UNIXNU FB: timer idle`; the next swap or subscription should
   log first `FUXI UNIXNU FB: timer wake` and present within the normal tick.
   Pause on static UI, scroll, lock, tap the black screen, wake and scroll again.
   No stuck black display or lost first frame is acceptable. No idle log is
   expected while an active vsync subscriber keeps requesting ticks; check
   selector-72 `notifications type 5 on/off` logs before attributing this to a bug.
5. **Combined GPU path:** same GPU-enabled baseline, then all three options plus
   `sweetfb_noconsole=1`. Exercise the existing admitted Metal/GPU smoke route,
   scroll and lock/wake while frames are queued. Existing
   `FUXI GPU: presents GPU <N> CPU <N> ... busy ticks <N> failed <N>` must show
   successful retirement; no black clear or CPU copy may race an in-flight
   GPU write. Diff counters advance only for eligible CPU fallback frames.
   Use the existing separately reviewed GPU fault/containment test to validate
   fallback if desired; do not introduce new device fault injection here.
   Verify full CPU repaint after fallback and no SMMU fault, DMA lifetime panic,
   stale shadow, UFS quarantine or input regression during a 15-minute run.

Record counters, frame-time distributions, dropped swaps, thermal readings and
screen observations for each control/option pair. An idle log or lower byte
count does not establish lower power; compare existing read-only thermal/clock
telemetry with the same workload, startup temperature and monitor settings.
If any option regresses, omit just that option on the next temporary boot;
reboot is required to clear a quarantined UFS allocation. No runtime toggle,
flash operation or automatic deployment is part of 0106.
