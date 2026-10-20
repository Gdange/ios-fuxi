# Milestone 7c — retry initialization while CPU domains come online

**OBSERVED on fuxi-0097b (coordinator device result, 2026-10-20):** boot log
order is CPU1 `PSCI CPU_ON rc=0`, then:

```text
FUXI PERF: disabled, step=enable domain=gold(1) PA=0x17d92000 offset=0 value=0x2 access=1 row=-1
```

CPU2 through CPU6 subsequently report `PSCI CPU_ON rc=0`, followed by
`FUXI GPU: ready, ... async one-in-flight, no timer sleep`. Silver passed;
gold's enable bit 0 was clear when the governor initialized before its cores
ran. Loader probes in other boots read `enable=0x3` after the cores run.
This identifies an initialization timing failure on 0097b; it does not prove
that the earlier 0097 failure came from a LUT-tail read.

0097c uses bounded retries rather than coupling initialization to CPU-online
accounting. Every failed initialization leaves **all hardware votes untouched**
and publishes no partial policy. Successful MMIO mappings are cached across
attempts. The worker retries once per second for at most 60 seconds (initial
attempt plus at most 60 retries). Input-triggered calls obey the same timestamp
gate and cannot accelerate or extend that window. Only the first failure and
the final outcome are logged; the final failure carries the most recent full
diagnostic. Example outcome shapes:

```text
FUXI PERF: init retry after 0 retries, step=enable domain=gold(1) PA=0x17d92000 offset=0 value=0x2 access=1 row=-1
FUXI PERF: ready, LUT cool ... kHz high ... kHz, hold 1500 ms guard 60/68 C after 2 retries
FUXI PERF: disabled after 60 retries, step=enable domain=gold(1) PA=0x17d92000 offset=0 value=0x2 access=1 row=-1
```

At runtime, the worker checks each domain's enable on every sample, including
samples with no vote change, and again immediately before **each per-core
write**. A clear bit or failed read skips writes to that domain and permanently
removes its boost eligibility for this boot. If it becomes enabled again, only
its known cool vote is requested. Vote/readback errors also fail that domain
cool; unaffected domains retain their policy. Each domain failure logs once.
These read-before-write checks cannot make a software read and a later hardware
power transition atomic.

Offline ASan/UBSan tests compile the actual worker, retry and MMIO functions.
They cover the observed gold startup failure, every initialization failure
class recovering, no votes or partial policy on failure, cached mappings,
input/swaps during backoff, two-line diagnostic limits, timeout/latest
failure, success at the final retry, late callbacks, unchanged high policy
noticing enable loss, failed enable reads, recovery to cool, per-core enable
loss between writes, and returned write/readback errors. Existing LUT,
thermal quorum, boost/expiry and actual XNU 16 KiB `io_map` tests also pass.

Native build exited 0 in **ci-lane-gpu6 only**; all sixteen clean-patch
source inputs match the native tree. Offline GPU submit, memory/USB, external
firmware-probe, SCM instruction/register and linked abort-recovery checks pass.
Both loader builds pass `-Werror`; boot-v4/Image geometry and every embedded
kernel segment are verified. ADT bytes, full RAM, seven CPUs and pinned root
identity match 0097b. Both payloads preserve its command line and append exactly:

```text
sweetsample=backboardd,Preferences,PosterBoard sweetscan=PosterBoard,Preferences
```

The CPU payload differs in exactly one byte, `fuxigpu=0` instead of `=1`.
Firmware remains external and untracked. No phone access or push was performed;
0097c initialization and runtime behavior still need a device run.

Reproduce the governor tests with:

```sh
python3 tools/test-fuxi-perf.py --xnu-tree "$XNU_SOURCE"
```

