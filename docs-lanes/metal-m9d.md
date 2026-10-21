# Metal M9d: QuartzCore vertex descriptors and the next boundary

2026-10-22. Source, static analysis and compile checks only. No test-executable
runs, GPU submissions, device access, deployment, workflow dispatch or push.
backboardd and SpringBoard remain DENIED. The supplied third isolated CA smoke
is an observation of the preceding build: both lph functions are found, then
the descriptor gate refuses twice and CA aborts. No stage-0 success is claimed.

## The descriptor CA constructs

The inspected image is the pinned iOS 16.7 QuartzCore, build 20H392, UUID
9E40B2EB-260C-3AF1-AE45-A1A551A8C1B9. The existing M8 resolved static trace
was followed through `get_pipeline_spec`, `derive_vertex_spec` and
`create_pipeline_state`. The `vertex_layout_info` table was independently read
from the read-only shared-cache mapping. These are static public-value facts,
not a newly captured live descriptor. No Apple code, IR or disassembly is
included here.

`create_pipeline_state` caches descriptors by the two-bit vertex layout. It
configures **layout/buffer 1**, sets its step function to PerVertex (1), and
leaves its default step rate at 1. Each attribute's bufferIndex is also 1.
It does not derive the descriptor from the lph/lpf arithmetic precision suffix.

| Layout | Stride | Configured attributes: index, Metal format (ordinal), byte offset |
| --- | ---: | --- |
| Surface / ordinary triangles, layout 0 | 48 | 0 Float4 (31) at 0; 1 Float2 (29) at 16; 2 Float2 (29) at 24; 3 Half4 (27) at 32 |
| Line, layout 1 | 16 | 0 Float2 (29) at 0; 3 Half4 (27) at 8 |
| Point, layout 2 | 16 | 0 Float2 (29) at 0; 4 Float (28) at 8; 3 UChar4Normalized (9) at 12 |

Other attributes remain Invalid and other layouts retain zero stride and
PerVertex/rate-1 defaults. The stage-0 solid layer follows the Surface triangle
path, so **attribute 3, Half4, buffer 1, offset 32, stride 48** explains the
observed generic attribute rejection. A shader may specialize away texcoords;
CA still installs all four Surface attributes. The Line and Point descriptors
are described and accepted as descriptors; line and point draw topologies are
still outside this lane's render profile.

For a generic vertex spec, the generic bit bypasses the descriptor assignment.
`fixed_vert_lph_gen` manually loads buffer 1 using vertex_id and a layout
configuration in buffer 0; the public getter may subsequently expose a lazily
created empty descriptor. It must not be assigned the specialized descriptor
merely because its function name shares the same prefix.

The default metallib's AIR signatures corroborate this distinction.
`fixed_vert_lph_spc` has position float4 at attribute 0, conditional point size
float at attribute 4, conditional float2 texcoords at attributes 1 and 2, and
color **half4 at attribute 3**. Buffer 2 supplies a 64-byte float4x4 MVP;
conditional texture/chroma matrices use buffers 3 and 4. Its position output is
float4 and its color varying is half4. `fixed_vert_lph_gen` instead declares
buffer 1, vertex_id, a four-byte configuration at buffer 0, the same MVP at 2,
and optional matrices at 3/4. The pipelinelib's embedded AIR entries were also
checked: specialized entries retain position/color and whichever texcoords
and matrices their specialization needs; the generic entry retains the manual
buffer path. Thus a half color input is real; neither Half2 nor instanced or
constant stepping is present in these three descriptor tables.

## Accepted profile and backend consumption

The source pin remains metal2vulkan
`720ef1ce9ae2a2068999a8675717418b9416283e`. Its
[`metal/vertex.rs`](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan/src/metal/vertex.rs)
maps Metal half to R16 SFLOAT, float to R32 SFLOAT, normalized integers to
UNORM/SNORM, and integer inputs to UINT/SINT (or scaled formats when the shader
expects float). BGRA maps to B8G8R8A8_UNORM; signed/unsigned 10:10:10:2 map to
A2B10G10R10 SNORM/UNORM, and RG11B10 maps to B10G11R11_UFLOAT_PACK32.
Three-component formats preserve their three-component width; they are never
silently widened into a four-component memory read.

The inspected Mesa revision matches the Turnip build pin,
`85fda3b36405bcaea8bffc64aa4f2651dc90770d`. Its
`src/freedreno/fdl/fd6_format_table.c` supplies vertex formats for those mappings,
including three-component 8/16/32-bit forms and BGRA/packed formats.
`tu_formats.cc` advertises VERTEX_BUFFER from that vertex-format support.
RGB9E5 is texture/color capable but has **no vertex format** there. It stays
rejected even though the translator knows an exact E5B9G9R9 mapping.

