# Native Adreno HostImport source increment

2026-10-21. Source and Linux host tests only. This supersedes the no-import
kernel limitation recorded in [Metal M6](metal-m6.md), without qualifying M6 on
hardware or completing CoreAnimation. No SSH or device access, deployment,
workflow execution or push was used.

## Shared ABI and ownership

NativeAdrenoABI **2** keeps the existing record sizes and direct selectors.
`BoImport` (16) now implements the 16-byte `{address, bytes}` request and returns
the existing 8-byte BO-handle reply. Query advertises `HostImport`; the minimum
address/length alignment is **16,384 bytes**, not the SMMU's 4 KiB leaf size.
The kernel's two generated header copies, Turnip's configured ABI include and
FuxiMetal's direct include use the same canonical `NativeAdrenoABI.h`. Turnip
checks Query's version and both userspace components assert ABI 2 at build time.

The caller must be the task that opened the root-only connection. Import uses
a persistent writable task memory descriptor, prepares/wires the entire range,
and validates size, overflow, quota, managed physical pages, physical address
width and Normal WB/NC cache attributes. Missing/read-only/foreign-device
mappings fail. A kernel alias inherits the memory entry's actual cache policy;
its virtual range is also explicitly wired so SMMU translation and watchdog
cache operations cannot demand-fault. No user VA is translated in kernel_pmap,
no physical descriptor is manufactured over managed pages, and no CPU cache
attribute is changed. The user allocation must remain alive and CPU/other
producer access must be externally synchronized throughout GPU ownership.

BoMap installs the same pages as Normal NC, outer-shareable GPU leaves and
registers the wired kernel alias for cache handoff. A submission can indirectly
reference any mapped BO, so every mapped import participates. Under the existing
GPU mutex, submit cleans/invalidates each 64-byte CPU cache line to PoC, with
DSB SY barriers before the doorbell. Retirement invalidates those CPU lines
before publishing fence completion. The watchdog performs the same retirement
without requiring WaitFence. An acknowledged fault drain also invalidates;
a failure before handoff never discards dirty CPU data. This uses explicit ARM
cache instructions because XNU's IO-coherent helper configuration can omit
physical cache maintenance on this non-coherent GPU.

Imported BOs have no CPU-map token; Turnip keeps the original host pointer.
Free first removes GPU leaves and synchronizes the TLB, then unwires the kernel
alias and completes/releases the descriptor. Close/task death follows the same
path after a bounded drain. If DMA isolation cannot be confirmed, the mapping
and pin remain quarantined until reboot, as ordinary BOs already do. Failed
kernel-alias unwiring also retains ownership for retry. There is no GPU recovery
or quarantine reclamation worker in this increment.

## Turnip and Metal

`mesa-fuxi-hostimport.patch` overlays the existing pinned UniXNU implementation
of `VK_EXT_external_memory_host`. It sets the 16 KiB alignment, validates host
allocation type, pointer, size, compatible memory type and capability, rejects
foreign-device imports and conflicting FD imports, and gates image/buffer
queries on actual kernel support. Host images advertise linear 2D import only,
with no export or dedicated-only claim. Type 0's host-coherent behavior is
provided by the kernel handoff; the caller needs no user cache instructions.
The original pointer, BO handle, GPU VA, submit and fence paths are reused.
The extension's allocation-lifetime/alignment rules are described by the
[Khronos host-memory contract](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_host.html).

There is no new IOSurface-ID API. FuxiMetal's existing
`newTextureWithDescriptor:iosurface:plane:` selector now reaches the implemented
VA importer via Vulkan. It retains, increments use count and locks the exact
surface, checks plane/format/extent/stride/allocation requirements and imports
those pages as a linear image. The same texture can be a sampled source or
render target. Native builds require the advertised 16 KiB alignment. No upload
copy substitutes for import. The existing 256-square BGRA, exact Vulkan row
pitch, plane-0, one-sample and quiescent-producer restrictions remain.

## Build/deployment coupling and stage 5 oracle

The existing CI entry points remain:

* `tools/ci/build.sh kernel-metal4`: prepare-kernel.py copies the required XNU
  patches, which apply in the builder's lexical order. 0098 carries ABI 2;
  0100's tracing/discovery delta was regenerated against it.
