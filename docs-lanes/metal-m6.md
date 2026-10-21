# Metal M6 source increment: guarded surfaces and sampled composition probe

2026-10-21. **Source and Linux host tests only. CA enablement is incomplete.**
No Mac, SSH, phone, deployment, workflow execution or push was used. The user's
starting qualification is M5c stages 0–4 on A740; it does not qualify this change.
The daemon default-deny file is unchanged. No Apple shader input, translated
Apple output, Apple binary, firmware or third-party implementation is included.

## Evidence and entry points

The iOS 16.7 evidence is the existing [QuartzCore requirements](metal.md#coreanimation-requirements-from-the-ios-167-cache)
and [renderer-selection analysis](metal-gles-route.md#renderer-selection-in-the-display-server).
This increment does not reproduce or add disassembly. That evidence establishes
call-site families, not a complete live selector census on either daemon.
The exact spellings below are public Metal APIs unless marked private. Public
API availability alone does **not** establish that CA calls a selector.

Apple references: [IOSurface texture factory](https://developer.apple.com/documentation/metal/mtldevice/maketexture(descriptor:iosurface:plane:)?language=objc),
[texture descriptor](https://developer.apple.com/documentation/metal/mtltexturedescriptor),
[blit operations](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Blit-Ctx/Blit-Ctx.html),
[sampler descriptor](https://developer.apple.com/documentation/metal/mtlsamplerdescriptor),
and [function specialization](https://developer.apple.com/documentation/metal/mtllibrary/makefunction(descriptor:)).
These define API contracts; the repo supplies the version-specific CA evidence.

| Family / selector | Evidence | M5c gap and M6 coverage |
| --- | --- | --- |
| `MTLCreateSystemDefaultDevice`, `initWithAcceleratorPort:` | CA device selection; private plugin construction | Existing discovery/gate retained. Root allow-list edits still cannot bypass missing import capability: daemon preflight returns nil before registration. |
| `newCommandQueue`, queue `device`, `label`, `setLabel:`, `commandBuffer` | CA context creates/labels a queue and checks identity | Existing synchronous queue. Added bounded `newCommandQueueWithMaxCommandBufferCount:` as a public compatibility entry, not an observed call-site claim. Private priority/dispatch-queue setters remain unsupported. |
| `supportsFamily:`, `supportsFeatureSet:`, `supportsRasterOrderGroups` | Observed CA capability queries | Existing conservative false answers. `supportsTextureSampleCount:` now accepts 1 only. |
| Private `supportsMemorylessRenderTargets`, `minConstantBufferAlignmentBytes`, `iosurfaceReadOnlyTextureAlignmentBytes`, `maxFragmentTextures`, `maxFragmentSamplers`, `maxTextureWidth2D`, `maxTextureHeight2D`, `maxColorAttachments`, `vendorName`, `supportsYCBCRFormats12`, `supportsNativeHardwareFP16`, `supportsTileShaders`, `supportsSIMDGroup` | Names in existing CA init notes | Added honest bounded values: no memoryless/YUV/FP16/tile/SIMD claims, 256 alignment, one texture/sampler/target, 256-square limit. Other private queries return typed zero and log. |
| `newLibraryWithURL:error:` | Observed `default.metallib` load | Existing checked file loader accepts runtime input, max 4 MiB; no shader-name whitelist or bundled CA input. `newLibraryWithData:error:` remains supported, not observed in that CA path. |
| `newFunctionWithDescriptor:error:`, `newFunctionWithName:constantValues:error:` | Descriptor form and constants observed | Added error-returning entries. Unspecialized lookup works; constant values, specialized names/options are explicitly refused. **CA's fc_generic/fc_vertex_layout/fc_texcoord_count/fc_base_color variants remain missing.** Public constant-value objects expose no enumeration API; this code neither reads their private layout nor silently ignores constants. |
| `newRenderPipelineStateWithDescriptor:error:` | Observed CA pipeline factory | Existing BGRA8, one sample, no blending/depth, empty vertex layout. Translation now accepts the bounded planned sampled-image binding described below. CA buffer layouts, blending, other formats, tile/compute and precompiled pipeline libraries remain unsupported. |
| `newTextureWithDescriptor:iosurface:plane:`; texture `iosurface`, `iosurfacePlane` | Observed CA IOSurface import, including usage 5 | Previously missing. Added retained/locked, single nonplanar BGRA surface, plane 0, shared storage, 256-square import. It uses exactly those host pages through Vulkan external-memory import or returns nil. **Current NativeAdreno cannot import.** |
| `newTextureWithDescriptor:`; texture dimensions/format/usage/storage getters | Ordinary Metal texture API; CA descriptor construction documented | Private BGRA8 256-square textures may now be sampled, render targets, or both. Metadata reflects their usage/storage. Views, mipmaps, planes, sRGB, YUV, compression, arbitrary dimensions and CPU replace/getBytes remain unsupported. |
| `newSamplerStateWithDescriptor:`, render `setFragmentTexture:atIndex:`, `setFragmentSamplerState:atIndex:` | Public sampling contract; needed by CA-shaped control, exact runtime CA selector inventory not established in repo | Previously missing. Added slot 0, same device, normalized nearest/linear, clamp-to-edge, no mip/aniso/compare/argument buffers. Other states log and refuse. |
| Render `setRenderPipelineState:`, `drawPrimitives:vertexStart:vertexCount:`, `endEncoding` | Existing M5 render path; CA render pipeline requirement | Existing one triangle; added six-vertex triangle-list quad with bound texture/sampler. Vertex/fragment buffers/bytes, indexed/instanced draws, viewport/scissor changes, multiple draws and destination reads remain unsupported. |
| Command `blitCommandEncoder`; blit fill and buffer copy | Public operations, existing smoke qualification | Existing operations retained, bounds/alignment/overlap checked. |
| Blit buffer→texture, texture→texture, texture→buffer region selectors | Apple public copy contract; CA-shaped control, not a complete observed CA blit census | Added full 256-square BGRA upload and image copy, readback across submissions and from imported images. Region/slice/level/stride restrictions log and invalidate the command. Options variants, partial regions, format conversions, mip generation and synchronize/fence/event calls remain unsupported. |
| Drawable `texture`, `sharedEvent`; command `presentDrawable:` and timed variants, event wait/signal | Existing analysis observes drawable shared-event consumption; presentation APIs are public candidates | No Fuxi drawable/layer/presentation integration. Unsupported command-buffer calls reject submission. Cross-process producers, display reuse, shared events/fences and presentation remain missing. Do not infer a `newSharedEvent` CA init requirement; no direct factory call was found in the existing analysis. |

## Implementation boundaries and failures

`Backend.hpp` enables `VK_EXT_external_memory_host` only when advertised. Import
checks external image-format importability, dedicated-allocation restrictions,
pointer/allocation alignment, compatible memory types, allocation size and the
**exact** Vulkan linear-image offset/row pitch. It never uploads a staging copy
and calls it an IOSurface import. See the [Vulkan host-memory contract](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_host.html).

The current native kernel's Query advertises HostImport=0 and BoImport is not
implemented. Changing this requires page pinning, address-space validation,
cache maintenance and lifetime/teardown work in the native kernel and external
Turnip winsys. Merely adding a Vulkan device extension string is unsafe.
There is no kernel/Mesa ABI change in this increment. Even an importer-capable
future ICD must pass the exact image-layout checks; linear color/sampled usage
is not universally supported.

Successful imports retain and increment use count on the IOSurface and hold a
CPU lock with AvoidSync until the texture is destroyed. This is an offscreen,
quiescent-producer experiment, **not** cross-process CA synchronization. The
smoke releases imports before taking its consumer lock. After an uncertain GPU
submission/timeout, Vulkan allocations and surface references/locks remain
owned until process teardown. Fence waits remain bounded at two seconds;
smoke stages have their existing wall alarms (stage 5: 60 seconds).

Image layouts and defined-content state are tracked across submissions; failed
recording restores them. Reading/sampling an undefined image is refused before
submission. Vulkan barriers cover host, transfer, sampled and color writes.
Copies reject aliasing the same texture/surface; render attachment feedback is
refused. Commands retain sampled textures, samplers and destinations.

The external translator stays at revision
`720ef1ce9ae2a2068999a8675717418b9416283e`, separately dynamically loaded. The
original C bridge is now **ABI 2**; frontend and translator must be updated
together. It validates the translator's actual combined-image-sampler plan,
view, slot, descriptor layout and push/specialization data. Only fragment
texture(0)/sampler(0), BGRA8 2D, identity view and the supported plan fit.
Other bindings, variants, generated buffers, dependencies and emulation are
refused; requirements are never discarded to make a shader appear supported.
Vulkan specialization constants from an accepted binding plan are forwarded;
these are distinct from the still-missing Metal function constants above.

Parsing occurs at library load; translation is lazy at pipeline creation. An
8-entry / 16 MiB process-only LRU caches accepted SPIR-V/plans by verified AIR
function hashes, container hashes, names and device/driver identity/UUID. The
fixed BGRA8 profile and ABI live with the process. Miss/failure does not persist
anything. No Apple input or translated output is written to disk. Different
function bytes, entry names or device profile cannot reuse a stale entry.

Missing protocol/inherited selectors log `unsupported <class> <selector>` and
return zero/nil using their actual method signatures. Rejected encoder calls
and caught descriptor-validation exceptions record a command error; commit
refuses **the entire command**, including any valid prefix, then invokes
completion handlers. Previously unsupported scalar/void forwarding raised an
exception in allow mode. Unknown private selectors have no established ABI;
the fallback signature is object/word only. This is another reason this is not
a complete private CA interface or a universal no-crash guarantee.

**CA may abort after nil library/pipeline creation.** The repo establishes this
already; error-returning methods do not guarantee software fallback after Metal
selection. On the current no-import kernel, backboardd/SpringBoard therefore
fail the early device preflight even if root adds them to the gate. A future
host-import-capable kernel is not CA qualification: the remaining constants,
buffers/blending and synchronization must be resolved before enabling it.

## Stage 5, build and root edits

`tools/ci/build.sh metal` still calls `tools/build-fuxi-metal.sh`. The latter
links IOSurface and compiles the two new **original** quad/sample functions into
the existing `metal-smoke.metallib`. No new output filename or artifact allowlist
entry is needed. The source archive includes the updated original ABI-2 bridge
and replacement instructions; Mesa/translator remain external. Build/sign/audit
and legacy checksum filenames are preserved. No new Mac/iOS artifact was built
in this task.

Stage 5 imports two IOSurfaces, fills four distinct source quadrants, translates
our textured quad, creates the pipeline twice to exercise a cache hit, draws,
blits back with sentinel edges, checks all 65,536 pixels, then checks that the
IOSurface target contains the same bytes after releasing import locks.

Expected **current-kernel failure**, after stages 0–4 succeed:

```text
FUXI MTL: unsupported FuxiMTLDevice newTextureWithDescriptor:iosurface:plane:
FUXI MTL: IOSurface import FAIL NativeAdreno HostImport / VK_EXT_external_memory_host unavailable
FUXI MTL: stage 5 composition FAIL IOSurface source import unavailable (same-memory required)
```

Conditional success oracle, **not yet observed on device**:

```text
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: translation cache MISS
FUXI MTL: translation cache HIT
FUXI MTL: stage 5 composition PASS IOSurface BGRA8 256x256 textured quad 65536 pixels + same-memory target + sentinel edges
FUXI MTL: done PASS
```

Additional function/translation/timing lines may occur between those lines.
An unsupported shader plan instead produces `pipeline/library FAIL` and the
stage-5 `sampled pipeline` failure. Unsupported selectors name the next gap.

Root-image edits, root:wheel ownership, directories 0755; no edits were applied:

| File | Mode | Contents / purpose |
| --- | --- | --- |
| `/usr/local/share/fuxi/metal-smoke.conf` | 0644 | `5` followed by newline to request stage 5. Shipped default remains `4`. |
| `/usr/local/share/fuxi/metal-clients.conf` | 0644 | Shipped contents `fuxi-metal-smoke` plus newline. For no-GPU census append `census backboardd`. For an explicit experimental allow replace that line with `backboardd`; adding `SpringBoard` independently allows that process. Do not include both census and allow for a name. Policy is read once per process, so restart is required. Current-kernel preflight still refuses a CA device. |
| `/System/Library/Extensions/FuxiMetal.bundle/FuxiMetal` | 0755 | New signed frontend, preserving whole bundle signature. |
| `FuxiMetal.bundle/Info.plist` and `_CodeSignature/CodeResources` | 0644 | Copy complete signed bundle under the Extensions directory above. |
| `/usr/local/bin/fuxi-metal-smoke` | 0755 | New signed smoke executable. |
| `/usr/local/lib/libmetal2vulkan.dylib` | 0755 | New signed ABI-2 external translator. |
| `/usr/local/lib/libvulkan_freedreno.dylib` | 0755 | Existing separately supplied signed ICD; current one cannot import. |
| `/usr/local/share/fuxi/metal-smoke.metallib` | 0644 | New build of our source, including quad/sample entry points. |
| `/usr/local/share/fuxi/metal2vulkan-source.tar.gz` | 0644 | Corresponding external library source/notices and original bridge/build recipe. |

A root allow example is exactly:

```text
fuxi-metal-smoke
backboardd
```

Keep the unchanged default for normal use. These root edits alone do **not**
make the incomplete CA renderer usable.

## Validation and remaining qualification

Passed on Linux, external software Vulkan, ASan/UBSan:

* `tools/test-fuxi-metal-backend.py`: existing 32 fill/copy submissions, cache
  transitions and injected timeout, device loss and corruption refusal.
* `tools/test-fuxi-metal-m6.py`: real upload/image-copy/readback, samplers,
  sentinel edges/reuse, undefined-image refusal, same-memory host-image import
  and read/write. With `--glslang`, original GLSL controls also sample nearest
  and linear into the imported target and check every byte. This isolates Vulkan
  behavior; it is not AIR/Metal sampling qualification.
* `tools/test-fuxi-metal-pipeline.py`: actual pinned AIR translator with the
  existing original M5 smoke metallib, four renders/reopen, correct 8192 red /
  57344 clear pixels, cache hit, SPIR-V Vulkan 1.2 validation, malformed input,
  unknown function, timeout/loss/corruption refusal.
* Rust 1.98.0 check/build of the original bridge against the unchanged external
  pin; client-gate tests; Python/shell syntax; staging on a temporary root using
  existing signed M5 artifacts, including stage-5 config acceptance. The staging
  check tests packaging logic, not new artifact contents.

Updated Mac frontend tests check nil/error refusal instead of escaping
validation exceptions, whole-command rejection and resource lifetime; they
were **not run** here. Mac compilation/linking, new shader compilation, iOS 16.7
symbol import audit/signature, sampler lowering of the new AIR, stage-5 pixels
on A740, true IOSurface/Turnip/KMD coherence, CA default.metallib specialization,
CA entry-point completeness, fallback, display synchronization/presentation
and daemon stability remain unvalidated. This increment must not be described
as completed accelerated CoreAnimation or a passing M6 device milestone.
