# A740 into XNU: ownership and first accelerated present

This is an implementation plan, not an XNU driver or a device result. Loader
M3d has verified GMU/HFI, GX, SQE/CP and a memory fence. M4's 2D sequence still
needs coordinator device validation. Secure mode stays unchanged; TRUST=1 is
reported, with no zap/PAS/SCM or secure SID2 work in this plan.

## 1. Reserve memory before permitting handoff

The loader scratch is `[0x8f0000000,0x8f2000000)` (32 MiB), inside the current
XNU RAM span `[0x8c0000000,0x900000000)`. Removing request49's lock alone would
let XNU allocate live GMU instructions, HFI queues or GPU page tables.

First implement a conservative boot configuration with usable RAM ending at
`0x8f0000000`: `physBase=0x8c0000000`, `memSize=0x30000000`. Update the payload
builder, loader payload validation and build script's matching geometry check,
boot_args `memSize`/`memSizeActual`, and ADT RAM declaration together. Check
kernel segments, ramdisk, ADT, stacks, USB monitor and topOfKernelData against
that bound. This sacrifices the last 256 MiB of the old window but avoids
assuming the current XNU bootstrap supports a hole. Do not just add a DT
reserved-memory property and assume XNU honors Linux's binding.

A later early-pmap/VM reservation can recover upper RAM: admit
`[0x8c0000000,0x8f0000000)` and `[0x8f2000000,0x900000000)` only after proving
no pmap bootstrap, compressor, VM free list, hibernation or DMA allocator can
return a page in the hole. Use 16 KiB XNU alignment throughout (both reservation
ends already qualify); GPU SMMU tables still use their separate 4 KiB granules.

Retain the entire 32 MiB initially, including unused gaps, not only the current
occupied pages. Current contents:

| Physical range | Owner/use |
|---|---|
| `0x8f0000000` onward | GMU SMMU table pool |
| `0x8f0100000..0x8f10fc000` | GMU icache backing (see firmware mappings) |
| `0x8f1100000..0x8f1111000` | GMU data, log, HFI, debug buffers |
| `0x8f1200000..0x8f1300000` | GPU SMMU table pool |
| `0x8f1300000..0x8f1320000` | SQE firmware backing |
| `0x8f1320000..0x8f132a000` | ring, rptr shadows, fence page |
| `0x8f1400000..0x8f1442000` | M4 source and guard pages |
| `0x8f1500000..0x8f1542000` | M4 destination and guard pages |

The live scanout `[0xb8000000,0xb89e3400)` is outside this XNU RAM window.
Keep it reserved separately, map it without a conflicting cache alias and map
only its rounded 4 KiB span in the GPU domain. Never reclaim ABL scanout while
MDSS is reading it. No display-controller ownership changes are needed for blits.

## 2. One IOKit service owns the hardware

Add built-in `FuxiAdreno` (`IOService`) with an `IOWorkLoop`, command gate,
completion timer and interrupt sources. Split portable table/HFI/PM4 encoding
from register access; use checked IOMemoryMap accesses, bounded waits and a
single device state machine. Port the proven cold ordering literally: allocate
and verify mappings and SQE bytes, program bases and shadows, barrier, then the
sole SQE RUN write. Do not use recovery's SQE_CNTL=3 in cold initialization.

First boot XNU with the loader GPU probe **never started** and request49's
existing lock still in place for probe-started boots. Start the service from
cold state after XNU is running; this avoids adopting unknown live firmware.
Use externally supplied, hash-pinned GMU/SQE assets built into the kernel or a
trusted early ramdisk. The buffers and firmware must outlive every DMA user.

For eventual live adoption, define a versioned, checksummed reserved handoff
record: reservation bounds, firmware hashes, CB/ASID/SMR ownership, full mapped
ranges, HFI queue addresses/sequence/indices, ring pointers, fence values,
GPU_SET/OOB state and power/clock votes. The monitor must stop touching GPU,
SMMU and queues before publishing ownership. XNU verifies bounds/readbacks and
idle completion before acknowledging adoption. Failure keeps the GPU pinned
and unavailable until reset; no freeing or second initializer. Unlock handoff
only after the RAM exclusion is proven and either this protocol works or the
loader has never started the GPU. The USB monitor can remain otherwise active.

Own SID5 (GMU), SID0/1 (GPU) on the dedicated Adreno SMMU, leaving apps-SMMU and
other devices alone. Keep the probe's selection and masked-SID collision checks;
CB1/CB2 and ASID1/2 are observed selections, not general constants. Pin pages,
flush CPU table writes before TLBIASID/TLBSYNC, poll bounded completion, and
publish streams last. Never disable a context with pending GPU accesses.
Read-only firmware/source mappings and unmapped guard holes remain useful.

XNU cached memory needs explicit ownership transitions: clean source/ring/PTE
cache lines before GPU reads; invalidate GPU-written fence/readback lines before
CPU reads, with proper barriers. Start with consistently uncached mappings of
the fixed reservation to match the probe, then introduce coherent/cache-managed
buffers with measured tests. Avoid cached/uncached aliases. Use a physically
contiguous staging surface initially; later map pinned IOMemoryDescriptor pages
into a contiguous GPU IOVA, rather than assuming XNU kernel VA is GPU VA/PA.

