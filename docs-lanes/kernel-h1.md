# Kernel H1: AMFI queries, container paths and buttons

Implemented offline, 2026-10-23. No phone access, hosted build, CI dispatch or
push was performed. Device behavior below is a test plan, not a boot result.

## Patch and source ownership

`xnu/patches/0103-fuxi-kernel-h1.patch` is an overlay after 0101. It updates the
stand-ins introduced by 0035/0050, the HID service from 0058, both PMIC key
readers (0060/0092), and the battery SPMI reader from 0062. Earlier patches stay
unchanged. The 0097 input performance notification is preserved.

Canonical final files are in `xnu/fuxi-h1/`. After editing them, run:

```sh
python3 tools/sync-fuxi-h1.py
python3 tools/sync-fuxi-h1.py --check
python3 tools/test-fuxi-h1.py
```

The sync tool reconstructs only affected files by applying every earlier
relevant patch with `git apply`, generates 0102 from those files, and checks
that the overlay applies. This catches changes to earlier patch contexts as
well as drift between canonical source and the shipped patch. Headers are
installed next to their including sources; no new compilation unit is needed.

## AMFI ABI and dispatch

XNU's `security/mac_base.c::__mac_syscall` matches the requested policy name
literally and calls its `mpo_policy_syscall`. The previous policy was named
`SWEET AMFI` and had only a signature hook, so a query to `AMFI` got
`ENOPOLICY`. That is also the error retained for every unknown AMFI request.

H1 adds a second, minimal policy named `AMFI`, containing only the syscall
hook. Keeping the original signature/entitlement policy avoids duplicate vnode
hooks and preserves its registration, root-filesystem platform marking and XML
entitlement handling. With `fuxiamfi=0`, the second policy is not registered;
AMFI queries therefore have exactly their former missing-policy behavior.

The supplied HackiOS reference, `kern_minimac.c`, documents these iOS 16.x
libamfi calls and wire layouts. There were no libamfi request definitions in
this repository's headers or notes to corroborate the private selectors.
Apple's [dyld-1066.8 source](https://github.com/apple-oss-distributions/dyld/blob/dyld-1066.8/dyld/DyldProcessConfig.cpp)
provides the eight output feature bits for this generation and calls
`amfi_check_dyld_policy_self`; the private libamfi header is not published in
that release. The selectors/layouts below are confirmed against the supplied
reference, not a new disassembly or trace of the installed 20H392 userspace.

| Policy/request | Argument at user address | H1 response |
| --- | --- | --- |
| `AMFI`, `0x5a` (90), `amfi_check_dyld_policy_self` | 16 bytes: `uint64_t input_flags`, 64-bit user pointer to output flags | Copy in the structure; copy out a 64-bit `0xff` through its output pointer; return 0 |
| `AMFI`, `0x60` (96), `amfi_interface_query_bootarg_state` | User pointer directly to a 64-bit result | Copy out `0x86`; return 0 |
| `AMFI`, any other request | Not inspected | `ENOPOLICY`, as before |
| `Sandbox`, 4 | 40 bytes: signed 64-bit pid, 64-bit pidversion, user path pointer, 64-bit capacity, 64-bit check-version flag | Look up target process and copy out its assigned container home, including NUL |

`copyin`/`copyout` errors propagate. No request treats a user pointer as a kernel
pointer. AMFI logs each distinct signed request number once per boot with the
first calling process name, including unknown and negative numbers. A mutex
protects a sparse set so concurrent first calls do not duplicate logs; entries
remain until reboot. Unusual distinct request numbers allocate kernel memory;
this diagnostic facility, like the policies, is intended for this research
system rather than hostile multiuser workloads.

For dyld, bits 0 through 7 enable, respectively: `@` paths, path environment
variables, custom shared caches, fallback paths, print variables, failed library
insertion, library interposing, and embedded environment variables. Returning
`0xff` intentionally allows all eight, regardless of input restrictions.
**Security implication:** `DYLD_*` variables, including library insertion and
path overrides, become honored in stock processes subject to dyld's remaining
checks. This permits changing the code those processes load. It is acceptable
for this research device and is default ON, but can be disabled with
`fuxiamfi=0`. That switch restores these query behaviors; it does not restore
Apple's overall security model or undo the older permissive policies.

`0x60` is a boot-argument-state bitmask, not the dyld feature mask and not a
boolean enforcement result. `0x86` sets bits 1, 2 and 7. The HackiOS reference
reports that ExtensionKit accepts any of these bits (`state & 0x86`) to skip
signature verification of the extension-point table
`/System/Library/xpc/extensions.plist.sig` through the absent AMFI IOKit service.
Its reference explicitly does **not** identify which individual AMFI boot arg
maps to which bit. The available headers and primary source do not resolve
that mapping either; H1 does not invent names for those bits. It reports the
same combined non-enforcing state, without adding an IOKit AMFI service.

