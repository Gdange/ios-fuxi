# CoreAnimation GLES route: iOS 16.7 / 20H392

Research date: 2026-10-21. **Yes: this QuartzCore contains a reachable OpenGL
ES 3 compositor for the display render server.** It can avoid the Metal AIR
translation problem. It is not a switch that makes an unmodified Mesa library
an Apple GLES driver. The missing pieces are an EAGL/private-dispatch bridge,
IOSurface integration, and qualification of the resulting GL workload.

For the earliest accelerated CA composition on this project's A740, recommend
a bounded **Mesa GLES + Zink + existing Turnip** experiment, with CPU readback
and the existing CPU presentation path initially. Keep M5b proceeding
independently. This is an engineering priority recommendation, not a measured
speedup or a claim that the GL bridge already exists.

## Scope and evidence

Static analysis of the supplied **iPhone10,3, 20H392, arm64** shared cache.
Main-file SHA256:
`76874e8375252ee643011ff8063f0cce12355f05cf5f9897a34e5679667a65aa`.
The main file is only the cache header/mappings; matching subcaches and
`.symbols` are required. All addresses below are **unslid**, specific to this
cache. Tool: `ipsw 3.1.739`, build
`1f8b657858d7510219f59bf9ba183bf4c0a59104`.

Only this document is changed. Analysis used the already supplied cache on
the research host, with output under ignored `research/`; the Mac tree was
not modified or built. No device access, kernel build, deployment, copied
third-party implementation, or runtime experiment was performed. Findings
below distinguish observed instructions from proposed implementations.

