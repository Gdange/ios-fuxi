# Turnip T1: UniXNU nine-patch baseline

P-MESA appends the HackiOS rounding fix to this baseline; see
[the patch audit and verification](mesa-port.md). The pins below are unchanged.

2026-10-23. Source rebase and Linux host validation only. Mesa stays pinned at
`85fda3b36405bcaea8bffc64aa4f2651dc90770d`; UniXNU moves to
`fb6aad6ab1f50199a4d99d2da1d0ca403b067633`. This is the current Turnip recipe,
superseding the historical M2/M3 patch lists in [metal.md](metal.md) and the
five-patch baseline in [metal-hostimport.md](metal-hostimport.md).

`tools/ci/build.sh mesa()` downloads exactly nine text patches from the pinned
commit's `patches/mesa/` directory. It does not fetch a UniXNU tree. The recipe
verifies each SHA256 and applies this insertion-ordered list before our overlays:

| Order / upstream patch | SHA256 |
| --- | --- |
| 1. `freedreno-tools-nir.patch` | `77e966b6e9a725892d10889a4d8deeefb03a32ae3a007904868132e56350191f` |
| 2. `ir3-darwin-build-id.patch` | `be3ca7840a16cb7696d8d7e67195c8d097cf0e38a3b5011edc674a5aed9c6975` |
| 3. `ir3-portable-asm-roundtrip.patch` | `bfc5552bc2bd6e1668d75773cce7bf62fd19065e6281bf7da49f9ddfc2e79339` |
| 4. `offline-adreno-compiler-probe.patch` | `b7d7a113beb5ef99edea24858d906194d4b4755c987520545ad2f7f3136bb4fb` |
| 5. `turnip-darwin-kmd.patch` | `f3bc6d470f8e0bde1b1d0d370175ec105ee45fe50075e43279581e61b1455c2b` |
| 6. `turnip-graphics-cache-feedback.patch` | `07f397e38d9acfb0085db4c19567c84fd8a3bcd63d6fd05192857abfbde5aed2` |
| 7. `turnip-empty-scissor.patch` | `dec657626dcd8e8d3f1447e44574f31e4afb8c90b6ebf7d589ba10ee0f8b6461` |
| 8. `turnip-builtin-shader-pack.patch` | `3c606dc99643c3ca3e4736c5783348cd85cbff3fbc55161e53226ab459f34473` |
| 9. `turnip-host-import-images.patch` | `8bc141db41848a59b16493b4ca8a359e0f18f1cad5f14147ff18d15f79c0c3c4` |

The first four patch hashes are unchanged. Only the KMD patch changes among
the original five. After all nine, apply `mesa-fuxi-smoke.patch`, then
`mesa-fuxi-hostimport.patch`. Mesa and all upstream patches remain external;
only our retained overlay diffs are tracked here.

## Hunk decisions

Every hunk of the previous five local patches is accounted for below. Rebased
line numbers and surrounding context change; the retained behavior does not.

| Previous patch / hunk | Decision and reason |
| --- | --- |
| `ios16`: `tu_physical_device::has_host_import` declaration | Drop. Upstream patch 8 now declares the field, while patch 5 sets it from Query. This was the local patch's only hunk, so remove the file and recipe entry. It never set the deployment target; the recipe still supplies iOS 16.0 compile/link flags. |
| `smoke`: `dw_wait_fence_locked_free` wait-slice cap | Keep in upstream's new `dw_wait_fence_raw` helper. Cap the final 1 ms slice to the remaining deadline, including a zero-duration poll. This also covers nonblocking deferred-retirement polls. |
| `smoke`: `darwin_queue_wait_fence` deadline addition | Keep. Saturate relative-to-absolute addition instead of wrapping large finite timeouts into immediate polls. |
| `smoke`: `vk_darwin_sync_wait_many` wait-any result | Keep. Return any result other than timeout, including device loss, instead of swallowing errors or polling the same fence twice. |
| `smoke`: `darwin_submit_chunk` accepted-submit log | Keep. The M3 smoke/triage oracle uses `FUXI KMD: submit IBs ... fence ...` to distinguish accepted work from rejection. |
| `smoke`: `darwin_queue_submit` atomic IB limit | Keep. Fuxi resets CP context on every submit. Reject more than 16 top-level IBs before the first doorbell; splitting could partially execute with lost state. |
| `hostimport`: `tu_get_properties` alignment | Keep. Advertise `minImportedHostPointerAlignment = 16384`, matching whole XNU task pages, rather than upstream's 4096-byte GPU leaf alignment. |
| `hostimport`: `tu_AllocateMemory` host branch | Keep. Require actual capability, a non-null pointer, nonzero allocation, type 0, host-allocation handle type, no simultaneous FD import and 16 KiB address/length alignment. These checks are absent upstream. |
| `hostimport`: `tu_GetMemoryHostPointerPropertiesEXT` | Keep. Clear memory-type output on failure, reject null/misaligned/foreign pointers or missing capability, and expose only coherent type 0. Retain the comment explaining our kernel's submit/retirement PoC cache handoff and external producer synchronization. |
| `hostimport`: KMD file header ABI version | Keep. Describe the ABI actually required by this build: version 2. |
| `hostimport`: KMD ABI static assertions | Keep both assertions. Compile against NativeAdreno ABI 2 and its 16384-byte host page constant; Query's upstream runtime version comparison remains in place. |
| `hostimport`: `darwin_bo_init_host` validation | Keep before upstream's retirement collection. Clear the output and reject missing capability, null/zero range, over-limit size, non-16-KiB range and pointer overflow before IOVA allocation/import. Preserve upstream collection and map-failure cleanup. |
| `hostimport`: external image-format query | Keep. Advertise imports only with actual kernel capability, linear tiling and a 2D image; no export or dedicated-only claim is added. |
| `hostimport`: external buffer query | Keep. Advertise host-allocation importability only when Query reports HostImport. |
| `hostimage`: dedicated-allocation requirements | Drop local copy. Upstream patch 9 provides the same shared-host-import offset/dedicated-allocation behavior. |
| `empty-scissor`: `tu6_scissor_size` | Drop local copy. Upstream patch 7 already reserves zero words when the pipeline has no scissors. |
| `empty-scissor`: `tu6_emit_scissor` | Drop local copy. Upstream patch 7 already suppresses invalid zero-register packets. |