## 3. IRQ, power and failure ownership

Supplied `sm8550.dtsi` candidates, to cross-check against Fuxi's live DT before
enabling delivery (SPI numbers exclude the GIC's +32):

| Source | DT SPI | GIC INTID |
|---|---:|---:|
| GPU RBBM | 300 | 332 |
| GMU HFI | 304 | 336 |
| GMU watchdog/error | 305 | 337 |
| Adreno SMMU global | 673 | 705 |
| SMMU CB0/CB1/CB2 | 677/678/679 | 709/710/711 |

Route only owned banks; derive the complete CB-to-IRQ mapping from the actual
resource list. Begin with polling and masked GPU interrupts as the probe does.
Then add level-high GIC routing/EOI, acknowledge only serviced RBBM/GMU sources,
and drain HFI on the workloop with a bounded budget. Use RBBM fence-completion
interrupts once their event flag/clear path is verified; M4's fence has no IRQ
request. SMMU fault handlers freeze FAR/FSYNR/SID and stop submissions. Do not
clear/resume a stalled translation to continue an unknown job. Watchdog/timer
failure fails fences and quarantines buffers until a proven reset path exists.

Centralize CX/GX GDSCs, GPUCC PLL/RCGs and branch clocks, GCC GPU/SMMU/DDR votes,
RPMh GX/CX/MX and DDR bandwidth votes, RSCC/PDC and GMU OOB in this service (or
shared platform providers with reference-counted votes). Do not let another
clock driver drop a loader vote. Start at the proven low HFI performance level,
hold GPU_SET across submissions, and defer runtime collapse/IFPC and suspend.
Add coordinated HFI idle/drain, fence retirement and power-down only after
interrupts and reset recovery work; never release DDR/SMMU clocks before DMA
stops. Persistent power is the first implementation's explicit limitation.

## 4. Accelerate the existing framebuffer copy

The call site is `AppleCLCD::present` in `iokit/Kernel/arm/SweetFramebuffer.cpp`
(patches0046/0056), after latest-swap coalescing and before the CPU layer copy.
Keep the existing timer/present policy initially. Introduce a private kernel
`copyBGRA(surface, sourceRect, destinationRect, completion)` API; no userspace
raw PM4, IOVA or MMIO API. Validate bounds, pitch, nonoverlap and opaque copy
semantics, retain the SweetSurface/IOMemoryDescriptor through fence completion,
and queue jobs on the GPU workloop without blocking the vsync callback.

For each present, map or stage the pinned linear BGRA source, clean its CPU
writes, encode the validated M4 blit state, append CACHE_CLEAN with a unique
user32 fence, then ring the doorbell after barriers. At 4320-byte scanout pitch,
use M4's aligned one-row destinations and X phase; do not truncate `4320>>6`.
A full2400-row job exceeds the probe's 32 KiB ring. The driver must use bounded
IB batches or larger ring management with rptr-based space accounting and
sequence fences; keep surfaces retained across every chunk. Add multiple
rectangles/batches only after one rectangle passes every-pixel readback.

Deliver completion after the fence and coherency step, then unpin/unmap retired
buffers and emit the existing notification at the appropriate completed-frame
point. Preserve layer order; initial API supports copies only, no alpha blend,
scaling or rotation. Unsupported formats/composition use the CPU path. On a GPU
hang, stop and quarantine first; do not concurrently run CPU fallback onto an
in-flight destination. Reclaim/retry only after reset guarantees DMA has ended.
Keep ABL autorefresh; do not issue a competing manual MDSS START. CPU readback
proves memory output; panel visibility/tearing still needs a physical check.

## 5. What iOS gains, and the boundary

This can offload SweetFramebuffer's surface-to-scanout copies while iOS and
software renderers still produce pixels on CPU. It does not make Adreno an AGX
GPU, accelerate arbitrary CoreAnimation composition, or implement Metal/OpenGL
ES. Advertising `AGXAccelerator`/`IOAccelerator` names alone cannot satisfy
Apple's user clients, IOSurface synchronization, command ABI or shader ISA.

A custom native app could later use a deliberately small validated blit/compute
API. Broader acceleration requires an Adreno userspace stack (Mesa Freedreno or
Turnip plus its kernel memory/submit interfaces) and an iOS graphics translation
or replacement path. Apple's precompiled AGX shaders and Metal runtime cannot
submit unchanged to A740. Treat that as a separate substantial project after
copy acceleration, memory safety, reset, suspend and multi-client isolation.

Acceptance order: reserved-memory stress boot with GPU cold; XNU stages8..13;
scratch every-pixel blit; one scanout rectangle; IRQ fence completion; repeated
retained surfaces/presents; forced timeout/reset; full frame chunking; only then
power management and multiple clients. Keep the loader lock for today's image.