Artifacts and logs are ignored; `xnu/out-fuxi-0097c/SHA256SUMS` covers this set
(paths are relative to the repository root):

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-0097c/kernel.development.fuxi` | `9db8be7564c9c23b97aa007fb0a145beb3632a45766394075d867f858417400a` |
| `xnu/out-fuxi-0097c/kernel.development.fuxi.unstripped` | `680eb5c273cef44d3d2334ae57ecfe543efcf655488da273f763605d7dfb695a` |
| `xnu/out-fuxi-0097c/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-0097c-boot.img` | `c70cd66b2b46c406cef5ee065b3b0aaed2286e5aecb0315332b79515ef219e96` |
| `shim/out/fuxi-0097c-payload.bin` | `df4c8f11866976e8e2857ef813ae58b00060ff48ca6f5206a539c0c1ecc035e0` |
| `shim/out/fuxi-0097c-cpu-boot.img` | `16eb95657bdd15dc350ef32bd4bccb5b0031c627f6913638809fb72d82b2756e` |
| `shim/out/fuxi-0097c-cpu-payload.bin` | `af09a519e69db90e812d03ccae55de45c005219284a70a9c8a5e296449d5f3e8` |

# Milestone 7b — init diagnostics and fresh-sensor quorum

**OBSERVED on 0097 (coordinator device results, 2026-10-20):** SpringBoard runs
without a panic, but touch boost never starts: `FUXI PERF: disabled, EPSS/LUT
mapping or access failed`, before the GPU driver's first line. Loader 0x57
reads with the MMU off in the same boot succeed: all domain enables are `0x3`,
DCVS is readable (for example `0x9b30`), and perf-state registers are readable.
Individual CPU TSENS sensors sporadically lack VALID, affecting about 10% of
10-second samples, with varying sensors (example cpu-1-4 status `0x2400221`).
These are observations of 0097, not validation of 0097b.

**Fact versus hypothesis:** the old initializer unconditionally read all 40
LUT slots before the policy checked for repeated frequencies. An abort in an
unused tail slot therefore disabled an otherwise usable domain. 0097b stops
MMIO reads immediately at the first repeated decoded frequency and passes the
explicit row count into selection. The regression model injects faults at
every unused tail slot. **That a tail access caused the device failure remains
a hypothesis**: the old log does not name a failing register, and loader MMU-off
success does not establish the outcome of XNU's MMU-on accesses. The captured
LUTs themselves select successfully, without changing core-count eligibility.

| Domain | Observed rows / core-count bits 18:16 | Cool level / MHz | High level / MHz |
| --- | --- | --- | --- |
| silver | 16, all count 3, 307.2..2016.0 MHz | 8 / 1228.8 | 8 / 1228.8 |
| gold | 20, count 5 through 2707.2, final 2803.2 count 4 | 8 / 1401.6 | 19 / 2803.2 |
| prime | 21, count 5 through 2956.8, final 3187.2 count 1 (turbo) | 6 / 1363.2 | 19 / 2956.8 |

`tools/fixtures/fuxi-epss-0097.h` contains the observed frequency words. Its
repeated last word is a modeled terminator; later slots are fault-injected.
The model verifies every required row failure, map/enable/DCVS/selection/TSENS
map failures, all 40 distinct rows, both vote modes and all sensor positions.
Init failure now emits one line, for example:

```text
FUXI PERF: disabled, step=LUT domain=silver(0) PA=0x17d91000 offset=0x140 value=0xffffffff access=0 row=16
```

Steps are `map`, `enable`, `dcvs`, `LUT` (with row index), `level selection`,
and `tsens map`. `access=0` means no register value was returned; `0xffffffff`
is the read-failure sentinel, not an observed value. Mapping failures report
zero. Cool indices/frequencies are published only after both selections succeed.

The exact native `osfmk/arm64/machine_routines.c` calls `io_map`, which computes
`start_offset = PA - trunc_page(PA)`, maps `round_page(size + start_offset)` and
returns `start + start_offset`. On 16 KiB pages, EPSS windows at
`0x17d91000/0x17d92000/0x17d93000` map the `0x17d90000` page and return offsets
`0x1000/0x2000/0x3000`; TSENS `0xc271000/0xc272000` similarly use `0xc270000`.
The successful display mapping at `0x0ae36000` uses offset `0x2000` by the same
contract. No extra offset or change to those mappings is required. The offline
test can compile the supplied tree's actual `io_map` with VM stubs, exercising
both early and VM-running paths for all six addresses. `ml_io_map` is NOFAIL:
its null check cannot diagnose allocation exhaustion, which would panic.

The `fuxi_gpu_access` read/write instructions and barrier are covered by linked
copyio recovery records. `handle_kernel_abort` in `osfmk/arm64/sleh.c` looks up
the faulting PC, restricts the Fuxi fixup to that primitive's range, saves fault
state on the current thread and redirects to its failure return. There is no
GPU-ready check or driver-owned recovery context. A sleepable thread-call can
use it before GPU startup and with `fuxigpu=0`; interrupt-stack aborts still
panic. The linked records are verified in the 0097b kernel. This audit does not
prove bus accessibility or containment of asynchronous/interconnect hangs.

0097b takes the hottest **fresh valid CPU sensor** if at least **14 of 18**
(75%, rounded up) are ready, VALID and plausible (-40..150 C). A sensor read
failure/invalid bit or implausible value removes only that sensor. A controller
without TRDY contributes no valid sensors. Thus one missing silver controller
still leaves 15/18; missing TM0 leaves 3/18 and forces cool. No old temperature
is reused. One invalid sensor cannot deny or cancel a boost; fewer than 14
valid forces cool. The <=60 C entry and <=68 C continuation rules are unchanged.

0097b builds natively in **ci-lane-gpu6 only**. Both images keep full RAM,
seven CPUs, pinned root identity and `fuxiperf=1`, differing only in `fuxigpu`.
No phone access or push; 0097b init success and thermal behavior await a device
run. See [GPU build handoff](gpu.md) for artifacts, hashes and offline checks.

# Milestone 7 — opt-in XNU touch boost

Patch `0097-fuxi-touch-boost.patch` adds `fuxiperf=1` (default **off**).
The FTS report hook (0091/0093) and debounced button-post hook (0092) publish
input timestamps and wake a sleepable thread-call. AppleCLCD starts it in both
GPU and CPU modes and records swap timestamps. Hooks do no MMIO and do not
sleep. Only the worker owns EPSS/TSENS accesses; repeated input coalesces on
that one thread-call. A 100 ms worker sample bounds thermal/expiry response.

All three EPSS domains use their measured addresses below. The worker reads
up to 40 frequency LUT rows, stops MMIO at a repeated frequency, excludes
one-core turbo rows, and chooses indices by decoded frequency, not assumed
index order. Cool votes are the highest eligible rows at or below silver
1228.8, gold 1401.6 and prime 1363.2 MHz. High votes for gold/prime are the
highest eligible non-turbo LUT rows. Silver stays at its cool vote during
boost, avoiding a rail boost for the loader's monitor core. Domain enable and
per-core-vote mode are read first; every applicable perf-state register at
`0x320 + 4*core` is written and read back. No PLL/voltage registers are changed;
hardware CPR/LMh remain in control. A mapping/LUT/returned-access failure never
raises an unknown domain; vote/readback failure disables further boosting and
requests all known cool votes.

Only recent input can start boosting. Once high, either input or frame swaps
extend it; after **1500 ms without either**, the next sample requests cool.
Swaps alone cannot start a boost. Every decision uses fresh TSENS data from
TM0 sensors 1..15 (cpuss and gold/prime) and TM1 sensors 1..3 (silver), matching
the thermal lane's CPU sensor inventory. At least 14 of 18 CPU sensors must
be ready, VALID and plausible in the current sample (-40..150 C); the hottest of these is used. A smaller quorum
forces cool. A new boost requires hottest CPU <=60 C;
an existing boost can continue through 68 C, but **above 68 C forces cool**.
It cannot restart above 60 C. These are software votes sampled at 100 ms,
not new hardware trips or a guarantee about package/skin temperature.

Do **not** pass `--cpu-level` or `--cpu-at` to the host monitor on a
`fuxiperf=1` run: loader CL and XNU would race over the same votes. The monitor
now rejects that combination before opening USB. `--monitor-idle 2` and
read-only `--thermal-log` remain compatible. `fuxiperf=0` or an absent argument
leaves host-controlled EPSS behavior available.

Expected log shapes:

```text
FUXI PERF: ready, LUT cool .../.../... kHz high .../.../... kHz, hold 1500 ms guard 60/68 C
FUXI PERF: stats boosts N boosted N ms max temp N dC valid N state high|cool
```

Stats use a ten-second wall window, independently of display activity; boosted
time includes the portion of a boost crossing a window. `max temp` is the
maximum valid CPU sample in that window, in signed deci-C (-400 is the sentinel
if none were valid); `valid` describes the last sample. Errors log once on
initialization or vote failure. Offline tests compile the actual policy,
register read/vote and sensor routines: duplicate/turbo/nonmonotonic LUTs,
60/68 C boundaries, invalid samples, signed sensors, all per-core votes,
input-only boost entry, swap extension and expiry. Native build and device
acceptance are separate: no phone test or thermal improvement is claimed.

# Thermal lane (Fuxi, SM8550): measuring temperature and cooling iOS down

Historical thermal-lane design notes follow. For device observations and current
0097b behavior, see the milestone section above.
Tags used below: **[DT]** read from the live FDT `research/device/probe-09/live.dtb`;
**[src]** read from the XNU tree `research/ci-0081/xnu` or our patches; **[Linux]**
the mainline `qcom-cpufreq-hw` / `tsens-v2` register layout, which I recalled and
matched against UniXNU-sheng `package/CpuFreqProbe.cpp` (also not validated there);
**[guess]** a guess.

## Hardware facts

### TSENS (qcom,tsens-v2), 3 controllers, 41 sensors [DT]
| ctrl | TM (reg[0]) | SROT (reg[1]) | sensors |
|---|---|---|---|
| tsens0 | 0x0c271000 | 0x0c222000 | 16: aoss-0, cpuss-0..3, cpu-1-0..10 (gold/prime cores; emergency trips name cpu3..cpu7) |
| tsens1 | 0x0c272000 | 0x0c223000 | 16: aoss-1, cpu-0-0..2 (silver), nspss-0..3, video, ddr, mdmss-0..3, camera-0/1 |
| tsens2 | 0x0c273000 | 0x0c224000 | 9: aoss-2, gpuss-0..7 |

Each window is 0x1ff bytes. The order (TM first, then SROT) matches cinder-thermal.dtsi and the
qcom-tsens.yaml example. Register layout [Linux]: `TM + 0xa0 + 4n` = sensor n status
(bits 11:0 = temperature in signed deci-degC, bit 21 = VALID), `TM + 0xe4` = TRDY,
`TM + 0x4` = interrupt enable, `SROT + 0x0` = HW version, `SROT + 0x4` = CTRL (bit 0 =
enable, bits 18:3 = sensor enable mask). DT trips (Android thermal-engine config): CPU
emergency 108-112 C, DDR 90 C, GPU/NSP tj 95 C, reset-mon 115 C. None of these trips is
active under XNU. Only the TSENS hardware critical threshold (configured by firmware) and
LMh still protect the SoC.

### EPSS CPU frequency (qcom,cpufreq-epss) [DT]
| domain | base | CPUs | cores |
|---|---|---|---|
| 0 | 0x17d91000 | cpu@0, @100, @200 | 3x A510 (silver). **cpu@100 is the USB monitor core** |
| 1 | 0x17d92000 | cpu@300..@600 | 2x A715 + 2x A710 (gold) |
| 2 | 0x17d93000 | cpu@700 | 1x X3 (prime) |

Registers [Linux epss_soc_data]: 0x0 enable (bit 0), 0x20 domain state (bits 7:0 = L value
of the current, possibly LMh-throttled, frequency, x 19.2 MHz), 0xb0 DCVS ctrl (bit 0 =
per-core votes), 0x100 + 4i frequency LUT (bits 31:30 src, 18:16 core count (1 = turbo
row), 7:0 L; src 0 = GPLL0/2 = 300 MHz), 0x200 + 4i voltage LUT (bits 11:0 mV), 0x320
perf-state index vote (+4*core when per-core). LUT: up to 40 rows, and two equal
frequencies in a row end the table. Linux sets the frequency only by writing the LUT index
to 0x320. The hardware handles voltage, CPR and LMh.
**Nobody writes the perf state after ABL.** XNU has no cpufreq driver, so every cluster stays at
whatever level ABL left [src: no EPSS code in our patches]. The actual level is unknown until
the first `--thermal-probe` [guess: bootloaders often boost to a high level for fast boot].

### Idle states [DT]
PSCI 1.0 with hierarchical domains (cpu-pd0..7 -> cluster-pd), so Linux uses OSI mode. The per-core
power-collapse param is 0x40000004 (silver/gold/gold-plus "rail-pc"). Cluster domain states:
0x41000044 (L3 off), 0x4100c344 (LLCC off). No standby/retention state is listed.

### XNU idle [src]
`osfmk/arm64/cpu.c cpu_idle()`: `wfi` defaults to 1 (TUNABLE). `idle_enable` is TRUE unless the
`jtag` boot-arg is set. The 0091d cmdline has neither `wfi=` nor `jtag=`, so **idle CPUs execute
`dsb sy; wfi`**. XNU runs at EL1 under the Qualcomm hypervisor, so WFI probably traps to EL2,
and what EL2 does with it (clock gating only, or deeper) is unknown [guess].
SWEET.h does not define `__ARM_COHERENT_IO__`, so `platform_cache_idle_enter()` runs
`FlushPoU_Dcache()` (set/way clean+invalidate up to CLIDR.LoUIS) before every WFI. When
LoUIS is 0, this costs nothing. The MI reply now returns CLIDR_EL1 of cpu@100 (A510) to check it.
XNU never calls PSCI CPU_SUSPEND, so cores never power-collapse.

### USB monitor loop (shim/fuxi-probe/probe.c) [src]
After delegation, `probe_secondary_main` on cpu@100 runs `core_report[8]++`, then
`core_report[9]=cntpct`, then `dsb sy`, then `usb_poll()` (one MMIO read of GEVNTCOUNT) **in a
tight loop with no wait**. The MMU is off, so every loop variable access is an uncached DRAM
access. This means one A510 at 100 % at ABL's silver frequency, plus a constant stream of
DRAM and config-NoC transactions, for the whole iOS run. The 0x54 reset-deadline check is
in the same loop. Before the handoff, `probe_main` on cpu0 also busy-polls, but only briefly.

## Ranked heat sources (to be confirmed by the logs)
1. **CPU clusters left at ABL's perf level with no DCVS** [guess on the level, fact that nobody
   lowers it]. Rail voltage follows the vote, so even idle WFI cores leak at that voltage.
   The X3 and the 4 gold cores matter most. Fix: 0x57 'CL' (below).
2. **iOS doing CPU work that a GPU would normally do**: CoreAnimation software rendering in
   backboardd/SpringBoard, plus the 10 MB framebuffer copy at up to 10 Hz (patch 0056 present
   thread, 60 Hz vsync timer notifying clients). This is real CPU load during any animation.
   Owned by the display/GPU lanes. Report only.
3. **Monitor core busy loop** (fact). It costs one core plus uncached DRAM traffic. Fix: 0x57 'MI'.
4. **No core power collapse** (fact: XNU only uses WFI). PSCI CPU_SUSPEND 0x40000004 would
   need a warm-boot resume path in XNU. This is a stretch goal and is not implemented.
5. **Touch polling** (patch 0091): GPIO read every 8 ms, FIFO read over GPI DMA every 32 ms
   with `delay(1)` busy waits. Small. Touch lane, report only.
6. **USB charging while the phone is tethered**, with no charger driver (PMIC defaults), and
   **backlight** left at ABL's level. Neither is measurable with TSENS (no battery or skin
   sensor) [guess]. Report only.

## Changes in this lane
- `shim/fuxi-probe/probe.c`: one separated block, "Thermal lane (request 0x57)", plus
  two hook lines. Request 0x57 (vendor IN) is allowed before and after the XNU launch:
  - `wValue 'TH' 0x5448`, `wIndex` mask (1/2/4 TM of tsens0/1/2, 8 SROT, 0x10 EPSS state):
    a 360-byte read-only snapshot (status words of all sensors, TRDY, per-domain enable/state/
    dcvs/perf), plus the monitor idle mode and CNTKCTL.
  - `'LU' 0x4c55`, `wIndex` domain: 40 frequency + 40 voltage LUT rows, plus the valid-row count.
  - `'CL' 0x434c`, `wIndex = domain | level<<8`: writes the perf-state index (also the per-core
    registers when DCVS ctrl bit 0 is set). Refused (rc 1) if the domain is not enabled, and
    refused (rc 2) if level >= LUT count. Level 0xff only reports. Returns the state before
    and 2 ms after the write.
  - `'MI' 0x4d49`, `wIndex` 0/1/2: cpu@100 idle between polls. 0 = busy poll (default, the
    current behaviour), 1 = `delay(500 us)`, 2 = WFE woken by the generic-timer event stream
    (CNTKCTL_EL1 EVNTEN, EVNTI 12, about 0.43 ms at 19.2 MHz). After any USB event the
    monitor stays busy for 20 ms, so back-to-back console reads keep full speed. The 0x54
    deadline check is unchanged (it runs every loop iteration).
- `tools/fuxi-xnu-monitor.py`: `--thermal-probe` (staged first contact, one block per
  request, writes `thermal-stage.txt` before each request), `--thermal-test S` (1 Hz
  samples before the handoff), `--thermal-log S` (samples after the handoff into
  `thermal.jsonl`), `--thermal-mask`, `--monitor-idle N`, `--cpu-level '0:IDX,1:@MHZ,...'`
  (before the handoff), `--cpu-at SECONDS=SPEC` / `SECONDS=idle:N` (mid-run). `--no-handoff`
  now always stops before 0x49.
- `tools/run-ios-thermal.py`: lane runner (0091d kernel/payload, `--touch-power`, 60-min
  deadline renewal, `--loader-only`). Takes the device serial from `FUXI_SERIAL`.
- No XNU patch (0094 unused). Everything is controlled live from the monitor. If CLIDR shows
  LoUIS > 0, a candidate 0094 would skip `platform_cache_idle_enter()` on FUXI (WFI keeps the
  caches).

Artifacts (gitignored):
`shim/out/lane-thermal/{fuxi-xnu-boot.img,fuxi-xnu-monitor.py,run-ios-thermal.py}`.
The boot image is the 0091d payload plus this loader. SHA-256 values are in the commit message
and the final report.

## Test plan (coordinator, serial device access)
Run all commands from the repository root as root, with `FUXI_SERIAL` set.
1. **T1, loader only, safe** (phone in Android; resets itself at 90 s):
   `python3 shim/out/lane-thermal/run-ios-thermal.py research/device/thermal-01 --loader-only --thermal-probe --monitor-idle 2 --thermal-test 30`
   Expect five `thermal probe stage ... ok` lines, three LUT lines (MHz, mV), the
   CLIDR/LoUIS value, and 30 samples. All sensors should be VALID, with temperatures
   plausible at 30-50 C. Per-request latency with MI 2 should be at most a few ms. If the SoC
   resets, `thermal-stage.txt` names the block: do not reuse that address and report it.
   Optional T1b: add `--cpu-level 0:@1000,1:@1200,2:@1200` and check `state_after_mhz`.
2. **A/B in one iOS boot** (phone in OrangeFox, as for run-ios-0091d). Start cool
   (aoss below 35 C):
   `python3 shim/out/lane-thermal/run-ios-thermal.py research/device/thermal-ios-01 --thermal-log 10 --cpu-at 900=idle:2 --cpu-at 1800=0:@1200,1:@1400,2:@1400`
   This gives 0-15 min baseline (ABL levels, busy monitor), 15-30 min monitor idle, and
   30-45+ min lower CPU levels. Check that UI and touch stay usable after each step. Choose
   the MHz values from the LUT printed in T1. `@` picks the highest non-turbo row at or
   below that frequency.
   Alternatively, use separate runs: A = `--thermal-log 10`, B = `--thermal-log 10
   --monitor-idle 2 --cpu-level 0:@1200,1:@1400,2:@1400`, 15 min each, with a cool-down in
   Android between them.
3. Expected results [guess]: if ABL left the clusters at high levels, the CPU and cpuss sensors
   should drop by 5-15 C in steady state after the CL step, with smaller drops on the
   GPU/DDR/aoss sensors. The MI step should give a few C on the silver sensors (cpu-0-1 is
   the closest to cpu@100), plus fewer DRAM accesses. If ABL already left low levels, CL
   changes little, and the heat comes from iOS CPU load (item 2). The `state_mhz` vs
   `vote_mhz` columns also show any LMh throttling.
