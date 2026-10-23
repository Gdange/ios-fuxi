# Backlight phase 1: opt-in observation service

## OBSERVED

`research/bl/keys74-iok.log` records these failed lookups:

| Consumer | Trace lines | Lookup | Result |
| --- | --- | --- | --- |
| backboardd | 31, 45 | `IOPropertyMatch { backlight-control = true }` | none / zero services |
| backboardd | 35 | `IODeviceTree:/backlight` | none |
| SpringBoard | 186, 229 | `IOPropertyMatch { backlight-control = true }` | none |

The Boolean match contains no `IOProviderClass` restriction. A registered
IOService with an **OSBoolean true** property satisfies this predicate;
an integer 1 or an ADT byte property is not the same property type.
`IOServiceNotificationUser` is matching infrastructure metadata.

The scripted Power events at lines 267–283 lead to `SWEET FB: display off`
(273) then `SWEET FB: display on` (277). There is also an earlier off at 154.
The trace therefore demonstrates working display power requests despite the
missing backlight service. The user reports the post-wake lock screen keeps
animating at approximately 58 Hz and never auto-dims/auto-locks. Framebuffer
stats continue after the wake; their presented-frame rate is not itself a
measurement of animation or vsync frequency.

No successful backlight match, brightness read/write, backlight user-client
type, private selector ABI, or request for `IODisplayConnect` appears in this
trace. It cannot establish what happens after a successful match.

## Public API evidence

Apple's [IODisplayLib.c](https://github.com/apple-oss-distributions/IOKitUser/blob/main/graphics.subproj/IODisplayLib.c)
resolves an `IODisplay` directly or recursively below a supplied framebuffer.
It reads `IODisplayParameters`, expects parameter dictionaries containing
numeric `min`, `max`, `value`, and sends flat parameter dictionaries through
`IORegistryEntrySetCFProperties`. Float setters scale through the advertised
integer range. This is public IOGraphics behavior, not evidence of the closed
iOS 16.7 BacklightServices implementation.

XNU's [IORegistryEntry.cpp](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/iokit/Kernel/IORegistryEntry.cpp)
funnels the string and symbol property overloads through the OSSymbol
get/set methods. Copy-property wrappers call getters, while whole-table
serialization uses a separate snapshot path. The probe instruments both.

## GUESS and experiment

BacklightServices and SpringBoard may need a usable brightness endpoint to
advance their idle dim/off policy after wake. The missing endpoint is a
plausible cause, not an established cause: idle assertions, timer state,
framebuffer replies, or another missing service could also explain it. The
earlier off in this same trace limits the claim that backlight discovery is
always required for display off.

AppleARMBacklight-style property matching does not require that class name.
Publish `FuxiBacklight`, an IOService through a small `IODisplay` compatibility
metaclass, attached below AppleCLCD. This also gives public IODisplay helpers
a display descendant without implementing an IOGraphics framebuffer family.
It is not an implementation of Apple's private AppleARMBacklight ABI.

`IODisplayConnect` normally represents a framebuffer connection in the
IOGraphics service hierarchy; adding an empty service bearing that name
would not implement its semantics. No such nub is added in phase 1. The
direct `IODisplay` conformance handles the public helper's discovery rule.

## Published contract

Phase 1 uses **`fuxibacklight=1`**. Modes 2 and 3 below also publish the
service. Absent, zero, and values other than 1, 2, or 3
return before allocation, attachment, property publication, or probe logging.
Both publication and start verify the argument; all code and the AppleCLCD
call are FUXI-only. The built-in source is in `iokit/conf/files.arm64`.
AppleCLCD publishes the child before registering itself, so a consumer
awakened by the framebuffer's publication can already find the child.

```text
AppleCLCD
  FuxiBacklight : IODisplay : IOService
    backlight-control = true (OSBoolean)
    IODisplayParameters = {
      brightness = { min = 0, max = 65536, value = 65536 }
    }
```

