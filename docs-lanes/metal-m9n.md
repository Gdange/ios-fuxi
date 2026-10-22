# Metal M9n: survey texture formats and aliased blits

2026-10-23. FuxiMetal, the default Metal → external metal2vulkan → Turnip
lane. NativeMetal selection, bridge ABI 8 and the external translator pin
remain unchanged. The original Fuxi bridge adapter is extended and must be
rebuilt; no external translator sources are changed or vendored. Source and Linux software-Vulkan validation only; no phone,
Mac SDK execution, deployment, CI dispatch or push.

## Evidence and remaining XR formats

The supplied read-only device excerpt reports:

```
FUXI MTL: invalid aliased texture copy
FUXI MTL: backboardd survey summary distinct=16 refusals=25
```

The first error is important: `FuxiBlit` and its Vulkan backend already implement
texture copies, buffer/image copies, fill and mip generation. The old frontend
blanket-refused *every same-image, same-mip copy*, even disjoint rectangles and
identical source/destination rectangles. Its catch path reports the selector on
both FuxiBlit and FuxiCommandBuffer and rejects the command buffer. Asking that
rejected command buffer for another blit encoder causes the subsequent
`unsupported FuxiCommandBuffer blitCommandEncoder` line. Those lines alone do
not demonstrate missing Objective-C methods.

The supported census formats now map as follows. Allocation still requires the
actual ICD's transfer, sampled and requested attachment features.

| Metal value | Meaning | Vulkan format | Bytes per texel |
| --- | --- | --- | --- |
| 43 | BGR5A1Unorm | A1R5G5B5_UNORM_PACK16 | 2 |
| 23 | R16Uint | R16_UINT | 2 |
| 25 | R16Float | R16_SFLOAT | 2 |
| 55 | R32Float | R32_SFLOAT | 4 |
| 105 | RG32Float | R32G32_SFLOAT | 8 |

BGR5A1 stores B in bits 0–4, G in 5–9, R in 10–14 and alpha in bit 15.
This agrees with the [MoltenVK format correspondence](https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKPixelFormats.mm)
and the [Vulkan packed-format definitions](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormat.html).
The new Vulkan test clears opaque red and requires the actual packed word
`0xfc00`; a byte round-trip alone would not catch a channel-order mistake.
BGR5A1 is also accepted in render pipeline descriptors. Existing format mappings
and identity-swizzle validation are retained.

