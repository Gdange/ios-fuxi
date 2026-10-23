# Cooling lane: idle heat (P-COOL)

The phone runs warm while iOS sits idle on the home screen. Earlier settled
seconds showed (observed, NOTES): launchd at about 657-969 ms of CPU per
second, mediaserverd about 510 ms/s, 2-4k IPIs per second per CPU, a timer
interrupt storm, and CPU sensors at about 49-55 C with `fuxiperf=1` capping
the clusters. Most of that is work iOS starts on its own: 334 of the 530
daemons in the 16.7 launchd cache start by themselves, and some of them keep
retrying hardware we do not have.

This lane ports the parts of HackiOS (which boots iOS 16.5 on a Snapdragon
778G phone) that cut that work, and adds a per-CPU load log so the effect can
be measured. **Nothing here changes the current boot by default**: the
launchd change is a root-edit tool you run, the kernel parts are boot-args,
and the Metal change only tightens a gate that already returned no GPU.

| Piece | Where | How to turn it on |
| --- | --- | --- |
| Daemons on demand | `tools/launchd-on-demand.py` | run on the mounted root (undo: `--undo`) |
| mediaserverd / SpringBoard never get Metal | `tools/fuxi-metal/ClientGate.hpp` | always, after FuxiMetal is restaged |
| Per-CPU idle log | patch 0105, `osfmk/arm64/fuxi_idle.c` | `fuxicpuload=N` (seconds, 1..3600) |
| Refuse system sleep | patch 0105, `IOPMrootDomain.cpp` | `fuxinosleep=1` |
| xpcproxy keeps its pid version | patch 0105, `kern_exec.c` | `fuxixpcid=1` |
| A/B summary of runs | `tools/fuxi-thermal-summary.py` | offline, on monitor output |

## 1. Daemons on demand (`launchd-on-demand.py`)

Ported from HackiOS `hackios-xnu-userland` `userland/mkjobs.py` (MIT), the
`--rest` pass. HackiOS writes its own job plists; we edit the compiled cache
`/System/Library/xpc/launchd.plist` in place, like `disable-launchd-jobs.py`.

For every `com.apple.*` job outside the keep list it removes the keys that
make launchd start a job by itself: `RunAtLoad`, `KeepAlive`, `StartInterval`,
`StartCalendarInterval`, `WatchPaths`, `QueueDirectories`, `StartOnMount`
and `LaunchEvents`. The job stays registered, so it still starts the moment a
program looks up one of its Mach services or connects to its socket; a job
with neither never starts (reported as such). The removed keys are saved in
the job under `_FuxiOnDemand`, so `--undo` restores the exact stock cache.
Disabled jobs and non-Apple jobs (`org.fuxi.*`) are never touched.

```sh
python3 -I tools/launchd-on-demand.py MNT --dry-run   # report only
python3 -I tools/launchd-on-demand.py MNT             # apply
python3 -I tools/launchd-on-demand.py MNT --undo      # back to stock
python3 -I tools/launchd-on-demand.py --list-keep     # keep list with reasons
```

`--keep LABEL` keeps one more job (and restores it if an earlier run stripped
it); `--drop LABEL` takes one off the keep list. Unknown labels stop the tool
before it writes anything.

**Keep list (27).** HackiOS's core list (`IOS_JOBS` in `userland/build.sh`)
plus wifid, labels checked against 16.7: notifyd, both cfprefsd daemons,
logd, logd_helper, syslogd, diagnosticd, distnoted, fseventsd, both
containermanagerd jobs, runningboardd, backboardd, SpringBoard,
mobile.usermanagerd, mobilegestalt.xpc, lsd, powerd, analyticsd,
iconservicesagent, pluginkit.pkd, wifid. Added for this port: configd (USB
Ethernet), UserEventAgent-System (it delivers the LaunchEvents of the kept
jobs), peakpowermanagerd (host of the `sweetexecswap` CA smoke exec), keybagd
(data-protection keybag at boot) and usb.networking.addNetworkInterface.
mediaserverd is deliberately **not** kept: we have no audio, and it is the
second-busiest process at idle. It still starts on demand when a client asks
for one of its services; `--keep com.apple.mediaserverd` brings it back.