The numbers are 32-bit OSNumbers. **0–65536 is a normalized integer range**,
chosen to preserve fractional brightness when public helpers map 0.0–1.0.
It is not nits, a measured panel maximum, or a DCS register range. Initial
65536 is a synthetic requested value; actual bootloader brightness is unknown.

`setProperties` accepts `brightness = OSNumber`, `brightness = { value =
OSNumber }`, and either shape wrapped in `IODisplayParameters`. Accepted
values 0–65536 are stored in `fRequested` and reflected in a newly allocated
immutable registry dictionary under a lock. Out-of-range or malformed
brightness returns bad argument, other requests return unsupported. Failed
allocation leaves the previous request intact. No arbitrary dictionary merge,
commit operation, calibrated nits, fade, or power-state implementation is claimed.

A diagnostic IOUserClient can be opened; it forwards property writes to the
service. All external selectors return unsupported and clear output counts.
Their numbers and scalar inputs are logged without guessing which one means
brightness. A request through an unknown selector is not treated as brightness.

## Logging and interpretation

Look for `FUXI BACKLIGHT:` followed by:

- `published observation-only brightness min=0 max=65536 value=65536`;
- `read` / `read-all` with property names, value types, numeric/Boolean values;
- `set` / `setProperties` with the requested dictionary shape;
- `brightness ... requested=... valid=... hardware=untouched`;
- `open ... process=... type=... -> ...`, `close`;
- `method ... selector=... scalars=...` and `scalar[index]=...` lines.

Each category (reads including snapshots, individual sets, setProperties,
opens/closes, external methods, brightness requests) has an atomic counter:
first 64 events, then event numbers divisible by 256. All scalar arguments
of a sampled method are printed. Dictionary logging is bounded to 64 entries
in total per event and three nested levels; strings are identified by type, binary
data by length, and keys are limited to 96 characters. Suppressed log events
still execute and store valid requests. Ordinary reads may come from kernel
matching as well as user space; they do not identify a particular consumer.

No MMIO, panel/DSI/PMIC writes, framebuffer pixel writes, or
DCS commands occur in this service. Mode 1 sends no power notifications.
Even brightness zero only changes stored
state. Existing `SWEET FB: display off/on` remains the independent power path.
Phase 1 may change userland policy by satisfying discovery; it does not promise
to restore auto-lock or reduce power. A later phase can map validated requests
to DCS `0x51` once units and protocol have been observed.

ADT is unchanged. `IOService` publication does not create
`IODeviceTree:/backlight`, so that lookup will still fail. If a later opt-in
trace shows the missing node prevents progress after service discovery,
`tools/mkadt-fuxi.py` / `tools/mkadt.py` can add a separately opt-in metadata
node with only properties actually requested. An ADT node alone cannot provide
a user client or live brightness handling; there is currently no evidence
that it is required in addition to this service.

## Source maintenance and host validation

`xnu/fuxi-backlight/` is canonical for patch `0109-fuxi-backlight.patch`;
`tools/sync-fuxi-backlight.py [--check]` maintains its source snapshots and
build-list hook. The AppleCLCD call stays in canonical
`xnu/fuxi-unixnu/SweetFramebuffer.cpp`, maintained by
`tools/sync-fuxi-unixnu.py` in patch 0106. This follows the existing split
between framebuffer integration and separately compiled IOKit services.

`python3 tools/test-fuxi-backlight.py` checks synchronization, patch application
after replaying all preceding build-list patches, publication/gating/observation structure, and runs
the production property parser with host dictionaries under ASan/UBSan.
It exercises input shapes, endpoints, invalid values, allocation failure, and
rate limits. `tools/ci/build.sh` runs it for both kernel profiles. These host
checks do not establish iOS acceptance; no device validation is part of phase 1.

Validation for this change: the backlight host test (ASan/UBSan), all eleven
`sync-fuxi-* --check` tools, ARM64 syntax checking of the new service against
local XNU 8796 headers, and the kernel CI shell syntax/dry-run checks passed.
The broad host sweep had 43 passing scripts; 22 need explicit external build
inputs and two need absent generated build artifacts. The staging test's
artifact-independent `--policy-storage-only` mode also passed. No kernel link,
boot, or device acceptance test was performed.