The old local patches did not implement BO import/free ownership themselves.
The new KMD's bounded deferred queue, captured fences, collection on allocation
and fence observation, busy-free retry, map-failure cleanup, and failed-release
IOVA retention are kept as upstream code. No duplicate local implementation is
layered over them. Imported BOs still use the original CPU pointer and no
CPU-map token; kernel ABI 2 provides cache handoff and quarantine behavior.

## Build profile and diagnostics

The recipe now explicitly passes `-Dbuildtype=release -Db_ndebug=true`, matching
UniXNU's release profile. Compile and link flags remain
`-target arm64-apple-ios16.0`, the iPhoneOS SDK and
`-Werror=unguarded-availability-new`; `platforms=macos` still only selects Mesa's
Darwin platform sources. NativeAdrenoABI comes from our canonical header.

Both `tu-builtin-source-id` and `tu-builtin-pack-header` are explicitly empty,
including during reconfigure, so the compiler path remains active and an old
build directory cannot silently retain an embedded shader pack. No pack or
shader binary is fetched/generated for T1. Mesa disk shader cache stays disabled;
the graphics-cache-feedback patch fixes application pipeline-cache hit reporting
independently of that disk-cache option.

The smoke uses runtime `TU_DEBUG=nocb`; upstream's flag handling remains active
in release. `mesa_logi` and `mesa_loge`, including our accepted-submit line and
driver error messages, remain compiled in. Release disables C assertions and
does not request debug information; no current smoke/probe oracle requires
assertions or debug symbols. The exported ICD entrypoint check, signing and
Mach-O audits remain in the recipe. Linux harnesses deliberately retain their
own assertions and ASan/UBSan instrumentation.

## Verification

Use an external clean Mesa worktree and the nine downloaded patches:

```sh
python3 tools/build-fuxi-turnip.py --mesa "$MESA_SRC" \
  --patches "$PATCH_DIR" --build "$MESA_BUILD" --verify-inputs-only
python3 tools/test-fuxi-turnip-submit.py \
  --upstream-patch "$PATCH_DIR/turnip-darwin-kmd.patch"
python3 tools/test-fuxi-turnip-hostimport.py \
  --mesa "$MESA_SRC" --patches "$PATCH_DIR"
python3 tools/test-fuxi-hostimport.py
python3 tools/test-fuxi-metal.py
python3 tools/test-fuxi-metal2h.py
python3 tools/test-fuxi-adreno-mapping.py
```

Linux results:

* All nine fetched files match both their SHA256 pins and the read-only UniXNU
  reference at the selected commit.
* The full nine-plus-two sequence passes sequential `git apply --check` and
  `git apply` on clean pinned Mesa.
* `--verify-inputs-only` accepts the fully patched tree and a clean prefix
  (applying the complete stack). It rejects both a tampered upstream patch hash
  and an unexpected edit in a patched source file.
* The Turnip submit harness passes finite/poll/overflow waits, retirement-error
  propagation, wait-any loss propagation, 17-IB rejection before submission,
  16-IB submission and empty-submit signaling under ASan/UBSan.
* The updated Turnip host-import harness compiles actual query, host allocation,
  import/map and deferred-retirement functions. ASan/UBSan passes ABI-2/16-KiB
  checks, import/map unwind, original CPU pointer, BO refcount, nonblocking
  deferred free, captured-fence lifetime, busy retry, permanent free failure and
  failed-map cleanup with IOVA retention.
* Kernel host-import, shared ABI/backend/cache-handoff, managed-BO lifetime and
  mapping regression scripts listed above all pass under ASan/UBSan.

The native macOS/iPhoneOS build, link, signing and binary audit were **not run**.
No device was accessed, no CI/workflow was run and nothing was pushed. This
source validation does not qualify A740 execution, real IOSurface/cache behavior
or release performance. The disposable Mesa worktree is removed after checking;
the original Mesa clone and read-only UniXNU reference are unchanged.
