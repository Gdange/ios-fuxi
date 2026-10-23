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

Phase 1 uses **`fuxibacklight=1`**. Mode 2, described below, also publishes the
service. Absent, zero, and values other than 1 or 2
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
