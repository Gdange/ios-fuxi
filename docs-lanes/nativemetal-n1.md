# NativeMetal N1: iOS 16.7 source port behind the Fuxi gate

2026-10-23. Source/build recipes and Linux validation only. No phone, Mac SDK
build, signing run, workflow dispatch, deployment or push was performed.

NativeMetal is [UniXNU NativeMetal](https://github.com/UniXNU/NativeMetal), MIT,
Copyright (c) 2026 NativeMetal contributors. The pinned upstream licence travels
in the helper bundle and corresponding-source archive. Our patches and safety
adapter are MIT. No upstream source is vendored. Read this alongside
[the two-key gate](metal-m9j.md), [survey behavior](metal-m9l.md),
[startup policy](metal-m9m.md) and [the Turnip baseline](turnip-t1.md).

## Architecture and selection

Discovery remains exactly what the existing kernel publishes:

| Kernel property | Value |
| --- | --- |
| `MetalPluginName` | `FuxiMetal` |
| `MetalPluginClassName` | `FuxiMTLDevice` |
| Discovery bundle / principal | `FuxiMetal.bundle` / `FuxiMTLDevice` |
| Optional helper | `FuxiNativeMetal.bundle/FuxiNativeMetal`, **no principal class** |

The helper is a dynamically loaded library inside a bundle, reached only after
Fuxi admission; it has no discovery class or constructor opening a GPU. No
kernel changes are needed. The ordinary `metal` build also understands the
selector, but selecting native without installing its helper fails discovery.
There is no silent retry on FuxiMetal after a native runtime failure.

```text
Metal discovery -> FuxiMTLDevice -> existing ClientGate + BackboardGate
  default / fuximetal=fuxi -> existing FuxiMetal backend
                            -> libmetal2vulkan.dylib -> Turnip
  fuximetal=native         -> host-import Query + CLOSE
                            -> FuxiNativeMetal.bundle
                            -> libnative_metal_bridge.dylib -> Turnip
```

`fuximetal=native` and `fuximetal=fuxi` are exact whitespace-delimited kernel
boot arguments. Absence selects FuxiMetal. Unknown values, trailing characters
and duplicate `fuximetal=` keys deny discovery for an otherwise admitted GPU
client. Unreadable boot arguments select the existing default; backboardd's
independent `fuxibbgpu` reader still fails closed. Selection is frozen once per
process. It never grants admission. A census client uses the existing Fuxi
selector recorder and constructs neither backend.

The two backboardd keys remain a root-owned, non-group/world-writable
`/usr/local/share/fuxi/metal-clients.conf` plain `backboardd` entry **and** exact
`fuxibbgpu=1` or `fuxibbgpu=2`. Executable-path identity, permanent SpringBoard
denial, uid checks, boot-session UUID, private state, lifetime lease, durable
armed/strike/denied records and exit 75 for CPU respawn remain in the original
entry. Neither new bundle has an alternative allow list. Removing the native
selector restores FuxiMetal on a subsequent process/boot; an existing same-boot
strike still denies backboardd. Remove `fuxibbgpu` to select CPU recovery.

After admission, the wrapper queries the discovery accelerator using **only**
open type `0x4147` and method 1. It requires the full ABI-2 reply and HostImport
bit, and closes the connection even on a refused/truncated/wrong-version reply.
A failed close refuses opening NativeMetal. Turnip opens only after this closes,
using `NativeAdrenoGpuPublished` and type `0`, as the existing Fuxi backend does.
It loads the existing `FUXI_MTL_ICD` path convention (default
`/usr/local/lib/libvulkan_freedreno.dylib`); no Turnip copy enters either bundle.

## Pinned inputs and patch series

`tools/ci/pins.env` pins NativeMetal to
`a96feceda55e8fed1e20c0a2998736cef7aab006`; metal2vulkan remains
`720ef1ce9ae2a2068999a8675717418b9416283e`. Turnip uses T1's pinned Mesa and nine
UniXNU patches. The native build uses a **separate** metal2vulkan checkout from
the unpatched Fuxi fallback adapter.

Apply these NativeMetal overlays in order:

1. `nativemetal-fuxi-0001-ios16-plugin.patch`: replaces the upstream discovery/
   diagnostics plugin with the gated helper entry and persistent-home-first cache
   selection; `_MTLDevice` must be exactly **528** bytes on iOS, rather than 536.
   Removes the iOS 17 QuartzCore slide/stack/private-object reads and `vm_write`.
   Removes status/telemetry/mailbox/scanout/cache-export/marker threads and all
   dependencies on their unsupported kernel calls, control files, priming jobs,
   addon paths, sheng boot arguments and oslog exporter. No background kernel
   tunnel is needed to make the Metal device.
2. `nativemetal-fuxi-0002-safety-hooks.patch`: the versioned `FuxiNativeHooks`
   callback ABI, refusal/exception/completion observation, strict pre-open size
   check, inherited-private-method forwarding and survey submission suppression.
   Replaces forwarded slots with actual NativeMetal methods and runtime answers;
   no Apple base implementation runs on uninitialized private state.
3. `nativemetal-fuxi-0003-no-production-fixtures.patch`: default production AIR
   fixture arrays are empty. Upstream self-test fixtures remain behind the
   explicit `host-fixtures` feature, which this recipe never enables. There is
   no production LLVM/fixture assembler requirement, embedded shader fixture,
   shader-pack generation or image shader extraction.

The helper compiles only `NativeAdrenoMetalPlugin.mm` and
`NativeBridgeMetalFrontend.mm`. Inspection of UniXNU's `AdrenoPM4` submodule at
`445f8fe6d115738352b9d78dc70af6d61aa7f62f` found that the original plugin needs
it through the removed user-test/telemetry/scanout headers. The frontend and
Rust bridge do not use it. Consequently N1 needs **no kit fetch or PM4 object**.
Nothing from NativeAdreno's kernel implementation is compiled into the helper;
preflight uses our canonical ABI header.

Before device discovery/CA framebuffer selection, admitted clients retain
`TU_DEBUG=sysmem,nocb` (an explicit diagnostic override is preserved) and
`CAEnableIOSurfaceCompression=false`. Backboardd additionally gets
`CA_DISABLE_FRAMEBUFFER_COMPRESSION=1` (overwrite),
`CAEnableFramebufferCompression=false` and **`CAEnableDeepFramebuffer=false`**.
Preferences use the current application domain, without synchronize/global writes.
Denied/census clients receive no rendering preference/environment changes.

For backboardd, cache selection tries its passwd home
`~/Library/Caches/com.apple.backboardd/m2v` first, creating parent directories,
then `confstr` user cache and user temp `m2v` directories. Other admitted clients
use those two `confstr` candidates. Once a writable directory is found, opening
is attempted once; GPU initialization failure is not retried in another directory.
There is no addon directory, pack import/export or export-lock path. Runtime
shader caches and the pipeline journal stay local, never build artifacts.

## Failure and survey hooks

All callbacks use the original `failureLog` path. It increments the diagnostic
failure counter, calls `BackboardGate::fault()` to durably persist denial,
and either exits 75 immediately in mode 1 or records a bounded refusal in the
existing survey in mode 2. These are synchronous hooks, not a polling thread:

| NativeMetal site | Exact action |
| --- | --- |
| `noteProblem` / `NBLOG` | Calls the wrapper's failure callback before updating diagnostic history; includes encoder refusals, missing selectors, allocation/translation/runtime errors. |
| `nbError`, `failWith`, concrete render/compute descriptor rejection | Reports errors even if the caller omitted its NSError output; pipeline runtime failures report before returning. |
| `refuseSurface` | Reports step, FourCC, plane/count, offset/stride/allocation, dimensions and Metal format before returning nil. |
| Every Objective-C method implementation's exception boundary | Catches NSException and C++ exceptions, reports the reason, then rethrows. Fatal external assertions/signals cannot be caught here. |
| Runtime `completed` callback | Checks command-buffer error status and forwards its error (including device loss) before client completion handlers. Successful completion reports recorded draws/timestamps and whether a stored IOSurface attachment qualifies the gate. |
| `waitUntilCompleted` / `waitUntilScheduled` | Forwards failed waits; the former tests the C ABI's **zero-success** return correctly. |
| Wrapper preflight, helper loading/open/nil/exception | Refuses through the same latch; no fallback backend constructor or second runtime open. |

The optional `newPipelineLibraryWithFilePath:error:` fallback remains a clean
nil/NSError because it requests Apple pipeline binaries this GPU cannot use.
It does not strike the gate; ordinary shader/pipeline creation does.

A successful stored IOSurface render pass qualifies a completion once per command
buffer. Empty/private/discarded/transfer-only work does not qualify. Positive GPU
times are reported only when the runtime supplies them. Native completed draws
and timing samples feed the existing smoke exports. Native cache hit/miss exports
report the runtime's shader artifact statistics; Fuxi cache-file byte/result and
CPU submit-breakdown exports remain zero for this backend. Their meanings are
not interchangeable, and no timing/cache success is fabricated.

Once the survey latch is set, command `commit` takes a CPU-only completion path:
status Completed, scheduled/completed handlers delivered, waits bypassed, queued
process-local signals advanced, and normal presentation completion callbacks
retained. It reports no GPU draw/frame/timing success. New pipeline calls return
handle-free diagnostic placeholders, so CA can keep enumerating refusals.
Texture upload/readback paths skip runtime staging after the latch. Resources
remain available for inspection as in the existing survey. Previously accepted
GPU work may still drain; no later frontend submission is admitted. The original
64-distinct/20-second recorder and exit remain in charge. An external CA assertion,
uncaught exception, watchdog or kernel hang can end a survey early; this cannot
promise a usable screen or recovery from a GPU hang.

## Mac build and offline staging

From a Mac with full Xcode and Rust 1.98.0:

```sh
bash tools/build-fuxi-nativemetal.sh "$VULKAN_INCLUDE" "$NATIVE_OUT"
# Or the existing hosted/local entry point; do not dispatch CI for this lane:
bash tools/ci/build.sh nativemetal
```

The workflow has a separate `nativemetal` boolean, **default false**. The normal
`metal` target stays available. Source-only checkouts/build trees remain ignored
under `deps`; explicit artifact copying includes the two signed bundles, both
separate translator dylibs, corresponding-source archives, authored smoke tools
and their existing configs/metallib. No Apple fixture/AIR or SDK is published.
CI records checksums through its existing manifest mechanism.

The generated Cargo manifest follows UniXNU's edition 2024/Rust 1.98 setup:
`metal2vulkan` with `ash`, `metal2vulkan-runtime`, `libc`, `sha2` with `asm`, and
release `panic=unwind`. It applies NativeMetal's bridge patches **0001–0007** to
the pinned external translator before building. The deliberate difference is
`crate-type=["cdylib"]`, not staticlib. Rust and Objective-C target iOS **16.0**
(`arm64-apple-ios16.0` / `aarch64-apple-ios` deployment 16.0). The helper links
only the C ABI in `/usr/local/lib/libnative_metal_bridge.dylib`. That dylib
contains metal2vulkan under the LGPL-3.0-or-later licence choice; neither
Objective-C bundle contains translator objects. It can be rebuilt/replaced and
ad-hoc signed independently, preserving the replaceable-library arrangement in
`build-fuxi-metal-translator.sh`.

`nativemetal-source.tar.gz` accompanies it with patched native/translator source,
transitive crate sources, Cargo.lock, licences, Rust notices, recipes/patches and
an offline `REBUILD.sh` that relocates the manifest and uses the included crates.
Keep the `nmb_*` C ABI and Fuxi callback ABI 1 when replacing components. The new
Mach-O audit permits exactly this one additional external dependency for the
helper, checks that the Rust dylib exports its imports, and retains the existing
arm64/minimum-OS/signature/CTR_EL0 checks. Audits do not prove every private SPI
exists on 16.7; native build and device qualification remain necessary.

The existing offline stager accepts the complete optional helper trio. On an
already mounted writable iOS root, use the usual Fuxi inputs plus:

```sh
sudo python3 tools/stage-fuxi-metal.py "$IOS_ROOT" "$ICD" \
  "$NATIVE_OUT/FuxiMetal.bundle" "$NATIVE_OUT/fuxi-metal-smoke" \
  --translator "$NATIVE_OUT/libmetal2vulkan.dylib" \
  --metallib "$NATIVE_OUT/metal-smoke.metallib" \
  --source "$NATIVE_OUT/metal2vulkan-source.tar.gz" \
  --ca-smoke "$NATIVE_OUT/fuxi-ca-smoke" \
  --clients "$NATIVE_OUT/metal-clients.conf" \
  --native-bundle "$NATIVE_OUT/FuxiNativeMetal.bundle" \
  --native-bridge "$NATIVE_OUT/libnative_metal_bridge.dylib" \
  --native-source "$NATIVE_OUT/nativemetal-source.tar.gz"
```

This does not edit boot arguments or enable backboardd. Later backboardd testing
also requires the existing `--enable-backboardd` staging option and its separate
boot key. The stager validates both native Mach-O inputs and helper metadata,
requires its source archive, preserves signatures and installs root:wheel modes.

## Validation and remaining qualification

The Linux preparation check uses disposable external checkouts under a temporary
directory, removes them afterwards, applies both complete patch sequences and
checks exact input revisions, repeatability and unexpected-source rejection:

```sh
python3 tools/test-fuxi-nativemetal.py --native "$TEMP_NATIVE" \
  --metal2vulkan "$TEMP_M2V"
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-stage.py --policy-storage-only
```

Portable selection/preflight controls cover exact/default/invalid/duplicate
backend tokens, independent admission keys, SpringBoard denial, full ABI-2
HostImport replies and close-after-every-query ordering including errors.
Source checks reject the removed iOS17 reads/writes and unsupported kernel paths.
Python compilation, shell syntax, workflow planning/dry-run and git whitespace
checks are part of this lane. Actual Linux results are recorded below.

On a Mac, additionally run the existing full frontend suite and build a host
Rust library, then run the native checks:

```sh
python3 tools/build-fuxi-nativemetal.py --target host \
  --native "$TEMP_NATIVE" --metal2vulkan "$TEMP_M2V" \
  --build "$HOST_BUILD" --out "$HOST_OUT" --headers "$VULKAN_INCLUDE"
```

Then run:

```sh
python3 tools/test-fuxi-nativemetal.py --native "$TEMP_NATIVE" \
  --metal2vulkan "$TEMP_M2V" --bridge "$HOST_NATIVE_BRIDGE"
# --compile-only prepares the same binaries without running them.
```

`NativeGateFrontendTests.mm` uses the actual Fuxi entry with injected backend
and preflight calls: denied/two-key/SpringBoard/census cases, native delegation,
no Fuxi construction, preflight failure, exception/nil opening, all failure
callbacks, successful gate qualification, immediate exit and survey recording.
`NativePortTests.mm` compiles the **actual patched external frontend** and links
the host Rust dylib: CPU-only refusal/surface/exception hooks, optional pipeline
fallback, placeholder pipelines and no-op scheduled/completed delivery. Neither
test opens a GPU. These and native iOS compilation/linking/signing/audits were
**prepared but not run on Linux**. Upstream fixture-based host tests need an
explicit separate host-fixture setup; production artifacts do not supply them.

Device plan, for a separately authorized session:

1. Run Mac tests/build/audits first. Default controls deny backboardd and
   SpringBoard. Stage smoke-only policy and select `fuximetal=native`.
2. Use the existing exec-swap procedure for `fuxi-metal-smoke`, then
   `fuxi-ca-smoke` stages 0–5, one process at a time. Require pixels, no frontend
   failures and meaningful draw/timestamp/cache observations; do not relax the
   smoke's 60 Hz/pixel requirements to make the port pass. Re-run Fuxi selection
   as a control. Never exec-swap SpringBoard or the actual render server.
3. Fresh boot/session, both backboardd keys, `fuxibbgpu=2` and native selection:
   require armed, persistent cache choice, detailed refusals, no later GPU work
   after a strike, bounded survey/exit and same-boot CPU respawn.
4. Only after smoke/survey issues are resolved, use a fresh boot with
   `fuxibbgpu=1`. Require visually correct composition, increasing stored-surface
   frames, eventual good, zero errors, positive useful GPU times and CPU recovery
   on a controlled failure. Inspect text, wallpaper, translucency, widgets,
   snapshots, animations and prolonged activity. Remove the boot key to recover.

Known gaps: one kernel GPU connection prevents concurrent clients; current
`MaxBos=64`, 256 MiB wired cap and 64 imports compete with Turnip's deferred
retirement queue. Methods 7/8 (timestamp/status) and raised limits require the
separate **K1** kernel milestone; no kernel change is made here. Missing status
can hide device loss until another Vulkan/command failure. NativeMetal's iOS17
shader/SPI coverage is unqualified on 16.7, including two-plane widget surfaces.
Forwarding may reveal more required selectors. Cache-byte/CPU-breakdown metrics,
upstream shader priming and cross-process events remain outside N1. Passing
Linux tests establishes source/portable behavior, not A740/IOSurface correctness,
60 Hz performance or a successful native build.


## Results from this lane

All listed Linux checks passed:

- Fresh pinned NativeMetal/metal2vulkan fetches into a disposable temporary root,
  complete overlay/bridge patch application, repeated preparation and source
  tamper rejection. The preparation-only mode needs no Mac SDK or shader fixtures.
- Native Rust `cargo +1.98.0 check --offline` and a host cdylib build. One missing
  registry source was fetched into the temporary root at its Cargo.lock checksum;
  builds then used offline Cargo. The actual bridge passed lavapipe fill/copy
  pixels, untouched sentinels, scheduling/completion, zero-success waits,
  null/invalid handle refusal and empty production-fixture controls.
- Offline corresponding-source creation/vendor resolution, licence/rebuild
  members and archive exclusion of git/build trees, fixture directories and
  AIR/metallib/SPIR-V/binary products. `REBUILD.sh` syntax passed; its Mac/iOS
  execution still needs a Mac.
- `test-fuxi-metal-frontend.py` portable gate/strike/survey/startup/selector,
  constants, vertex, timeline and resource-cache tests; native suites skipped.
  `test-fuxi-metal-stage.py --policy-storage-only` includes the new helper-trio
  staging, smoke-only policy and destination-containment controls using authored
  command-layout models, not real signing evidence.
- `test-fuxi-metal.py`, `test-fuxi-metal2.py`, `test-fuxi-metal2g.py`,
  `test-fuxi-metal2h.py`, `test-fuxi-metal-backend.py --headers ...`, and
  `test-fuxi-metal-m6.py --headers ...`. The backend harness's stale `nocb`
  expectation was corrected to the existing M9m default. Corruption now injects
  after fence completion, including on coherent memory where invalidation is
  skipped; timeout/device-loss/corruption controls pass.
- M9g/M9h/M9i/M9k/M9l/M9m regressions with a freshly built, separate, clean-pin
  ABI-8 Fuxi adapter: pixels, imported/linear memory, caching/persistence/concurrent
  writers, loss/timeout fallback, mipmaps, MRT and ordered four-input fetch.
  The optional M6 GLSL sampler and artifact/Apple-input suites were not requested;
  no Apple shader input was fetched for them.
- Seven fallback Rust adapter unit tests, M9b cache controls and M9b compile-only
  controls. Its old cache test was updated for the existing persistent-cache
  open signature and eager generation recreation (the removed maxPairs/lazy-get
  API predated M9i). The complete M9a/M9b runner needs unavailable `spirv-val`;
  it was not claimed as a full run.
- Python/shell syntax, workflow matrix including the opt-in target, legacy plan
  compatibility, dry-run and git whitespace checks. No workflow was run.

The disposable external sources and build products were removed. Mac/iOS builds,
Mac native test execution, signing, binary audits and every device-plan step
remain unrun. No deployment or hardware result is claimed.