The public [ios-topaz README](https://github.com/MakrSas/ios-topaz#roadmap)
reports GLES composition through Mesa on Adreno 610, with privately shared
patches and CPU frame copying. Its public IOSurface work and accelerator-wait
description support investigating this route; they do not establish an A740
implementation or reveal the private GL bridge. No code from that unlicensed
repository was copied or used to implement anything here.

## Renderer selection in the display server

`CA::OGL` is a common namespace containing **MetalContext, GLESContext and
SWContext**. An OGL symbol alone is insufficient evidence of GL rendering.
The concrete construction and drawing paths below establish GLES presence.
This cache has CAEAGLLayer; no CAOpenGLLayer class was found in its QuartzCore
symbols. The iOS EAGLServer path is the relevant evidence, rather than a
desktop CAOpenGLLayer analogy.

| Decision/function | Exact observed behavior |
| --- | --- |
| `CA::WindowServer::AppleDisplay::new_server()` `0x1883abea4` | Constructs **EAGLServer** at `0x1883abeec` for the IOMFB-backed Apple display. Despite its name, that server supports both Metal and GLES. |
| `EAGLServer` constructor `0x1883da11c`; initialization at `0x1883da838..0x1883da874` | Reads **`CA_ENABLE_OGL`** once into process-global `enable_ogl`. Unset means **true**; otherwise `atoi(value) != 0`. This gates the GLES attempt, not the preceding Metal attempt. Setting it to 1 does **not** force GL over Metal. |
| `EAGLServer::renderer()` `0x1882eaf3c` | Reuses an existing renderer, except when its context invalidation flag (context `+0x308`, bit 12) triggers `purge_contexts`. With no renderer, reads display's cached Metal device (`+0x108`); if absent calls **CAMetalContextCreate** at `0x1882eaf9c`. |
| Metal branch `0x1882eafa8..0x1882eb03c` | A non-nil Metal device selects `CA::OGL::new_metal_context` at `0x1882eafc4`, followed by construction of a CA Renderer. There is **no capability/preference test choosing GL before this branch**. Metal context/library/pipeline failure is not a reliable route to GL: this branch does not first test the returned context for null, and later Metal initialization can abort. |
| GLES gate `0x1882eb040..0x1882eb118` | If no renderer resulted and `enable_ogl` is true, reuses display's EAGL context (`+0x100`) or creates one. Properties explicitly request **Accelerated=true** and **ClientRetainRelease=true**. Tries `initWithAPI:properties:` with API **3**, then API **2** if API 3 returned nil. Sets private parameter `0x260` to zero on the newly created context. |
| GLES acceptance `0x1882eb118..0x1882eb180` | Tests `[context API] == 3`. Only then constructs **GLESContext** at `0x1882eb140` and a CA Renderer. An ES2-only implementation does **not** pass this display-server branch, despite the earlier ES2 creation attempt. Disabled GLES or failed/rejected EAGL leaves a null renderer. |
| `EAGLServer::render_update()` `0x1883601dc` | Calls its renderer via the vtable at `0x188360200`. Non-null goes to `Display::render_display`; null branches at `0x188360254..0x18836026c` to base `Server::render_update`. That base calls `Server::sw_renderer()` at `0x1884f4b6c`. |
| `EAGLServer::render_surface()` `0x188365724` | Uses the accelerated renderer only with a surface exposing its IOSurface and a non-null renderer; otherwise delegates to base `Server::render_surface` at `0x1883657d8`. |
| `Server::sw_renderer()` `0x1884f4c54` | Allocates **SWContext** and Renderer, caches them separately, and logs initialization failure of the accelerated contexts. This establishes software fallback for these server entry points, not universal fallback after arbitrary GL/Metal errors. |

Thus the normal first-initialization conditions for display GLES are:
**no usable cached/default Metal device, GLES enabled, successful EAGL API 3
context, and a renderable surface**. Once selected, GL also needs valid limits,
dispatch, shaders, framebuffer attachments and completion behavior. Merely
returning a non-null EAGLContext is insufficient.

### Accelerator absence, preferences and misleading switches

**CAMetalContextCreate** at `0x18838421c` runs a process-local `dispatch_once`
block, then calls **MTLCreateSystemDefaultDevice** at `0x188384240` on each
invocation. Its block `0x18853e5f8` obtains the IOKit main port and enumerates
**IOServiceMatching("IOAcceleratorES")**. At
`0x18853e664..0x18853e6d4`, any matching service ends the wait; no plugin
property or GPU feature is tested there. With no service it permits **300
100,000-us sleeps** (nominal 30 s), with a final enumeration after those
sleeps. A matching error/main-port failure also ends the wait. This verifies
the substance of the supplied patch-0138 observation, including its
once-per-process scope.

The wait **does not select GLES**. Afterwards Metal discovery still runs.
`MTLCreateSystemDefaultDevice` (`0x181a37760`) initializes the device array;
its selection block (`0x181a37050`) leaves the result nil when the array is
empty, returns its sole device when the count is one, and reports failure
for more than one. `_MTLRegisterDevices` (`0x181b144c8`) matches
IOAcceleratorES, obtains a Metal plugin class and adds only a non-nil
`initWithAcceleratorPort:` result (`0x181b14570..0x181b1458c`). The existing
[Metal discovery analysis](metal.md#ios-167-metal-discovery-re-derived)
records the MetalPluginName/class checks. None of this establishes that a
registered device supports CA rendering.

No accelerator normally means no stock Metal device and therefore a GLES
attempt, but EAGL separately needs a working implementation. An empty matching
service only removes the delay; it creates neither a Metal device nor a GLES
driver. Conversely, a published Metal plugin returning a non-nil device can
capture this server before GLES, even if that device cannot render CA.
For a future GL experiment, keep backboardd's Metal discovery denied **before
device creation**, including any display cache; do not rely on a late queue
failure. The existing default-deny policy is described in [metal.md](metal.md).

No **CA_USE_GL** or **CA_USE_METAL** string was found in QuartzCore's string
inventory, and neither is read in the above selector chain. No
`com.apple.CARenderServer` preference selecting GLES was found there either.
That string is concretely used as a **bootstrap service name** at
`0x18830e648`; `CARenderServerGetDebugOption` (`0x18847666c`) is a Mach request,
not evidence of a defaults key. `-[CAWindowServer setRendererFlags:]`
(`0x1884e331c`) propagates flags to server `+0xb8`; the selector above consumes
them as rendering flags after construction, not as a GL-over-Metal choice.
These are bounded negative findings for this build, not a claim about every
private CA control in other releases.

Two other controls must not be confused with display selection:

* `CAEnableMetalWhippet` and `CADynamicBackendSwitching` are preferences in
  the separate **CA::CG::AccelRenderer::acquire** path (`0x1885076d0`, reads at
  `0x188507754` and `0x188507810`). That path selects Metal or software, not
  this server's GLES compositor.
* **UseMetal**, domain **com.apple.opengl**, exists in **libGFXShared** at
  `0x1d3f6e4d8..0x1d3f6e588`. It selects a driver *behind GLES* when both
  `IOGLESBundleName` and `IOGLESMetalBundleName` exist; it does not choose
  QuartzCore's MetalContext. If the preference is absent, the service's
  `IOGLESDefaultUseMetal` boolean controls that choice; absent/false prefers
  the ordinary GL bundle. If only one bundle property exists, it uses that
  available bundle. A GL route must avoid accidentally selecting an Apple
  GLES-over-Metal driver that recreates the Metal dependency.

The auxiliary `IOMFBDisplay::get_ctx` once block (`0x18853bfe8`) is Metal-only
and skips context construction for a nil device. It does not remove the
independent EAGLServer GLES path.

## EAGL, dispatch, shaders and surfaces

| Requirement | Cache evidence and implementation consequence |
| --- | --- |
| Private macro context | `GLESContext` constructor `0x18840960c` calls **getMacroContextPrivate** at `0x188409690` and stores the result at `+0x7f8`. EAGL's method (`0x1d3f5211c`) returns its private allocation plus `0x20`. CA reads the backend-context pointer from table offset 0 and invokes table slots with that explicit context argument. These functions do **not** have the same ABI as ordinary `gl*` entry points. |
| Limits and extensions | Constructor queries strings/extensions at `0x18840971c..0x188409a5c`, then **GL_MAX_TEXTURE_IMAGE_UNITS** (`0x8872`) and **GL_MAX_TEXTURE_SIZE** (`0x0d33`) at `0x188409aa4..0x188409ac8`. `GLESContext::update` (`0x188405544`) rejects two or fewer texture units as unsupported. `CA_MAX_TEXTURE_UNITS` and `CA_MAX_TEXTURE_SIZE` can cap queried limits; `CA_DISABLE_WORKAROUNDS` changes workarounds, not the backend selector. |
| Shader compiler | `GLContext::create_shader` (`0x1884b30d8`) supplies vertex source at `0x1884b37c4..0x1884b37e0` and compiles at `0x1884b37e4..0x1884b37f0`; fragment equivalents at `0x1884b39bc..0x1884b39ec`; links at `0x1884b3a60..0x1884b3a6c`. Macro slots `0x12a0/0x12a8/0x12c0` correspond to ShaderSource/CompileShader/LinkProgram: compare OpenGLES public stubs, whose offsets are larger by `0x20`. This is a GLSL source path, so a Mesa bridge can compile CA's runtime submissions without translating **default.metallib**. No Apple shader text is reproduced here. |
| IOSurface render target | `GLESContext::make_buffer_current` (`0x188407980`) gets the WindowServer surface's IOSurface, creates a texture, calls **attachImage:toCoreSurface:invertedRender:** at `0x188407ac8`, and attaches it to an FBO color attachment at `0x188407af4..0x188407b0c`. This actual server target uses a texture/FBO, rather than requiring the public CAEAGLLayer renderbuffer API. |
| IOSurface imports | `create_surface_from_iosurface` (`0x188406e90`) checks type/dimensions/maximum texture size, creates texture state and calls **texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:** at `0x188407080`. Image sampling has further imports at `0x188408e58` and `0x188409048`. Need plane, pixel-format, stride, orientation and ownership handling for both source images and destinations. |
| EAGL IOSurface protocol | `attachImage:toCoreSurface:invertedRender:` (`0x1d3f51c60`) maps IOSurface format/dimensions to the texture import method. The latter (`0x1d3f51efc`) gets **IOSurfaceGetID**, then sends GLI **SetInteger 0x38e** with an import descriptor and **0x399** with inversion state (`0x1d3f51f48..0x1d3f51fb0`). A generic EGL texture import extension does not implement this Apple ABI automatically. |
| App EAGL drawables | `-[EAGLContext renderbufferStorage:fromDrawable:]` (`0x1d3f518f4`) and present variants (`0x1d3f51b48` onward) also exist. These bind/present native-window storage through GLI. They matter for CAEAGLLayer app content; the minimal display compositor can start with ordinary layer contents and the IOSurface texture/FBO route above. |
| Fences/lifetime | GLESContext has supports/set/test/delete-fence methods (`0x1884058b0..0x18840591c`), finish (`0x1884059d8`) and release-display-surface (`0x188408754`). Constructor probes **GL_APPLE_fence**, cubic filtering, YCbCr/XR, tile-order rotation, framebuffer fetch, float textures and compression extensions. Many are capability branches, not all unconditional requirements. Advertise only implemented behavior; texture support alone does not establish fence, shader-extension or display reuse correctness. |

An interposer for exported `glDraw*`/`glShaderSource` alone misses CA's direct
macro-table calls. A replacement must supply a compatible **EAGL context and
private dispatch table**, or preserve Apple's EAGL/GLEngine and implement the
lower driver ABI. Context currentness, sharing, explicit-context thunks,
parameter handling and IOSurface callbacks belong in that boundary.

## Two plugin boundaries, neither a Mesa drop-in

1. **OpenGLES -> GLEngine (GLI).** `_eagl_init` (`0x1d3f52444`) locates bundle
   `com.apple.opengles`, appends `GLEngine.bundle/GLEngine`, and dlopens it
   (`0x1d3f52550..0x1d3f52588`). It resolves **gliInitializeLibrary**, probes
   `IOAcceleratorES`, then `IOAcceleratorESLegacy`, and passes the service to
   the engine. If neither exists, it calls initialization with zero services
   (`0x1d3f52714`), rather than failing at the lookup itself. It resolves
   **18 required GLI entry points** from the table at `0x1d3f53350`:
   pixel-format/renderer queries, context creation/destruction, drawable
   attachment, attributes/parameters, version, and view bind/present. Missing
   exports/version/pixel-format failure prevents successful initialization.
   Sharegroup creation also invokes **gfxCreateSharedState**
   (`0x1d3f52170`); replacing only a dylib filename and 18 symbol names is not
   sufficient to remove libGFXShared's shared-state/object ABI.
2. **GLEngine/libGFXShared -> device driver (GLD).**
   `gliInitializeLibrary` (`0x1f6e96388`) calls `gfxInitializeLibrary`
   (`0x1d3f6e2f0`). The latter reads **IOGLESBundleName**, optionally the Metal
   variant described above, constructs `Extensions/<name>.bundle/<name>`,
   dlopens it at `0x1d3f6e8cc`, resolves **gldInitializeLibrary**, and checks
   **gldGetVersion**. The checks at `0x1d3f6e948..0x1d3f6e9cc` include private
   version fields 4, 1 and 16; these are not a claim of desktop GL 4.1 support.
   It then requires **129 `gld*` exports** in the table at `0x1dabc2c20`
   (`0x1d3f6ea10..0x1d3f6ea34`): device/context/sharegroup, buffers/textures,
   programs, framebuffers, synchronization and other operations. Those
   functions consume private Apple object/state layouts, not Mesa's EGL,
   DRI or Gallium plugin ABI.

A GLD adapter to Mesa is possible in principle and would preserve more Apple
code, but requires substantial independent ABI reconstruction and state
translation. A Mesa `.dylib` cannot be registered by setting IOGLESBundleName
alone. A GLI replacement is another possible boundary, with the additional
GFX shared-state work above. For the narrow first experiment, favor an
**original EAGL-method/private-dispatch shim backed by Mesa GLES**, including
the methods CA actually uses. Ordinary GLES exports/current-context behavior
can be bridged too, but are not the whole interface. This lane implements
none of these alternatives and does not establish their load/interposition
mechanism on the root image.

The wrappers and engine inspected here do not themselves open a GPU user
client; that work is delegated behind the driver boundary. The shared cache
contains GLEngine and AGXMetalA11, but no separately listed AGX GLD bundle
whose GL open selectors could be derived here. **Exact stock GL driver
user-client open types/selectors remain unestablished**; do not invent them
from the Metal implementation. The known IOGPU helper
`_IOGPUDeviceCreateWithAPIProperty` (`0x1ece1ce38`) opens type
`1 | (api << 16)`; that is not evidence that CA's EAGL path must open that
type or that our NativeAdreno connection is compatible with it.

Retaining a stock Apple hardware driver would require its matching
IOAccelerator/IOGPU kernel contract and Apple-GPU-specific behavior. Replacing
the driver/engine/EAGL boundary with Mesa can instead use our native Turnip
user client. **Apple IOGPU compatibility is not intrinsically required by
CA's GLES drawing calls.** Real IOSurface IDs, metadata, cross-process content
and lifetime still are. The existing native ABI lacks general host/IOSurface
memory import, as recorded in [metal.md](metal.md); changing API names does
not remove that limitation.

## Effort, risks and recommendation

Mesa's [Zink documentation](https://docs.mesa3d.org/drivers/zink.html) describes
a Gallium driver emitting Vulkan calls and feature requirements that can be
checked against a Vulkan implementation. This provides the proposed
GLES-to-Vulkan bridge, not an existing EAGL port. The current project Turnip
build explicitly disables Gallium; Zink, Mesa's GLES state tracker and a
headless/platform integration must be built and qualified separately.

The [Freedreno documentation](https://docs.mesa3d.org/drivers/freedreno.html)
describes distinct Gallium GL and Turnip Vulkan drivers sharing compiler and
hardware infrastructure but with separate state/command generation. The
reported A610 Gallium success does not qualify A740, or provide a Gallium
Darwin winsys using our ABI. Reusing this project's working Turnip through
Zink avoids starting a second native GPU submission backend. Verify the
chosen Mesa revision's actual A740 and extension support before selecting a
direct Gallium route; do not extrapolate from the GPU family name.

| Route | Remaining work and expected first-pixel risk |
| --- | --- |
| Metal / external metal2vulkan (M5b) | Reuses qualified Metal discovery/transfers and Turnip. Still needs CA AIR specialization/binding translation, a much larger MTL render interface, IOSurface textures, synchronization and presentation integration. CA's selected Metal path can abort on missing libraries/pipelines. M5b's offscreen shader result will reduce a major uncertainty; it does not by itself supply CA composition. See [the existing plan](metal.md#shader-translation-decision). |
| GLES / Zink / Turnip | Avoids AIR/default.metallib translation and much of the MTL object surface; CA already submits GLSL and GL render operations. Adds Mesa GLES/Zink cross-build, EAGL/private macro dispatch and IOSurface compatibility. Vulkan smoke coverage does not qualify Zink's larger feature, synchronization and pipeline workload. Best candidate for earlier CA pixels if the bridge works; no measured advantage yet. |
| GLES / direct Freedreno Gallium | Avoids both AIR and the Zink translation layer. Adds a Gallium Darwin winsys/native-ABI port, A740 qualification and the same EAGL/IOSurface bridge. A610 private-patch success is insufficient evidence this is the shorter route on our hardware. |
| GLES / stock GLEngine + new GLD plugin | Offers a real load boundary but needs the large private GLD state/object ABI and all required exports. Highest reverse-engineering risk for the first milestone; not recommended as the initial shortcut. |

Engineering estimate: an isolated GLES bridge/readback proof is a **days-scale
spike**, while correct display-server integration is plausibly **weeks or
longer**, not a one-setting change. These estimates assume Mesa/Zink can be
made to load against the current Turnip; ABI or extension problems can defeat
them. No latency, memory, thermal or cold-compilation comparison was run.
Zink adds translation and pipeline-cache costs; GL being older does not imply
faster frames. The expected advantage is **less new compatibility and shader
translation work before the first composed frame**, not proven performance.

Mesa's core is **MIT**, with per-file/component licenses that must be retained
and checked for the selected build; see its
[license policy](https://docs.mesa3d.org/license.html). An original Mesa-backed
GL shim avoids the external GPL/LGPL metal2vulkan translator dependency for
this route. It does not license the unlicensed/private Topaz/HackiOS patches,
or permit shipping Apple binaries/shaders. Use only independently written
bridge code and the user's externally supplied system frameworks.

## Minimal first milestone and promotion gate

**First end-to-end target:** backboardd selects **GLESContext**, composites a
small known layer scene on the GPU, and produces a correct **BGRA IOSurface**
which the existing **CPU present path** displays. Do not require direct
GPU-to-panel rendering, CAMetalLayer drawables or zero-copy memory import for
this milestone.

Proposed acceptance sequence, for a separately authorized implementation lane:

1. In an isolated unsandboxed test process, create the shim's API-3 EAGL
   context and call it through CA's private macro ABI, not only public GLES.
   Exercise an original solid quad plus a small sampled texture, blend,
   clipping and shader compile/link. Verify exact pixels, bounded completion,
   teardown/reopen and non-software Mesa renderer identity. First expose only
   implemented extensions; audit CA-generated GLSL compatibility externally.
2. Bind a BGRA IOSurface through the actual `attachImage`/`texImageIOSurface`
   contracts. Initially use a Mesa-owned offscreen GPU target, wait for
   completion, then bounded readback/staging copy into the retained CPU-mapped
   IOSurface, respecting stride, BGRA/RGBA order, alpha and vertical origin.
   This is GPU composition **followed by CPU transfer**, not direct rendering
   into imported IOSurface pages. It avoids needing new host-import kernel
   work merely to prove the route. Source IOSurfaces can likewise start with
   synchronized CPU uploads. Test content preservation for partial updates.
3. With the first two gates passed, select GLES early in backboardd by keeping
   Metal device discovery nil and GLES enabled. Demonstrate CA-produced
   layers, text/texture contents, transparency and a moving/dirty rectangle
   reaching the existing present path. Count actual GL draws/GPU submissions
   and correct surface output; absence of a crash or a GPU renderer string
   alone is insufficient. Retain references until both GPU work and display
   consumption finish. Test repeated surface reuse and failure/restart into
   the existing software configuration.
4. Only then expand to the lock screen, home screen and Settings workload;
   compare CPU time/frame, wall latency, cold shader cost, RSS and thermal
   behavior against software and the progressing Metal route. Optimize shared
   memory import, fences and direct surface rendering after correctness.

The decision after that spike should be based on the EAGL/private-table and
IOSurface gate, not a generic Mesa triangle. A successful result would make
GL the preferred short-term CA compositor while Metal continues for Metal
clients. A bridge that requires full GLD reconstruction or unsupported Zink
features loses that time advantage; reassess using M5b's actual results.

## Reproduce without committing Apple material

Set `IPSW` to the external analysis tool and `DSC` to the supplied main cache
with its companion subcaches. `ANALYSIS` must be inside an ignored directory.
Commands below are read/analysis only; their output, symbol caches, extracted
images and any runtime shader inputs must remain outside tracked artifacts.

```sh
mkdir -p research/gles-analysis
git check-ignore research/gles-analysis
ANALYSIS=research/gles-analysis
"$IPSW" version
shasum -a 256 "$DSC"
"$IPSW" dyld info "$DSC" --dylibs > "$ANALYSIS/images.txt"
for image in QuartzCore OpenGLES GLEngine libGFXShared.dylib; do
  "$IPSW" dyld macho "$DSC" "$image" --symbols > "$ANALYSIS/$image.symbols"
  "$IPSW" dyld macho "$DSC" "$image" --strings > "$ANALYSIS/$image.strings"
  "$IPSW" dyld disass "$DSC" --image "$image" > "$ANALYSIS/$image.disass"
done
rg 'GLESContext|gles_new_context|EAGLServer|CAOpenGLLayer' "$ANALYSIS/QuartzCore.symbols"
rg 'CA_USE_GL|CA_USE_METAL|CA_ENABLE_OGL' "$ANALYSIS/QuartzCore.strings"
"$IPSW" dyld disass "$DSC" --symbol _CAMetalContextCreate --symbol-image QuartzCore
"$IPSW" dyld disass "$DSC" --symbol _MTLCreateSystemDefaultDevice --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol ___MTLCreateSystemDefaultDevice_block_invoke --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _MTLRegisterDevices --symbol-image Metal
"$IPSW" dyld disass "$DSC" --vaddr 0x1882eaf3c --count 185
"$IPSW" dyld disass "$DSC" --vaddr 0x1883da838 --count 25
"$IPSW" dyld disass "$DSC" --vaddr 0x18853e5f8 --count 90
"$IPSW" dyld disass "$DSC" --symbol _eagl_init --symbol-image OpenGLES
"$IPSW" dyld dump "$DSC" 0x1d3f53350 --size 576
"$IPSW" dyld disass "$DSC" --symbol _gliInitializeLibrary --symbol-image GLEngine
"$IPSW" dyld disass "$DSC" --symbol _gfxInitializeLibrary --symbol-image libGFXShared.dylib
"$IPSW" dyld dump "$DSC" 0x1dabc2c20 --count 129 --addr
"$IPSW" dyld disass "$DSC" --symbol _IOGPUDeviceCreateWithAPIProperty --symbol-image IOGPU
```

Use the function names/addresses in the evidence tables to inspect surface,
shader and fallback functions in the ignored disassembly. These commands and
the prose findings are the reproducible deliverable; raw Apple instructions,
shader bodies, dumps and binaries are deliberately not repository artifacts.