## Power wake investigation and mode 2 interest experiment

**Root cause status:** the missing logical backlight power notifications are
an established interface gap. Their responsibility for the missing post-wake
auto-lock is **GUESS**, not a demonstrated root cause. Mode 2 repairs that
specific gap as an opt-in experiment; no device result is claimed.

### OBSERVED: two boot traces

The supplied `research/it/bl79.log` and `research/it/wake80.log` both use
`fuxibacklight=1`. The service is discovered, brightness is read, and
`IOGeneralInterest` is installed. No backlight `setProperties` call appears.
The first 64 property writes would be logged, so the missing first call is
not explained by rate limiting.

In `wake80`, the initial on/off requests are at lines 2487/3462. The supplied
timing observation puts that off about 40 seconds after on. The scripted
Power down at line 3541 produces another on at 3546; Power up is at 3557.
There is no subsequent framebuffer off during the reported 420 seconds
without input. This establishes failure to request auto-off, not the internal
state of a SpringBoard timer. The comparative boot observation is that
publishing the service permits initial auto-lock; it does not suffice after wake.

`TouchDetectionMode=0x101` appears at 3554. AKS selectors 17/26/8 also occur,
with selector 8 returning `0xe00002c7` both before and after wake. That error
does not distinguish the successful initial idle cycle from the failed wake
cycle. Ordinary SpringBoard/BLS/CoreBrightness stderr and OS logging are not
available on this kernel console; missing userland messages prove nothing.

### OBSERVED: local iOS code and XNU contract

The supplied ARM64 shared cache was inspected offline with the already
installed `ipsw` disassembler. A small read-only HFS catalog reader also
extracted backboardd from the existing local root image. Extracted bytes,
symbol tables, and disassembly remain only in ignored `research/idle/`.
The following addresses are unslid entry points in those supplied images,
not runtime addresses or a portable ABI across iOS versions.

| Image / entry point | Static finding |
| --- | --- |
| CoreBrightness `_DisplayCreateWithID`, `0x1af0d8510` | Reads `IODisplayParameters` / `brightness` / `value`; initializes the cached backlight-on word at context offset `0x68` from whether value is nonzero. The synthetic initial 65536 therefore describes an on backlight. |
| CoreBrightness `_BacklightGeneralInterestNotificationCallback`, `0x1af0e29e4` | Recognizes **`0xe0000230` = ON** and **`0xe0000210` = OFF**, setting that same word to 1 or 0. The inspected callback ignores the payload and does not reread brightness or dispatch a BLS completion. |
| CoreBrightness `___DisplayFadeUpdateFactorFade`, `0x1af001adc` | Checks the cached on word during a positive-factor fade and has a bounded wait/retry path when the backlight is still considered off. This proves the word affects fade handling; it does not prove an idle-timer dependency. |
| IOMobileFramebuffer `_kern_GetBlock`, `0x1af224cac` | Selector **79 (`0x4f`)** reads a generic block, with an identifier and byte offset in scalar inputs. It copies data in chunks up to 4096 bytes. A 40-byte zero reply is not a power-completion acknowledgement. The trace does not identify the requested block. |
| IOMobileFramebuffer `_kern_RequestPowerChange`, `0x1af2284f0` | Uses selector 12. Power callbacks use the separate notification registration path. Neither boot trace contains selector 72 registration or a `notifications type` line, so there is no observed power-callback subscriber to notify. |
| SpringBoard `-[SBBacklightIdleTimer reset]`, `0x1c5b3447c` | Enables its attention timer if disabled, resets its expectation, and reconfigures attention timeouts with reset requested. A Power event reaching HID is not proof this method ran. |
| SpringBoard `-[SBBacklightController backlight:didCompleteUpdateToState:forEvent:]`, `0x1c5e2f218` | Invokes a pending change completion. The request completion block subsequently notifies backlight observers. This is a BLS completion path, distinct from the CoreBrightness interest callback. |
| BacklightServicesHost `-[BLSHBacklightOSInterfaceProvider transitionToDisplayMode:withDuration:]`, `0x1ef791c48` | Has a CBDisplayStateClient path and a legacy path using `BKSHIDServicesSetBacklightFactorWithFadeDurationAsync` plus its own completion timer when that client is unsupported. The runtime branch has not been measured here. A mandatory kernel display-on-complete handshake cannot be inferred from the method name. |
| backboardd `-[BKHIDUITouchDetectionModeDigitizerServiceWrapper applyUIMode:]`, `0x100080da8` | ORs **`0x100`** into the digitizer mode for hardware-button change sources; its log formatter calls this `fromHardwareButton`. Thus 0x101 carries source metadata, not an additional touch contact or a completion acknowledgement. |
| backboardd `-[BKUserEventTimer systemShellDidFinishLaunching:]`, `0x100031bf0` | Queues the block at `0x1000324c4` that sets `_safeToResetIdleTimer` and posts the safe-to-set notification. This inspected gate is a shell-startup event, not a per-wake backlight acknowledgement. |
| AttentionAwareness `AttentionServiceEventFilter::filter`, `0x1ab135e68` | Maps eligible HID events, including keyboard events, to attention masks and passes their timestamp to `processHIDEvent:mask:timestamp:`. This path does not itself wait for a backlight-service message. Delivery to a particular timer still depends on its client configuration. |