**550 and 554 remain refused; zero refusals is not established by this patch.**
554 is the public `MTLPixelFormatBGR10_XR`, not ordinary BGR10A2Unorm/94.
[Apple specifies](https://developer.apple.com/documentation/metal/mtlpixelformat/bgr10_xr)
32-bit B/G/R 10-bit fields, two zero padding bits and the biased conversion
`float = (code - 384) / 510`. Ordinary Vulkan UNORM instead divides by 1023.
For example XR black is code 384, which UNORM would read as approximately 0.375.
Neither a component swizzle nor an sRGB view implements this transform. Mapping
554 directly to A2R10G10B10_UNORM_PACK32 would silently corrupt sampled values,
alpha and rendering, including values outside [0,1].

550 has no public SDK enum entry. It is `RGB10A8_2P_XR10` in the pinned
translator's [enum census](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan/src/metal/generated.rs),
also reported by the original researcher's
[private-format census](https://gist.github.com/shinyquagsire23/81c86f4bf670aaa68b5804080ff964a0).
The observed 2D dimensions, usage 0x5 and identity swizzle alone do not establish
its plane offsets, strides or the render/sample contract. This Linux run cannot
inspect the build Mac's iOS 16 headers; the maintainer should cross-check those.

The pinned translator's [format model](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan/src/metal/format/mod.rs)
represents 554 as raw `R32_UINT`, and 550 as raw `R32G32_UINT` containing a color
word and alpha. Those are **conversion representations**, not direct normalized
color formats. Consuming them correctly requires its extended-range read/write,
render/blend and clear contracts plus CPU plane packing. Fuxi's current bounded
consumer does not implement those contracts: it still refuses shader-swizzled
views and storage-attachment render plans. This is a consumer limitation, not
an assertion that the external translator lacks XR lowering. 550 must not be
aliased to public 552 (BGRA10_XR) or 554 just because the enum values are nearby.

Both values continue through the existing `texture reject pixelFormat ... desc=`
log and ordinary survey/fault policy. Tests require refusal before allocation.
Full XR support requires consuming those extended-range contracts and packed
CPU-transfer handling, or a proven CA format-selection policy that
avoids these requests. This change does not manufacture such support or suppress
probe refusals. A later device survey must determine whether these allocations
were fallback probes following the initial alias failure or remain independent
blockers. If they recur, M9n's zero-refusal objective remains open.

## Native 1D images and runtime checks

Type 0 textures now retain their Metal type, with height/depth 1, array length 1
and sampled usage. They use `VK_IMAGE_TYPE_1D` and `VK_IMAGE_VIEW_TYPE_1D`,
including rebased sampled views, CPU transfers and blits. They retain the normal
4096 dimension and 128/256 MiB resource bounds. The pinned translator has a
native SPIR-V Dim1D path. The Fuxi adapter now reflects texture shape and numeric
class when constructing the initial binding plan and retains the reflected shape
in runtime variant/draw plans. Its previous all-2D stand-ins prevented 1D
pipelines from compiling. Descriptor validation admits only bounded 1D/2D views.
The optional ABI-8 `fuxi_m2v_texture_type` metadata query lets the backend reject
a 1D/2D shader-binding mismatch. Older adapters remain usable for 2D, but cannot
qualify 1D sampling. BGR5A1 is added to the adapter attachment-format check.
Runtime integer samplers are accepted with nearest filtering and the existing
clamp-to-edge addressing; comparisons and linear integer filtering are refused.
No 2D emulation is used: a height-one 2D view cannot satisfy a Dim1D shader
without corresponding lowering changes. If Turnip refuses the actual 1D image
configuration, allocation fails with the existing descriptor/allocation log.
1D render attachments and buffer-backed linear 1D textures remain refused.

`vkGetPhysicalDeviceFormatProperties` checks transfer support and sampled/color
attachment features according to usage. When available,
`vkGetPhysicalDeviceImageFormatProperties2` also checks the exact image type,
tiling, usage, extent, mip count and sample count before creation. Image/view
creation errors retain the same refusal path. Optimal sampled allocation no longer
requires *linear filtering*: that would incorrectly reject R16Uint. Linear
filtering is checked when a dynamic sampler requests it; the translator still
validates constexpr samplers. Mip generation requires blit source/destination
and linear-filter features. Blended attachments check the relevant tiling's
blend feature at submission.

## Blit recording and safety

Nonaliased texture copies continue to use `vkCmdCopyImage`. Copies sharing an
image, Vulkan allocation or overlapping imported host allocations now snapshot
the complete source rectangle into a tightly packed temporary transfer buffer,
then use `vkCmdCopyBufferToImage` to write the destination. This covers forward,
backward and vertical overlap, disjoint/identical rectangles, different mips and
nested views, and distinct IOSurface wrappers over the same backing. No host
memcpy or extra synchronous submission is inserted between GPU commands.

The source snapshot precedes any destination initialization. A transfer read/write
barrier separates snapshot and write; existing operation barriers order reuse.
One scratch buffer sized for the largest aliased rectangle is reused within a
submission. It is charged under the ordinary resource budget, freed after fence
completion, rolled back on pre-submit errors and preserved on uncertain GPU
ownership/device loss. Undefined source mips, unequal formats, out-of-bounds
regions, invalid slices/levels/depth and image/buffer aliases remain refusals.
The CPU test model also snapshots entire rectangles before writing rows.

The existing buffer↔texture methods preserve offsets, row strides, origins and
view-relative mips. Their `options:` overloads now accept None and explicitly
refuse unsupported depth/stencil/PVRTC options. Fill and buffer copies retain
the existing in-bounds four-byte alignment rule. Synchronize-resource now checks
encoder state and resource class/device; synchronize-texture checks slice/level.
iOS shared resources have no managed shadow copy to synchronize; participating
resources already receive host flush/invalidate and fence completion. Mipmap
generation continues through the existing bounded Vulkan blit chain.

No DrawSafety null-pipeline protection, resource lifetime, admission key,
SpringBoard denial, strike, survey deadline, or post-fault submission rule is
relaxed. A genuine refusal still records, latches, exits and recovers on CPU.

## Validation

Linux commands use warnings as errors and ASan/UBSan:

- `python3 tools/test-fuxi-metal-m9n.py`: production Vulkan format/packed-channel
  tests, four native 1D formats and mip transfers, overlapping/disjoint/identical
  copies, same-image mip copies, distinct same-host imports, padded buffer
  read/upload, fills, mip generation, undefined/bounds/format refusal,
  pre-submit rollback and scratch-budget restoration.
- `python3 tools/test-fuxi-metal-frontend.py`: portable policy, gate, survey,
  startup, staging, constant, vertex, timeline and cache tests. Native suites
  are explicitly skipped on Linux.
- M6 and M9m regressions, plus M9g/M9h/M9i/M9k/M9l with the separately
  rebuilt ABI-8 adapter. The external checkout is clean at the unchanged pin and
  the built bridge adapter matches this tree. Its seven Rust unit tests pass.
  M9k's former same-level refusal control
  now checks an identical self-copy preserves pixels. M9k additionally derives a
  1D control from the tree's hand-authored AIR fixture (no extracted inputs), then
  checks actual uint/half/float/RG sampling through rebased 1D views and BGR5A1 render
  pipeline packing.

The Mac runner includes `BlitFrontendTests.mm` for protocol/selector overloads,
1D descriptor queries/views/CPU pixels, overlapping view-copy pixels, owner
retention, buffer-image/fill ordering and invalid regions/options/strides/state.
GateFrontendTests first completes a legal self-copy under admitted survey mode
and verifies no failure latch, refusal or deadline, then exercises the existing
timer/cap and post-latch no-submission controls. Mac compilation/execution has
not been performed here. Run the frontend harness normally and with
`--compile-only` on the build Mac before building an iOS bundle.

## Next device qualification, by the maintainer

1. Build FuxiMetal with the rebuilt ABI-8 adapter after the full Mac harness
   passes. Confirm the iOS 16 format enum values; record authoritative private
   format-550 layout information if available. Keep the default FuxiMetal lane.
2. Use a fresh boot UUID and both admission keys, with **survey first**
   (`fuxibbgpu=2`). Check the compression setup precedes framebuffer allocation.
   Observe at least 20 seconds of actual rendering activity, with zero refusals
   and no hangs. A zero-refusal run does **not** print `survey summary ...`:
   the existing timer starts only on the first refusal. Do not require a
   fictitious `distinct=0` summary.
3. The expected disappearing lines are `invalid aliased texture copy`, its
   FuxiBlit/FuxiCommandBuffer texture-copy selector reports, the consequent
   rejected-command-buffer `blitCommandEncoder` report, format-43 allocation
   refusal and type-0 refusals for 23/25/55/105. A real Turnip feature/layout
   limitation may instead produce a precise allocation refusal; keep it.
   Format-550/554 pixelFormat refusals remain expected if CA still requests them.
   In that case require the bounded summary and `next-respawn=CPU`, verify
   recovery, and resolve XR before proceeding to ordinary admission.
4. Only after zero refusals, test a fresh boot with `fuxibbgpu=1`. Require
   `backboardd first_frame_ms=... gpu_ms=...`, increasing stored-surface frames,
   positive GPU timing, `failures=0`, and eventually
   `backboardd gate good frames=120`. Inspect text, wallpaper, translucency,
   snapshots, animations and long interactive use for corruption or hangs.
5. Default/no-key backboardd and every SpringBoard configuration must stay
   denied. A controlled fault must still produce
   `backboardd gate runtime-failure next-respawn=CPU persisted=1` and subsequent
   `backboardd gate deny reason=strikes ...`, with working CPU recovery.