Frontend and bridge now admit the 52 defined formats with ordinals 1–42 and
45–54: scalar/vector byte and short signed/unsigned integer and normalized
forms, Half/Half2/Half3/Half4, Float/Float2/Float3/Float4, signed/unsigned
32-bit scalar/vectors, the two 10:10:10:2 normalized formats, normalized BGRA,
and RG11B10. Invalid is an inactive attribute, 43/44 are holes, and 55
(RGB9E5), unknown values and values outside that set refuse.

The bounds remain explicit: attributes 0–7, buffer/layout indices 0–3,
referenced strides 1–256, and offset plus the exact format byte width at most
256. Multiple independent buffers already travel through the ABI; buffer 1
needs no remapping. Nondefault layouts are checked even at zero stride, so a
constant or instance step cannot sneak through the old `if (stride)` check.
Only PerVertex/rate 1 is accepted. Errors name instance/constant stepping or a
divisor as deferred. ABI 4 remains unchanged: it contains no step state, and
the bridge explicitly refuses any translated instance rate or divisor other
than 1 rather than flattening it into VK_VERTEX_INPUT_RATE_VERTEX.
Supporting instance rates would require transmitting that state and enabling
and consuming VK_EXT_vertex_attribute_divisor where required; CA stage 0
does not require it.

The pinned compiler can choose shader-pulled fetch for containment, alignment
or format reasons. Existing binding plans still govern that choice. For actual
fixed-function output, Backend additionally checks the translated Vulkan
format's bufferFeatures for VERTEX_BUFFER before pipeline creation. Unsupported
runtime dependencies continue to refuse. The CPU fetch bounds check now uses
the exact byte width, including Half4 = 8 and Float3 = 12, with subtraction to
avoid bound-offset overflow. This fixes the old 16-byte guess that falsely
overran the last Half4 of a tightly sized compact quad. That conservative
check still covers every configured attribute, even one specialized away.

## Beyond the descriptor gate

The supplied stage-0 pipeline already requests one BGRA8Unorm attachment,
write mask 15, fixed-function blending off, sample count 1, and no depth or
stencil formats. Those settings fit the existing backend. Blending off alone
does **not** establish that the fragment shader has no destination input.
CA can perform blending in its fixed fragment function.

A likely next pipeline refusal is **ordered framebuffer fetch**. The context
constructor unconditionally sets its fetch-mode feature bit 11, independently
of `supportsFamily:` and `supportsRasterOrderGroups`. `get_pipeline_spec`
copies that to fragment-spec bit 5, and `create_fragment_shader` supplies
`fc_framebuffer_fetch` from it. With one attachment, the non-generic AIR
initializer enables its destination-0 input when this mode is true. This is
stronger evidence than simply observing that some M8 catalog entries fetch.
Whether the particular solid specialization prunes the input/its uses is
still a compiler boundary, not a result of this source-only lane. If retained,
our empty Vulkan EnableSet cannot satisfy the ordered input-attachment or
fragment-interlock alternatives and translation refuses before Vulkan pipeline
creation. CA retries and aborts on nil just as in the supplied log. Enabling
the ordering extension, attachment roles, layouts, descriptors and barriers
together is substantial work described in M9b; changing a capability answer or
substituting an unordered texture read is not a bounded fix here.

If translation and pipeline creation succeed, the static draw path is otherwise
quite small. `start_render_encoder` installs the color target and load/store
actions, labels the encoder and calls `update_encoder_state` with zero dirty
bits. That first update sets a viewport over the surface, with znear 0 and
zfar 1. The draw routine intersects its scissor with the window/surface bounds.
For this surface both fit the existing 256-square checks. CA constructs its
default depth/stencil object without altering the public descriptor: Always,
no writes, no stencil tests. Existing neutral-state support covers it; there
is no evidence that the solid stage needs stencil storage or stencil ops.

Later dirty-state restoration can request clockwise winding, no culling,
fill mode, depth bias (0,1,0), zero blend color and disabled visibility queries
at offset zero. These cheap, inert cases now have explicit bounded adapters;
other culling/fill/winding/query requests still poison the command. Depth bias
also accepts (0,0,0), and finite 0–1 blend constants are inert under the existing
factor profile, which excludes constant blend factors. The initial zero-dirty
update does not call all these setters; they are anticipated restoration
calls, not a newly observed stage-0 refusal.

The inspected MetalBufferPool uses shared `newBufferWithLength:options:0`,
CPU contents and ordinary `setVertexBuffer`/`setFragmentBuffer`, then offset
updates when the buffer is reused. Its minimum allocation is 256 KiB; requests
larger than that use the requested size. Stage-0 vertex data is four ordinary
48-byte vertices for a quad (192 bytes), and the MVP is 64 bytes, aligned to
16. The generic path can additionally use a four-byte vertex configuration.
Optional texture matrices occupy vertex slot 3; chroma at slot 4 and other
advanced inputs remain refused if requested. None is needed for a plain
solid layer. Fragment slot 1 uniforms are encoded as 176 or 240 bytes, aligned
to 16; generic configuration at slot 0 is 52 bytes, aligned to 4. These fit
existing allocation limits and the four resource slots. The inspected pool
does not use `setVertexBytes` or `setFragmentBytes` for this path; their existing
1–4096-byte copied, retained implementations remain available.

