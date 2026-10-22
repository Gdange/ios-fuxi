# Metal M9m: render-server startup and entry-point fixes

2026-10-23. Source and Linux host validation only. No phone access, staging,
boot, deployment, CI dispatch or push. This lane ports design observations from
**UniXNU NativeMetal** on Adreno 740/iOS 17 into FuxiMetal's bounded iOS 16.7
frontend. It does not establish a working real backboardd frame or a resolved
GPU hang on Fuxi hardware.

The path remains Metal → dynamically loaded external metal2vulkan at
`720ef1ce9ae2a2068999a8675717418b9416283e` → Turnip. Bridge ABI **8** and the
translator/bridge source are unchanged. No translator, Mesa or UniXNU files,
Apple binary/shader/IR, firmware or downloaded product is added. References were
read only; the implementation and tests are authored in the Fuxi tree.

## Startup before framebuffer selection

**Credit: UniXNU NativeMetal, `a96fece`, and the compression policy in
`NativeAdrenoMetalPlugin.mm`.** Its reported BV/BR stall occurred when the binning
thread ran ahead across GMEM/concurrent-binning passes and the threads stopped
waiting on each other inside an IB. Fuxi now defaults `TU_DEBUG=sysmem,nocb`
before loading Turnip. An existing value, including an explicitly empty value,
is preserved as a diagnostic override. The backend also applies this default
for direct backend users, before ICD discovery.

`checkClient` applies the following policy once, after policy and backboardd
lease admission. The existing constructor calls it before device discovery and
CA framebuffer selection:

| Admitted process | Environment and current-application preferences |
| --- | --- |
| backboardd | `CA_DISABLE_FRAMEBUFFER_COMPRESSION=1` (overwrite), `CAEnableFramebufferCompression=false`, `CAEnableIOSurfaceCompression=false` |
| Other GPU client | `CAEnableIOSurfaceCompression=false` |
| Denied or census client | No startup environment/preference writes |

Preferences use `CFPreferencesSetAppValue` and
`kCFPreferencesCurrentApplication`, with no synchronize or global-domain write.
One `FUXI MTL: compression <process> ...` line names the applied settings.
Compile-time environment/preference hooks isolate tests from real preferences;
there is no runtime policy bypass. A runtime failure still preserves the M9j
strike/exit behavior and M9l survey semantics.

## Core Image memory queries

**Credit: UniXNU NativeMetal, `075efd8`.** Its CIContext initialization aborted
on an unrecognized `dedicatedMemorySize`/`sharedMemorySize` query. Fuxi now
returns 0 dedicated bytes and `NSProcessInfo.physicalMemory` shared bytes.
`recommendedMaxWorkingSetSize` reports the smaller of physical memory and the
existing 256 MiB aggregate resource budget. `maxBufferLength` remains 128 MiB.
`currentAllocatedSize` reports the backend's live charged resource bytes under
its mutex, including live staging/cached resource backing; it is neither process
RSS nor a measurement of all opaque driver pipeline allocations. Discovery-only
census objects safely report zero without a backend. These answers do not relax
resource bounds or advertise another Metal family.

## IOSurface planes

**Credit: UniXNU NativeMetal, `f272a64` and `bcbbc70`, plus its bridge patches
0006/0007 for the draw and two-plane designs.** Nonplanar and one-plane BGRA8
surfaces now use the selected plane's dimensions, bytes per element, base offset
and bytes per row. The entire surface allocation is imported, charged, retained
and locked. Vulkan binds the image at the plane offset; host allocation alignment,
image binding alignment, remaining allocation size and exact linear row layout
are validated before binding. Cache flush/invalidate and alias checks continue
to cover the whole allocation. There is no copy fallback or interpretation of an
Apple compressed byte layout. The startup policy requests plain rows.

Two-plane RGB `_RB4` (`0x5f524234`, RGB8_444_2P/542) remains explicitly refused.
The pinned adapter accepts ordinary single-image formats, not a two-plane RGB
rendering/sampling contract. UniXNU NativeMetal's support required extending the
external runtime surface-format table. M9m does not patch that runtime, discard
a plane or masquerade it as BGRA. The refusal occurs before the ordinary Metal
format check so it includes one complete line with the fourcc in hex, requested
plane, plane count, overall dimensions/row/allocation size and each plane's
dimensions, element size, row bytes and offset (bounded to eight planes).
Invalid plane indices, unsupported formats and incompatible Vulkan layouts also
refuse. Native MipFrontendTests replaces the old blanket planar expectation with
nonplanar success/invalid-plane controls, one-plane offset/stride success and
both `_RB4` plane refusals with layout-log assertions.

## RenderBox and Core Image entry points

**Credit: UniXNU NativeMetal, `fbd282e`.** The existing three command-buffer
presentation forms were already completion-handler adapters and remain so:
`present`, `presentAtTime:` and `presentAfterMinimumDuration:` run after command
completion, once per registered request. They retain the drawable through the
handler. Native controls now cover all three on ordinary completions, alongside
the existing survey tests. No CAMetalLayer/drawable factory is introduced.

