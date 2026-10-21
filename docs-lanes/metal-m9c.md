# Metal M9c: QuartzCore precision selection and labels

2026-10-21. Source and documentation only; no device access, deployment,
workflow dispatch, test-executable runs or push. backboardd and SpringBoard
remain DENIED. The two supplied isolated CA smoke logs are observations of
earlier builds, not results of this change.

## Deciding query

The iOS 16.7 QuartzCore Metal renderer calls the private device selector
`supportsNativeHardwareFP16`. Fuxi explicitly returned NO. That selects the
`lpf` shader family even though this iOS bundle contains only the `lph` fixed
functions. Changing this one scalar to YES is sufficient to select `lph` on
the existing Qualcomm device; no Apple GPU family claim is needed.

The supplied cache directory contains the arm64 main cache, numbered subcaches,
data/linkedit subcaches and symbols. The existing ignored M8 QuartzCore analysis
provides the resolved call sites, strings and constant tables for that cache.
The query and two precision-consumer instruction sites were also checked
directly against the supplied cache's read-only mappings. The trace below
follows those functions and their consumers; no Apple binary,
shader, IR or disassembly listing is included here.

In the MetalContext constructor inlined into `CA::OGL::new_metal_context`,
`supportsFamily:1001` (Apple1) is saved separately. The low-precision-half flag
is set when `supportsNativeHardwareFP16` OR Apple1 is true. When both are false,
there is one further fallback: a lowercased `vendorName` containing `intel`
with `supportsFamily:2002` (Mac2). Fuxi's Qualcomm vendor and false families
previously left that flag clear. Neither `supportsFeatureSet:`, device name nor
registry ID supplies this precision decision. Apple1 also sets other renderer
flags, so raising that family would change more than precision.

The constructor stores this result in context feature bit 20.
`MetalContext::get_pipeline_spec` transfers it into fragment-spec bit 4.
`Pipeline::Spec::derive_vertex_spec` transfers that fragment bit into
vertex-spec bit 8. `create_fragment_shader` and `create_vertex_shader` test
those respective bits to append `_lph` or `_lpf` to ordinary shader names.
Fixed shaders use constant name tables indexed by the same precision bits;
there is no `fixed_frag_%s` format-string construction on this path. The fixed
fragment table contains `fixed_frag_lph_cph`, `fixed_frag_lph_cpf`, a nil slot,
and `fixed_frag_lpf_cpf`. Thus the code knows a float variant that this iOS
resource bundle does not ship. The nil slot is the float/half-color combination
and leads to a `function_name != nil` assertion if selected.

## The separate color-precision suffix

Fragment-spec bit 12 selects `cph` when clear and `cpf` when set. This is
independent of the native-hardware-half bit. In `get_pipeline_spec`, CA derives
a half-color eligibility flag from context-state flag bit 1 and a mask indexed
by the blend function. If that flag is absent, it sets bit 12 unconditionally.
Otherwise bit 12 follows `CADeviceUseCIF10`. This explains why ordinary solid
rendering can request `cpf` even after low-precision arithmetic selects `lph`.

`CADeviceUseCIF10` uses a process-once MobileGestalt boolean query with the
literal key `Aixt/MEN2O2B7f+8m4TxUA`. If the answer is true, an explicitly
present per-application `CAEnableDeepFramebuffer` preference can override it.
The false initial answer bypasses the preference lookup. This is CA's hardware
configuration/color-state decision, not another Metal half selector. M9c does
not change it. The next corresponding first-stage request should therefore be
`fixed_frag_lph_cpf`; `fixed_frag_lph_cph` remains available for other specs.

## Bounded capability and NativeMetal comparison

Fuxi now returns YES only for `supportsNativeHardwareFP16`. The production
backend already admits only the Qualcomm A740 device identity, whose native
FP16 support is the hardware fact this selector reports. Family and legacy
feature-set queries remain false; tile shaders, raster order groups,
memoryless targets, YCbCr, resource counts and 256-square limits are unchanged.
This does not qualify full Apple feature tables or a complete CA workload.

The inspected NativeMetal revision is
`6dcd4cbb69eb5625988706de42aa8ad3009b8ee7`. Its frontend installs runtime
selector answers through `installAnswers` and `nmb_device_answer`; the
`SupportsNativeHardwareFp16` private flag uses the runtime device profile's
`ShaderFloat16` feature. Its bridge separately forces Apple1–3 and Common1–2
family answers, journaling waived feature rows, while legacy feature sets stay
false. Fuxi does not adopt those family waivers. The shared translator pin
remains `720ef1ce9ae2a2068999a8675717418b9416283e`.

