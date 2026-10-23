# GPU-CLOCK: fixed A740 level and DDR votes

2026-10-23. Offline source audit and implementation. No phone access, deployment,
push, CI/workflow, firmware or reference build. Canonical driver sources remain
in `xnu/fuxi-adreno/` and `shim/fuxi-probe/`; regenerated **0098** overlays the
standalone 0096 baseline. Metal, external metal2vulkan, external Mesa/Turnip,
NativeAdreno ABI 2, admission and display ownership contracts are unchanged.

## What the current path actually requests

`fuxibbgpu=1` admits backboardd; it does not select clocks. With the existing
`fuxigpu=1 fuxigpuabi=1` setup, `fuxi_gpu_start()` runs the shared `gpu_probe()`
cold-start path. Neither Metal nor the ABI submit path subsequently changes
GPU/GMU performance levels. This is fixed startup policy, with no GPU DVFS or
thermal governor. Successful HFI ACKs prove acceptance, not measured execution
frequency or voltage. The supplied frame timings alone cannot identify either.

| Owner / step | Existing configuration |
| --- | --- |
| GPU HFI PERF_TABLE, stage 9 | OFF, 220, 295, 348, 401, 475, 550, 615, 680, 719 MHz, ascending indices 0..9. ARC levels 0, 0x34, 0x38, 0x3c, 0x40, 0x50, 0x80, 0x90, 0xc0, 0xe0. Top is historically rounded to 719 MHz in this table. |
| GX voltage vote | `g_arc_vote()` resolves `gfx.lvl` and `gmxc.lvl` (otherwise `mx.lvl`) from the actual boot command DB. Packed word is primary level <<16, secondary index <<8, primary index. It is an ARC level/index request, **not a voltage readback**. |
| GPU GX_BW_PERF_VOTE, stage 10 | Blocking HFI command 30, GPU index **1**, DDR index **0**. No later higher vote. |
| DDR BW_TABLE | One **off** row for SH0/MC0/ACV, each data word 0x40000000, DDR wait mask 7. Addresses come from command DB. No active DDR throughput request. Other inherited/shared SoC users can still keep DDR running. |
| CNOC | One CN0 command, two rows: 0x40000000 off, 0x60000001 on, wait mask 1. This is distinct from DDR bandwidth. |
| GMU HFI table | OFF/200000/500000 kHz; cx.lvl/mx.lvl ARC levels 0/0x40/0x80. The host does **not** explicitly select a GMU table index. Advertising an entry does not select it. |
| GMU physical clock, stage 6 | GPUCC PLL1 L=0x16, alpha=0xeaaa, 19.2 MHz XO: about 440 MHz. GMU RCGR source3, HID=3 divides by 2: **about 220 MHz**, despite the 200 MHz HFI low entry. Existing running PLLs are retained. No host switch to the 500 MHz table entry. |
| Other clocks / rails | GPLL0 GPU votes bits15/16; hub source5/HID5 gives 200 MHz from GPLL0/3. Enables AHB, SMMU vote, CX_GMU, CXO, DEMET, HUB_CX_INT and GCC AXI/memnoc branches. CX GDSC is brought up with retention. |
| PLL0 / GX | Cold startup configures PLL0 L=0x1e/alpha=0xbaaa if not already running, about 590 MHz **source** configuration. GMU firmware owns the actual GX clock/rail transition following the HFI GPU vote; the host does not force that PLL configuration as the GPU rate. |
| RPMh / power retention | `g_rpmh_init()` programs RSC sequence/hidden data and enables AOP-owned PDC without replacing its code. HFI supplies ARC/BCM votes for the GMU to issue; no independent host DDR RPMh client is implemented. GPU_SET OOB is held from CP startup through reboot, with no runtime idle-collapse path. ACD/IFPC policy is unchanged. |

Thus **220 MHz was not a stale requested GPU rate**. It was a hardcoded,
unmeasured claim in the ready line, and happened also to describe the separate
GMU physical clock. The replacement says `requested 220000 kHz` (or the chosen
rate); new diagnostics keep these concepts separate. Neither firmware throttling
nor actual GX rail voltage can be established by this offline audit.