XNU `IOMessage.h` names the two recognized constants
`kIOMessageDeviceHasPoweredOn` and `kIOMessageDeviceWillPowerOff`.
`IOService::messageClients` fans out through `gIOGeneralInterest`, including
interest notifiers. Simply changing a registry property does not execute
this callback. The inspected CoreBrightness `DisplayOn` dictionary update is
an internal state/event path; it does not establish a required property named
`DisplayOn` or `backlight-state` on FuxiBacklight. No such property is invented.

### Change and limits

**`fuxibacklight=2`** publishes the same observation service as mode 1 and
sends the two recognized, payload-free messages from that child after each
accepted SweetFramebuffer selector-12 request: zero means OFF, nonzero means
ON. Repeated requests also report the current logical state. No message is
sent at publication, before userland has registered its interest.

Only mode 2 runs power requests through AppleCLCD's workloop gate, keeping
concurrent power requests and their notifications ordered. The framebuffer
lock is released before invoking interest clients, allowing callback re-entry.
The child is found under the requesting framebuffer rather than through a
global service pointer. Both the framebuffer integration and the notification
function check the opt-in. Absent/0/unknown values remain disabled; mode 1
retains its previous direct request path and sends no new messages.

This reports **logical power accepted by the existing framebuffer path**.
It is not a panel-ready signal: black clearing/redraw and GPU retirement may
finish later. Brightness remains the separately stored request; no synthetic
zero-brightness write, HID user-activity event, periodic auto-off timer,
selector-79 data, or IOMFB power callback is added. These choices keep the
experiment limited to the observed interest protocol.

**GUESS:** failing to keep CoreBrightness's backlight-on latch consistent
with framebuffer power might interfere with its fade/transition handling and
subsequent idle policy. The callback only changes that latch; the investigation
has not shown it re-arms SpringBoard's timer. Assertions, attention-client
configuration, or a BLS/scene completion that never arrives remain alternatives.

### Exact acceptance experiment

Run matched boots with modes 1 and 2, retaining the existing boot configuration
and using `fuxikeyscript=150 fuxikeyscriptn=1`. Do not add touches or other
keys. Record at least 420 seconds after the scripted Power release. Check:

1. Initial auto-off still occurs. Mode 2 prints
   `FUXI BACKLIGHT: logical power interest notifications enabled` once.
2. Each `SWEET FB: display off` is followed by
   `FUXI BACKLIGHT: power #... off message=0xe0000210 result=0x0 logical-only`;
   each on by the corresponding `on message=0xe0000230` line. The message
   result is fanout status, not proof userland received or processed it.
