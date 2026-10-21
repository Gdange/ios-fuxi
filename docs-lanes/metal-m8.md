# Metal M8: CoreAnimation qualification inventory and M9 plan

2026-10-21. **Plan and offline Linux analysis only; CA stays disabled.** The
user reports A740 / iOS 16.7 smoke stages 0–6 passing with FuxiMetal,
metal2vulkan and Turnip. That observation is accepted as the starting state;
this lane did not repeat it. backboardd and SpringBoard remain denied by the
client policy, and CA continues using its software renderer with the kernel
IOMobileFramebuffer. No device access, deployment, workflow run or push occurred.

## Evidence and reproduction

The local arm64 cache's QuartzCore identifies minimum OS 16.7, SDK 16.7.3,
source version 1045.17.2 and image UUID
`9E40B2EB-260C-3AF1-AE45-A1A551A8C1B9`. Findings below come from its Mach-O
sections, symbols, selector references and offline control-flow inspection.
Static references establish possible paths, not the first-frame working set.
The installed `ipsw` could not parse the cache's optimized Objective-C class
hash table during disassembly. Symbol/call annotations and the separate
selector-reference inventory still worked; indirect dispatch may hide more calls.

QuartzCore has **no embedded metallib section**. Its constructor resolves the
QuartzCore bundle's **default.metallib** through `NSBundle`, then uses
`newLibraryWithURL:error:`. The shader inputs therefore came from the local
HFS+ root image's framework resources, read with 7z without mounting, replaying
a journal or modifying the image. A cache alone does not contain these resources.
There is no observed `newLibraryWithSource:options:error:` path in this renderer.
Its `compile_shader` name refers to loading/specializing library functions,
not evidence of runtime MSL compilation. GLSL text elsewhere in QuartzCore
belongs to other rendering paths.

The original tools are [inventory-metal-ca.py](../tools/inventory-metal-ca.py)
and [metal-ca-probe.rs](../tools/metal-ca-probe.rs). For example, with variables
pointing to existing local inputs:

```sh
python3 tools/inventory-metal-ca.py --source "$M2V_SOURCE" \
  --dsc "$DSC" --ipsw "$IPSW" --root-image "$ROOT_IMAGE" \
  --icd "$LAVAPIPE_ICD" --output research/metal-m8
```

Alternatively supply `--metallib` and `--pipeline-functions` with already
extracted resources. `--reuse` reuses private cache analysis, not translation
results. Cargo builds offline against a clean external checkout at the same
`720ef1ce9ae2a2068999a8675717418b9416283e` pin used by the existing tests and
build recipe. The tool copies our current ABI-3 bridge into its ignored build,
queries lavapipe with Vulkan 1.2 and no optional enabled features, and actually
calls both the translator's function compiler and the shipped bridge's
open/compile/free API. No Vulkan command buffer is submitted.

All extracted containers, private analysis, logs, build products and detailed
`results.tsv` / `functions.jsonl` / `selectors.json` stay in ignored output.
The table identifies a function by container and name, since catalog names
repeat. No Apple bytes, shader IR, generated SPIR-V, disassembly, translator
source or Mesa source is part of this commit.

## Renderer selection and failure boundaries

