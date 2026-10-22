# Metal M9l: multiple color targets and bounded backboardd survey

2026-10-22. Local source and Linux validation only. The supplied device sequence
is the starting observation: mip count 10, then memoryless RGBA16Float backing,
then `renderPass reject colorAttachments[1].texture`. Each refusal latched the
M9j gate; the respawn restored CPU composition. This lane has no new device
result, phone access, staging, deployment, CI dispatch or push.

FuxiMetal still dynamically loads the separate external metal2vulkan bridge at
pin `720ef1ce9ae2a2068999a8675717418b9416283e`, then submits to Turnip. Neither
translator nor Mesa is vendored. The added AIR test control is hand authored;
no Apple shader, IR, library, binary, firmware or derived product is included.

## Render targets and translation

- One through eight single-sample color targets, indexed by Metal slots 0–7.
  Sparse slots and an absent color 0 work. Vulkan framebuffer attachments are
  compact; color references, shader output locations and fetch input indices
  preserve the original Metal slot. An attachment-free pass still refuses.
- Each target carries its own texture, view-relative mip level, load action,
  store action and clear color. Same-format level-range views retain their
  parent. All present targets must have identical dimensions at their selected
  levels. Textures remain 2D, so each slice/depthPlane must be zero. Aliased
  target memory and attachment/sampled-image or buffer feedback still refuse.
- Load, Clear and DontCare loads, and Store and DontCare stores work independently
  on every color. Intermediate draws in an encoder store all targets and the
  following draw loads them; the final draw applies each requested store.
  DontCare store invalidates only that target's selected level. Undefined Loads
  refuse. DontCare-loaded pixels not covered by draws remain unspecified.
- Pipeline formats and every target's blend enable, RGB/alpha factors and
  operations, and write mask pass through the bridge. Existing BGRA8Unorm,
  RGBA8Unorm, RGBA16Float, R8Unorm, RG8Unorm and BGR10A2Unorm formats remain.
  Finite clear values are accepted; Vulkan format conversion clamps normalized
  formats, while float targets retain values outside 0–1.
- Vulkan `independentBlend` is queried and explicitly enabled when supported;
  the bridge receives the actual device enable, never support alone. Unsupported
  independent state or format/blend capabilities refuse through the ordinary
  pipeline path. Physical color-attachment limits are checked.
- Ordered framebuffer fetch extends from color 0 to any present color 0–7,
  using the existing enabled rasterization-order attachment access path.
  The bridge consumes each input attachment's slot, format and descriptor.
  Storage-interlock and multisample variants remain refused. This does not
  advertise general Metal raster-order groups or tile shader support.
- Render areas use renderTargetWidth/Height (zero means the selected mip's full
  dimensions); viewport and scissor bounds follow that area. Partial empty
  encoders now use a Vulkan render pass, so each clear respects the top-left
  area. Full-size empty passes keep the existing transfer-clear fast path.
  defaultRasterSampleCount 0/1 and absent visibility/tile/imageblock state pass.

Render passes are created lazily for independent action tuples, at most 256
per pipeline. They remain compatible with the pipeline's format/sample/fetch
contract. Framebuffer cache keys include every target and selected level;
retiring any target invalidates its entries. All target layouts and definition
bits participate in pre-submit rollback. Resources survive synchronous GPU
completion, including secondary IOSurfaces. A stored, defined surface in any
color slot can qualify a M9j completion, once per command buffer.

**Bridge ABI 8 requires rebuilding and replacing the bundle and bridge together.**
The dynamic ABI check rejects missing/old/future bridges before consuming other
symbols. CompileOptions is 40360 bytes and Translation is 2488 bytes on the
64-bit ABI; constant and draw-input layouts remain unchanged. All color state
and device enables contribute to translation/object-cache identity. The existing
persistent-cache identity hashes the bridge source, so old products invalidate.

## Audit and explicit remaining refusals

M8's CA call inventory has no sample-count/resolve construction calls. M9f's
qualified graphics recipes have no active depth or stencil target. Those
observations justify preserving the single-sample, neutral depth/stencil
profile; they do not establish that all future CA workloads fit it. Tile and
compute shader families exist in M8 and remain outside this lane.