3. After the single scripted Power wake/release, look for another
   `SWEET FB: display off` without input. Compare its interval to the initial
   idle cycle; allow 120 seconds before declaring a prompt re-arm failure,
   then retain the full 420-second observation for comparison with `wake80`.
4. Repeat the matched pair before attributing a restored auto-lock to the
   change. `0x101` may remain: its hardware-button bit is expected metadata.

If mode 2 posts both messages successfully but auto-off still fails, the
notification experiment has not repaired idle re-arm. The next discriminating
experiment is a temporary userland observer, writing explicit markers through
a verified kernel-console sink (not ordinary stderr): capture the wake-time
BLS state/transition completion, `SBBacklightIdleTimer` reset/descriptor
timeouts, and attention-client enabled state before wake, after Power release,
and at +60/+120 seconds. Use the entry points above to select hooks; do not
guess a new registry property or treat selector 79 as an acknowledgement.
That distinguishes a missing completion from an idle assertion or attention
timer that remains disabled despite completion.

### Host validation

`tools/test-fuxi-backlight.py` now executes the production notification fanout
and framebuffer power-request bodies under ASan/UBSan in addition to the
existing parser tests. It covers all argument gates, both message directions,
repeated/nonzero requests, provider isolation, missing children, iterator
allocation failure, state-before-notification ordering, callback property
re-entry without the framebuffer lock, and the unchanged mode-1 direct path.
The framebuffer host adapter also recognizes the IOReturn declaration.
Host tests cannot establish auto-lock restoration on iOS.

Validation for mode 2: 45 complete host scripts passed, including the backlight,
framebuffer, HID/key, idle and governor tests; all eleven sync checks passed.
Twenty-two other scripts require external build inputs. Two full artifact
checks lack generated Metal/WLAN outputs; the Metal staging policy-only check
and M9i compile-only check passed separately. ARM64 syntax checks passed for
both changed translation units against saved XNU 8796 headers, and for the
framebuffer without FUXI. Both kernel-profile CI dry runs and shell syntax
checks passed. No kernel link or device boot was performed.

## Round 2: isolate ON delivery; do not infer a missing brightness acknowledgement

### OBSERVED device results

These results supersede the untested mode 2 hypothesis above. Mode 1 remains
the preferred existing configuration. Durations are the supplied observations,
not timestamps reconstructed from console line numbers.

| Trace / mode | After SpringBoard display-on | Scripted Power result |
| --- | --- | --- |
| `research/it/bl79.log`, mode 1 | Initial auto-off, roughly 20–40 s across the mode 1 runs | First click wakes; this older script also has later clicks |
| `research/it/wake80.log`, mode 1 | Initial on/off at lines 2487/3462 | Single click wakes at 3546; no re-lock for 420 s |
| `research/it/bl2w81.log`, mode 2 | On at 2489; no auto-off for about 100 s | Click locks at 3820 |
| `research/it/bl2long82.log`, mode 2 | On at 2480; no auto-off for about 250 s | Click locks at 3885 |

Mode 2's startup off/on and scripted off have matching interest log lines,
all `result=0x0`: kernel fanout success, not a userland acknowledgement.
Neither mode 2 run measures auto-lock **after a Power wake**; its single click
turned an already-on display off. The startup off before SpringBoard's on
is distinct from the later mode 1 idle off.

All four traces lack a backlight `setProperties`, user-client open or external
method request. Publication's `set #1 ... IODisplayParameters` is our own
initial dictionary; `set #2 ... IOGeneralInterest` installs a notifier.
Neither is a userland brightness request. The first such request would be
logged, so this absence is meaningful.

### OBSERVED static paths: power, transition completion, and attention timeout

These findings are from the same supplied cache/root images inspected offline.
They establish conditional paths, not runtime branch selection. Extracted
images and long disassembly remain only in ignored `research/idle/`.