| Boundary | Offline finding |
| --- | --- |
| Default Metal device | `CAMetalContextCreate` asks `MTLCreateSystemDefaultDevice`. `EAGLServer::renderer` and `IOMFBDisplay::get_ctx` use it. A nil device prevents construction of their Metal context. The latter caches its context through a once block, so changing a gate after initialization is insufficient. |
| Queue/context admission | `new_metal_context` creates or reuses an MTLCommandQueue and checks its device identity. Queue creation failure or a mismatched device/queue returns null. The current census mode deliberately stops here, before library loading. |
| Window server fallback | `EAGLServer::renderer` tries Metal first. If no renderer was obtained, the enabled OpenGLES route tries an accelerated GLES context. `EAGLServer::render_update` / `render_surface` fall through to the base `Server` software rendering path when no accelerated renderer is available. The base path constructs `SWContext`. This is initialization fallback, not a demonstrated recovery mechanism after a Metal frame fails. |
| OpenGLES environment | `CA_ENABLE_OGL` defaults to true; if supplied it is interpreted using integer conversion and nonzero enables the GLES route. Setting it to zero does **not** force software while Metal remains available. |
| Other software routes | `CA::CG::AccelRenderer` skips Metal when its caller disables that route, when the default device is nil, or when context construction returns null. Its surface/backend policy can either permit creation of `SWContext` or return null. `CA_NO_ACCEL` is an integer environment override in the separate CG accelerated-queue path, default false; it is not proven to force the backboardd compositor to SW. |
| Null | `new_null_context` / `NullContext` exist as a separate no-output implementation. Returning a null pointer on failure is different from choosing `NullContext`. No inspected Metal error path automatically switches to that implementation, and no universal Metal/SW/null environment selector was established. Do not use null rendering as a safety fallback. |
| Memoryless | `CA_ENABLE_MEMORYLESS` is consulted only after a positive `supportsMemorylessRenderTargets` query and applicable context flags. In that branch it defaults on; an explicit integer zero disables it. A false capability keeps the path off. |
| Renderer feature flags | `CA_FORCE_GENERIC_SHADERS`, `CA_DISABLE_GENERIC_SHADERS`, `CA_DISABLE_PRECOMPILED_PIPELINES`, `CA_DISABLE_TILE_SHADERS`, `CA_DISABLE_INPLACE_SHADERS` and `CA_ENABLE_MPS_BLURS` are present as feature controls. They do not establish a safe renderer switch. Precompiled pipelines are bypassed for the renderer disable flag, supported capture conditions, or unsupported/virtual hardware. Do not infer undocumented default values from names alone. |
| Late failure | A missing bundle/resource URL asserts; failure to load the default library reaches `abort_with_payload`. Shader specialization and pipeline construction also have fatal diagnostic paths. Unsupported selectors returning zero/nil and failed command buffers cannot guarantee automatic SW fallback after selection. |

There is no evidence here that changing a preference or setting
`CA_DISABLE_RENDER` provides a recoverable SW transition. That name is present
among feature strings; disabling rendering is not equivalent to rendering in SW.
No defaults, environment, allowlist or boot arguments were changed.

## API and feature inventory

The generated names-only inventory contains **228 Objective-C selector
references** in MetalContext/MetalBufferPool and shader-loading functions,
including descriptor/Foundation/MPS helpers, plus **17 C API names**. It includes
selectors cached with `methodForSelector:` and invoked through IMPs, not just
direct message-send annotations. The following are the integration-relevant
families; names indicate references, not successful Fuxi implementation.