## UniXNU comparison and port

Audited the supplied UniXNU superproject
`fb6aad6ab1f50199a4d99d2da1d0ca403b067633`, NativeAdreno
`cedcaa2949c795e71a47e78ae413c8a41b17d7d0`, AdrenoFirmware
`40fdb8ea87606612ac6f1e80bbb49955d0ce4051`, and the adjacent UniXNU-sheng board
checkout `8a0c9ddab92038a50abc07fffd4882ec4e301f45`. Read-only text inspection;
no reference scripts, imports, tests, builds or firmware were run.

NativeAdreno's `NativeAdrenoService.cpp` gets board-generated startup policy
through `ShengGmuBuildStartup()`. The operational board path in
`package/GpuPowerProbe.cpp` sends `{blocking=1, GPU=1, BW=1}` after cold start:
**220 MHz plus active bandwidth**, not an automatic high GPU clock. Its
`GpuPowerBoard.h` uses the same PLL1/GMU/hub clock initialization as ours.
`package/ShengGmuVotes.c` and `scripts/prepare-sheng-gmu-votes.py` construct the
GPU/GMU tables from their enabled OPPs and active bandwidths. Their shared
`AdrenoHfiVotes.c` decodes BCM aux data, computes proportional SH0/MC0 votes,
sets ACV valid/performance bits and groups commit/wait bits by VCD.

Ported the missing **BCM aux/arithmetic/VCD-grouping** logic, adapted to our
160-word packet, in `gpu-clock.h` (BSD-2-Clause-Patent, source SPDX/provenance
header retained; exact upstream notice in `LICENSE.AdrenoFirmware`). README's
single UniXNU Credits entry includes this port. No GPL implementation or board
OPP generator is copied, and no Mesa/translator source is vendored.

Fuxi policy is deliberately its own: both DT power bins share the 220..550 MHz
frequencies, ARC levels and nominal `qcom,bus-freq` values used here. Hardware
facts from Fuxi's existing DT: `qcom,bus-table-ddr` entry1 = **2136718 kB/s**,
entry3 = **6074218 kB/s**; 220 MHz uses entry1 and 295..550 MHz use entry3.
Our HFI table deduplicates these into indices 1 and 2. SM8550 SH0/MC0 proportional
bus widths are 16/4 bytes. Both selected bandwidths are below ACV's performance
threshold (16500000 kB/s), so this bounded profile uses ACV valid-only. No sheng
OPP rates, rail addresses, command-db units, widths, VCDs or captured memory
are embedded. All resource addresses and BCM aux metadata come from this boot's
command DB. Missing/duplicate/malformed BCMs, zero proportional unit/width,
invalid address/slave/VCD or aux lengths refuse startup **before CM3 release**
with HFI stage result `0x90a`; the existing permanent CPU fallback applies.

This ports active bandwidth only when explicitly requested. The absent/zero
option keeps the original one-row bandwidth packet and vote unchanged. No
ACD/IFPC enable, warm resume, runtime level changes, automatic recovery, new
regulator driver or thermal governor is added.

## Options

Keep existing GPU/ABI/backboardd prerequisites. New arguments do nothing when
`fuxigpu` is disabled.

| `fuxigpulevel=N` | GPU index / requested MHz | HFI BW index / requested kB/s |
| --- | --- | --- |
| absent or 0 | 1 / 220, legacy policy | 0 / 0, legacy off row |
| 1 | 1 / 220 | 1 / 2136718; bandwidth-only comparison |
| 2 | 2 / 295 | 2 / 6074218 |
| 3 | 3 / 348 | 2 / 6074218 |
| 4 | 4 / 401 | 2 / 6074218 |
| 5 | 5 / 475 | 2 / 6074218 |
| 6 | 6 / 550 | 2 / 6074218 |
| outside 0..6, including negative numeric values | rejection logged, legacy 0 | legacy 0 |