**CoreBrightness:** the interest callback (`0x1af0e29e4`) sets context `+0x68`
to 1 for ON or 0 for OFF. Apart from logging, it neither writes brightness nor
calls a fade/BLS completion. `_DisplayCreateWithID` initializes this word from
the advertised brightness, so mode 1's initial 65536 can already seed it to 1.
Absence of an ON message does not mean the latch is off.

`___DisplayFadeUpdateFactorFade` (`0x1af001adc`) delays a positive-target ramp
only when its gated-fade argument is set, context `+0x54 != 0`, `+0x68 == 0`
and `+0x58 == 0`. It stores the wait start at `+0x60`. For **less than 1.0 s**
it moves the fade start time to now and returns. At one second it logs
`Ramp delay hit timeout` and falls through to interpolation. ON removes this
delay. It does not start an infinite wait for a brightness write.

When the elapsed fade portion reaches 1, it stores the target (capped at 1),
clears the gated-fade flag at `+0x58`, calls the callback at fade `+0x28` with
its `+0x30` context (`0x1af002088` onward), and clears the fade period. It does
not poll `IODisplayParameters` or await a hardware acknowledgement. Its caller,
`___DisplayFadeCallback` (`0x1aefe2f58`), applies the factor separately. A legacy
physical-brightness path calls `IORegistryEntrySetCFProperties`
(`0x1af0eec08`) synchronously. Our parser already returns success and updates
the dictionary for valid brightness requests; none was observed here.
An upstream path failing to reach that setter remains possible.

**BLS/CBDisplayStateClient:**
`BLSHBacklightOSInterfaceProvider transitionToDisplayMode:withDuration:`
(`0x1ef791c48`) selects CBDisplayStateClient when supported. Its support query
uses the CoreBrightness property `DisplayStateControlSupported`; its completion
handler (`0x1af11cc84`) dispatches
`didCompleteTransitionToDisplayMode:withError:` to its delegate. This is a
CoreBrightness client/service contract, not a registry property to invent on
FuxiBacklight. The legacy branch calls
`BKSHIDServicesSetBacklightFactorWithFadeDurationAsync` and schedules its own
completion timer; its block (`0x1ef792240`) calls that same delegate method.
No brightness write therefore does not prove this branch is stuck.

Scene completion is separate:
`BLSHEnvironmentTransitionState animationCompleteToTarget:forEvent:animated:`
(`0x1ef798f18`) conditionally reports the completed target to its owner.
A framebuffer selector-12 ON does not prove any of these callbacks occurred.

A concrete idle dependency exists in
`BLSHBacklightIdleProvider backlight:didCompleteUpdateToState:forEvent:`
(`0x1ef795e70`). It compares old/new states and a resume condition logged as
`needToResumeIdleTimer`. On a change or needed resume it calls
`updateForBacklightState:` (`0x1ef79519c`), which updates the AW idle timeout
with reset requested and activates the provider for the interactive state
when permitted. The completion path also reports `backlightTurnedOnForEvent:`.
SpringBoard's separate `backlight:didCompleteUpdateToState:forEvent:`
(`0x1c5e2f218`) invokes its pending change completion; the request completion
then notifies transition observers. These are actual completion boundaries
to measure after display-on, rather than equating ON interest with readiness.

**SpringBoard/attention:** `SBIdleTimerFactory` (`0x1c5aa8278`) uses the shared
`SBBacklightIdleTimer` for timer modes 0–2; mode 3 is a disabled timer.
The active timer wraps `ITAttentionAwareIdleTimer` from IdleTimerHosting,
which configures an AW client. No ordinary NSTimer fallback appears in the
inspected resume/reconfiguration paths when the AW client fails. SB timer
`reset` (`0x1c5b3447c`) enables it, resets its expectation and reconfigures
with reset requested. Expiration arrives via
`idleTimer:attentionLostTimeoutDidExpire:` (`0x1c5b34500`); the policy
aggregator subsequently requests a fade-out. Physical display-on alone
does not perform this reset/timeout sequence.