| Family | API / selector names |
| --- | --- |
| Device and limits | `MTLCreateSystemDefaultDevice`, `newCommandQueue`, `supportsFamily:`, `supportsFeatureSet:`, `vendorName`, `maxFragmentTextures`, `maxFragmentSamplers`, `maxTextureWidth2D`, `maxTextureHeight2D`, `maxColorAttachments`, `supportsMemorylessRenderTargets`, `supportsRasterOrderGroups`, `supportsTileShaders`, `supportsSIMDGroup`, `supportsNativeHardwareFP16`, `supportsExtendedXR10Formats`, `supportsYCBCRFormats12`, `supportsYCBCRPackedFormats12`, `minConstantBufferAlignmentBytes`, `iosurfaceReadOnlyTextureAlignmentBytes`, `deviceLinearReadOnlyTextureAlignmentBytes` |
| Queue and command buffer | `device`, `setLabel:`, `setGPUPriority:`, `setBackgroundGPUPriority:`, `setSubmissionQueue:`, `setCompletionQueue:`, `setOwnerWithIdentity:`, `setFineGrainedBackgroundVisibilityEnabled:`, `commandBuffer`, `enqueue`, `commit`, `commitAndWaitUntilSubmitted`, `addScheduledHandler:`, `addCompletedHandler:`, `waitUntilScheduled`, `waitUntilCompleted` |
| Libraries / functions / pipelines | `newLibraryWithURL:error:`, `newFunctionWithDescriptor:error:`, `setName:`, `setConstantValues:`, `setConstantValue:type:withName:`, `newRenderPipelineStateWithDescriptor:error:`, `newComputePipelineStateWithFunction:error:`, `newRenderPipelineStateWithTileDescriptor:options:reflection:error:`, `newPipelineLibraryWithFilePath:error:`, `newRenderPipelineStateWithName:options:reflection:error:`, `setDisableRunTimeCompilation:`, `functionDescriptor` |
| Render encoder | `renderCommandEncoderWithDescriptor:`, `setRenderPipelineState:`, `setVertexBuffer:offset:atIndex:`, `setFragmentBuffer:offset:atIndex:`, `setVertexBufferOffset:atIndex:`, `setFragmentBufferOffset:atIndex:`, `setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:`, `drawPrimitives:vertexStart:vertexCount:`, `drawPrimitives:vertexStart:vertexCount:instanceCount:`, `drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:`, `setViewport:`, `setScissorRect:`, `setCullMode:`, `setFrontFacingWinding:`, `setTriangleFillMode:`, `setBlendColorRed:green:blue:alpha:`, `setDepthBias:slopeScale:clamp:`, `setDepthStencilState:`, `setVisibilityResultMode:offset:`, `endEncoding` |
| Compute / tile / blit | `computeCommandEncoderWithDispatchType:`, `setComputePipelineState:`, `setBuffer:offset:atIndex:`, `setTexture:atIndex:`, `setThreadgroupMemoryLength:atIndex:`, `dispatchThreadgroups:threadsPerThreadgroup:`, `dispatchThreadsPerTile:`, `setTileBytes:length:atIndex:`, `setTileTexture:atIndex:`, `setTileFunction:`, `setThreadgroupSizeMatchesTileSize:`, `tileWidth`, `tileHeight`, `blitCommandEncoder`, `copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:`, `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:`, `generateMipmapsForTexture:` |
| Buffers / textures / heaps | `newBufferWithLength:options:`, `newBufferWithBytesNoCopy:length:options:deallocator:`, `newHeapWithDescriptor:`, `newTextureWithDescriptor:`, `newTextureWithDescriptor:iosurface:plane:`, `newTextureWithBytesNoCopy:length:descriptor:deallocator:`, `newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:`, `newTextureViewWithPixelFormat:`, `newSamplerStateWithDescriptor:`, `newDepthStencilStateWithDescriptor:`, `newResourceGroupFromResources:count:`, `encodeCacheHintTag:resourceGroups:count:`, `encodeCacheHintFinalize:resourceGroups:count:`, `getBytes:range:`, `getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:`, `replaceRegion:mipmapLevel:withBytes:bytesPerRow:`, `replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:`, `setPurgeableState:`, `contents`, `length`, `storageMode`, `cpuCacheMode`, `pixelFormat`, `usage`, `width`, `height`, `depth`, `mipmapLevelCount`, `protectionOptions` |
| Descriptors / pass state | `attributes`, `layouts`, `colorAttachments`, `renderPassDescriptor`, `setVertexFunction:`, `setFragmentFunction:`, `setVertexDescriptor:`, `setFormat:`, `setOffset:`, `setBufferIndex:`, `setStride:`, `setStepFunction:`, `setPixelFormat:`, `setTexture:`, `setLevel:`, `setLoadAction:`, `setStoreAction:`, `setClearColor:`, `setBlendingEnabled:`, `setSourceRGBBlendFactor:`, `setDestinationRGBBlendFactor:`, `setRgbBlendOperation:`, `setSourceAlphaBlendFactor:`, `setDestinationAlphaBlendFactor:`, `setAlphaBlendOperation:`, `setWriteMask:`, `setRenderTargetWidth:`, `setRenderTargetHeight:`, `setStorageMode:`, `setHazardTrackingMode:`, `setProtectionOptions:`, `setUsage:`, `setTextureType:`, `setWidth:`, `setHeight:`, `setDepth:`, `setMipmapLevelCount:`, `setFramebufferOnly:`, `setAllowGPUOptimizedContents:`, `setCpuCacheMode:`, `setMinFilter:`, `setMagFilter:`, `setMipFilter:`, `setMaxAnisotropy:`, `setSAddressMode:`, `setTAddressMode:` |
| IOSurface / format helpers | `IOSurfaceAllowsPixelSizeCasting`, `IOSurfaceCopyAllValues`, `IOSurfaceGetBytesPerElement`, `IOSurfaceGetBytesPerRow`, `IOSurfaceGetCompressionTypeOfPlane`, `IOSurfaceGetHeight`, `IOSurfaceGetHeightOfPlane`, `IOSurfaceGetID`, `IOSurfaceGetPixelFormat`, `IOSurfaceGetPlaneCount`, `IOSurfaceGetPropertyMaximum`, `IOSurfaceGetProtectionOptions`, `IOSurfaceGetWidth`, `IOSurfaceGetWidthOfPlane`, `MTLGetTextureLevelInfoForDeviceWithOptions`, `MTLPixelFormatGetInfoForDevice`, `MTLPixelFormatGetName` |