* `tools/ci/build.sh mesa`: build-fuxi-turnip.py applies the five hash-verified
  upstream patches, then ios16, smoke and hostimport overlays, on Mesa
  `85fda3b36405bcaea8bffc64aa4f2651dc90770d`. Mesa remains external.
* `tools/ci/build.sh metal`: builds FuxiMetal with the shared ABI header. Output
  names and artifact allowlists are unchanged. `--verify-inputs-only` for the
  Turnip recipe also runs on Linux; actual builds still require macOS.

Deploy the ABI-2 **kernel and rebuilt Turnip ICD together**, plus the rebuilt
complete signed FuxiMetal bundle. Rebuild the raw GPU ABI test if used. The
already-required M6 smoke executable, original smoke metallib and external
translator/bridge ABI 2 must also be present for stage 5. The translator bridge's
ABI 2 is independent of NativeAdreno ABI 2; its source/pin did not change here.
Do not mix a new ICD with an ABI-1 kernel.

Root changes are limited to those already described in M6: stage configuration
`/usr/local/share/fuxi/metal-smoke.conf` containing `5\n`, signed bundle under
`/System/Library/Extensions/FuxiMetal.bundle`, signed ICD at
`/usr/local/lib/libvulkan_freedreno.dylib`, and the M6 smoke/shader/translator
artifacts. Preserve root:wheel ownership, 0755 directories/executables and 0644
configuration/resources. No root image or shipped stage-4/default-deny policy
was changed. Leave backboardd/SpringBoard denied: host import does not resolve
CA function constants, buffer layouts, blending, presentation or synchronization.

Expected boot capability line:

```text
FUXI GPU ABI: NativeAdrenoGpuPublished v2 HostImport 16K, open 0x4147, root only, watchdog 100 ms
```

Expected stage 5 success lines, **not observed on device**:

```text
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: IOSurface import PASS BGRA8 256x256 same-memory
FUXI MTL: translation cache MISS
FUXI MTL: translation cache HIT
FUXI MTL: stage 5 composition PASS IOSurface BGRA8 256x256 textured quad 65536 pixels + same-memory target + sentinel edges
FUXI MTL: done PASS
```

Other translation/timing lines may intervene. Exact linear layout/format,
translation or unsupported selector failures remain real failures; no source
change here constitutes a device PASS.

## Host evidence and unvalidated work

Passed on Linux with ASan/UBSan:

* `test-fuxi-hostimport.py`: actual kernel import/drop/close methods; alignment,
  overflow, ownership, quota, all descriptor/physical-list/mapping/wiring failure
  unwinds, pin/refcount lifetime, busy free, normal task close and quarantine.
* `test-fuxi-metal.py`: shared ABI and actual SMMU/backend code, indirect imported
  resource cache handoff, independent watchdog retirement, drain/quarantine and
  fault-before-handoff behavior, plus the existing register/framing coverage.
* `test-fuxi-turnip-hostimport.py --mesa EXTERNAL_MESA --patches UPSTREAM_PATCHES`:
  exact pinned patch-set verification and actual Vulkan host-pointer/image/
  buffer queries, allocation host branch, KMD import/map failures, original CPU
  pointer, BO refcount, free retry and failed-free IOVA retention.
* Existing Turnip submit, managed BO mapping/lifetime, kernel mapping, surface
  cache and exec/trace tests; software Vulkan backend timeout/loss/corruption
  controls and M6 same-memory image import/read/write. Original GLSL controls
  sample nearest/linear into the imported target and compare the full quad.

All **100 required XNU patches** passed git apply --check/application in CI order
on fresh xnu-8796.141.3 with the three staged providers; the four applicable
optional pinned-builder patches also applied. Final ABI headers/client/backend
match canonical bytes. HFS staging used a minimal TargetConditionals header
for patch application only. This was not a kernel build or SDK validation.

macOS/XNU/ICD/frontend compilation, linking/signing/import audit, A740 SMMU and
real CPU/GPU cache behavior, task VM/COW races and death under real DMA,
performance of whole-import cache walks, IOSurface/Vulkan exact linear layout,
new AIR sampling, all stage-5 device pixels and CA/display synchronization
remain unvalidated. No Apple code/binary, firmware, Mesa source or device dump
was added to this repository.