## Sandbox container semantics

An extension needs a meaningful container path, not merely a successful syscall
that leaves its buffer unchanged. The pid form and audit-token form of
libsystem_sandbox's container lookup use request 4; the audit-token form supplies
a pidversion and asks for validation to avoid answering for a reused pid.

RunningBoard/PlugInKit already receive the container from containermanagerd and
pass it to the launched process as `CFFIXED_USER_HOME`. H1 reads the target's
saved exec strings through a referenced task/map, using XNU's procargs VM-copy
approach. It skips the executable path and argv, then finds a nonempty absolute
`CFFIXED_USER_HOME` path. A caller's argv cannot supply the answer. This reports
the existing launcher-assigned home; it neither creates a directory nor
establishes sandbox confinement. The environment is mutable, so this is
compatibility metadata, not a trustworthy isolation boundary.

Missing processes or pidversion mismatches return `ESRCH`; absent/invalid homes
return `ENOENT`; short buffers return `ENAMETOOLONG` without a truncated write;
bad user pointers return the copy error. Missing/stale task maps and VM failures
are handled with balanced references and allocations. No global fake container
is returned. This deliberately keeps `ENOENT` for an uncontained process rather
than the reference's success-with-untouched-buffer behavior. `fuxiamfi=0` keeps
the previous unconditional `ENOENT` for request 4. Other Sandbox operations and
privilege handling retain their previous results.

## Buttons and the arbiter

Both Fuxi and legacy Sweet readers now use `fuxi_key_state.h`:

- A key is ignored until two consecutive valid released samples have armed it.
  Startup presses and their eventual release generate no HID pair.
- Every later press/release needs two equal valid samples, 20 ms apart. A failed
  read breaks the pair; it cannot bridge an invalid sample.
- Eight consecutive polling cycles with a failed active peripheral stop the
  polling thread. All previously delivered downs receive balancing releases.
  One stop line replaces periodic error spam. A fully successful cycle resets
  the failure count; an intentionally withheld peripheral does not count.
- A timed-out observer lane is not reused that boot: a late completion must not
  become a new key sample. Subsequent reads fail and lead to bounded shutdown.

Fuxi checks PON type `0x01`, subtype `0x09` (GEN3 HLOS), and GPIO type `0x10`,
subtype `0x10..0x13` before interpreting status/configuration. Unknown peripherals
are withheld, including from opt-in configuration writes. Sweet checks its PON
and GPIO types before interpreting its older register layout.

The arbiter audit found three in-kernel SPMI command issuers: Fuxi keys, Sweet
keys, and Sweet battery (`sweet_spmi_read`). All now acquire the same statically
initialized sleepable mutex, defined in `sweet_spmi.c` and exposed through
`sweet_spmi_bus.h`. It covers command/data writes, completion polling and result
reads on both v5 and v7 readers, including the optional GPIO write. The battery
reader's private IOLock has been removed. Static initialization avoids a race
between battery and keys startup. Loader-side hardware probes run outside XNU
and cannot share this kernel lock.

Every `SweetHIDButtons::key` event now passes `kIOHIDEventOptionIsBuiltIn` to the
keyboard-event constructor. This includes power, volume, home and test events,
with the existing consumer-page usages unchanged.

| Boot arg | Default | Effect |
| --- | --- | --- |
| `fuxiamfi=0` | H1 compatibility enabled (`1`) | Omit syscall-only AMFI policy and restore Sandbox request 4 to `ENOENT` |
| `fuxikeys=0` | Fuxi buttons enabled (`1`) | Existing Fuxi button driver disable |
| `fuxikeys_gpiocfg=1` | `0` | Existing opt-in GPIO6 input setup, only for a recognized peripheral owned by EE 0 |
| `sweetkeys=0` | Sweet buttons enabled (`1`) | Existing legacy button driver disable |

## Host validation

`tools/test-fuxi-h1.py` compiles the actual production header, Sandbox dispatcher,
and both polling-thread functions natively with `-Wall -Wextra -Werror` and
ASan/UBSan. It tests AMFI layouts/results, unknown operations, copy faults,
disabled behavior, concurrent log-once calls, container argv rejection, missing
homes, pidversion validation, exact/short buffer bounds, malformed string
fuzzing, VM failure cleanup, held keys, release arming, bounce, failed samples,
failure reset, eight-failure shutdown and balancing HID releases. The test also
checks source integration and invokes the patch consistency check.

Validation results:

- H1 native tests and all four patch-source sync checks pass.
- 28 existing host scripts pass: the 26 runnable without external inputs
  (Metal staging in `--policy-storage-only` mode, M9i in `--compile-only` mode),
  plus `test-fuxi-vulkan.py --headers /usr/include` and
  `test-fuxi-metal-m6.py --headers /usr/include`.