`newTextureWithDescriptor:iosurface:plane:` is used for both compositor targets
and image planes. CA checks actual dimensions, FourCC, plane geometry,
protection, texture limits and device alignment. Its destination/import paths
include shader-read/render-target usage. A successful 256-square BGRA smoke
import does not qualify full-display imports or cross-process producer ordering.
Internal MetalContext fence operations use command-buffer scheduling/completion;
they do not establish a public MTLFence implementation requirement.

| Requested feature | Observed scope and qualification consequence |
| --- | --- |
| MTLSharedEvent | No `newSharedEvent` or command-buffer `encodeWaitForEvent:value:` / `encodeSignalEvent:value:` call found in MetalContext. CAMetalDrawable separately holds an **IOSurfaceSharedEvent**, creates listeners and uses `notifyListener:atValue:block:`. Do not conflate the two interfaces or claim event support is unnecessary for all clients. |
| Drawables / CAMetalLayer | Present in QuartzCore: `nextDrawable`, `present`, `presentAtTime:`, `presentAfterMinimumDuration:`, `sharedEvent`, listener and scheduled/presented bookkeeping. These are client drawable paths; the server compositor also renders directly into IOSurface-backed destinations. A first backboardd frame is not evidence of CAMetalLayer qualification. |
| MTLBinaryArchive | No public `newBinaryArchiveWithDescriptor:error:` / `setBinaryArchives:` reference found in the renderer. There is an older private **pipeline library** path and binary-archive failure wording. Those names do not prove use of the public MTLBinaryArchive object API. Apple GPU-specific pipeline binaries cannot be fed to Turnip. |
| MTLDynamicLibrary | No dynamic-library factory/linking selector found in the inspected renderer. |
| Indirect command buffers | No creation/execution/reset/optimization selectors found. Direct indexed draws are present and include cached IMP dispatch. |
| Argument buffers | No `newArgumentEncoderWithBufferIndex:` or argument-encoder setter path found. MetalBufferPool's “arg” functions bind ordinary buffers/offsets to encoder slots. This is not proof about every shader ABI; runtime resource tables must still follow translator plans. |
| Heaps | Yes, conditional external-buffer allocation through MTLHeapDescriptor / `newHeapWithDescriptor:`; protection and hazard-tracking semantics matter. |
| Memoryless | Yes, explicit capability/environment-gated path and a memoryless-clear helper. Fuxi currently reports false. |
| Tile shaders | Yes, tile descriptors/pipelines, tile textures/bytes and dispatch. Four tile entry points exist. Disabling the capability must select a qualified alternate workload path, not merely suppress an error. |
| Raster order groups / programmable blending | Yes: capability query plus framebuffer-fetch requirements in translated fragments. Fixed-function blend factors alone do not cover destination-reading blend modes. No optional Vulkan feature was enabled by this probe, matching the shipped bridge. |
| sRGB | Explicit sRGB processing exists, including the named overdrive sRGB-sampling functions. This does not establish use of hardware sRGB Metal pixel formats. No direct sRGB format allocation was established in the inspected renderer paths; exact requested formats still require a workload trace. M7's explicit refusal of sRGB sampling/rendering remains a separate gap. |
| MSAA | No sample-count setter, multisample allocation or resolve-attachment selector found in the renderer call inventory. The inspected construction uses descriptor defaults; this supports a single-sample initial profile, not a universal claim about all CA clients. Protocol metadata declaring `sampleCount` is not a call site. |

