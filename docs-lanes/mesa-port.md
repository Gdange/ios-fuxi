# P-MESA: HackiOS ir3 audit

2026-10-23. Source and Linux host verification only. No Mesa build, device
access, deployment, CI/workflow execution or push. This extends
[Turnip T1](turnip-t1.md) with one compiler fix; XNU and the current root/boot
configuration are unchanged.

## Sources and series

* Mesa stays at `85fda3b36405bcaea8bffc64aa4f2651dc90770d`, whose `VERSION`
  is `26.2.0-devel`. HackiOS's patches target Mesa 25.0.7; applicability was
  checked against our source, rather than inferred from those descriptions.
* HackiOS reference: `hackios-xnu-userland` commit
  `906425b75e02b4f99eb5d7bb1ef7c9583472d772`, `tools/mesa/*.patch`.
  Its own changes are MIT, copyright 2026 OOSEX GANG. The ported patch retains
  that full notice and a `Ported from` header. Mesa's existing MIT file headers
  remain in place. No HackiOS kernel/APSL code is needed here.
* UniXNU reference: `fb6aad6ab1f50199a4d99d2da1d0ca403b067633` (project
  BSD-2-Clause-Patent; existing component notices remain applicable). The supplied
  checkout's HEAD and available branch tips are this commit: it contains no
  post-fb6aad6 Mesa changes. All nine files in `patches/mesa/` match the names
  and SHA256 pins already adopted in T1. No additional Mesa patch appears
  elsewhere in its `patches/` inventory. This is an audit of the supplied
  reference, not a claim about a later remote HEAD. No new UniXNU code is copied.

The insertion order in `tools/build-fuxi-turnip.py` is authoritative:

1. The nine hash-verified UniXNU patches, in T1's documented order.
2. `mesa-fuxi-smoke.patch`.
3. `mesa-fuxi-hostimport.patch`.
4. `mesa-fuxi-ir3-round-default.patch`.

`tools/ci/build.sh mesa()` already calls this recipe and archives
`tools/patches/mesa-fuxi-*.patch`; no CI wiring or pin change is necessary.
Do not infer application order from the archive's lexical filename order.

## Every HackiOS Mesa patch reviewed

| Reference under `tools/mesa/` | Decision and bug/scope |
| --- | --- |
| `ir3-round-default.patch` | **Port** as `mesa-fuxi-ir3-round-default.patch`. `create_cov()` creates a zero-initialized `cov`; its zero rounding field means truncation (`ROUND_ZERO`). When NIR float controls leave the mode undefined, neither existing branch sets the field. Dynamic integer-to-float conversion can therefore disagree with NIR constant folding, which uses nearest-even. The shared conversion path is used by A740 Turnip as well as Gallium. |
| `ir3-shift-split.patch` | **Already resolved in pinned source.** The older `create_shift()` distributed an unsigned right shift over an `iadd` with a constant. For `x=24`, `C=0xfffffffc`, `s=2`, `(x+C)>>s` is 5 but `(x>>s)+(C>>s)` is `0x40000005`. Unaligned constants can also lose carries. Our pin has no `create_shift()` or SSBO addition-splitting path: `lower_offset_for_ssbo()` shifts the entire offset using `nir_ushr_imm`, accounting for `offset_shift`. Keep that implementation; do not reintroduce the old optimization just to add its guard. |
| `ir3-ssbo-subdword.patch` | **Omit for A740 Turnip.** This is a real descriptor/access-width mismatch on Gallium's dword-only SSBO descriptors, and on Turnip devices lacking narrow storage. Narrow loads can address the wrong bytes; stores require atomic dword lowering to avoid losing neighboring threads' bytes. A740 inherits `storage_16bit=True` and `storage_8bit=True` from `a7xx_base` in `freedreno_devices.py`. `tu_device.cc` passes both to ir3; `write_buffer_descriptor_addr()` in `tu_descriptor_set.cc` writes the narrow descriptors, and `lower_ssbo_descriptor_instr()` in `tu_shader.cc` selects them. The patch's widening condition is false on this device. Gallium is disabled in our recipe, so porting it would not fix an A740 bug. |
| `freedreno-darwin.patch` | **Omit.** Primarily HackiOS's Gallium/redwood DRM shim, OSMesa IOSurface import, Darwin build/clock/memory plumbing. Its first-framebuffer-read CCU/UCHE barrier fixes a real A642L Gallium GMEM issue, but that draw/batch path is not built or called by Turnip. The ir3 Darwin cache hunk overlaps T1's existing Darwin build-id handling and our disabled disk cache. No evidence justifies transplanting a Gallium barrier into Turnip. |
| `osmesa-es-profile.patch` | **Omit.** Adds an OSMesa GLES context profile; OSMesa/Gallium are not built. This is API support, not an A740 Turnip compiler fix. |
| `air-frontend.patch` | **Omit.** Builds HackiOS's separate AIR frontend; Fuxi's existing external translation path is unchanged. |
| `air-ir3.patch` | **Omit.** AIR/chip-id support and a duplicate `ir3_nir_lower_variant()` removal in HackiOS's standalone Gallium compiler tool. The duplicate lowering is a real tool bug, but this older tool path is absent from our pin and our recipe uses `-Dtools=`. It does not affect Turnip's shader compilation. |
| `air-metal.patch` | **Omit.** Adds the Gallium-backed `libredwoodair` compute library and its test; not our Metal/Vulkan architecture. |

