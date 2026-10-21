# Metal M6b: stage-5 dependency acceptance

Source and Linux validation only, 2026-10-21. No device access, deployment, CI
execution or push. This supplements the historical [M6 notes](metal-m6.md).

The supplied device observation is stages 0–4 PASS, two same-memory IOSurface
imports PASS, then our bridge's `M6 unsupported pipeline resources` rejection.
Linux reproduced that rejection using the CI metallib after verifying its
SHA-256 against the accompanying CI manifest, and the external translator at
`720ef1ce9ae2a2068999a8675717418b9416283e` (the unchanged build-recipe pin).
No Apple inputs, binaries, generated shaders or external implementation are
included in this change.

## Root cause and contract

On lavapipe, the **only failing predicate in the original resource gate** was
`!pipeline.dependencies.is_empty()`. Stage count, set number/flags, binding
count and vertex-input checks passed. The pipeline reports these dependencies:

| Fact, fragment slot 0 | Mechanism | How the bounded profile meets it |
| --- | --- | --- |
| SamplerCoordinates | Specialization | Plan specializes for normalized coordinates; its specialization data reaches Vulkan. |
| SamplerReduction | PlanTimeCheck | Plan checks the default weighted-average sampler. |
| SrgbTexture | Variant | BGRA8Unorm requires the default variant. |
| YCbCrTexture | Variant | Single-plane BGRA8 requires the default variant. |
| CubicSampler | Variant | Nearest/linear requires the default variant. |
| CrossClassReads | Variant | Float sampling of BGRA8Unorm requires the default variant. |

These exact fact/mechanism/stage/class/slot combinations are allowed. The plan
must still request the default variant, without diagnostics, extra vertex
buffers or unsupported draw resources. Other dependencies fail closed. A
second plan verifies that normalized linear filtering has the same variant
and specialization as nearest filtering; otherwise ABI 2 refuses it.

The accepted descriptor is one fragment combined image sampler, set 0/binding
0, count 1, texture slot 0/sampler slot 0, BGRA8 2D identity view. The existing
backend creates its set layout, pipeline layout, per-draw pool and set, writes
the imported source image view and runtime sampler in shader-read layout, and
binds the set before drawing. Descriptor counts are now bounded to one at both
sides of the ABI. Frontend recording and backend submission reject missing
sampling resources; the backend also refuses imported-memory feedback before
recording layout transitions.

The only accepted push field is fragment `ColorWriteRule(0)`. The translator's
own `instantiate_draw` bytes are copied into the cached ABI-2 product and then
the pipeline, with the reported range passed to pipeline-layout creation and
`vkCmdPushConstants` before **each** draw. The reproduced plan requires offset
64, size 4, value `0x2222`. No color-rule value is synthesized by production
Fuxi code. Push offsets must be aligned; unrelated dynamic push fields fail
closed. No ABI change is needed.

Resource rejections now identify the failed check and key values without
printing an entire layout. Dependency examples fit on one short line:

```text
FUXI MTL: translation FAIL M6 dependency SamplerCoordinates/Specialization Fragment/Sampler/slot1
```

The failure reason precedes timing/cache details, and error output is one line.

## Linux reproduction and tests

With a clean external checkout at the recipe pin, Vulkan headers, lavapipe,
Rust 1.98.0 and clang++ installed, run the new `test-fuxi-metal-m6b.py` with
`--source`, `--metallib`, `--sums` and `--headers` pointing to those inputs.
`--build` selects an ignored/external Cargo output directory; the default is
under ignored `deps/`. Nothing fetches or vendors translator source. The
metallib and manifest are mandatory external inputs and remain untracked.

Passed:

- Rust dependency acceptance/refusal by mechanism, stage, class and slot;
  compact single-line rejection diagnostics.
- Actual `fuxi_quad`/`fuxi_sample` AIR compile, plan and draw instantiation;
  cached pipeline creation, specialization and planned push bytes.
- ASan/UBSan Vulkan integration with imported host-memory source **and** target,
  nearest/linear samplers and both original/cache-hit pipelines. All 65,536
  asymmetric BGRA pixels, same-memory target bytes and readback sentinels match.
- Vulkan call interception checks descriptor/pipeline layout creation,
  pool/set allocation, descriptor writes/binds, and offset-64 `0x2222` push
  bytes on every draw. Missing texture/sampler, attachment feedback including
  a second import of the same memory, and invalid vertex counts are refused.
- Existing backend fill/copy/cache/fence fault tests and M6 transfer/import
  tests; existing AIR triangle integration still produces its expected pixels.

## Redeployment and expected device result

Rebuild through the existing CI `metal` target after integration into main.
Deploy the **complete newly signed FuxiMetal.bundle** and the **new signed
external libmetal2vulkan.dylib**, plus its matching corresponding-source archive.
The upstream pin and ABI remain unchanged, but the adapter in that dylib changed;
replacing only the bundle cannot fix the rejection. The existing CI smoke
executable and metallib already contain stage 5 and need no source changes.
No kernel/ICD change is required by this patch. Retain the stage-5 smoke config
for qualification; the shipped default and client allowlist are unchanged.

Expected, not yet observed with this patch on A740 (timings/precision lines
may interleave):

```text
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: translation cache MISS
FUXI MTL: translation PASS ...
FUXI MTL: translation cache HIT
FUXI MTL: translation PASS ...
FUXI MTL: stage 5 composition PASS IOSurface BGRA8 256x256 textured quad 65536 pixels + same-memory target + sentinel edges
FUXI MTL: done PASS
```

Mac/iOS compilation, signing/audit and Objective-C frontend tests were not run
here. A740-specific translation/profile requirements, Vulkan execution,
IOSurface/Turnip/KMD coherence and stage-5 pixels remain unvalidated. This
bounded smoke does not qualify general CA shaders, synchronization,
presentation or daemon enablement.