## Shader census and offline results

`default.metallib` is **1,014,944 bytes**, SHA-256
`11c74cc42ce5137b73d44d427f75764f886656d4ddb374422d56b609ed2762d9`.
It has **141 named entry points: 39 vertex, 95 fragment, 7 kernel**. Six entries
declare function constants: two specialized fixed vertices with four constants
each, two generic fixed vertices with five each, and two fixed fragments with
58 each. Across these declarations there are 59 distinct constant names and
134 declaration occurrences, using bool, uchar and uchar3. This describes the
declarations, not the number of runtime permutations.

The separate **default.pipelinelib/functions** container is 3,587,004 bytes. It
holds **451 bounded one-function MTLB records**, representing **377 distinct
container/AIR hashes**; 74 records repeat existing products. These entries have
no remaining declared constants. Their four entry-name families are:

| Family | Records | Distinct products |
| --- | ---: | ---: |
| `fixed_vert_lph_spc` | 6 | 6 |
| `fixed_vert_lph_gen` | 1 | 1 |
| `fixed_frag_lph_cph` | 139 | 131 |
| `fixed_frag_lph_cpf` | 305 | 239 |
| Total | 451 | 377 |

These are bundled specialized AIR products, not 451 different public names.
The resource directory also contains `pipelines.desc` and hardware-specific
`pipelines.g*` files. The latter were not translated or treated as portable
shader input. The inventory does not infer the number of pipeline descriptors
or all possible runtime constant combinations from those file sizes.

Two per-function compiler attempts were made for every record: constants
undefined, and every declared typed value zero (including uchar3). Zero is an
explicit diagnostic input, **not a captured CA specialization**. The four tile
entries receive no invented attachment configuration. PASS below means the
per-function compiler accepted AIR lowering; it does not mean stage linking,
SPIR-V validation, GPU execution or correct rendered pixels.

| Input | Records | Undefined PASS | Zero-typed PASS | Other outcomes with zero values |
| --- | ---: | ---: | ---: | --- |
| Main metallib | 141 | 111 | 117 | 20 framebuffer-fetch capability refusals; 4 need tile attachments |
| Pipeline-library AIR records | 451 | 290 | 290 | 161 framebuffer-fetch capability refusals |
| Total | 592 | 401 | 407 | 181 framebuffer-fetch refusals; 4 need tile attachments |

The undefined case additionally reports six missing-function-constant results.
Of the 181 framebuffer-fetch refusals, 155 require either rasterization-order
color attachment access or fragment pixel interlock plus fragment stores and
atomics; 26 require either rasterization-order color attachment access or
fragment pixel interlock. These are profile/enable requirements, not malformed
AIR or evidence that Turnip cannot ever support the operation.

