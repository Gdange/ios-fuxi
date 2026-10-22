# Metal M9k: mipmapped compositor textures

2026-10-22. Source and Linux host validation only. The supplied first real
backboardd GPU boot admitted uid 501, then refused `mipmapLevelCount 10`;
launchd's next process was denied by the strike and brought up the CPU UI.
Those are user-observed starting results, not measurements repeated here.
No phone access, deployment, remote build, workflow dispatch or push occurred.

The chain remains FuxiMetal → dynamically loaded external metal2vulkan
`720ef1ce9ae2a2068999a8675717418b9416283e` → Turnip. No translator/Mesa source,
Apple binary/library/shader, firmware, cache product or downloaded blob is added.

## Mip storage and commands

Ordinary textures use one optimal-tiled Vulkan image with all requested mip
levels. Valid counts are 1 through `floor(log2(max(width,height)))+1`.
Each level's width/height is `max(1,base >> level)`; rectangular, non-power-of-two
and one-pixel-wide/tall textures work. The frontend does not invent buffer
row offsets for optimal images. Vulkan owns the layout, and the full image's
memory requirements are charged, including mip levels and alignment. The
4096 dimension, 128 MiB single allocation and 256 MiB aggregate budget remain.
For example, a full RGBA16Float 4096-square chain may exceed the single-allocation
bound even though its base level fits. CPU staging is charged while live too.

Supported operations now include:

- Shared texture `replaceRegion` and both `getBytes` forms at any level, with
  caller row/image strides, region bounds, partial updates and untouched padding.
  Private textures use blits; direct CPU transfers refuse.
- Buffer→texture, texture→buffer and texture→texture blits at explicit levels.
  Copy coordinates and extents use the named levels. Distinct levels of the
  same image may be copied; same-level aliases still refuse. Whole-texture copy
  copies every level of matching-size/count textures and never resizes.
- `generateMipmapsForTexture:` records a GPU `vkCmdBlitImage` linear-filter chain,
  with transfer write/read barriers between levels. It queries Vulkan blit and
  linear-filter support, refuses unavailable formats and requires a defined base.
  A one-level texture is a no-op. Generation on a level-range view only changes
  levels within that range. There is no CPU mip-generation fallback.
- Nearest and linear mip filtering and finite, nonnegative ordered LOD clamps.
  NotMipmapped uses Vulkan LOD 0. Filters and clamps also reach the translator's
  runtime shape/variant plan. Core immutable sampler plans retain their existing
  translator-provided mip/filter/LOD behavior.
- Same-format 2D views of any nonempty valid level range, including nested views.
  Width, height, level count, parent-relative level and resource ownership follow
  the range. Sampling image-view cache keys include base/count; framebuffers
  include attachment level. Views retain the owning resource and heap charge.
- Color rendering and clear/load/store at a selected attachment level, including
  a view-relative level. Framebuffer-fetch input attachments use that same
  single-level view. Viewport, scissor and render-area bounds use its dimensions.

Content validity is tracked per level. Upload/copy/clear/draw defines only the
written level; generation defines its destinations; DontCare store invalidates
only its level. Pre-submit failures restore every definition bit and the image
layout. As a conservative current restriction, all levels exposed by a bound
sampled texture must be defined, even when a particular sampler would only
access level zero. Use a narrower view or initialize/generate the chain first.
Feedback from any level of an active attachment remains refused. Texture Empty
purge invalidates the whole ordinary texture; a later Load/read/sample cannot
silently use discarded contents. Heap/view resources remain nonvolatile.