- The additional `test-fuxi-metal-backend.py --headers /usr/include` fails in
  its existing proxy: it expects `TU_DEBUG=nocb` while the current setup defaults
  to `sysmem,nocb`. With `TU_DEBUG=nocb` supplied, normal submissions pass but
  its `BAD_FILL` injection still fails to trigger the expected error. The same
  failure reproduces using an untouched HEAD archive. No Metal/GPU code or
  tests were changed for H1.
- ARM64 Clang syntax checks pass for all five changed translation units against
  the available XNU 8796 build-tree headers. The C++ check preincludes
  `device/device_types.h` to supply a type omitted by that snapshot's include
  order; it is not a full linked kernel build.

The full Metal staging test needs unavailable signed artifacts; only its
storage-policy portion is claimed. Tests needing signed artifacts, external
translator/source checkouts or image/firmware inputs are not claimed as run.
A full linked XNU build and device verification remain outstanding.

## Device test plan (not executed)

1. Build the complete patch stack for DEVELOPMENT ARM64 FUXI. Keep the current
   root image and command line for an A/B comparison, adding only `fuxiamfi=0`
   for the control run. Do not change the hardware configuration for this test.
2. With H1 enabled expect the existing `SWEET AMFI` and `SWEET Sandbox` policy
   registration lines, followed by:

   ```text
   FUXI AMFI: query policy registered (0), dyld=0xff boot-state=0x86
   FUXI AMFI: <first process> request 0x5a
   FUXI AMFI: <first querying process> request 0x60
   SWEET Sandbox: <first process> op 4
   ```

   Only one AMFI request line per distinct number should appear. Confirm a
   harmless `DYLD_PRINT_*` diagnostic in a controlled launch is honored. Check
   that libamfi receives a 64-bit `0xff` for 0x5a and `0x86` for 0x60, and
   unknown selectors still produce `ENOPOLICY`. The control boot should print
   `FUXI AMFI: disabled (fuxiamfi=0)` and retain the earlier query failures.
3. Inspect extension registration and pkd logs. The hypothesis is that bypassing
   verification through the missing AMFI service lets ExtensionKit load its
   extension-point table, reducing "Unable to find AMFI", unknown extension
   point and invalid-container errors. Compare widgets and extension-based
   wallpaper registration. **Black wallpaper/widgets are a hypothesis to
   verify**, not an established AMFI root cause: rendering, assets, daemons or
   other missing services may still explain them. Record surviving errors.
4. Query a running contained app/extension by pid and audit token: the returned
   path should match its assigned `CFFIXED_USER_HOME`; verify a stale pidversion,
   uncontained daemon and too-small output buffer return the documented errors.
5. Check `FUXI KEYS` init logs: PON type `0x1`/subtype `0x9`, GPIO type `0x10`
   with subtype in `0x10..0x13`, enabled input and readable APIDs. Start with the
   default read-only GPIO behavior. Unknown types must withhold the key without
   trusting status or writing configuration.
6. Boot while holding each key in separate trials, including volume keys used
   to enter the boot mode. Until released there must be no HID down or repeat;
   the first release must not emit an unmatched up. After release, press each
   normally: one down and one up per press, power locks/wakes, volume shows the
   HUD. Check quick taps/bounce and simultaneous keys, and inspect the HID event
   built-in option in the event client. The ordinary logs remain:

   ```text
   FUXI KEYS: power down (pon=0x80 gpio6=0x1)
   FUXI KEYS: power up (pon=0 gpio6=0x1)
   ```

7. With a diagnostic kernel that injects read failures (never by altering PMIC
   power rails), verify eight consecutive failing active polls produce exactly
   one `FUXI KEYS: stopped after 8 consecutive read failures st=...` line, release
   outstanding HID downs and cease SPMI polling. Test recovery before the limit,
   invalid-sample debounce reset, and concurrent battery/key readers. Exercise
   the existing optional GPIO setup only in a separately planned opt-in run.

## Reference credit

[HackiOS kernel](https://github.com/oosexgang/hackios-xnu-kernel), APSL 2.0:
`xnu/files/bsd/kern/kern_minimac.c` supplied the private AMFI selector/layout,
ExtensionKit mask and launcher-home container ideas;
`xnu/files/bsd/dev/redwood_buttons.c` supplied held-at-start, debounce,
peripheral-validation and bus-serialization ideas. H1 was written independently
in this repository's style; no HackiOS source files or verbatim code were pasted.
The device addresses and consumer HID mapping remain this project's existing
Fuxi implementation. No GPL source, firmware or Apple binaries were added.