On a scratch copy of the real 16.7 cache (observed, host only): `327 jobs on
demand (32 of them never start), 0 restored, 27 kept, 30 disabled left
alone`, and `--undo` gives a cache equal to the original.

## 2. No Metal for mediaserverd and SpringBoard

HackiOS logs show mediaserverd's camera pipeline (CMCapture, AppleCVAPhoto)
preloading shaders and aborting about 28 times a boot, and SpringBoard's Siri
orb prewarm aborting every 8.3 s, each abort followed by a relaunch. Our
FuxiMetal gate already gave both a GPU-less (nil) device by default.
`ClientGate.hpp` now hard-denies both names (`hiddenClient()`) before
`allow`/`census` from the clients file or a boot-arg is considered, and
`stage-fuxi-metal.py` refuses a clients file that names either. There is
nothing to switch on; it takes effect when FuxiMetal is rebuilt and
restaged. HackiOS's `mediamtl=1` (give mediaserverd Metal again) and
`siri_orb=0` hook are not ported: the first would undo the point, the second
is not needed because SpringBoard never gets a Metal device here.

## 3. Kernel patch 0105 (`xnu/patches/0105-fuxi-idle.patch`)

All three parts compile only with `FUXI` and act only when their boot-arg
is set exactly as shown; without them the kernel behaves as before (the
patch adds lines only, the stock pid-version bump stays in place). Sources
are kept in `xnu/fuxi-idle/`. Remember the loader command line limit (under
608 bytes, see `build-fuxi-metal-images.sh`) when adding them.

**`fuxicpuload=N`** (from HackiOS `redwood_cpufreq.c` `cpuload=MS` and
`ml_cpu_idle_time()` in `redwood.patch`, APSL 2.0). A kernel thread wakes
every N seconds, with 100 ms timer leeway so it does not add wakeups of its
own, and prints one console line:

```
FUXI IDLE: cpuload every 10s over 8 CPUs (fuxicpuload=10)
FUXI IDLE: cpuload 10s up 600s avg 12% irq 817 ipi 1940 tmr 384 | c0 23% u5 s17 i812 p1934 t377 | c1 1% u0 s0 i5 p6 t7 | ...
```

`avg` is the mean busy share over CPUs; per CPU: busy % (wall time minus
recount idle time, so interrupt time counts as busy), `u`/`s` user and system
run share, then `i`/`p`/`t` interrupts, IPIs and timer interrupts per second
(cpu_stat counters). The header totals are per second over all CPUs. A CPU
not yet registered prints `-`. Use 10 or more: the line is about 400 bytes.

**`fuxinosleep=1`** (from HackiOS `redwood.patch`, which sets
`gSleepDisabledFlag` by default and re-enables sleep with `syssleep=1`).
`checkSystemSleepAllowed()` returns `kPMConfigPreventSystemSleep`, so idle
and demand sleep are refused. Rationale from HackiOS: powerd asks for sleep
once the display is off, and the CPU sleep path then hangs or loops; we have
no sleep/wake path either. It uses its own flag because the device tree
`sleep-disabled` property overwrites `gSleepDisabledFlag` at boot complete.
Logs `FUXI IDLE: system sleep refused (fuxinosleep=1)` at boot and the 1st,
2nd, 4th, 8th ... refusal with its reason. Whether our boot ever reaches a
sleep attempt is not known yet; the refusal count answers that.

**`fuxixpcid=1`** (from HackiOS `redwood.patch` xpcproxy hunk). When the
process that execs is `xpcproxy` (launchd's spawn trampoline that execs the
real daemon in place), the exec keeps the pid version instead of taking a
new one, so the audit token launchd recorded for the spawn still matches the
daemon. HackiOS keeps this on by default; their reason is that launchd
otherwise can treat the daemon as a different instance. That this causes any
of our launchd CPU time is a **guess**; the A/B run below tests it. Logs the
first eight: `FUXI IDLE: xpcproxy pid P exec keeps pidversion V (fuxixpcid=1)`.

## UniXNU idle and thermal code: nothing ported