Face detection is only one event source. In
`_reconfigureAttentionClientAndReset:` (`0x1c5b34fdc`), recovery masks are
`0xa5e` for timer mode 1 and `0x856` otherwise. Finite nonzero sampling adds
**0x80** to the maintenance mask. IT translates this descriptor into AW
timeouts and sampling configuration. Disabling Face ID attention removes
sensor sampling but still uses AW for HID timeouts. Missing Face ID hardware
and a missing/nonfunctional attention service are different failure modes.
iPhone X identity alone proves neither that sampling was enabled nor that
attention stayed true: `SBAttentionAwarenessSettings` also checks supported
events bit 7, accessibility and managed settings.

**backboardd:** `BKUserEventTimer isSafeToResetIdleTimer` (`0x100031e50`)
reads its `+0x39` byte. The block queued by `systemShellDidFinishLaunching:`
sets it and posts the safe-to-set notification. The inspected per-wake path
does not clear/re-acknowledge it. `resetTimer:mode:` queues its own timer
work; this is distinct from SpringBoard's IT/AW timer. Forcing this startup
gate or injecting recurring HID activity is not justified by these results.

### GUESS: one explanation for both observations, not an established cause

The leading shared explanation is **initial timer configuration succeeds,
while a later interactive transition/reset path fails to finish or re-arm**.
Mode 1 can expire its initial timeout, then remain physically on after a
Power wake if a BLS/scene completion, SB reset, descriptor/disable assertion,
or AW reconfiguration prevents the next timeout. The 20–40 s boot interval
is consistent with a short idle policy; it does not establish a 30 s effective
auto-lock preference. Lock-screen policy has its own timeouts/sanitization.

Mode 2 changes both OFF/ON latch transitions and request scheduling. ON can
remove the one-second ramp-start delay and expose a completion/attention-reset
ordering problem that initial setup otherwise avoids. Its boot could then
resemble mode 1's failed post-wake cycle. Other interest subscribers or the
workloop change could also matter. The inspected callback does not disable
the idle timer, and mode 1 can already start with its latch set. Thus the
existing comparison **does not isolate ON as the cause** or prove this race.

"Nobody sets brightness, so our driver never completes the nonzero fade" is
not established: this factor fade completes by elapsed time and its off-latch
delay is bounded. A missing upstream display-ready/factor/scene completion
remains plausible, but no kernel acknowledgement ABI has been demonstrated.
Do not invent a ready property, selector reply, brightness request or periodic
power-off workaround. Missing Face ID is another testable hypothesis, not
proof of permanent attention. Console logs do not expose live BLS/IT/AW state.
If the initial off really is an AW idle expiration, a completely absent AW
service throughout boot would be a poor sole explanation for the asymmetry;
the source of that first off and the live service state still need measurement.

### Change: `fuxibacklight=3`, OFF-only interest diagnostic

Mode 3 publishes the same service/brightness as modes 1/2. It preserves mode
2's serialized framebuffer path and sends OFF after every accepted zero-state
request, including duplicates. For nonzero requests it updates framebuffer
power normally, **withholds ON**, logs the decision, and returns success
without traversing interest clients. This deliberately leaves an OFF latch
stale after wake; it is a discriminator, not a correct power protocol or a
claimed auto-lock repair. The bounded ramp-delay path remains available if
its other predicates hold.

Absent/0/unknown values remain disabled; mode 1's direct path and mode 2's
both-message path are unchanged. No publication notification, delayed ON,
timer, brightness mutation on power changes or hardware access is added.
Later positive brightness requests are stored but never release an ON.
Power logs retain the first-64/each-256 sampling policy.

### Exact acceptance experiment

Compare **modes 1, 2, 3** with the same root, other boot arguments and GPU
configuration, using `fuxikeyscript=300 fuxikeyscriptn=1` for each. No touches
or other keys, and no attention preference change during this comparison.
Repeat the triplet under similar cooling/boot conditions. Record startup and
at least 420 s after the single scripted Power release.

1. Record SpringBoard display-on, spontaneous off before the script and the
   click's actual direction. Script text `expect lock` does not measure it.