The rounding port preserves explicit `f2f16_rtne`, `f2f16_rtz` and per-width
RTNE/RTZ float controls, and does not change integer-destination conversions.
Only the missing default branch is added, with a shorter comment describing
the NIR agreement rather than claiming a universal API rounding requirement.
For `pack_half_2x16`, whose rounding is undefined, choosing nearest-even is
permitted but is not itself a correctness fix. FP16 hardware rounding, including
the ordinary `f2f16` path, is not established by the host harness or HackiOS's
reported F32 measurements.

## Host verification

Use a disposable, external clean checkout at the Mesa pin and the nine UniXNU
text patches; nothing from either reference checkout is executed or built:

```sh
python3 tools/build-fuxi-turnip.py --mesa "$MESA_SRC" \
  --patches "$PATCH_DIR" --build "$MESA_BUILD" --verify-inputs-only
python3 tools/test-fuxi-mesa-ir3.py --mesa "$MESA_SRC" --patches "$PATCH_DIR"
python3 tools/test-fuxi-turnip-submit.py \
  --upstream-patch "$PATCH_DIR/turnip-darwin-kmd.patch"
python3 tools/test-fuxi-turnip-hostimport.py \
  --mesa "$MESA_SRC" --patches "$PATCH_DIR"
python3 tools/test-fuxi-vulkan.py --headers "$MESA_SRC/include"
```

Passed on Linux:

* Sequential `git apply --check` and application of all twelve patches on clean
  pinned Mesa, followed by the recipe's exact reconstructed-tree comparison.
  The pre-existing eleven-patch T1 cache also upgrades successfully; a complete
  P-MESA cache is accepted. Tampered upstream patch text and an unexpected edit
  to `ir3_compiler_nir.c` are rejected.
* The new ASan/UBSan C harness extracts the actual conversion-rounding block,
  `set_cat1_round()` and NIR rounding helpers from the patched external source.
  Scalar and 2/3/4-component repeats, FP16/FP32 mode selection, explicit opcode
  precedence, controls for the other width and integer destinations pass.
  An unpatched-source negative control fails the default-rounding assertion.
* Six unsigned integer boundary values are checked with bit-exact nearest-even
  and truncation host IEEE-float oracles. These model the selected mode; they
  do not execute an Adreno instruction. In particular `0x01000003` must produce
  F32 bits `0x4b800002` (nearest-even), versus `0x4b800001` (truncation).
* The actual SSBO offset-shift block passes 32-bit modular-addition models for
  negative constants, unaligned offsets, high bits, wraparound and existing
  `offset_shift` values. Both old splitting counterexamples are reproduced.
* Existing Turnip submit/wait and host-import/deferred-free ASan/UBSan harnesses
  pass with the expanded recipe. The Vulkan fake-ICD no-submit/failure/cleanup
  and offline mode/owner staging regression passes.

This does not compile all of ir3 or Mesa. No macOS SDK build/link/signing,
FP16 execution or real A740 shader result is claimed. No kernel C changes.
No Mesa, NativeMetal, metal2vulkan, GPL code, Apple binary, firmware or shader
blob is added to the repository.