Our existing bridge still enables no optional Vulkan features. The pinned
translator can lower half arithmetic using widened float containers and
rounding when native shaderFloat16 is not enabled; its arithmetic implementation
does not require that optional feature for every half shader. This change does
not enable native Vulkan FP16 instructions or relax shader-plan refusal.
The distinction between the A740 hardware answer and the currently enabled
translation profile is deliberate; numerical/pixel qualification remains work
for the isolated smoke. No translator or Mesa source was vendored or changed.

## Labels and lookup failure

All eleven existing resource, state, library, function, queue, command-buffer
and encoder classes inherit copied NSString label storage from
`FuxiLabeledObject`. Assignment copies before releasing the prior string, so
self-assignment and mutable input work; nil clears it. Destruction releases it.
The setter has no encoder-state check, rejection or failure-counter path,
including after endEncoding. Sampler/depth descriptor labels retain their
initial behavior, and render pipeline states now also copy the descriptor label.
The label-bearing base is separate from `FuxiObject`, whose methods are copied
onto the runtime device subclass; the private device layout stays untouched.

The earlier `unsupported FuxiCommandBuffer setLabel:` line is explained by
`FuxiRender::rejectSelector:` propagating its missing setter to its command.
Command buffers already had a setter. Textures and pipeline states lacked
labels, and function objects also lacked the setter that CA calls immediately
after a successful shader lookup. The common implementation covers these next
calls as well as render encoders, queues, buffers, blit encoders and states.

Function lookup returns nil and the detailed translator NSError, logs/counts
once per lookup attempt, and converts translator C++ exceptions to nil/error.
The specialized wrapper previously discarded the detailed error and added a
second generic failure. Invalid names also return nil/error. Missing functions
are never aliased to another precision family or represented by placeholders.

QuartzCore's `load_shader_function` creates an MTLFunctionDescriptor and calls
`newFunctionWithDescriptor:error:`. `create_fragment_shader` retries the same
lookup once after nil, then calls `abort_with_payload` with the function/spec
and error description. This accounts for the two EntryNotFound attempts and
SIGABRT in the supplied second smoke. The vertex path likewise treats repeated
lookup failure as fatal. A nil/error return is friendly to ordinary Metal
callers, but cannot recover this CA renderer or replay the transaction in SW.
The existing isolated-client failure latch remains necessary, and neither
compositor is admitted by this patch.

## Validation and next observation

The existing Mac harness includes the updated FrontendTests and
PipelineFrontendTests with `-Wall -Wextra -Werror` and ASan/UBSan. Added checks
exercise copied/mutable/replaced/self/nil labels on every existing object class,
encoder labels before/after ending without command rejection or failure-count
changes, the native-half answer with families and unrelated capabilities still
false, and actual lookup wrappers over a modeled translator boundary. Lookup
checks cover detailed NSError propagation through the descriptor and constants
APIs, the no-error API, a thrown C++ exception, invalid names, error clearing on
success and a labeled successful function. No Apple shader fixture is required.

On this Linux host, ConstantHooksTests and the existing M9b cache/bounds test
translation units compiled with warnings as errors and sanitizer instrumentation;
their executables were not run. Build-script shell syntax, harness Python syntax
and whitespace checks passed. A Mac SDK/xcrun is unavailable here, so native
Objective-C builds/tests and the iOS `metal` CI build are not claimed as passed.
The `metal` workflow selection and build recipe are unchanged. Test additions
are included by the existing native test translation units and need no new
framework or CI dependency.

On the next separately authorized isolated-device run, expect gate/IOSurface
import PASS as before, followed by `FUXI MTL: function fixed_frag_lph_cpf`
with `class=FuxiFunction stage=2 device=1`, and a matching `fixed_vert_lph_spc`
or `fixed_vert_lph_gen` vertex with stage 1. Ordering depends on CA's pipeline
work. Label refusals and EntryNotFound for `fixed_frag_lpf_cpf` should disappear.
Translation/pipeline PASS and `FUXI CA: stage 0 solid PASS` are the desired
subsequent observations, not guaranteed by correcting selection: descriptor,
binding, fetch and runtime-dependency limits can still stop CA later.
