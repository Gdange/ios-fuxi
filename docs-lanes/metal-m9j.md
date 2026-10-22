# Metal M9j: opt-in backboardd GPU admission with boot recovery

2026-10-22. Local implementation and host validation. No phone access, image
boot, deployment, CI dispatch or push. The supplied isolated CA result (stages
0–5 PASS, 1080×2400, about 30 layers, 60 frames at 60 Hz, GPU about 3.6 ms,
late=0/59, working HostImport) and second-boot cold-frame improvement to about
560 ms are starting observations, not measurements made by M9j. Real home
screen composition still needs the device qualification below.

The chain stays FuxiMetal → dynamically loaded external metal2vulkan → Turnip.
The translator pin stays `720ef1ce9ae2a2068999a8675717418b9416283e`, bridge ABI 6;
no external source, Apple product, shader extraction or cache blob is shipped.

## Exact switches and staging

Both independent keys are required for the executable basename `backboardd`:

1. A root-owned, non-group/world-writable
   `/usr/local/share/fuxi/metal-clients.conf` containing the plain allow line
   `backboardd`. A census entry does not qualify.
2. The exact whitespace-delimited kernel boot argument **`fuxigpu=backboardd`**,
   read using `sysctlbyname("kern.bootargs")`. Absent, unreadable, truncated,
   malformed, prefix/suffix matches and duplicate `fuxigpu=` arguments deny.

SpringBoard is denied unconditionally, including allow and census entries.
Identity comes from dyld's executable path, never inherited daemon argv.
The normal shipped conf contains only the two smoke allow entries and comments.
Default staging rejects a supplied backboardd entry. To enable in an already
mounted, writable offline iOS root, run the existing staging command as root
with the new flag:

```sh
python3 tools/stage-fuxi-metal.py "$IOS_ROOT" \
  "$ICD" "$METAL_OUT/FuxiMetal.bundle" "$METAL_OUT/fuxi-metal-smoke" \
  --translator "$METAL_OUT/libmetal2vulkan.dylib" \
  --metallib "$METAL_OUT/metal-smoke.metallib" \
  --source "$METAL_OUT/metal2vulkan-source.tar.gz" \
  --clients "$METAL_OUT/metal-clients.conf" \
  --conf "$METAL_OUT/metal-smoke.conf" \
  --ca-smoke "$METAL_OUT/fuxi-ca-smoke" --ca-conf "$METAL_OUT/ca-smoke.conf" \
  --enable-backboardd
```

The flag adds the backboardd line to the installed copy and prepares storage;
it does not modify the input conf or set any boot argument. Separately add
`fuxigpu=backboardd` to the intended boot's existing kernel arguments. Preserve
other arguments. Run the real backboardd job; do not exec-swap the render
server or SpringBoard. Remove the isolated CA exec-swap for this experiment.
Removing the boot argument suffices to select CPU on the next process/boot;
default restaging with the shipped conf also removes the config key. Neither
operation clears the strike record. No mid-process policy reload is added.

## Recovery state and ordering

`BackboardGate.hpp` implements the production state machine, tested directly
on host filesystems. After the two policy keys, discovery reads
`kern.bootsessionuuid`, validates a UUID, opens private storage and takes a
nonblocking, process-lifetime exclusive flock on `gate.lock`. Missing UUID,
untrusted/unwritable storage, malformed records or a competing live process
refuse discovery. No GPU backend is constructed before this admission.

The bounded text record is `FXBB1 <boot-uuid> <strikes> <state>\n` in
`gate.state`. States are `armed`, `good` and `denied`; the strike limit is **1**.
The exact same-boot transitions are:

| Previous state | Next process decision |
| --- | --- |
| No record | Persist `armed`, then allow |
| `armed` | Add a strike, persist `denied`, return nil: CPU |
| `good` | Preserve strike count, persist a new `armed`, then allow |
| `denied` or strike limit reached | Deny for the remainder of this boot |

A different valid boot UUID resets the old valid record and retries. A process
marks `good` only after **120 successful command-buffer completions that store
a defined IOSurface render target**. At most one completion is counted per
command buffer, regardless of draw or surface count. Empty completions,
event-only commands, ordinary private-texture work, discarded targets and
failed submissions cannot qualify. This is a completed-composition proxy;
there is no physical present/vsync callback in this frontend. Multiple CA
command buffers per screen refresh can produce multiple counted completions.