Parsing uses a 64-bit destination so negative and wide values cannot wrap into
a valid 32-bit level. These are ascending **HFI** indices, not descending
Android KGSL power-level numbers. 615/680/719 MHz remain advertised as before
but cannot be selected with this argument. The bound avoids the higher rail
corners and SKU-specific top bin; it is not proof of thermal safety.

`fuxigpuclock=1` optionally borrows **RBBM counter3** before display/ABI
publication, selects ALWAYS_COUNT (0), enables the global counter bit and
samples once over about 1 ms against the ARM architectural timer. It uses
bounded high/low/high reads and restores selector/global enable on exit. The
startup sample consumes that counter's prior history. It never submits work,
disables HWCG, changes a PLL or adds periodic MMIO. A stopped, unstable,
faulting or implausible sample reports unavailable. Diagnostics use independent
fault results rather than poisoning the driver MMIO-failure latch.

**Counter rate can be lower than core clock under clock gating.** A successful
sample measures RBBM count rate over that startup interval, not guaranteed
sustained GPU MHz under load. OOB/GX/CX prerequisites are checked first; an
asynchronous bus fault is still a hardware qualification risk. Without this
argument, only GPUCC readbacks are added: raw PLL0/PLL1 and GMU RCGR, and a
register-derived GMU clock. Unsupported source/running-state readbacks give
`gmu_derived_khz=0` (unknown), not a claimed 0 Hz operating point.

## Expected logs (formats, not observed results)

For `fuxigpulevel=4`, after successful cold startup, where `<...>` denotes a
boot-dependent lowercase hexadecimal readback as printed by `%#x`:

```text
FUXI GPU CLOCK: level=4 gpu_index=4 requested_khz=401000 gx_arc=<vote> bw_index=2 ddr_kBps=6074218 hfi_ack=1
FUXI GPU CLOCK: GMU table_khz=0/200000/500000 cx_arc=<off>/<low>/<high> (no host GMU perf-index vote)
FUXI GPU CLOCK: PLL0 mode=<mode> l=<l> alpha=<alpha> PLL1 mode=<mode> l=<l> alpha=<alpha> gmu_rcgr=<cfg> gmu_derived_khz=<rate> core_measured_khz=unavailable
FUXI GPU: ready, BGRA strided rects + one fence, 4 cached backings, requested 401000 kHz GPU vote (not measured), async one-in-flight, no timer sleep
```

On the programmed PLL1 L=0x44440016, alpha=0xeaaa, GMU CFG=0x303 path,
`gmu_derived_khz=219999` (integer truncation of about 220 MHz). Inherited clocks
may produce different readbacks. An access failure replaces the PLL line with:

```text
FUXI GPU CLOCK: gpucc_readback=unavailable core_measured_khz=unavailable
```

With `fuxigpuclock=1`, a readable/powered GPUCC snapshot additionally emits one
of these before the ready line; decimal fields depend on the actual sample:

```text
FUXI GPU CLOCK: rbbm_count_khz=<rate> sample_us=<elapsed> (clock-gated sample)
FUXI GPU CLOCK: rbbm_count_khz=unavailable
```

The absent/zero default first line has `level=0 gpu_index=1 requested_khz=220000`,
`bw_index=0 ddr_kBps=0`; ready says `requested 220000 kHz GPU vote (not measured)`. An invalid example:

```text
FUXI GPU CLOCK: rejected fuxigpulevel=7 (allowed 0..6); legacy level=0
```

The existing five-second backboardd periodic line appends one decimal field:

```text
FUXI MTL: backboardd frames=<total> gpu_ms=<mean> late=<total> failures=<total> hz=<rate> window_frames=<count> gpu_busy_est_pct=<percent>
```

`gpu_busy_est_pct` = completed compositor GPU timestamp durations / reporting
wall time, clamped to 100.0; -1.0 means frames completed without timestamp data.
No-frame windows report 0.0. It estimates compositor occupancy, not whole-engine
hardware utilization: timestamp spans include GPU waits, completions can cross
window boundaries, and other clients/unretired work are excluded. It reuses
existing timestamps and introduces no timer, counter reservation or GPU wait.

## Validation and unverified behavior