2. Mode 3 must log `FUXI BACKLIGHT: OFF-only interest experiment enabled; ON withheld`.
   For each sampled on, require `FUXI BACKLIGHT: power #... on message=0xe0000230 withheld experiment=off-only`.
   OFF still logs `message=0xe0000210 result=0x0 logical-only`.
   No mode 3 ON should report a fanout result. Retain brightness/open/method
   diagnostics: a new request changes the evidence.
3. If mode 3 restores initial auto-off while mode 2 reproduces suppression,
   ON delivery is implicated independently of retained OFF/gating. If mode 3
   still suppresses it, isolate OFF/gating next; an ON-only explanation has
   failed this test. Neither result measures the userland completion state.
4. If initial auto-off occurs, the click wakes. Require another spontaneous
   off within 120 s and retain the full 420 s. Initial recovery without
   post-wake recovery leaves the original failure unresolved. Without initial
   auto-off the click locks, so post-wake acceptance is **not measured**.
   A separate `fuxikeyscript=300 fuxikeyscriptn=2` run can then lock and wake
   (the two clicks are eight seconds apart); verify directions from FB logs
   and record 420 s after the second release.
5. A possible workaround requires repeatable initial **and** post-wake
   auto-off plus working explicit Power transitions and no new errors.
   Even success does not validate a deliberately missing ON as a final driver.

If wake idle still fails, the next direct measurement is a temporary userland
observer: BLS requested/completed states, scene/ramp completion, pending SB
completion, active idle descriptor/disable assertions, SB timer reset, IT
enabled state, AW resume/reset errors, timeout callbacks and configuration
generation. Capture before/after Power release and at +60/+120 s through a
verified console sink; ordinary stderr/OS-log silence proves nothing.
No observer or device experiment was run in this offline round.

### Proposed root experiment, not applied

In the mobile user's **`/var/mobile/Library/Preferences/com.apple.springboard.plist`**,
merge this Boolean entry, preserving other entries and ownership, before a
fresh boot to avoid cfprefsd/SpringBoard cached values:

```xml
<key>SBDisableAttentionAwareness</key>
<true/>
```

The spelling/type/default false are supported by `SBIdleTimerDefaults`
`_bindAndRegisterDefaults` (`0x18e6547ac`, binding at `0x18e654900` onward).
The abstract domain initializes to `com.apple.springboard` (`0x18e613714`).
`SBIdleTimerDescriptorFactory _shouldUseAttentionSensor` (`0x1c57f6290`)
explicitly returns NO when this setting is true. It disables sensor sampling
for this policy; it does **not** replace the AW timer or repair an absent AW
service. Do not substitute a guessed auto-lock key or change timeout settings
in the same experiment.

Use a separate matched pair with **mode 1**, `fuxikeyscript=300 fuxikeyscriptn=1`,
key absent/default versus true. Retain 420 s after wake and require both
initial and post-wake auto-off. Restore the saved original entry (or remove
the newly added key) before a fresh boot. If a verified userland log sink is
available, `_shouldUseAttentionSensor: NO because SBDisableAttentionAwareness is set`
confirms consumption; it is not expected on the kernel console. A negative
result does not rule out AW service failure. No root preference/image edits
were made here.

### Round 2 validation

The ASan/UBSan host test executes production parser, notification and framebuffer
request/action bodies. It covers gates, duplicates/nonzero states, mode 2's
unchanged message sequence, mode 3's OFF-only fanout and withheld log lines,
callback re-entry outside the framebuffer lock on the gate, no delayed ON
after brightness writes, missing children and iterator failure. Canonical
sources regenerate patches 0109 and 0106. Host tests cannot prove iOS timer
re-arm or device acceptance.

Round 2 checks passed: the backlight ASan/UBSan test, framebuffer/UFS host
test, HID/key, idle and governor tests, all eleven patch sync checks, ARM64
syntax of both changed translation units against the saved XNU headers, and
framebuffer syntax without FUXI. Both kernel-profile CI dry runs and shell
syntax passed. No kernel link, device boot or root edit was performed.