`draw_elements` maps triangles/strips to Metal's corresponding primitive types.
Without indices, CA calls `drawPrimitives:vertexStart:vertexCount:`; with
indices, it writes UInt16 indices and calls the existing non-instanced indexed
selector. A quad strip has four vertices, or its triangle list has six indices.
The generic draw routine also has an instance-count selector for counts at
least two. A one-instance alias now records the same bounded operation;
multiple instances explicitly refuse. Lines, points, UInt32 indices, true
instancing, stencil attachments and GPU-written index buffers are not added.

If fetch prunes away and the pipeline succeeds, the remaining candidates are
the compiler's particular stage interface/resource/runtime plan, pooled-buffer
offset alignment, optional resources beyond slot 3, later pool purgeability
calls, and completion/clear-only encoder behavior. These are not statically
proven failures of the solid draw. The backend still refuses an undefined LOAD
attachment, unsupported plan, feedback, unsafe fetch or failed command before
submission, and the isolated CA failure latch remains in place. No system
compositor admission or software transaction replay follows from this change.

## Diagnostics and validation

The existing `pipeline descriptor` line now appends every configured public
attribute and layout, including ignored/nondefault unused state and the
referenced stride/step values. Each traversal is bounded to 31 entries, with
at most 64 descriptor records per process. Failure logs remain capped at 64;
the pipeline/library message permits up to 768 diagnostic characters so the
index, numeric/name format, bufferIndex, offset, stride, stepFunction and
stepRate survive together. An invalid bufferIndex is never used to index an
SDK layout array. Layout errors include the first referring attribute when
one exists; an unused bad layout explicitly says attribute=none.

Expected Surface descriptor fragments on a separately authorized next smoke
are `attribute[0] format=31(Float4) bufferIndex=1 offset=0`,
`attribute[1] format=29(Float2) bufferIndex=1 offset=16`,
`attribute[2] format=29(Float2) bufferIndex=1 offset=24`, and
`attribute[3] format=27(Half4) bufferIndex=1 offset=32`, each followed by
`stride=48 stepFunction=1 stepRate=1`. The same line ends with
`layouts={1:stride=48/stepFunction=1/stepRate=1}`. The old generic
`FAIL vertexDescriptor attribute` should disappear for that descriptor.
An unsupported layout instead produces, for example,
`FUXI MTL: pipeline/library FAIL vertexDescriptor layout[1] stride=48 stepFunction=2 stepRate=1 ... reason=stepFunction requires PerVertex (instance/constant deferred)`.
The ellipsis here abbreviates the associated attribute fields, not actual log
truncation. If ordered fetch is next, expect `translation cache MISS`, then
`pipeline/library FAIL` mentioning framebuffer_fetch/FramebufferFetch and the
ordering requirement, a retry, and potentially the same CA SIGABRT line.
Only an actual translation and `Vulkan pipelines PASS` line establishes that
the pipeline boundary was passed; neither guarantees stage-0 pixels.

Native CPU-model tests cover all three CA layouts reaching the backend,
complete summaries, all 52 accepted widths, last valid offsets, malformed
buffer/layout indices, zero strides, RGB9E5 and step/divisor refusal, plus
neutral state restoration and one-versus-two-instance calls. The existing
PipelineFrontendTests translation unit includes these checks in
`tools/test-fuxi-metal-frontend.py`; PipelineDefaultsTests also shares them.
SDK-independent VertexProfileTests exercises exact fetch endpoints and offset
overflow and is included in that same harness on Linux and Mac. Rust unit
checks cross-check widths against the pinned translator's mapped Vulkan
format blocks. M7's authored AIR/lavapipe regression adds half coordinates,
Half4, Float3, normalized ushort, BGRA and packed color fetches across two
buffers, exact buffer endpoints and asymmetric red pixels to expose swizzles.

On this Linux host, VertexProfileTests, ConstantHooksTests, the M9b controls
and the actual Backend/M7 test translation unit compiled with
`-Wall -Wextra -Werror` and ASan/UBSan instrumentation. `cargo check --tests`
completed offline against the clean pinned translator, including the new Rust
unit tests. Python syntax, build-script shell syntax and whitespace checks
passed. No executable tests, including M7/M9b lavapipe tests, were run. Native
Objective-C++ tests require Mac Foundation/Metal and are unrun/uncompiled here;
the `metal` CI target, recipe, ABI and deployment target are unchanged, but no
new CI build result is claimed. Apple analysis products remain ignored, and
no Apple binaries, shaders, IR, disassembly, translator or Mesa sources are
committed.