`armed` is durable before allow, including before backend creation, cold
pipeline builds and the HostImport preflight. Thus an abort, jetsam, watchdog
kill or other death during startup/qualification leaves a strike for the next
KeepAlive launch. The HostImport capability preflight remains mandatory and
refuses discovery if unavailable. Backend creation/registry failures also latch
failure. The native admin-only GPU user-client policy, sandbox, memory limits,
features and display driver are unchanged. Resolving a nonroot account for
storage does not grant it new kernel or sandbox privileges: a rejected driver
open safely refuses discovery rather than weakening those boundaries.

Every frontend failure (including unsupported selectors or translation errors)
and every checked Vulkan error latches failure for admitted backboardd.
Optional pipeline-cache calls also report device loss/timeout even though
ordinary cache errors remain memory-only. Failure after `good` writes `denied`,
blocks further backend work/device discovery and causes CPU on the next launch.
The ordinary 64-line frontend log cap cannot suppress this latch. A healthy
process that exits after `good` may retry GPU; an arbitrary abort after `good`
without a reported rendering error does not count as an early-death strike.

There is no supported replay of an existing CA transaction/context onto CPU.
M9j guarantees the recovery decision at discovery/next respawn, not a live
switch or an unsolicited daemon restart. A process stuck outside the existing
GPU fence timeout still depends on the system watchdog/KeepAlive to restart.
Kernel/system-wide unrecoverable GPU hangs remain outside a userspace gate.

Records are private, regular, single-link 0600 files with no symlink following.
Updates use an exclusive 0600 temp, file fsync, rename and directory fsync;
the lifetime lease serializes writers. A crash during the `good` update may
leave `armed` and conservatively select CPU. A failure to persist `good`
blocks the current process and attempts to persist `denied`. If a runtime
failure cannot create/rename its temp (for example disk exhaustion), it first
invalidates/fsyncs the existing record inode, then attempts a denied rewrite.
A torn/empty rewrite is refused, including on a later boot, until repaired by
root. If the filesystem cannot persist even that invalidation, the log reports
`persisted=0`; permanent storage failure cannot provide a durable guarantee.
No signal handler does filesystem I/O and no stale on-disk lock blocks a new
process once the old process has died.

## UID and pipeline-cache storage

Staging reads the mounted image's
`System/Library/LaunchDaemons/com.apple.backboardd.plist`. An explicit UserName
is resolved against the target's master.passwd/passwd, never the host's user
database. A system launchd job without UserName has the launchd root default.
Missing/unresolvable metadata fails enabled staging. At runtime **geteuid()**
determines the private directory name and required owner; the allow log prints
that UID. There is no hardcoded mobile/backboard UID or ownership workaround.

The layout is:

```text
/private/var/db/fuxi-metal/                         root:wheel 0755
  backboardd-<effective-uid>/                       target uid/gid 0700
    gate.lock, gate.state                         effective uid 0600, nlink 1
    backboardd.cache                              effective uid 0600, nlink 1
  <other-root-client>-0/                           root 0700
    <client>.cache                                root 0600, nlink 1
```

Only root may create client directories; a nonroot backboardd requires its
staged directory and cannot create or replace the parent/sibling directories.
Existing unsafe owner/mode or linked directories are refused, not repaired.
Enabled staging permits the previous root-owned 0700 M9i parent and changes it
to 0755; client directories and all cache/record contents remain private. No
directory is made world writable. Runtime root-only use also accepts a 0700
parent. Per-client ownership and exact 0700/0600 checks are enforced on open.
An unexpected runtime UID consequently selects CPU when its gate directory is
missing or owned differently.

All M9i payload bounds, driver/translator identity, Vulkan header, checksum,
no-follow, exact-read, atomic-save and single-link checks remain. Cache I/O
failure alone does not turn a rendering failure counter on. Cache files move
from M9i's flat namespace into the private client directories; old flat files
are left untouched and are not automatically reused. Expect a fresh first
cache build when switching this storage layout, then normal reuse. No warm
smoke cache is assumed to contain all real backboardd pipelines.

## Observability

Every gate decision starts `FUXI MTL: backboardd gate ...`. Representative
successful lines, with variable timing/counts:

```text
FUXI MTL: backboardd gate allow armed strikes=0 uid=... good_frames=120
FUXI MTL: gate allow backboardd
FUXI MTL: IOSurface import PASS BGRA8 ... same-memory
FUXI MTL: backboardd first_frame_ms=... gpu_ms=...
FUXI MTL: backboardd gate good frames=120
FUXI MTL: backboardd frames=... gpu_ms=... late=... failures=0 hz=... window_frames=...
```