`drivers/ArmCpu` is empty in our checkout; its test scripts only show CPU
halt parking in WFI with interrupts masked (quiesce), which XNU already does
in its idle loop. UniXNU-sheng's `CpuFreqProbe.cpp` (`sheng-cpufreq`) raises
EPSS to the highest level, the opposite of cooling, and our `fuxi-perf`
already owns EPSS votes. Its `sheng-disable-jobs` boot-arg turns off
identityservicesd, locationd, CommCenter and iconservicesagent; the first
three are already on demand after section 1, and iconservicesagent is kept
for the home screen. No deeper CPU idle states exist in either project.

## Left out

- HackiOS's DENY list (jobs removed outright, not made on demand): on demand
  covers them and stays reversible with `--undo`.
- HackiOS's injected userland libraries and Metal/Mesa work: other lanes,
  not cooling.
- `mediamtl=1` and `siri_orb=0`: see section 2.
- `syssleep=1` as the default-on inversion: our default must not change, so
  the sleep refusal is opt-in (`fuxinosleep=1`) instead.

## Expected effect (guesses until measured)

Fewer daemons should mean fewer launchd spawn/exit cycles and fewer timer
and IPI wakeups, so launchd's share and the `irq`/`ipi`/`tmr` rates should
drop, followed by a few degrees on the CPU sensors once settled. Keeping
mediaserverd from starting should remove its ~510 ms/s, if nothing asks for
its services at idle (unknown). The size of each step is not known; measure
each separately.

## How to measure

Each boot with the monitor's thermal log, e.g. `run-ios-thermal.py OUT
--thermal-log 5` (writes `OUT/thermal.jsonl` and `OUT/boot.log`), left on the
home screen for at least 20 minutes without touching it. Then:

```sh
python3 tools/fuxi-thermal-summary.py OUT_A OUT_B OUT_C --settle 300
```

It skips the first 300 s after handoff and prints, per boot, the medians of
the hottest CPU sensor and of the silver, gold/prime, CPUSS, GPU and DDR
groups, median EPSS MHz per domain, the median `FUXI IDLE: cpuload` values
and counts of the cooling log markers, with the difference to the first
(baseline) boot. Room temperature and the case matter: run A/B back to back
and start each from a cool phone.

## Device test plan

Kernel with patch 0105 built by CI; FuxiMetal rebuilt and restaged; the
same ADT and command line as the current boot plus the args named per run.

1. **A, baseline:** current root, `fuxicpuload=10`. Expect the start line
   `FUXI IDLE: cpuload every 10s over 8 CPUs (fuxicpuload=10)` and one
   `FUXI IDLE: cpuload 10s up ...` line every ~10 s; no other `FUXI IDLE`
   lines; boot otherwise identical to the current one.
2. **B, daemons on demand:** on the mounted root run the tool with
   `--dry-run`, then for real. Expect a summary line
   `327 jobs on demand (32 of them never start), 0 restored, 27 kept, 30 disabled left alone, ...`
   (counts may differ if the root changed). Boot with `fuxicpuload=10`.
   Expect SpringBoard on screen, USB Ethernet up, and no
   `FUXI MTL: gate allow mediaserverd`; if mediaserverd starts and its
   stderr reaches the log it prints `FUXI MTL: gate deny mediaserverd`.
   Compare A and B with the summary tool.
3. **C, xpcproxy identity:** B plus `fuxixpcid=1`. Expect up to eight
   `FUXI IDLE: xpcproxy pid ... exec keeps pidversion ... (fuxixpcid=1)` lines.
   Watch for launchd errors about job identity or daemons failing to check
   in; if any appear, drop the arg.
4. **D, no sleep:** C plus `fuxinosleep=1`. Expect
   `FUXI IDLE: system sleep refused (fuxinosleep=1)` at boot; let the display
   time out and count `... refused ... reason R options 0x... count N` lines.
5. If B breaks something, `--keep` the missing job or `--undo`, and record
   the job in the keep list with its reason.

Host tests: `tools/test-launchd-on-demand.py`, `tools/test-fuxi-idle.py`,
`tools/test-fuxi-thermal-summary.py`, `tools/fuxi-metal/ClientGateTests.cpp`
and the policy checks in `tools/test-fuxi-metal-stage.py`.
