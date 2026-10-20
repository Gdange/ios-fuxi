# Metal lane, milestone 2g: full-width cache sync and bounded NC access

2026-10-20, based on metal2e and Wi-Fi M6. No device execution in this lane.

**Observed metal2f:** exec-swap as peakpowermanagerd reached open, query and
DeviceInfo successfully, then panicked in `phystokv_range` with PA
`0xc4f40000`, below the managed RAM base `0x8c0000000`. No backtrace was
available. IOMFB_FDR_Loader previously passed `--launchd_boot`, which made the
old test exit with usage before opening the GPU.

**Root cause identified in the BO allocation cache path:**
`FuxiAdrenoUserClient::allocate` requests `kIOMapWriteCombineCache` from
`IOBufferMemoryDescriptor`. Changing wired pages from copyback to NC runs
`pmap_sync_wimg` -> `pmap_sync_page_attributes_phys` -> `flush_dcache` ->
`phystokv_range`. Upstream `osfmk/arm/pmap/pmap.c:6884` shifts the 32-bit
`ppnum_t` before casting: `(vm_offset_t)(pp << PAGE_SHIFT)`. The metal2f
kernel actually emits `lsl w0, w0, #14`. For example, managed PA
`0x8c4f40000` becomes the exact reported illegal PA `0xc4f40000`. The log
alone does not identify which original high RAM page was used; it does not
establish that the reported low address was a real carve-out allocation.

Patch 0101 widens before shifting. The final native kernel emits
`ubfiz x19, x0, #14, #32`, preserving all 32 PFN bits. For reserved pages it
looks up the entire page in the published GPU kernel_map NC windows and
flushes through that VA, with `phys=FALSE`. Unknown non-RAM/non-window PAs
are rejected before any physmap helper. Managed BO pages keep their full PA.

**Access audit:** the ABI backend, present mapper, user client and shared
GPU helpers contain no calls to physical zero/copy/read/write or physmap
conversion functions. The user client accesses BOs through their descriptor
KVAs. ABI/present `pmap_find_phys` calls translate wired source KVAs to PAs;
`pmap_enter_options_addr` installs the persistent NC mappings and
`pmap_remove` unwinds failed installation. These are not carve-out data
accesses through physmap. Table walks, zeroing, ring storage, firmware and
handoff reads now validate their full byte range through `G_PTR_RANGE` and
`fuxi_gpu_nc_pointer`, with overflow and window-edge checks. MMIO validates
the actual 4/8-byte width. There is no physmap fallback for NC windows.

The test ignores host arguments whenever argv[0]'s basename differs from
`fuxi-gpu-abi-test`. Only direct self invocation accepts `--watchdog`; host
arguments can never select the hanging IB. Direct invalid arguments still
return usage status 2.

**Validation:** ASan/UBSan tests execute the actual NC lookup, scratch zero,
patched pmap helper and argument parser. The original pmap helper fails the
strict physmap trap with the reported low PA. All mapped apertures, unknown
PAs, unpublished mappings, zero lengths, crossing ranges and overflow are
covered. Existing ABI, execswap, mapper, present, shared probe, memory, WLAN
M6 and final native SCM checks pass. Artifact checks verify firmware bytes,
geometry, recovery table, service symbols, signed iOS arm64 tool and hashes.

The Mac tree was synchronized to the current worktree, including replacing
0099 with Wi-Fi M6, before building. The kernel and ad-hoc signed test were
built on the Mac; the loader was packed with the existing host toolchain.
The metal2g pack lane requires absolute external firmware/tool paths and
copies metal2f's ADT and command line exactly: peakpowermanagerd exec-swap,
bluetoothd/fairplayd.H2 blocked, and no `fuxiwlan` (Wi-Fi off).

Artifacts: `xnu/out-fuxi-metal2g/`, `shim/out/fuxi-metal2g-boot.img`, and
`tools/out/fuxi-gpu-abi-test`. SHA256s:

- Kernel: `c476cba84af5e795d93e165c029df669b4127bd22d9de368b22f2a1c7942e217`
- Unstripped: `dc80e71de18e10753eff3598fdc3ca248dbf13bfd0491dcc68f50e43b7fab7a3`
- Boot: `aceb71bdf42b0741db826d636e6bbbbbe348181ff9497d77a3dcc91e76110556`
- Signed test: `1114d442a5918ca05794acec6b0a4472ec92f62a5a1357a7886840682f252cb7`

The rebuilt test still needs offline root restaging. Expected next device
lines (not observed here), after the existing open/query/device-info lines:

```text
SWEET EXECSWAP: peakpowermanagerd -> /usr/local/bin/fuxi-gpu-abi-test pid N
FUXI ABI: alloc pid N rc 0x00000000
FUXI ABI: map pid N rc 0x00000000
FUXI ABI: submit pid N rc 0x00000000
FUXI ABI: wait pid N rc 0x00000000
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 1/2
```

Then close/reopen, another successful alloc/map/submit/wait and result
`rounds 2/2`, followed by close. A later respawn should report that the test
already passed 2/2 rounds this boot. No physmap panic is expected.

---

# Metal lane, milestone 2c: page-granular UCHE readback

2026-10-20, branch `codex-metal2c`, based on `2800fd5` (metal2b plus Wi-Fi
M5). Native build, all eleven host test scripts, shim host renderer, native
SCM audit and artifact checks pass. No device execution is claimed.

**Observed (supplied metal2b boot log):** GMEM MAX reads `0x12ff000` after
writing inclusive end `0x12fffff`. The other snapshot values match. This
explains the named init failure before the NC/aperture/table setup.

The canonical backend and regenerated patch 0098 now compare UCHE address
low words (WRITE_THRU, TRAP, GMEM MIN/MAX) through mask `0xfffff000` on both
operands. Address high words and NC mode retain exact comparisons; HBB
retains its exact extracted-field check and SMMU OAS its minimum check.
Raw snapshots and failure messages still report the unmasked values.
No programmed register value or userspace ABI changes.