This follows Metal's explicit mip allocation/upload model ([Apple mip texture
creation](https://developer.apple.com/documentation/metal/creating-a-mipmapped-texture))
and Vulkan's GPU blit mechanism ([Khronos mip generation](https://docs.vulkan.org/samples/latest/samples/api/texture_mipmap_generation/README.html)).
For exact 2:1 reductions linear blits have the CPU box filter footprint. NPOT
reductions use Vulkan's linear resampling footprint; no claim of an area-box
average for odd dimensions or bit-identical Apple filtering is made.

## Descriptor audit and remaining refusals

The M8 CA call/variant inventory explicitly contains mip allocation/upload,
`generateMipmapsForTexture:`, `setLevel:`, sampler mip filtering, linear/no-copy
resources and conditional heaps. It did not establish an MSAA/resolve allocation
call or hardware sRGB allocation on the server's initial path. Those absences
are prioritization evidence, not proof that every CA transaction avoids them.
`tools/fuxi-ca-smoke.m` was reviewed: its existing image stage requests nearest
filtering and its display stage does not qualify real server mip requests.
No smoke success is promoted into proof of home-screen coverage.

| Request | Result / reason |
| --- | --- |
| BGRA8Unorm, RGBA8Unorm, RGBA16Float, R8Unorm, RG8Unorm | Sample/transfer/color target, now with mip levels, subject to actual Vulkan feature bits. |
| A8Unorm | Sample/transfer, including mips; translator's alpha component mapping is preserved. A8 render targets and Unknown usage refuse. |
| BGR10A2Unorm (94) | Newly accepted sample/transfer/color target via `A2R10G10B10_UNORM_PACK32`; not the XR/sRGB/PQ formats. |
| sRGB, XR, compressed, integer, depth/stencil and other pixel formats | Explicit pixelFormat refusal. sRGB needs the translator's decode/twin/variant plans qualified, not merely a Vulkan enum. Depth/stencil requires real aspect/pass/test state; neutral state remains the only depth/stencil state. |
| Private / Shared, default / WriteCombined cache | Accepted. Cache mode and allowGPUOptimizedContents are placement/performance hints; pixel semantics remain the same. Public private-buffer contents is nil. |
| Unknown usage (0) | Accepted for supported ordinary color formats; backend allocates sampled/target/transfer uses conservatively. Public usage remains 0. |
| ShaderRead, RenderTarget, PixelFormatView combinations | Accepted ordinary textures. PixelFormatView does not enable format reinterpretation. ShaderWrite/atomic/unknown bits refuse. |
| 2DArray, cube, cube array, 1D, 3D; arrayLength other than 1 | Refuse. Need shader type/layer facts, layered views, copies and rendering across the bridge. No false capability advertisement. |
| sampleCount other than 1 / resolve attachments | Refuse. Need sample-aware translation, render pass, allocation and resolve state, not a single-sample alias. |
| Nonidentity swizzle or format-changing view | Refuse. Logical component mapping must be reconciled with runtime format/alpha/variant facts before accepting these. Logs contain all four swizzle ordinals. |
| Memoryless / Managed / untracked hazard | Refuse. No fabricated tile residency, CPU synchronization or resource mode. |
| Automatic Shared / Private heaps | Both accepted as independent nonaliasable allocations with a heap byte budget; cache/storage queries and matching texture descriptors are preserved. Full mip image bytes count toward heap and process limits. Shared/private buffers support default/WriteCombined cache and tracked/default hazards. Placement/sparse heaps, aliasing and unimplemented heap sizing selectors still refuse. |
| Purgeable state | Public valid states accepted. No heap/view eviction; ordinary texture Empty loses validity at every level. Invalid state refuses. |
| IOSurface | Still one unprotected BGRA8 plane-0, nonplanar surface, one mip/sample, Shared, exact dimensions/element/stride/allocation/HostImport checks. Unknown and PixelFormatView usage hints now pass common descriptor validation. Multiplane YUV, other FourCC/plane formats and mipmapped imports refuse. No invented plane offsets or producer-fence contract. |
| Buffer-backed linear image | Still one mip, Shared, exact Vulkan row pitch/offset/memory compatibility. Other layouts refuse with descriptor and row/offset/image-byte values. |
| Private newTextureWithBytesNoCopy:length:descriptor:deallocator: SPI | Explicit refusal with descriptor and length. The validated buffer/linear texture route remains implemented; unknown stride/lifetime semantics are not guessed. |

Every ordinary/linear/IOSurface descriptor gate now logs the failing public field
and value with the full descriptor in the same line, before invoking the normal
failure latch. For example:

```text
FUXI MTL: texture reject sampleCount 4 desc=type=... format=... size=... mips=... array=... samples=4 storage=... cache=... hazard=... usage=0x... swizzle=... optimized=... resources=0x...
```

Allocation/import failures use `texture reject allocation` / `linearAllocation` /
`IOSurfaceImport`, with `desc=... reason=...`. Surface-specific checks append
plane count, FourCC, size, element size, row bytes and allocation bytes. View
refusals print requested format/type/ranges and the originating descriptor.
Sampler refusals print filter/mip/LOD/address/compare/anisotropy state; heap
refusals print size/type/storage/cache/hazard. The existing 64-line failure cap
and uncapped failure counter/latch remain.

All explicit Frontend rejection sites and protocol/inherited-selector forwarding
were audited. Other boundaries remain: bounded constants and observed snapshots;
portable AIR/function lookup and translation-plan requirements; sixteen resource
slots; direct per-vertex triangle/list-strip draws, UInt16 indices, one instance;
one color attachment; neutral depth/stencil/cull/winding/fill/blend-constant and
visibility state; no tile/compute dispatch, indirect/argument tables, drawables,
cross-process shared-event handles, protection or Apple GPU pipeline binaries.
Queue/command lifecycle, foreign-device objects, event/listener bounds and
unsupported selectors still poison/refuse through the existing paths. Existing
pipeline logs include the full vertex/layout/render state. This lane adds no
capability/family claims for these remaining features.

## ABI, cache and switches

**Bridge ABI 7 is required.** DrawInput grows from 928 to 1056 bytes: per-slot
float LOD minima/maxima accompany filter bits (present=bit0, min/mag linear=bits
1/2, mip enum=bits3/4). The adapter validates the bits/clamps and builds the
corresponding sampler facts. CompileOptions and output layouts stay unchanged.
An ABI-6, future-version or missing-version bridge is refused before other
symbols are consumed. Rebuild and replace bundle/bridge together. The existing
translator source fingerprint automatically invalidates persistent cache
products from the older adapter. External translator pin and dynamic loading
remain unchanged.

No survey switch is implemented. Returning nil can cause CA to abort immediately;
continuing after a swallowed refusal has no safe compositor transaction/present
boundary here. Collecting N requests while guaranteeing no partial presentation
would require that integration. The M9j two-key gate, durable armed/strike/denied
state, first-failure latch, immediate backboardd exit and CPU next respawn remain.
Only the existing exact `fuxibbgpu=1` plus root-owned plain backboardd allow line
can admit the server. Default config stays smoke-only; SpringBoard stays denied.

## Host checks and next device qualification

Run with a matching bridge built offline against the clean external pin:

```sh
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-m9k.py --translator "$BRIDGE_SO"
python3 tools/test-fuxi-metal-m9g.py --translator "$BRIDGE_SO"
python3 tools/test-fuxi-metal-m9h.py --translator "$BRIDGE_SO"
python3 tools/test-fuxi-metal-m9i.py --translator "$BRIDGE_SO"
```

Linux results: seven Rust adapter unit tests pass; bridge builds offline. M9k
passes with -Werror and ASan/UBSan on lavapipe: NPOT/skinny mip counts, all-level
uploads/readbacks/copies, same-image disjoint-level copies, GPU mip generation
versus independent CPU box filtering (rounding tolerance per chain level), fixed
and fractional LOD sampling, nearest/linear/nonmip modes, rebased views,
render-to-level, framebuffer reuse, packed BGR10 rendering, small-channel/float
roundtrips, full-chain charge/bounds and failure rollback. Missing/old/future ABI
loader controls pass. M9g/M9h/M9i production regressions pass, including cache
persistence and loss/timeout fallback tests. M6 transfer/HostImport controls pass;
the optional GLSL sampler control was unavailable because glslang is absent
(the authored-AIR M9k sampler controls did run). Portable frontend policy, gate,
staging, constants, texture-level/budget, resource-cache, vertex and timeline
checks pass. Native Mac MipFrontendTests and existing frontend suites are
extended/wired but **skipped on Linux**; no Mac/iOS build result is claimed.

For a separately authorized device test:

1. First run the full native Mac suite and normal iOS bundle/bridge build. Stage
   the matching ABI-7 artifacts using the M9j procedure. Preserve the two-key
   gate and default denial control; do not enable SpringBoard.
2. Run isolated CA smoke stages 0–5 with zero frontend failures and the existing
   pixel/performance acceptance. This is a regression check, not the actual
   server's mip coverage oracle.
3. Fresh boot UUID, real backboardd, both keys: require armed, HostImport, no old
   mip-count refusal, increasing stored surface completions and eventually good.
   Inspect text, minified images/wallpaper, translucency, clipping and animation
   for wrong mip selection or stale levels; record first frame/GPU/cache timings.
4. A refusal must include its descriptor line, then persisted failure, exit,
   strike/deny and the normal CPU UI after respawn. Stop at the first refusal,
   corruption, stall or failed CPU recovery. Retain only private device logs.
   M9k does not establish a new successful backboardd frame on device.