Queues answer `getGPUPriority=1` and `getBackgroundGPUPriority=3`. Existing setters
still return NO because the single hardware queue does not apply those requests.
Render pipeline completion-handler methods call the synchronous builder on the
caller thread, deliver its state/error and reflection result before returning,
and release their owned state after delivery. Options None returns no reflection;
unsupported reflection options report the existing NSError. Both compute-function
forms and the descriptor/options form also deliver their result inline, including
the reflection variants. Compute remains unsupported and delivers an explicit
NSError; this is entry-point coverage, not compute execution support. Pipeline
creation failures still pass through the ordinary frontend failure/gate path.
The existing `newPipelineLibraryWithFilePath:error:` clean nil/NSError fallback
was already correct and is retained, without incrementing the failure counter.

## Missing pipeline draws

**Credit: UniXNU NativeMetal, `495d3df` and bridge patch 0006.** A nil pipeline
binding clears the previous binding so a failed creation cannot leave stale
state active. Draw recording drops requests with no pipeline wrapper/backend
pipeline before constructing an operation. Internal render-draw records are
explicitly marked so submission can distinguish them from valid transfers and
empty clears. Before translator use or Vulkan recording, the backend removes
marked missing-pipeline draws and pipelines lacking a translation or the selected
Vulkan pipeline handle. Dropping does not erase other transfers or clear-only
passes in that submission. Existing pipeline creation errors retain their gate
failure behavior.

Each dropped draw increments the process-wide `fuxi_metal_dropped_draws()` counter;
only the first prints `FUXI MTL: draw dropped missing render pipeline (counter
continues)`. Frontend drops append no operation and cannot increment completed
draws. The bridge itself already refuses a null product; M9m adds protection
before that boundary and before `vkCmdBindPipeline`.

## Local validation

Commands run on Linux with `-Wall -Wextra -Werror` and the harnesses' ASan/UBSan:

- `python3 tools/test-fuxi-metal-frontend.py`: portable client/gate/survey,
  injected startup preferences/environment, staging, constants, vertex, timeline
  and cache controls pass. **Native Foundation/Metal suites skipped on Linux.**
- `python3 tools/test-fuxi-metal-m9m.py`: production backend passes with lavapipe.
  Nonzero plane offset, padded rows, read/upload/clear pixels, allocation-edge
  sentinels, malformed offset/stride refusal, budget rollback and null-pipeline
  drop/count/scheduled-completion controls pass. The translator path is deliberately
  unavailable, proving these dropped draws never load it. Backend default and
  explicit `TU_DEBUG` override controls pass.
- M9g, M9h, M9i, M9k and M9l runners with the matching external bridge all pass:
  pixels/imported and linear resources, object reuse, persistent cache integrity
  and concurrent writers, loss/timeout gate fallback, mip/LOD handling and MRT.
  Lavapipe reports `ordered_fetch=1`; the M9l four-input fetch control executed.
  The external checkout was verified clean at the unchanged pin, and the reused
  built bridge's adapter source is byte-identical to this tree's ABI-8 adapter.
- `python3 tools/test-fuxi-metal-m6.py --headers "$VULKAN_INCLUDE"`: real Vulkan
  transfer/sampler/HostImport read/write controls pass. The optional GLSL sampling
  control was not requested; authored AIR sampling ran in the M9 regressions.
- TestBackend C++ syntax with explicit `-Wmissing-field-initializers`, changed
  Python syntax and git whitespace checks pass.

The native runner includes RenderServerFrontendTests for memory answers and live
accounting, queue defaults, presentation timing, handler-before-return ordering
and caller thread, successful state ownership and error/reflection paths, optional
pipeline-library fallback, and missing/stale/null-backend pipeline draw drops.
GateFrontendTests overrides both environment and preference writes, checks
once-only setup before backend construction and zero writes on denied/census
admission. Native flags explicitly include `-Wmissing-field-initializers` with
`-Werror`; concrete/protocol-return casts are explicit, including CA test repairs.
No Mac SDK/iOS build or native test execution is claimed from this Linux host.

## Separately authorized device qualification plan

1. On a Mac, run the full frontend runner (and its `--compile-only` mode when
   checking a new SDK). Require all native suites to compile with warnings as
   errors and pass. Build the normal bundle with the matching ABI-8 external
   bridge; retain the external pin and the M9j protected storage/two-key policy.
2. Default/no-boot-argument control must still deny backboardd; SpringBoard must
   remain denied for every argument/configuration. Require no compression-setup
   log for denied/census clients. Run the isolated CA smoke stages 0–5 with zero
   failures before enabling the real server.
3. With a fresh boot UUID and both admission keys, require the compression line
   before device creation/framebuffer allocation, the default Turnip setting,
   working HostImport and no CI memory-selector abort. Inspect actual home-screen
   text, wallpaper, translucency, animations, widgets and snapshot updates.
   Require increasing stored-surface completions, positive GPU timings, eventual
   good state, zero rendering failures and no recurring dropped draws.
4. Inspect any one-plane BGRA import's row layout and offset. `_RB4` is an explicit
   remaining blocker: require its full refusal line and normal failed/denied state,
   exit and CPU recovery on respawn. Do not call this lane successful widget
   rendering if that refusal occurs. Stop on a hang, corruption or failed recovery.
5. Compare long interactive sessions with the preceding build for the reported
   BV/BR freeze, and verify next-respawn CPU behavior after a controlled frontend
   failure. Only an actual Fuxi run can validate that `sysmem,nocb` resolves its
   display stalls. M9l survey mode remains diagnostic and opt-in, with the same
   bounded refusal collection and no further GPU submissions after its latch.