The tool also attempts each vertex/fragment through the **unchanged shipped
bridge**, paired with diagnostic `std_vert0_lpf` / `path_clear_frag_lpf`
companions from the main library, BGRA8Unorm, one attachment/sample, blend off,
eight Float4 attributes at 16-byte offsets and stride 128. Constants use the
bridge's scalar-zero representation; kernels are explicitly refused. The
layout and companions are declared probe inputs, not recovered CA pipelines.
In particular, unmatched varyings indicate an unsuitable diagnostic pairing,
not an independently proven unsupported function. Bridge PASS includes its
linked pipeline/binding-plan checks but no Vulkan pipeline creation or draw.

| Bridge diagnostic outcome | Main | Catalog records | Total |
| --- | ---: | ---: | ---: |
| PASS through bridge compile and plan | 56 | 62 | 118 |
| Unmatched stage interface in diagnostic pair | 46 | 220 | 266 |
| Framebuffer-fetch capability refusal | 20 | 161 | 181 |
| Unconsumed runtime dependency | 10 | 8 | 18 |
| More than 32 constants | 2 | 0 | 2 |
| Kernel stage outside shipped bridge | 7 | 0 | 7 |
| Total | 141 | 451 | 592 |

The 18 runtime-dependency refusals name `OutOfBoundsAlpha/Specialization` or
`SrgbTexture/Variant` at fragment texture slots 4, 5 or 9. They expose actual
consumer gaps in these diagnostic plans, including slots beyond M7's 0–3 bound.
They do not establish that a particular slot/format occurs on the first frame.
After removing repeated catalog products, its per-function zero-typed result
is 233 accepted / 144 framebuffer-fetch refusals across 377 products.

Detailed pass/fail rows by container/function and category are in the ignored
`results.tsv`. Raw error text stays private. No precision warning was converted
into a fabricated success or promoted to a claim of Metal numerical equivalence.

## Ranked gaps before a backboardd first frame

1. **Safe admission and recovery.** Deny is the current safety mechanism.
   The existing HostImport preflight alone cannot admit CA: initialization may
   subsequently abort, and there is no implemented compositor-level replay to
   SW. Do not add backboardd to the allowlist until that boundary exists.
2. **CA's initialization and encoder SPI.** Qualify queue scheduling/completion,
   depth/stencil initialization, resource/format helper requirements, combined
   texture/sampler binding and cached buffer-offset IMPs. Several are absent
   from the bounded frontend. Returning default values for unknown private
   calls is not an implementation or a safe late recovery strategy.
3. **Real function specialization and pipeline inputs.** URL library loading is
   already implemented, but the fixed fragments' 58 constants and uchar3 exceed
   M7's 32-scalar observer/ABI. Recover actual tuples, matching vertex/fragment
   interfaces and vertex layouts, and validate the bridge's complete plan.
   Bypassing precompiled pipeline lookup is viable only with a verified
   descriptor-based fallback. Never pretend Apple pipeline binaries are Vulkan.
4. **Full-display IOSurface resources and ownership.** The current frontend
   advertises at most 256×256 and imports only the bounded BGRA profile. CA needs
   real display-sized targets and producer images, row/plane validation, memory
   bounds, color formats and acquire/release synchronization across processes.
   Preserve CPU ownership and last-known-good scanout across uncertain submits.
5. **Destination-read rendering and broader scheduling.** The measured 181
   framebuffer-fetch refusals need a consumed Vulkan mechanism or an exact
   compositor-level alternate pass. Resource slots, dependencies, heap paths,
   compute/tile alternatives and multiple attachments must be qualified against
   actual workload requests. Advertised false capabilities must lead to tested
   paths; sRGB, drawable events and MSAA are not licensed by the smoke result.

These are ranked admission/first-frame risks, not a prediction that every
listed optional operation occurs on the first frame. Without a live workload
census the exact first unsupported call remains unknown.