**GMEM:** reconstruct the inclusive end before computing size:
`(0x12ff000 | 0xfff) - 0x1000000 + 1 = 0x300000` (3 MiB).
[Linux v6.16 A740 catalog](https://github.com/torvalds/linux/blob/v6.16/drivers/gpu/drm/msm/adreno/a6xx_catalog.c#L1370-L1376)
lists chip `0x43050a01` with `3 * SZ_1M`;
[Linux UCHE setup](https://github.com/torvalds/linux/blob/v6.16/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)
programs GMEM MAX as MIN + size - 1, with a 16 MiB base for A740.
DeviceInfo already advertises this base and 3 MiB size; it remains unchanged.
Query reports ABI/address/allocation limits, not GMEM size. Host tests now
compile and execute the actual Query/DeviceInfo branches and check these
values against the observed range, avoiding the 4095-byte underestimate.

**Follow-on audit:** `g3_gw`/`g3_write`/`probe_safe_wr32` and `fg_access`
report MMIO access faults, not exact value readback. Thus the eight NC and
BR/BV/default aperture writes have no similar equality assumption to fix.
They still fail closed with a named write failure. `g_zero`, root/L2/L3
construction and fence initialization use ordinary reserved NC memory
stores, followed by `dsb sy`, rather than hardware address registers.
Later TTBR switches do compare the 4 KiB-aligned root PA plus ASID exactly;
there is no inclusive-end offset to reconstruct or discard there. The
existing SMMU setup uses the same aligned addressing. Explicit BR/BV aperture
selection, ABI table use and userspace submissions still need device testing.

Host tests use `0x12ff000` by default, accept both rounded and inclusive MAX
models, exercise each of the 12 ignored offset bits in all four low words,
and reject page-address/high-word/HBB/NC corruption. All prior read faults,
eight write failures, publication/table refusal, one-time snapshot bounds,
PM4, mapping, watchdog, execswap, governor, root-tool and Wi-Fi M5 tests pass.

**Build:** only `ci-lane-metal2` was used. Its prior 32 native input hashes
were verified before synchronization. All tracked repository files were
synchronized; the native source overlay includes the current 0099 M5 source
and this backend change. The final 32-input manifest differs from metal2b
only in those two files; all hashes pass on the Mac. The packed command line
and ADT are byte-identical to metal2b, including the absent `fuxiwlan` gate
(Wi-Fi off). The existing signed raw tool and pinned loader inputs are
unchanged. Kernel linking has the existing toolchain/linker warnings; loader
packing completes without warnings.

Repack and verify using the same private build environment:

```sh
bash tools/build-fuxi-metal2-images.sh "$METAL2B_ARTIFACTS" metal2c
python3 tools/test-fuxi-metal-artifacts.py --lane metal2c \
  --base "$METAL2B_ARTIFACTS" --out xnu/out-fuxi-metal2c
```

Outputs, manifests and logs: `xnu/out-fuxi-metal2c/`.
Image: `shim/out/fuxi-metal2c-boot.img`, SHA256
`eca58669dd03844dbeee53d6d67e8ae54b5544c8e95a359d489b3f317129cc91`.
The complete artifact manifest is `xnu/out-fuxi-metal2c/SHA256SUMS`.

**Expected next device evidence, not an observed result:** retain the eleven
raw init lines, including `UCHE_GMEM_RANGE_MAX_LO [0xe0d]=0x12ff000`, with no
MAX mismatch. If subsequent setup succeeds:

```text
FUXI GPU ABI: NativeAdrenoGpuPublished v1
FUXI ABI: open pid N rc 0x00000000
FUXI ABI: query pid N rc 0x00000000
FUXI ABI: device-info pid N rc 0x00000000
FUXI ABI: submit pid N rc 0x00000000
FUXI ABI: wait pid N rc 0x00000000
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 1/2
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 2/2
```

Allocation/mapping/cleanup lines occur between these markers as in 2a. The
second round follows close/reopen; later diagnostic opens are refused after
two successful rounds. A named `init write ... failed`, service withheld,
nonzero rc, bad pixels, watchdog or missing round remains failure evidence.

---

# Metal lane, milestone 2b: identify ABI init failures

2026-10-20, branch `codex-metal2b`. Built in the existing `ci-lane-metal2`
native tree. All ten host test scripts, the shim host renderer and artifact
checks pass. This lane did not access a phone or boot this image.

**Observed:** the supplied `ios-0098-metal2a/boot.log` reaches GPU stage 16
PASS (including zap and clear/copy), then prints ready and
`FUXI GPU ABI: initialization failed, service withheld`. Execswap runs the
raw test as pid 84 and again as pid 241. There are no ABI open/query lines.
The old compound check does not record its failing operand. The root cause
of that refusal remains unresolved; no register-value correction is claimed.

**Source audit:** `g3_hw_init()` runs before CP startup in both ABI and normal
boots. It writes the following values using dword-indexed GPU offsets and
separate 32-bit low/high stores. `g3_sr()` instead takes a byte offset.

| Register | Offset(s) | Programmed / required value |
| --- | --- | --- |
| UCHE_WRITE_THRU_BASE | `0xe07/0xe08` | `0xfffff000 / 0x1ffff` |
| UCHE_TRAP_BASE | `0xe09/0xe0a` | `0xfffff000 / 0x1ffff` |
| UCHE_GMEM_RANGE_MIN | `0xe0b/0xe0c` | `0x1000000 / 0` |
| UCHE_GMEM_RANGE_MAX | `0xe0d/0xe0e` | `0x12fffff / 0` (3 MiB, inclusive end) |
| UCHE_MODE_CNTL | `0xe01` | `0x600000`; bits 22:21 = 3, HBB = 13+3 = 16 |
| RBBM_NC_MODE_CNTL | `0x534` | `1` (macrotile mode; HBB is in UCHE) |
| SMMU_ID2 | byte `0x28` | earlier observed `0x5511`, OAS code 1 = 36 bits |

The supplied Linux `a6xx_gpu.c` and `a6xx.xml` agree. Qualcomm's
[adreno_gen7.c](https://github.com/qualcomm-linux/kgsl/blob/2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f/adreno_gen7.c),
[register header](https://github.com/qualcomm-linux/kgsl/blob/2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f/gen7_reg.h)
and [Gen7.2 catalog](https://github.com/qualcomm-linux/kgsl/blob/2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f/adreno-gpulist.h)
also agree on the offsets, trap/write-through address, 16 MiB GMEM alignment,
3 MiB size and HBB16. Reference copies remain in ignored `out-fuxi-metal2b/refs`.
The old ABI verified WRITE_THRU, despite the DeviceInfo text discussing TRAP;
both are programmed identically. The new snapshot/checks include both pairs.
No source evidence establishes a read-as-zero mask for these registers, so
2b retains exact comparisons rather than guessing a mask or rewriting state.

`gpu_probe(16)` reaches `g5_run()`, which exits secure mode and explicitly
runs `gpu_m4(14)`. That clear/copy path already writes RB/TPL1/SP/GRAS NC
values `0x816/6/0x26/0x60`, even with the ABI enabled. No later software reset
or write to the checked UCHE registers was found between `g3_hw_init()` and
`fg_abi_init()`. `g3_hold_gx()` retains GMU OOB until reset, with no idle
collapse. Thus missing stage14 setup or a deliberate GX power release is
not supported by the code/observed stage results. This does not establish
what each UCHE register actually read back on the failing boot.

The ABI-only writes use the same GPU MMIO window and fault-contained helper
as stage14. The helper executes `dsb sy` after each access. Host aperture
`0xa00` uses PIPE bits 13:12, BR=1/BV=2, then restores zero, matching Qualcomm's
`gen7_host_aperture_set()` / GRAS setup. CP packet protection does not prohibit
these CPU AHB writes. Init finishes before ready is released to display
callers. RB/TPL1/SP were already writable during the observed clear/copy;
the explicit BR/BV GRAS selections still need device qualification.

The diagnostic change is in the canonical backend and regenerated patch
0098; patch 0100 stays unchanged. On the first init attempt, it takes one
snapshot of all ten GPU registers plus SMMU_ID2 before any comparison. It
prints eleven `FUXI GPU ABI: init regs ...` lines, then at most one named
`init check <name> got 0x... want 0x...` or
`init write <reg> [0x...]=0x... failed` line. The existing service-withheld
line follows failure. There is no retry or periodic dump. Access failures
have a separate `register_access` check; raw snapshot values following a
fault must not be treated as valid hardware facts. The first mismatch still
stops setup, and publication/table construction stay behind all checks.

Host coverage injects every register mismatch, GPU and SMMU read failures,
and each of the eight write failures into the actual C init function. It
checks the named reason, one-time dump cap, and untouched tables/unpublished
service after refusal. Existing PM4, mapping, async present, watchdog,
quarantine, execswap, governor, root-tool and SCM regressions pass. Artifact
checks verify the linked native recovery/SCM code, exact metal2a ADT and
cmdline, boot geometry, existing signed raw tool and all pinned firmware.
All 32 recorded native inputs match metal2a except the intended backend edit.
These are host tests; raw IB/readback and service publication are unproven.

Repack using the normal private loader/toolchain environment, after copying
the native kernels into `xnu/out-fuxi-metal2b`:

```sh
bash tools/build-fuxi-metal2-images.sh "$METAL2A_ARTIFACTS" metal2b
python3 tools/test-fuxi-metal-artifacts.py --lane metal2b \
  --base "$METAL2A_ARTIFACTS" --out xnu/out-fuxi-metal2b
```

Outputs and build/test logs: `xnu/out-fuxi-metal2b/`. Image:
`shim/out/fuxi-metal2b-boot.img`, SHA256
`0d0cb22d70c78380baab5fef91efb7a84f31eb5c2971a0943ad915b9be32f172`.
`SHA256SUMS` records the kernel, ADT, payload, image, loader and raw tool.

Expected next device output: eleven named raw-register lines, followed by
the precise failing check/write and service withheld if the failure repeats.
If all checks/writes pass, expect `FUXI GPU ABI: NativeAdrenoGpuPublished v1`
and then the existing bounded `FUXI ABI:` open/query/submit/wait/result trace.
Do not treat that success path as an observed 2b result.

---

# Metal lane, milestone 2a: route the existing raw test and observe it

2026-10-20, branch `codex-metal2`, base `b978770` (0098i integration plus
Wi-Fi M3/M4). Native build and all ten host test scripts pass. **This image has
not been booted by this lane.** The coordinator reports that 0098i reached
GPU stage 16 PASS, valid PERF temperatures and iOS userland, but never exec'd
the staged raw test. Stage 16 is not a userspace ABI test result.

## Patch 0100 and repeat handling

Apply `0100-sweet-execswap-gpu-abi-trace.patch` after 0099 (and 0095/0098).
0098 is unchanged; `sync-fuxi-metal2.py` maintains the canonical helper and
client delta. `sync-fuxi-adreno-shared.py --check` checks both generations.
The only Mac tree used was `ci-lane-metal2`, created with the requested APFS
clone of `ci-integ98`; 0099 and 0100 applied without fuzz. All 32 affected
native source files match a clean 0098i + 0099 + 0100 result, including the
m7c/m7d governor and default-off Wi-Fi sources. Logs/manifests are under
`xnu/out-fuxi-metal2a`. No phone/root-image access, push or binary rebuild.

`sweetexecswap=<name>:<absolute path>` substitutes the original executable
pathname after copyin and before namei, permissions, activation and signing.
It accepts one basename (1..31 bytes) and one non-root absolute target within
a 192-byte buffer; rejects truncation, lists, whitespace and extra colons.
Initialization and the eight-line boot log quota use a mutex. Without the
boot argument it changes no executable path. argv/env keep their original
userspace segment. A private unused image flag (0x10000) carries substitution
through activation, suppressing the late sweetnoexec check for this exec.
If the original name is also blocked, the routing line gains
` (wins over sweetnoexec)`. Other sweetnoexec entries retain their behavior.
No fallback to the original daemon occurs if the replacement fails.

The current signed test stays byte-identical (SHA below). For exactly
`bluetoothd:/usr/local/bin/fuxi-gpu-abi-test`, opens of type 0x4147 by the test
process (its actual exec basename, as exposed through truncated p_comm) are
marked diagnostic. After successful WaitFence, the kernel reads that client's
64 KiB BO at IOVA 0x20000000: word at +0x1000 must be 0xa740cafe, and every one
of 256 BGRA pixels at +0x2000 must be 0xff336699. It counts at most one verified
round per connection. Two successful rounds allow the existing binary's
close/reopen sequence, then refuse later diagnostic opens with kIOReturnBusy.
Other clients/types and boots without that exact swap configuration have no
round quota. This is a diagnostic signature check, not proof that stdout said
PASS or that a Metal/Vulkan client works. Later respawns exit quickly with the
existing binary's open-failure exit status; the kernel explains the refusal.

A single boot-wide budget of 64 `FUXI ABI:` lines covers open, query, device
info, alloc, GPU/CPU map, submit, wait, result, cleanup and refusal, including
pid and IOReturn. It works with bluetoothd's inherited stdout even if /dev/null.
The original independent 100 ms GPU watchdog and quarantine remain intact.

## launchd finding and future root edit

Inspected only the supplied `launchd.bin` (SHA256
`12370e5ee35bf28ae479b6c4b4455f4f4693ab803e37c5eb3032537c18cac61c`), using
strings, embedded configuration parsing and arm64 disassembly. Addresses below
are unslid and specific to this binary; Apple analysis outputs remain ignored.

* Embedded `Bootstrap.Paths` enumerates `/System/Library/LaunchDaemons`,
  LaunchAngels, AppleInternal, Nano and DeveloperMode directories. It omits
  `/Library/LaunchDaemons`. `SystemRoots` likewise omits `/Library`.
* At 0x100019c08..0x100019c40, `LaunchDaemons` is accepted as an XPC dictionary;
  0x100019f68..0x100019fc8 installs its entries into the cache. Merely adding
  a key is not itself a bootstrap request.
* The directory loader at 0x100019638..0x1000196f8 checks the configured path
  arrays and enumerates cached entries under the requested directory. Its
  callback at 0x100019e0c..0x100019e64 tests the entry's prefix before creating
  the load object. The prefix helper at 0x100016210 uses strlen/strncmp.
  Thus the old `/Library/LaunchDaemons/...` key is outside the default boot
  directory selection. This explains the observed absence of any exec attempt;
  confirmation of the corrected job still requires the next root edit/boot.
* ProgramArguments-only jobs are accepted (including embedded boot jobs).
  The supplied job has no `_LimitLoadTo*` restrictions. There is no evidence
  here that missing POSIXSpawnType or a mandatory Program key caused this skip.

`stage-fuxi-gpu-abi-test.py` now writes both the file and cache key at
`/System/Library/LaunchDaemons/org.fuxi.gpu-abi-test.plist`, explicitly sets
Program as well as ProgramArguments, and migrates/removes the legacy key/file.
It preserves unrelated jobs and validates the input before migration.
**This fix is committed for the next offline root edit; it was not staged.**

## Build, artifacts and expected device evidence

`build-fuxi-metal2-images.sh BASE_0098I_ARTIFACT_DIR` uses the same mkpayload
and loader/boot-v4 packing as 0098i, with its exact external GMU/SQE/zap blobs.
The ADT is byte-identical. The 463-byte command line is precisely 0098i's line
with `bluetoothd,` removed from sweetnoexec and
` sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test` appended.
`fuxigpuabi=1` stays on; `fuxiwlan` is absent/off, with patch 0099 still linked.
The build requires the same private toolchain/firmware environment as 0098i.

All host scripts passed: adreno-cache, adreno-mapping, adreno, gpu-probe,
memory, metal, metal2, perf, scm (including final native instruction audit),
and metal-artifacts (including firmware, raw signed Mach-O, geometry, symbols,
recovery table, exact command line/ADT and hashes). New ASan/UBSan tests compile
the actual parser/exec helper and result/mount/trace methods, checking parse
once, default-off, overlap logging, eight/64-line limits, failed pixels, two
rounds, repeat refusal, unrelated clients and stage migration/removal.
These are host checks, not device qualification.

Expected success sequence (N is the test PID, unchanged across its reopen):

```text
SWEET EXECSWAP: bluetoothd -> /usr/local/bin/fuxi-gpu-abi-test pid N
exec fuxi-gpu-abi-test
FUXI ABI: open pid N rc 0x00000000
FUXI ABI: query pid N rc 0x00000000
FUXI ABI: device-info pid N rc 0x00000000
FUXI ABI: alloc pid N rc 0x00000000
FUXI ABI: map pid N rc 0x00000000
FUXI ABI: cpu-token pid N rc 0x00000000
FUXI ABI: cpu-map pid N rc 0x00000000
FUXI ABI: submit pid N rc 0x00000000
FUXI ABI: wait pid N rc 0x00000000
FUXI ABI: result pid N rc 0x00000000 mem 0xa740cafe bad-pixels 0 rounds 1/2
FUXI ABI: cpu-unmap pid N rc 0x00000000
FUXI ABI: free pid N rc 0x00000000
FUXI ABI: close pid N rc 0x00000000
```

Then open/alloc/map/cpu-token/cpu-map/submit/wait/result/cleanup repeat, with
`rounds 2/2` (no second query/device-info in the current binary). A later
bluetoothd respawn, with a new PID M, should log the routing and exec again,
then:

```text
FUXI ABI: open refused pid M rc 0xe00002d5 already passed 2/2 rounds this boot
FUXI ABI: open pid M rc 0xe00002d5
```

Routing stops logging after eight substitutions; ABI tracing stops after 64
lines. Any nonzero step rc, nonzero bad-pixels, missing second round, watchdog
or fault must be retained as failure evidence. Do not infer a PASS from exec
or service publication alone.

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal2a/kernel.development.fuxi` | `247d669f7aa563e7bd03435e4d0268a3e557728b54146a6ae5f23141ec2781a1` |
| `xnu/out-fuxi-metal2a/kernel.development.fuxi.unstripped` | `71c87cfe4170c4ae5cc7ce3b20d77146cec4d15087c0086c709bc5bec886764d` |
| `xnu/out-fuxi-metal2a/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-metal2a-payload.bin` | `f281445725dfcc30a36194c071418e0155e504432378847710b4ee4f4e383dfb` |
| `shim/out/fuxi-metal2a-boot.img` | `b8c441ee639f0b2d125398d6748296d38625bac22262a9ca05faa439f6942056` |
| `shim/out/fuxi-metal2a-loader.elf` | `8385bb46c6346fa31788b48751e38ba9c2bcbe8fd70979a3fa06ceac4b9f6e25` |
| `tools/out/fuxi-gpu-abi-test` (unchanged) | `fc04e8157061a21d6f713197625010006e28b77b56c01cddf1d38ffd5cd85ae1` |

The complete artifact manifest is `xnu/out-fuxi-metal2a/SHA256SUMS`.

---

# Metal lane, milestone 1: NativeAdreno userspace ABI

2026-10-20, branch `codex-metal1`, based on `fuxi-bringup` `8dae9f4`.
**Native build and offline verification pass. No device execution is claimed.**
No phone access, push, firmware redistribution or Apple binaries in git.
`AGENTS.md`, `NOTES.md`, `FUXI.md`, patch 0097 and `xnu/fuxi-perf` are unchanged.
The only Mac build tree used was `ci-lane-metal`, cloned from `ci-lane-gpu6`.
The 27 native source inputs match the clean 0097b + 0098 patch result;
`native-inputs.sha256` and `native-inputs-check.log` accompany the kernel.

## Implemented contract

Apply `0098-fuxi-native-adreno-abi.patch` after 0096; 0097 is unchanged.
Canonical sources are in `xnu/fuxi-adreno`. `sync-fuxi-metal.py` generates
0098, and `sync-fuxi-adreno-shared.py --check` checks the combined snapshots.
0096 remains independently applicable. The default-off driver behavior and
loader addresses remain unchanged when `fuxigpuabi` is absent or not 1.

Both `fuxigpu=1` and `fuxigpuabi=1` are required. The service is an ordinary
`IOService` subclass, **NativeAdrenoGpuPublished**, registry name
`NativeAdrenoGpu`, accepting open type **0x4147** (also type 0 for Turnip's
direct backend). Only an administrator/root connection is admitted. There is
one live connection, recycled on close/death; no sandbox tunnel or mailbox.
For the unmodified Turnip patch use:

```sh
export TU_DARWIN_SERVICE=NativeAdrenoGpuPublished
export TU_DARWIN_OPEN_TYPE=0x4147
export TU_DEBUG=nocb
```

All 16 direct selectors and fixed struct lengths match NativeAdrenoABI v1.
Query, GetDeviceInfo, BoAlloc/Free/Map, BoCpuMap/Unmap, Submit and WaitFence
are implemented. Metadata and queue selectors are implemented as well.
GetTimestamp, GetStatus and BoImport are unsupported. Query does not advertise
HostImport or cached coherence. Queue 0 exists at open; extra queue IDs share
the same serialized hardware queue.

Limits: 64 BOs, 256 MiB per BO and 256 MiB live BO allocation budget per connection,
16 IBs per submission, one hardware job in flight. Allocation tries contiguous
wired memory first, then scattered wired pages unless contiguity is required.
It honors alignment 12..24 and constrains physical addresses to 36 bits.
Backing is rounded/zeroed to 16 KiB, while GPU mappings cover the requested
4 KiB-multiple length. BoCpuMap reports the actual CPU view length.
`clientMemoryForType` returns a stable physical-range descriptor for its token.
Both kernel and user BO mappings use Normal NC/write-combine. The descriptor
retains the prepared backing through its last CPU mapping, including forgotten
unmaps. Correct users unmap memory, release the CPU token, then free the BO.
Unreleased CPU mappings can retain memory until connection/task teardown.

The client has a separate 4 KiB SMMU root in the final 4 MiB of reserved
scratch (offset 28 MiB). Static bounds and the host model keep it beyond
the retained zap metadata, cold handoff and all three firmware slots.
Its full aperture `[0x20000000, 0x80000000)` is available; overlapping mappings
are rejected. The original scanout and four present-source windows stay in
the present root. With the ABI enabled only, runtime SQE/ring/pointer/fence
IOVAs move below the client aperture (`0x10000000` onward), identically in
both roots. TTBR switching on the existing GPU bank happens only with an idle
ring; ASID invalidation/TLBSYNC precedes a new doorbell and backing release.
No GMU bank, SID ownership or present PTE is repurposed for BO leaves.

A shared mutex serializes ring/control/PTE operations with present calls;
BO allocation and scheduler waits do not hold it. While a client owns the
ring, presentation uses its existing CPU copy. On successful client retirement
the present root is restored and GPU presents become eligible again.
Submission wraps client IBs in CP_INDIRECT_BUFFER with BR sync, context reset,
PERFCTR SRAM-init status wait, GPU cache flush/invalidate, BOTH threads and
IFPC off. Completion writes BV timestamp, waits on BR, writes the BR timestamp
with the IRQ bit, restores BOTH/IFPC and waits for idle. Context reset is emitted
on every return from the present context, including the first client submit.
RBBM interrupt delivery remains masked; no GIC/IRQ completion is claimed.

WaitFence sleeps in 1 ms slices and returns `kIOReturnTimeout` for a user or
hardware deadline. Zero timeout observes once. An independent delayed callback
checks every 2 ms, even when no client calls WaitFence, and declares a pending
job lost at 100 ms. Completion observed after the deadline wins if the ring is
already retired. Scheduling can delay observation beyond these intervals.
A client fault/hang permanently disables GPU submission for this boot, drains
GBIF, holds reset and faults SID0/1, allowing CPU presentation to continue.
Failed drain quarantines the wired BO references until reset. There is no
attempt to recover the CP or reset it beneath active UI work.

## Chip facts and reference provenance

References read: UniXNU `7b52e7968c76453cdc18b5aa174fc368d5293b91`,
UniXNU-sheng `8a0c9ddab92038a50abc07fffd4882ec4e301f45`, including
`NativeAdrenoABI.h`, `NativeAdrenoRing.cpp` and `GpuPowerProbe.cpp`.
MIT ABI/framing notices are retained in `LICENSE.NativeAdreno`; README credits
UniXNU. The sheng board code is BSD-2-Clause-Patent; it is a reference here.

| Field | Value and basis |
| --- | --- |
| chip_id, GMEM | `0x43050a01` / C510v2, 3 MiB, catalog entry using `a740_sqe.fw`, `gmu_gen70200.bin`, `a740_zap.mdt`; matches our previously qualified A740 firmware family. This milestone performs no fresh phone chip-ID read. |
| GMEM base / trap | `0x1000000` / `0x1fffffffff000`, exactly the UCHE values our existing bring-up programs; ABI publication checks their live readback and GMEM end `0x12fffff`. |
| HBB / swizzle / macrotile / UBWC | `16 / 6 / 1 / 4`: SM8550 platform configuration, encoder 4.0 (decoder is 4.3). Existing NC values are `0x816 / 6 / 0x26 / 0x60`; ABI init also initializes BR/BV GRAS banks. HBB UCHE bits are checked. |
| VA | start `0x20000000`, size `0x60000000`, ClientIova flag; driver policy, independently reserved in the client root. |
| Query widths | input 31, **output 36**. Recorded Fuxi SMMU ID2 is `0x5511`: OAS code 1 means 36 bits. The existing present mapper also rejects PA >= 2^36. UniXNU's Query advertises its own 40-bit allocation ceiling; we use the observed hardware limit. Turnip does not require 40. |

The pinned [Linux A740 catalog](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/gpu/drm/msm/adreno/a6xx_catalog.c)
was independently hash-checked (`ff1abc1b7735d6bd6aa07d5df776e2549be2ee70ffcc02ff24aac646dfd6a3c7`).
The same tree's [UBWC configuration](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/soc/qcom/ubwc_config.c)
and [SMMU ID decoding](https://github.com/ianchb/sm8550-mainline/blob/42f3b40c702abbc37c3a04c95af008cd283aeaf3/drivers/iommu/arm/arm-smmu/arm-smmu.c)
support the remaining configuration facts. Device-specific coherency still
requires execution; the catalog's coherent-capability quirk is not proof that
our XNU BO mappings are coherent.

## Raw test, build and coordinator procedure

`tools/build-fuxi-gpu-abi-test.sh` was run on the Mac using the installed
**iPhoneOS 27.1 SDK**, targeting **arm64-apple-ios16.0**, then ad-hoc signed.
The binary's LC_BUILD_VERSION is iOS 16.0. It dynamically resolves IOKitLib
because the public iOS SDK omits its headers. The script attempts the macOS
SDK with the same iOS target only if the iOS SDK is absent; that fallback was
not used or qualified in this run. Binaries go to gitignored `tools/out`.

The normal test waits at most 60 s for the service, opens 0x4147, queries,
allocates/maps a 64 KiB BO, writes an IB with CP_MEM_WRITE plus a 16x16 BGRA
solid CP_BLIT, submits, waits and verifies `0xa740cafe` and all 256 pixels
`0xff336699`. It then unmaps/frees/closes and repeats after reopening, exercising
address reuse and the enumeration/open-close pattern. Every step prints
PASS/FAIL to stdout; final success is `PASS: fuxi-gpu-abi-test`, exit 0.

The least invasive launch method is a dedicated root one-shot job in the
existing unsecure launchd cache, using the same plist editing mechanism as
`sweet-root-tweaks.py`. On an **already mounted writable root image**, the
coordinator can stage the tool offline:

```sh
sudo python3 tools/stage-fuxi-gpu-abi-test.py "$IOS_ROOT" tools/out/fuxi-gpu-abi-test
# After qualification, remove only this binary/job:
sudo python3 tools/stage-fuxi-gpu-abi-test.py "$IOS_ROOT" --remove
```

This copies to `/usr/local/bin/fuxi-gpu-abi-test`, writes
`/Library/LaunchDaemons/org.fuxi.gpu-abi-test.plist` and adds that job to
`/System/Library/xpc/launchd.plist`. It preserves other jobs, uses root without
a sandbox profile, RunAtLoad=true, KeepAlive=false, and stdout/stderr
`/dev/console`. No existing daemon or SpringBoard injection is needed. Keep
the existing root-image signing/AMFI policy; the tool is ad-hoc signed and no
new trust-policy bypass is added by this lane. The helper never mounts an image,
accesses USB or installs anything on a phone.

Coordinator acceptance, serially:

1. Install/stage through the existing root-image workflow and use
   `shim/out/fuxi-metal1-boot.img` through the coordinator's normal temporary
   boot procedure. Look for `FUXI GPU ABI: NativeAdrenoGpuPublished v1`.
2. Capture the complete PASS/FAIL output, exit status if available, GPU logs
   and present statistics. Require both normal rounds, reopening and readbacks
   to pass; exercise touch/app switching while presentation remains alive.
3. On a separate boot, stage with `--watchdog`. This deliberately submits an
   unsatisfied CP_WAIT_REG_MEM. Require `watchdog returns timeout`, a
   `client fence deadline (100 ms)` / GBIF containment log, continuing UI/CPU
   presents, and refusal of later client work. Failed drain should quarantine
   BOs and may make the free check fail; report it rather than treating it as
   safe reclamation. A reboot is needed to restore GPU submission.
4. Remove the diagnostic job afterwards. Test default-off by omitting
   `fuxigpuabi=1` from a separately packed image: no ABI service should appear
   and the existing present path should remain usable.

To reproduce the image, build the patched XNU in `ci-lane-metal` with
`tools/build-fuxi-native.sh` and the matching IIG cache, copy stripped/unstripped
kernels into `xnu/out-fuxi-metal1`, then supply the existing private toolchain
and GMU/SQE/zap environment variables:

```sh
bash tools/build-fuxi-metal-images.sh "$BASE_0097B_ARTIFACTS"
python3 tools/test-fuxi-metal.py
python3 tools/test-fuxi-adreno.py
python3 tools/test-fuxi-memory.py
python3 tools/test-fuxi-scm.py --kernel xnu/out-fuxi-metal1/kernel.development.fuxi.unstripped
python3 tools/test-fuxi-metal-artifacts.py --base "$BASE_0097B_ARTIFACTS" \
  --firmware "$FUXI_GMU_FIRMWARE" --sqe "$FUXI_SQE_FIRMWARE" --zap "$FUXI_GPU_ZAP_MBN"
sha256sum -c xnu/out-fuxi-metal1/SHA256SUMS
```

Offline coverage includes fixed layouts/selectors and overflow/flag checks;
an independent PM4 decoder checking reset/SRAM wait, parity, IFPC and BV/BR
ordering; the actual C backend compiled under ASan/UBSan with modeled register,
SMMU and scheduler interfaces; present exclusion and late fences; watchdog with
no WaitFence calls, faults and failed drain; clean patch application; existing
pixel/cache/lifetime regression models; native recovery/SCM instructions;
boot-v4/Image/payload/full-RAM bounds, unchanged 0097b ADT/root/CPU identity and
exact command line plus ` fuxigpuabi=1`; exact external firmware blobs; signed
raw Mach-O target. These are host models/build checks, not GPU execution.

## iOS 16.7 Metal discovery, re-derived

Facts from the supplied iPhone10,3 / 20H392 arm64 shared cache, inspected with
`ipsw dyld disass`, `macho --symbols`, `macho --objc` and `dump`. Main cache
SHA256: `76874e8375252ee643011ff8063f0cce12355f05cf5f9897a34e5679667a65aa`.
Addresses below are unslid and specific to this cache.

| Observation | Evidence |
| --- | --- |
| `_MTLRegisterDevices` at `0x181b144c8` matches `IOAcceleratorES` | String/matching at `0x181b14514..0x181b1452c`; iteration calls `_getMetalPluginClassForService`, `initWithAcceleratorPort:` (`0x181b14580`), then `_MTLAddDevice` (`0x181b1458c`) for a non-nil result. |
| Plugin properties and bundle lookup | `_getMetalPluginClassForService` at `0x181a3c7c8` reads `MetalPluginName`, builds `/System/Library/Extensions/<name>.bundle`, reads `MetalPluginClassName` or falls back to principalClass; checks `isSubclassOfClass:_MTLDevice` at `0x181a3c8d0`. |
| `_MTLDevice` instance size is **528**, not 536 | Class symbol `0x1e1d88a68`, decoded class data at `0x1d59e51a8`; class_ro fields flags=4, instanceStart=8, instanceSize=`0x210`. This must also be checked with `class_getInstanceSize` before constructing a runtime subclass. |
| Stock IOGPU open type | `_IOGPUDeviceCreateWithAPIProperty` at `0x1ece1ce38` constructs `1 | (api << 16)` at `0x1ece1ce68..0x1ece1ce80`, then calls IOServiceOpen. Our 0x4147 ABI is a different connection contract. |
| Kernel family availability | The linked metal1 kernel has the new native service metaclass and ABI symbols, and no `IOAcceleratorES` or `IOGPUFamily` implementation/metaclass symbols. This standalone payload does not include Apple's IOGPUFamily. Publishing an ordinary service alone cannot create an MTLDevice. |

Reproduce disassembly without copying any Apple code into git:

```sh
"$IPSW" dyld disass "$DSC" --symbol _MTLRegisterDevices --symbol-image Metal
"$IPSW" dyld disass "$DSC" --symbol _getMetalPluginClassForService --symbol-image Metal
"$IPSW" dyld macho "$DSC" Metal --symbols
"$IPSW" dyld dump "$DSC" 0x1d59e51a8 --size 24
"$IPSW" dyld disass "$DSC" --symbol _IOGPUDeviceCreateWithAPIProperty --symbol-image IOGPU
```

**Unverified:** iOS 16 sandbox selector/class allowances, private IOGPU object
vtable layouts, root-image plugin signing/loading and CoreAnimation's complete
requirements. Do not import UniXNU's iOS 17 IOGPU size 736 or `_MTLDevice` size
536 into this kernel. Its published plugin returns nil; its raw IB success does
not establish Turnip or Metal success. Its documented Turnip IBs still hang CP.

## Remaining milestones and acceptance gates

1. **M1 device qualification:** normal raw IB/readback/reopen, present coexistence
   and watchdog containment above. Current status: built and offline-tested,
   awaiting coordinator execution.
2. **M2 Turnip cross-build for iOS 16:** follow UniXNU
   `scripts/build-turnip-darwin.py` and `scripts/data/adreno-compiler-inputs.json`.
   The actual build pin is Mesa **85fda3b36405bcaea8bffc64aa4f2651dc90770d**;
   `gpu-metal-vulkan-inputs.json` also records **c823a83e...** as a reference
   survey tree, not the revision the build script checks. Apply, in manifest
   order: `freedreno-tools-nir.patch`, `ir3-darwin-build-id.patch`,
   `ir3-portable-asm-roundtrip.patch`, `offline-adreno-compiler-probe.patch`,
   `turnip-darwin-kmd.patch`. Retain MIT notices; keep source/build caches out
   of git. Change the cross target from ios17.0 to **arm64-apple-ios16.0**.
   Use iPhoneOS SDK, Meson `freedreno-kmds=darwin`,
   `tu-darwin-abi-dir` pointing at our ABI header, `vulkan-drivers=freedreno`,
   `platforms=macos`, no Gallium/GL/EGL/GBM/LLVM, and native glslang/build tools.
   Audit iOS availability, Mach-O load commands/dependencies, cache-line helpers
   and absence of EL0 CTR_EL0 instructions; the supplied patch now substitutes
   Darwin's 64-byte cache-line value. Require exported vk_icdGetInstanceProcAddr
   and a root-daemon load/enumeration before submitting shaders.
3. **M3 Turnip smoke:** adapt NativeTurnipSmoke's direct-library tests, without
   a Metal frontend. With TU_DEBUG=nocb, run Vulkan buffer fill, a small compute
   shader, then an offscreen triangle; map and compare every result, repeat
   BO/IOVA reuse and process reopen, capture the failing fence/CP/SMMU state.
   Stop and use CPU UI after any loss. Real cache/TTBR/coherency and A7xx reset
   sequencing remain hypotheses until these pass; do not infer success from
   a successful cross-build or CP_MEM_WRITE alone.
4. **M4 Metal discovery/frontend:** use the iOS 16 findings above and a runtime
   size guard. Decide between a small native IOAcceleratorES compatibility
   service and bringing in an appropriate family after auditing real kernel
   dependencies; the current kernel cannot inherit an absent metaclass.
   Verify bundle loading, signing and unsandboxed MTLCreateSystemDefaultDevice
   returning a real device first. Then derive iOS 16 sandbox access and actual
   selectors. Do not add a catch-all selector tunnel as a substitute for that
   audit. Implement device capabilities, resources, queues and encoders against
   the tested Vulkan path; enumerate unsupported features truthfully.
5. **M5 Translation and licensing:** keep **metal2vulkan external** at pinned
   `720ef1ce9ae2a2068999a8675717418b9416283e`; its declared choice is
   GPL-2.0+ OR LGPL-3.0+. Do not vendor it or statically fold it into this repo's
   kernel/plugin artifacts. Distribution needs a separate license/relinking
   plan before shipping a combined frontend. Validate AIR/metallib translation
   and shader caching separately. Imported host/IOSurface BOs, page lifetime
   and coherence are additional kernel work; M1 advertises no HostImport.
6. **M6 presentation/CoreAnimation:** implement IOSurface texture import,
   CAMetalLayer drawables, producer/consumer fences and buffer lifetime, pixel
   format/stride/orientation handling, and CA's required render/compute paths.
   Preserve the existing CPU-present fallback through frontend/plugin failure.
   Require actual GPU-rendered app and CA content, UI correctness, latency,
   thermal and memory measurements. Neither UniXNU's nil-returning plugin nor
   its partial frontend supplies this integration today.

## Risks and delivery

Real idle TTBR switching, CP context reset and BR/BV fences remain unbooted.
The raw solid fill is deliberately small; success will not prove shader or
Turnip execution. Raw PM4 is privileged and not validated as a security
boundary; only an unsandboxed root diagnostic client is supported. Arbitrary
root IBs can access privileged GPU state. Failed bus drain leaks/quarantines
memory until reboot; wedged interconnect/MMIO can exceed software containment.
There is no CP recovery, preemption, fair scheduling, host import, IRQ completion,
MTLDevice, drawables or accelerated CoreAnimation in M1.

Artifacts and their exact hashes are in `xnu/out-fuxi-metal1/SHA256SUMS` and
`tools/out/SHA256SUMS`; all binaries/firmware/Apple analysis output are ignored.
The command line is the supplied 0097b line plus exactly ` fuxigpuabi=1`.
The ADT remains byte-identical, including seven CPUs, pinned UFS root and
5088 MiB of XNU RAM with the existing 32 MiB GPU reservation.

| Artifact | SHA256 |
| --- | --- |
| `xnu/out-fuxi-metal1/kernel.development.fuxi` | `40662b2881e536fbcada825e92768718d92aa35999afa52d13c5dc8211b80088` |
| `xnu/out-fuxi-metal1/kernel.development.fuxi.unstripped` | `4376b4eefb3c66f0c1eac928459df7b7d3fab7d9473837ba889c7e45e7fc0b22` |
| `xnu/out-fuxi-metal1/fuxi.adt` | `39aacea4d7db372314bdc7928a66271b3b5cc2bd878bec4d9cfe2f5191e83533` |
| `shim/out/fuxi-metal1-payload.bin` | `781e358637e37205e6bc1140406d7965801293516ad74c9453a733273969eeb2` |
| `shim/out/fuxi-metal1-boot.img` | `c430735773e9b97f0fa60f37244da085ab219ad97c46224dbf2f297834b59851` |
| `tools/out/fuxi-gpu-abi-test` | `fc04e8157061a21d6f713197625010006e28b77b56c01cddf1d38ffd5cd85ae1` |