| Request | Result / representative refusal field |
| --- | --- |
| MultisampleResolve or StoreAndMultisampleResolve, Unknown store | Refuse `colorAttachments[n].storeAction`; no pretend single-sample resolve |
| Resolve texture/level/slice/depthPlane, nonzero store options | Refuse the exact per-color field, including resolve textures on absent colors |
| MSAA, array/3D textures, nonzero slices/depthPlane | Texture/sample or attachment field refusal |
| Mismatched selected mip dimensions | `colorAttachments[n].widthAtLevel` / `heightAtLevel` |
| Active depth/stencil targets or pipeline formats | `depthAttachment.texture`, `stencilAttachment.texture`, or pipeline format field; neutral depth/stencil state still works |
| Visibility result buffer / enabled query | `visibilityResultBuffer` / `visibility queries deferred` |
| Tile size, imageblock, threadgroup memory, rate map, sample positions, layered pass | Exact pass field refusal |
| sRGB/A8 color targets, unsupported blend factors, shader/runtime dependencies | Existing texture/pipeline refusal; all per-target pipeline state accompanies it |
| Missing colors, undefined Load/fetch, aliases, device errors, variant/cache bounds | Explicit frontend/backend refusal with the relevant operation descriptor |

Every render-pass rejection carries `desc=renderPass ... colors=[...]` in the
same line before the ordinary gate exit. All eight slots include the original
texture descriptor (format, dimensions, mips, storage, usage, swizzle and other
resource fields), view base/count, level, slice, depthPlane, load/store/options,
resolve fields and clear color. Depth/stencil descriptors and clears, area,
sample count, visibility and tile/imageblock state also appear. Pipeline
rejections include all eight formats/blends/masks and exact RGB/alpha state,
plus vertex layout and render state. Encoder failures retain their pass text;
backend failures append the failing operation's pass and pipeline descriptors.
Newlines inside errors are normalized to preserve one record per line. Records
are bounded at 32 KiB, sufficient for all bounded descriptor fields.

Representative prefixes (the actual line also includes every attachment):

```text
FUXI MTL: renderPass reject colorAttachments[2].storeAction 2 desc=renderPass ... colors=[0:texture=...;...;7:texture=...;] depth=... stencil=...
FUXI MTL: renderPass reject colorAttachments[1].widthAtLevel ... desc=renderPass ...
FUXI MTL: pipeline reject depthAttachmentPixelFormat ... desc=pipeline ... colors=[...]
FUXI MTL: submit FAIL load undefined attachment[1] desc=renderPass ... pipeline ...
```

## Survey mode: diagnostic only, default off

The existing protected `backboardd` allow entry and boot-scoped admission/lease
are still required. SpringBoard remains denied unconditionally. Exact,
whitespace-delimited boot arguments select:

| Argument | Behavior |
| --- | --- |
| Absent, malformed, duplicate key, or value other than 1/2 | Deny GPU admission |
| `fuxibbgpu=1` | Existing behavior: first refusal persists denial and exits 75 for CPU respawn |
| `fuxibbgpu=2` | Diagnostic survey: admit with the same gate, then collect refusals briefly after failure |

**Survey can leave the screen stale or black for up to about 20 seconds. It is
not a usable rendering mode or a live CPU fallback.** On its first refusal the
gate immediately persists the same failed/denied state as mode 1. No later GPU
command submission occurs. Command buffers, including rejected buffers, finish
with Completed status, scheduled/completion handlers are delivered, queued
presentation callbacks fire, and process-local event signals advance. Waits
are skipped so an unsatisfied event cannot stall a diagnostic no-op command.
Skipped work does not increment GPU draw/frame/qualification counters.

The survey logs each distinct normalized refusal message once, up to 64 lines,
independently of the ordinary failure-log counter. Storage is allocated only
when mode 2 is admitted and bounded to 64 × 32 KiB. Repeated messages cannot
extend the deadline. A dispatch deadline on an independent global queue starts
at the first refusal; the existing periodic reporter also checks it. After
20 seconds or the 64th distinct refusal, one summary is printed and the same
exit-75 hook runs once. Example:

```text
FUXI MTL: backboardd survey enabled diagnostic: screen may blank for up to 20s after refusal
FUXI MTL: backboardd gate runtime-failure next-respawn=CPU persisted=1
FUXI MTL: backboardd survey summary distinct=... refusals=... seconds=... reason=timer next-respawn=CPU
```

The next launch is denied for that boot and uses the CPU path. A new boot UUID
retries under the existing two keys. Missing timer scheduling time under severe
process/system load is not a real-time guarantee; backend fence timeouts and
uncertain-ownership quarantine remain unchanged. Survey cannot recover an
external CA assertion, system watchdog kill or kernel GPU hang. It deliberately
preserves safe resource/descriptor refusals rather than inventing resources.
The presentation adapter queues a supplied drawable's existing present method
on command completion; it does not implement a CAMetalLayer/drawable factory,
new display driver, or cross-process drawable/event protocol.

## Validation and device plan

Local checks run with warnings as errors and ASan/UBSan where the existing
harnesses enable them:

- External pinned checkout is clean at the unchanged pin. Its bridge rebuilt
  offline; seven Rust unit tests pass, including ABI sizes and bounded fetch.
- `python3 tools/test-fuxi-metal-frontend.py`: portable client/gate, survey,
  storage/staging, constants, resource cache, vertex and timeline tests pass.
  Native Foundation/Metal suites are **skipped on Linux**.
- `python3 tools/test-fuxi-metal-m9l.py --translator "$BRIDGE_SO"`: lavapipe
  pixels pass for 2/4/8 colors, mixed formats/blend/masks, sparse slots 1+7,
  mip targets, independent actions, partial empty clears, cache identity/reuse,
  alias/size refusal and rollback. Four-input fetch checks two overlapping
  full-screen primitives in one draw. The local lavapipe run reports
  `ordered_fetch=1`, so that path executed.
- M9g, M9h, M9i and M9k executable bridge regressions pass, including existing
  pixels, reuse, persistent-cache corruption/concurrency, loss/timeout latch,
  mip generation/sampling and ABI mismatch guards.
- Production Backend C++ syntax/ABI sizes, changed Python syntax and git
  whitespace checks pass. No native iOS or Mac build, device boot or CI run
  is asserted.

New native MRT tests cover descriptor formats/blends/masks, independent actions,
levels, sparse slot 7, retained resources and complete refusal text. Native gate
survey tests use the existing FUXI_MTL_BACKBOARD_EXIT hook, an injected monotonic
clock and captured deadline callback: exact dedupe beyond the ordinary log cap,
64-message exit, 20-second deadline, once-only summary/exit, no submissions,
scheduled/completed callbacks, events and all three presentation forms. SDK
protocol-return casts and nonnull sample-position pointers are explicit. Run
these on a Mac before any separately authorized device qualification.

For that later device experiment:

1. Run the complete Mac frontend suite and normal matching ABI-8 bundle/bridge
   build. Keep the M9j policy/storage staging procedure. Default/no-argument
   control must deny backboardd; SpringBoard must remain denied in every mode.
2. Run isolated CA smoke stages 0–5 with zero frontend failures and existing
   pixel/performance acceptance. Then fresh boot UUID and both keys with value
   1: require no old colorAttachments[1] refusal, correct real UI, increasing
   stored-surface completions, eventually good, zero failures and useful GPU
   timings. Inspect text, translucency, wallpaper, clipping and animations.
3. If another refusal occurs, require its complete descriptor line, persisted
   failure, exit and CPU recovery on respawn. Retain logs privately; do not
   infer the next unsupported request from the offline census alone.
4. Only for diagnosis, use a fresh boot with value 2 and both keys. Expect a
   possible stale/black interval, a bounded sequence of distinct refusals,
   summary within about 20 seconds of the first refusal (or at 64 messages),
   and CPU UI after exit/respawn. Verify no continued GPU submissions after
   the latch. Remove value 2 for normal use; test default denial again.
