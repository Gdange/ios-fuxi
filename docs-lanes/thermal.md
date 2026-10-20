# Milestone 7 — opt-in XNU touch boost

Patch `0097-fuxi-touch-boost.patch` adds `fuxiperf=1` (default **off**).
The FTS report hook (0091/0093) and debounced button-post hook (0092) publish
input timestamps and wake a sleepable thread-call. AppleCLCD starts it in both
GPU and CPU modes and records swap timestamps. Hooks do no MMIO and do not
sleep. Only the worker owns EPSS/TSENS accesses; repeated input coalesces on
that one thread-call. A 100 ms worker sample bounds thermal/expiry response.

All three EPSS domains use their measured addresses below. The worker reads
up to 40 frequency LUT rows, terminates at a repeated frequency, excludes
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
the thermal lane's CPU sensor inventory. Both TRDY bits and every CPU sensor's
VALID bit must be set; signed deci-C values must be plausible (-40..150 C).
Missing/invalid data forces cool. A new boost requires hottest CPU <=60 C;
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

Status 2026-10-20: offline work only. Nothing in this lane has run on the device yet.
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