A process-lifetime dispatch timer reports at most once per five seconds,
including idle windows, without adding a per-frame disk write. `frames` and
`late` are cumulative qualifying completions; `hz` and `window_frames` cover
the elapsed report window. `gpu_ms` is mean measured GPU time for qualifying
command buffers in that window (zero for no completions), not time spent in
separate intermediate-only command buffers. Concurrent command queues can
make process-counter attribution approximate. `late` counts qualifying commit
through synchronous-completion wall durations above 16.667 ms; it does not
measure CA encoding, actual vblank deadlines or physical presentation.
`first_frame_ms` measures admission through the first qualifying completion,
including initialization/cold compilation. `failures` counts gate failure
notifications; one underlying error may be reported by backend and frontend.
Static home screens may correctly have hz=0. Timer scheduling can be delayed
under load; rate limiting uses monotonic time.

Fallback examples:

```text
FUXI MTL: backboardd gate deny reason=policy-or-bootarg
FUXI MTL: backboardd gate deny reason=strikes strikes=1 uid=...
FUXI MTL: backboardd gate deny reason=host-import-unavailable
FUXI MTL: backboardd gate runtime-failure next-respawn=CPU persisted=1
FUXI MTL: gate deny backboardd
FUXI MTL: gate deny SpringBoard
```

An armed/allow line alone proves only admission. Increasing completion counts,
positive GPU time during animation, zero failures and a visually correct real
home screen are required to establish useful GPU composition. A strike or
runtime failure followed by gate deny and a working CPU UI establishes fallback.

## Host validation and short device plan

Run the portable and native-on-Mac controls with:

```sh
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-stage.py --policy-storage-only
python3 tools/test-fuxi-metal-m9g.py --translator "$BRIDGE_SO"
python3 tools/test-fuxi-metal-m9h.py --translator "$BRIDGE_SO"
python3 tools/test-fuxi-metal-m9i.py --translator "$BRIDGE_SO"
```

The frontend harness now runs production client parsing and recovery tests on
Linux as well as Mac, with -Werror and ASan/UBSan. Coverage includes both keys,
default/missing-arg/SpringBoard/census denial, exact boot tokens, crash/strike,
healthy rearm, new boot, fault after good, concurrent lease, corrupt/symlink/
FIFO/hardlink/mode/UID records and update failure. Staging tests cover generated
and explicit allow entries, target UID lookup, restrictive directory modes,
unsafe ownership/path refusal and the actual flag/config writer using temporary
Mach-O format fixtures. Fixtures do not claim native signing validation.
Native GateFrontendTests additionally exercise checkClient's once-only decision,
actual commit qualification versus empty/private/discarded work and a runtime
failure after the log cap. Mac Foundation/Metal compilation/execution is skipped
on this Linux host and must still run before device qualification.

M9g/M9h/M9i lavapipe executable regressions passed using the available external
ABI-6 bridge, including pixels, object reuse, persistence, corruption/concurrent
writers and memory-only fallback. M9i's fake-driver checks additionally cover
UID/mode/nlink and production Vulkan loss/timeout plus optional-cache loss
latching. Production Backend syntax, Python syntax, staging format fixtures and
git whitespace checks are part of local validation. No native iOS compilation
or device result is asserted here.

For a separately authorized device experiment:

1. First run the Mac frontend suite and build the normal matching bundle/bridge.
   Default-stage and boot without `fuxigpu=backboardd`: require gate deny for
   backboardd and SpringBoard and the existing working CPU home screen.
2. Enable-stage, still omit the argument: require backboardd deny. Then boot
   with both keys and the real render-server job. Require armed, HostImport,
   first-frame and increasing frame logs during animation, eventually good,
   failures=0, reasonable GPU time/rate and visually correct composition.
   Compare real interactive latency and system behavior; proxy hz/late alone
   cannot certify physical 60 Hz scanout. Inspect cache load/save lines and
   repeat a fresh boot to measure cache reuse/cold latency.
3. Exercise a controlled process termination before 120 qualifying completions
   in an isolated recovery test. The next KeepAlive launch must log strikes=1,
   gate deny and restore the working CPU UI; further launches in that boot must
   remain denied. Separately test a reported GPU failure after good: require
   persisted=1 and CPU at the next launch. Host fault injection is never a
   production boot argument or environment override.
4. A fresh boot UUID with both keys must log new-boot reset and retry. Removing
   the argument/default restaging must keep CPU regardless of old good state.
   Stop qualification at a rendering error, persistent stall, visual corruption
   or failed CPU recovery; retain private logs for diagnosis.
