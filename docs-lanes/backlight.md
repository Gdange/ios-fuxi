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

Only **`fuxibacklight=1`** enables publication. Absent, zero, and other values
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

No MMIO, panel/DSI/PMIC writes, framebuffer pixel writes, power callbacks, or
DCS commands occur in this service. Even brightness zero only changes stored
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