## Deployment gate and exact device test plan

No new boot-arg or tool option. A rebuilt ICD uses the fix only after the
existing explicit offline root-staging step; no root image was edited here.
Keep the existing ABI-2 kernel, `fuxigpu=1 fuxigpuabi=1` opt-in, root-only
NativeAdreno service and client allow/deny policy. Do not enable CoreAnimation
clients as part of this compiler test.

For a later authorized device session, first build/sign with the existing Mac
recipe and retain its checksums. Against a separately mounted test root, stage
the rebuilt ICD, existing no-submit probe and rebuilt smoke using:

```sh
python3 tools/stage-fuxi-vulkan.py "$IOS_ROOT" \
  "$OUT/libvulkan_freedreno.dylib" "$OUT/fuxi-vk-probe" \
  --smoke "$OUT/fuxi-vk-smoke" --conf "$OUT/vk-smoke.conf"
```

The stage config must contain `4\n`. Preserve root:wheel ownership, executable
mode 0755 and config mode 0644. Use the already documented temporary test-boot
setup in [Metal M3](metal.md), with
`sweetexecswap=peakpowermanagerd:/usr/local/bin/fuxi-vk-smoke` and the existing
`sweetnull=backboardd,SpringBoard,fuxi-vk-smoke,peakpowermanagerd` log policy.
For a no-submit pass first, substitute `fuxi-vk-probe` in the exec swap and
matching log policy; expect `FUXI VK: destroy-device PASS; no work submitted`
then `FUXI VK: done PASS`. Restore the smoke swap for the submission pass.

Expected kernel capability line:

```text
FUXI GPU ABI: NativeAdrenoGpuPublished v2 HostImport 16K, open 0x4147, root only, watchdog 100 ms
```

Expected smoke lines, with other timing/submission lines allowed between them:

```text
FUXI VK: smoke max-stage 4 TU_DEBUG=nocb
FUXI VK: stage 1 fill PASS verified readback
FUXI VK: verify compute words 16384 PASS
FUXI VK: stage 2 compute PASS verified readback
FUXI VK: verify triangle inside/outside 6 red 8192 black 57344 PASS
FUXI VK: stage 3 triangle PASS verified readback
FUXI VK: destroy-device/instance PASS; reopening connection in same process
FUXI VK: stage 4 reuse-reopen PASS verified 16 recreations + device/instance reopen + fill/compute
FUXI VK: done PASS
```

Accepted submits must also produce `FUXI KMD: submit IBs ... fence ...`.
Any timeout, fault/watchdog, device loss, wrong word/pixel, missing completion or
panic is failure. Compare against a T1 ICD on the same kernel/root configuration
and repeat once with the new ICD. These existing smoke shaders test driver
regressions; they do **not** exercise the newly fixed rounding boundaries.

Focused A740 qualification additionally requires a runtime conversion shader
fed through a host-filled SSBO, so NIR cannot constant-fold the inputs. For each
row, store `floatBitsToUint(float(input_value))` into a separate output SSBO,
wait with the existing bounded fence policy and compare every output word:

| Input uint | Default/explicit RTNE F32 bits | Explicit RTZ F32 bits |
| --- | --- | --- |
| `0xffffffff` | `0x4f800000` | `0x4f7fffff` |
| `0xffffff80` | `0x4f800000` | `0x4f7fffff` |
| `0x01000001` | `0x4b800000` | `0x4b800000` |
| `0x01000003` | `0x4b800002` | `0x4b800001` |
| `0x7fffffff` | `0x4f000000` | `0x4effffff` |
| `0x00ffffff` | `0x4b7fffff` | `0x4b7fffff` |

Check that the SPIR-V conversion has no rounding execution mode in the default
case; request explicit RTNE/RTZ through SPIR-V float controls only when the
device advertises support. Repeat scalar and vector conversion forms. Use
dynamic negative/unaligned SSBO byte-offset cases from the host regression to
confirm the existing whole-offset shift remains correct. A focused Vulkan
runner and FP16 capability/output checks are still future device-test work;
no existing executable emits a rounding-specific PASS line, and the smoke's
`done PASS` must not be presented as that evidence.