## M9 implementation and staged enablement

1. **Build the recovery boundary first.** Add a CA-specific admission state
   machine separate from the smoke allowlist: disabled → preflight → shadow →
   qualified → active; any failure latches disabled for the compositor epoch.
   Preflight checks the exact library, descriptors, constants, formats, resource
   bounds and synchronization contract before exposing a usable CA device.
   Unknown selectors and unsupported state poison the entire candidate frame.
   Keep SpringBoard denied throughout the initial backboardd qualification.
2. **Keep SW authoritative during qualification.** Retain the normal SW
   composition/scanout path and its last completed surface. Render the identical
   candidate transaction into a separate offscreen GPU surface; never publish
   partial GPU content. Compare completed results against SW, including startup,
   text, translucency, clipping, effects, color handling and producer updates.
   Translation success alone does not advance the state machine.
3. **Implement actual fallback at the compositor transaction boundary.** A
   frontend error must cause the same scene/update to be replayed with SW before
   presentation. This requires an integration point in the compositor/renderer
   selection path; an Objective-C forwarding stub cannot provide it. If safe
   in-process renderer recreation cannot be established for this QuartzCore
   version, remain in shadow mode. A supervised process restart with Metal
   durably denied is a recovery backstop, not a substitute for frame replay.
4. **Add bounded work and an independent watchdog.** Precompile the qualified
   working set off the display thread; cap compile time, memory, outstanding
   work and variants. Set explicit first-frame and steady-state deadlines from
   the measured display budget, always below the existing two-second fence
   ceiling. An independent watchdog revokes admission on timeout, device loss,
   unknown selector, unsupported descriptor/dependency, mismatch or missed
   completion. Reject stale callbacks by epoch. Do not unmap producer/scanout
   pages until ownership is known; quarantine uncertain GPU allocations.
5. **Make disablement survive the failure.** Proposed master boot argument
   `fuximetalca=0` and a root-owned `metal-ca.disabled` sentinel both override
   per-process admission. A missing/invalid enable policy defaults to disabled;
   the watchdog creates the sentinel before any restart. Poll disablement at
   transaction boundaries, since the existing process-once client gate will not
   observe a later file edit. Killing only the GPU worker must leave SW scanout
   intact. Re-enablement requires an explicit new qualification epoch.
6. **Advance in reviewable stages.** Offline real-descriptor replay and fault
   injection → native isolated offscreen CA client → backboardd shadow with SW
   still presenting → bounded GPU canary transactions with synchronous SW replay
   on failure → sustained backboardd operation. Before any active stage, inject
   library/pipeline refusal, unknown SPI, fence timeout/device loss, stale event,
   watchdog restart and kill-switch activation. Acceptance requires a continuing
   SW frame stream and intact display ownership for each failure. Do not claim
   “never black screen” until that compositor-level behavior is demonstrated.

M9 should start with ordinary BGRA, one-sample, no-protection surfaces and the
measured actual constant/layout working set, then separately qualify destination
reads, color formats, protected/multiplane resources and client drawables. No
advertised family/capability should be raised merely to get past selection.

## Changes and validation

M8 adds only offline tools and this summary. Unsupported-selector logging
already identifies class and selector; census mode deduplicates and caps its
records, and encoder rejection poisons its command before submission. No cheap
runtime change was necessary to improve that evidence. Client gates, constant
hooks, frontend/backend, translator bridge, smoke source/configuration and CI
build inputs remain unchanged.

The Linux probe compiled offline against the clean pinned checkout and queried
lavapipe. All 452 libraries / 592 records were inventoried. Python syntax,
container-carving boundary controls, shell syntax and the existing CI matrix
plan were checked. The `metal` target still selects the existing Mac/Xcode build
recipe; the new tools are not a build dependency. A Mac/iOS build and CI run
were not performed, so this lane does not assert a new native-build result.