Host checks (no phone or reference execution):

* `python3 tools/sync-fuxi-adreno-shared.py --check` (also checks 0098/0100).
* `python3 tools/test-fuxi-gpu-probe.py`: actual HFI generation/startup and fixed
  votes for all seven selections; exact legacy/active packets, VCD groups,
  proportional floor/saturation, invalid metadata refusal before CM3 release,
  existing HFI ACK/error/wrap/timeout, CP/SMMU/blit/protocol regression.
* `python3 tools/test-fuxi-adreno.py`: mapping/startup wide/negative level
  parsing, existing submit/cache/SCM tests plus `test-fuxi-gpu-clock.py`.
  Clock test compiles the actual diagnostic functions with ASan/UBSan models
  and as AArch64 ELF / arm64 Darwin objects; validates readback derivation,
  counter rollover/deadlines, faults/zero samples and selector/enable restore.
* `python3 tools/test-fuxi-metal.py`: ABI/backend/register/cache/containment and
  real 0098/0100/0102 application against baseline snapshots, canonical bytes.
* `python3 tools/test-fuxi-metal-frontend.py`: portable frontend/boot-gate/storage
  regressions; native Objective-C/Metal cases require macOS and are skipped.
* Actual shared loader `probe.c` compiled as freestanding AArch64 ELF with its
  loader flags and `-Wall -Wextra -Werror`, without linking/packing firmware.

No full XNU or signed Metal/macOS build, device execution, applied active BCM
vote, rail measurement, sustained-frequency result, thermal improvement or
frame-rate improvement is claimed. The original 220 MHz request and missing
active DDR vote are source findings, not proof those explain all observed
8–20 ms GPU frames or late frames. Compile/host arithmetic cannot establish
GMU firmware/BCM ownership, speed-bin correctness, physical rail safety or
counter behavior under real HWCG. Existing OOB retention and absent GPU thermal
control remain significant limits.

## Thermal estimate and later device test plan

**Guess:** sustained 401–550 MHz plus an active DDR floor will increase GPU/DDR
power and temperature under continuous composition. Dynamic GPU power roughly
scales with frequency times voltage squared, so going from 220 to 401/550 MHz
could cost roughly 2–4 times GPU dynamic power at equal utilization, depending
on rail voltage. Total phone power does not scale by that factor; reduced GPU
execution time may offset part of it. No credible degrees-C estimate exists
without measurements. The fixed level and DDR floor remain requested at idle,
and CPU thermal controls do not constitute a GPU thermal governor.

For a separately authorized device session:

1. Build the changed kernel and FuxiMetal using the normal build/signing process;
   keep the existing matching ABI-2 ICD/external translator/root and admission
   setup. Save hashes and the pre-change image for immediate rollback. No root
   or staging action was performed by this task.
2. Boot the existing `fuxibbgpu=1` setup with **no level option**; first establish
   unchanged behavior and the default clock lines. On a separate baseline boot
   add `fuxigpuclock=1` to qualify the optional sample independently.
3. Test `fuxigpulevel=1` first: same 220 MHz, active DDR. Then 2 and 4; try 5/6
   only if earlier runs remain stable and temperature is acceptable. Reboot
   between fixed levels; there is no live retuning API. Repeat each from a
   comparable cool starting state with the same scene and display brightness.
4. Record requested GPU/ARC/BW, GPUCC and optional counter lines, first-frame
   latency, five-second frame-rate/late/failure/GPU-ms/busy-estimate windows,
   existing CPU clocks/thermal readings, and idle behavior after composition.
   Compare short identical runs and recovery to baseline. A higher frequency
   sample is evidence only for its sampling window; use actual rendering
   timestamp improvements to evaluate the workload.
5. Stop on any new HFI error/0x90a, timeout, fault, hang, visual corruption,
   missing fence, panic, temperature limit or unexpected continuing temperature
   rise. Remove both new arguments and reboot to restore the legacy policy.
   Confirm invalid `fuxigpulevel=7` logs rejection and selects the default;
   evaluate thermal and performance limits before considering any default change.
