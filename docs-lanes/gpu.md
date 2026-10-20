# Milestone 4b — A7xx render-thread selection and blit-cache writeback

Branch `codex-gpu-m4b`, based on `fuxi-bringup` / M4 commit `1302cee`.
This section supersedes M4's artifact, ring counts and report layout below.
**Hardware acceptance is pending.** No phone, USB, adb or fastboot access was
used for M4b, and nothing was pushed. The GMU-start handoff lock remains.

## M4 device result (fact)

Coordinator's fresh boot of M4 image SHA256
`00fc43ebbba2d0b3b01fa2e3d42a4c0a5aae9b46e9cbf1ba2a39272807041782`
ran stages7..14 together. Stages7..13 PASS, including fence `a740f00d` and
rptr7→11. Stage14 returned `e06`, step6: all65536 destination pixels remained
`deadbeef`, first x/y0/0 expected`00000000`; source and every guard were intact.
The ring was consumed, rptr=wptr74, and the following event fence `a740b014`
landed. CP_INT, HW_FAULT, PROTECT, RBBM_INT and GFSR were0; both context banks
retained only the pre-existing FSR bit10 (`400`). Frozen RBBM_STATUS was1 and
STATUS1/2/3 were0. Private decoded evidence is `research-log/stage-14m4.log`;
it is not part of the commit. This establishes CP progress without visible
2D destination writes; it does not establish a working render pipeline.

CPU=0.000us was a decoder validity bug: M4 never ran a stage14 CPU comparator,
and failed GPU verification also prevented the stage15 comparator. Zero in an
unreached timing field is **not a measured zero-duration memcpy**.

## Source comparison and candidate fixes

Primary references are the supplied **Mesa25.0.7** tree and **Linux6.12** msm
source, not a newer driver's register names. UniXNU's A740 derivation scripts,
NativeAdreno hw-init table and AdrenoPM4 builders were read as secondary
cross-checks; no implementation was copied from them.

- **Confirmed omission:** Linux `a7xx_cp_init` emits THREAD_CONTROL=`08000000`
  to disable concurrent binning. That does not select BR: the XML enumerates
  BR=1, BV=2, BOTH=3. Turnip `tu_cmd_buffer.cc:tu6_init_hw` explicitly emits
  `08000001` before graphics state. Linux `a7xx_submit` also selects BR before
  submission. M4 did neither after CP init. M4b selects **BR, binning disabled**
  at the beginning of each clear/copy stream, preserving stages1..13's sequence.
  **Hypothesis:** M4's zero thread selector allowed CP control/fence progress
  while render work/state did not reach BR. The exact retail SQE behavior for
  selector0 remains unverified; source omission alone is not a device proof.
- **Confirmed omission:** Turnip's `FD_CCU_CLEAN_BLIT_CACHE` maps through
  `common/freedreno_gpu_event.h` to A7xx **CCU_RESOLVE_CLEAN26**. Its flush code
  explicitly requests this in addition to ordinary CCU/UCHE maintenance.
  M4 emitted color/depth cleans33/32 and CACHE_CLEAN49, but no26. M4b emits26
  before work and after each job, then color/depth clean and invalidate, then
  the write-enabled CACHE_CLEAN49 fence and WFI. **Hypothesis:** undrained
  A7xx blit writes contributed to the untouched CPU-visible destination.
- M4b adds WFI after static `RB_CCU_CNTL=0`, matching Turnip's requirement that
  static A7xx CCU configuration take effect across WFI. Resolve/unresolve
  concurrency stays disabled. Linux's A7xx `RB_CMP_DBG_ECO_CNTL` bit11 ordering
  workaround is applied by safe CPU read/modify/write at stage14, before jobs.
- **Already present/correct:** UCHE TRAP/WRITE_THRU bases=`1fffffffff000`,
  A740 GMEM VA range=`1000000..12fffff`, UCHE cache/CMDQ/GBIF setup, Linux's
  A740 HBB16 RB/TPL1/SP/GRAS NC configuration, HWCG and CP_PROTECT tables.
  These are not newly guessed missing writes. Mesa `tu_device.cc` allocates
  six256KiB depth CCUs before the color cache, yielding color offset`180000`;
  `emit_rb_ccu_cntl` and XML encode full caches as `RB_CCU_CNTL2=c0000000`.
- The `r2d_setup_common/src_buffer/dst/run` comparison retains format`30`,
  UNORM8 ifmt`10`, maskf, WXYZ BGRA swap1, SP output`f180`, A7xx source at
  `b2c0`, TPL1 TEX_2D at`b2d2`, inclusive BR coordinates, no scissor/UBWC/sRGB.
  Mesa25.0.7 A7xx source pitch is bytes<<3; for aligned1024 bytes its value
  `2000` equals M4's `(pitch>>6)<<9`. Destination pitch is bytes>>6. These
  encodings were rechecked, not changed. RM6_BLIT2DSCALE was already emitted.

The two principal omissions are corrected together; a passing M4b run cannot
attribute the fix to one of them. Missing additional cold graphics state or
retail secure-mode behavior (TRUST remains1) are residual hypotheses if neither
variant writes. No secure-mode transition, zap/PAS/SCM or SID2 mapping is added.

## One stage14 run: source-free clear, then independent patterned copy

After existing mapping/idle/fault/guard gates, stage14 submits a **solid-color
2D clear** using SOLID_COLOR bit7, RB/GRAS control`10f03080`, per-channel
R/G/B/A=`33/66/99/ff`, and destination BGRA pixel`ff336699`. It emits no source
surface registers. The original read-only source mapping is retained so CPU
checks can detect any source corruption. Clear uses its own fence
`40029030=a740c014`. Every destination/source pixel and every scratch/old
fence guard is checked after fence+rptr+wptr+idle completion and a100us delay.

A clear **pixel-only mismatch** is retained while the separately planned copy
runs once; any fault, timeout, safe abort, source corruption or guard damage
stops immediately. After a clean/invalidated, idle clear, CPU refills destination
with `deadbeef` so the copy test is independent. The original full256x256 pattern
copy then uses `40029010=a740b014`. Both variants must pass before stage14 can
PASS. There is no retry, recovery or ring reuse. Stage15 remains gated on14 PASS.

| Clear / copy observation | Interpretation limit |
|---|---|
| Both untouched, both fences and idle succeed | General 2D routing/state/writeback remains suspect; source fetch alone cannot explain the clear failure |
| Clear passes, patterned copy fails | Destination write path works for solid color; investigate source fetch/format/coordinates |
| Clear fails, copy passes | Inspect solid channel packing/clear state; new result `e0b` blocks15 |
| Both pass, all guards/source checks pass | Scratch acceptance; proceed to the separately authorized scanout test |

After GPU verification, stage14 now also times256 identical1024-byte CPU row
copies to scratch; stage15 retains64 row copies to its panel region. Counter
reads have ISB/compiler barriers; final DSB stays inside the interval. This is
still the loader's uncached volatile-byte memcpy, not an optimized XNU baseline.
CPU readback follows timing. A validity bit distinguishes an executed benchmark
from an unrun field, including failed-stage14 legacy-v6 reports in the decoder.

Expected successful rings: clear **11→82** (71 words), patterned copy
**82→156** (74 words), stage15 **156→1679** (1523 words). Page tables remain
3→5→10. Primary stage14 ring/timing fields describe the copy only. Its GPU time
excludes clear,100us diagnostic delay and CPU pixel verification.

## Report v7 (same fourteen512-byte pages)

Signatures become`a7400007`; selectors100..10d and7168-byte size stay unchanged.
The host still accepts v2..v6. Existing words0..42,45..53,64..95 retain meaning,
with primary ring fields valid only if copy-submitted is set. New layout:

| Local words on page12/13 | Meaning |
|---|---|
| 43 | Validity bits0 CPU benchmark ran,1 clear submitted,2 clear completed,3 copy submitted,4 copy completed,5 delayed copy state sampled |
| 44 | Clear result (0 not run,1 PASS,`e0b` pixels wrong; other M4 failures unchanged) |
| 54..58 | Clear end rptr target, observed clear fence, mismatch count, first actual mismatch, count still`deadbeef` |
| 59/60 | RBBM_STATUS / CP_INT sampled100us after clear wait returns, including timeout |
| 61..63 | Clear guard mismatches (scratch+old fence), source mismatches, completion polls |
| 96..111 | First16 **copy** command words (v6 stored32) |
| 112/113 | RBBM_STATUS / CP_INT sampled100us after copy wait returns, including timeout |
| 114..127 | Raw CCU_CNTL, CCU_CNTL2, RB_BLIT, GRAS_BLIT, SP_FORMAT, SRC_INFO, SRC_SIZE, SRC_PITCH, DST_INFO, DST_PITCH, TPL1_BLIT, RB_CMP_DBG, RB_DBG, APERTURE_HOST |

Raw context-register reads depend on host aperture; do not mistake a banked
readback for packet decoding. Defined XML RBBM busy bits7/8/9 report RB/CCU/A2D;
no speculative RB/CCU status offsets are accessed. Frozen final fault snapshots
remain in64..95. Safe diagnostic aborts stop acceptance. A successful14 has
validity`3f`, successful15=`39` (no clear there). CPU fields are N/A until bit0.

## Offline validation, artifact and device order

Native UBSan model (`-Wall -Wextra -Werror`) passes with synthetic and exact
sheng GMU/SQE inputs, including both variant readbacks, guards, no next stage
on failure, clear-only/copy-only write loss, omitted BR, omitted resolve-clean,
fence/stall/fault/abort/TLB/geometry/ring capacity cases and frozen replay.
The model deliberately separates render-thread routing and queued blit-cache
writes from CP consumption/fences; this tests handling of those hypotheses,
**not their real SQE/cache behavior**. Mesa XML field/event audit and Linux
54-HWCG/48-protection/39-offset audit pass. v7/v6/v5 decoding passes, and the
private M4 log was reconstructed/decoded offline with CPU=N/A.

```
python3 tools/test-fuxi-gpu-probe.py
python3 tools/test-fuxi-gpu-probe.py --mesa "$MESA" --linux-msm "$MSM" \
  --firmware "$FW/gmu_gen70200.bin" --sqe "$FW/a740_sqe.fw"
```

The firmware-enabled loader, explicitly empty-firmware loader and standalone
probe build with clang-r547379: **PASS, zero warnings**. Loader enforces-Werror;
C relocation, link and Image guards pass. Saved image inspection verifies boot
v4, empty ramdisk/padding, exact GMU/SQE bytes, unchanged integ1 payload and no
linked relocations. Build environment/recipe is the M4 section6 below, with
external firmware inputs; the firmware image was saved before empty-fw checks.

Ignored image: `shim/out/lane-gpu/fuxi-gpu-m4b-boot.img`, **20,881,408 bytes**.
SHA256: `d5807c0de62c50359e5453814e5ee8ca835eba4f2b4e91c2b367ad35633be397`.
Kernel Image20,873,296 bytes. Saved ELF GMU`147c0..24cdc`, SQE`24cdc..37350`,
BSS`38000..68000`, payload`78000`, end`13e8050`; firmware/payload hashes are
unchanged from M4. Saved ELF/Image and a SHA256 file sit beside the boot image.
No firmware, private log, generated binary or local absolute input path is
committed. Commit: the commit containing this section.

Coordinator order (temporary fresh boots; lane itself must not access device):

1. Regress7,8a ownership review,10, then11→12→13. Expect the proven7/11 init/fence
   counts, TRUST1/read-only, no new defined faults; retain both raw FSR`400`s.
2. Fresh stage14 with updated decoder `--raw --save <private-report>`; inspect
   **both** clear and copy. Require result1/step8, validity3f, rings82/156,
   both magic fences, all pixel/source/guard counts0 and CPU timing valid.
   Check delayed busy/CP_INT, raw CCU state and final faults. Stop on failure.
3. Only after14 acceptance, stage15 on a fresh boot (its14 gate reruns and must
   pass), then same-boot11→12→13→14→15 with report fetches/delays. Require15
   end1679, validity39, geometry and GPU/CPU/perimeter checks, and visually
   inspect bars. A readback alone is not proof of visible scanout.
4. Replay stages/report reads without new submissions; keep request49 refusal.
   Any failure: retain all14 pages, exact image hash and reset outcome; no
   climbing, retries or recovery in that boot.

Remaining risk: the source-derived fix has **no M4b device result** yet. Cold2D
may still hang/fault or fail readback, and new safe state reads may abort.
Stage15 additionally writes active scanout; it cannot run until both scratch
variants and the CPU comparator pass. No offline PASS is a claim of stage14
hardware success or physical panel output.

---

# Lane: GPU A740 — milestone 4, 2026-10-20

Branch `codex-gpu-m4`, based on main `b6996c1`. This section supersedes earlier
handoffs below; retain their history and **Device results, milestone3**.
No phone/USB/adb/fastboot access, no push, no handoff. The GMU-start lock stays.

## Verified baseline and new device hypotheses

Coordinator M3d verified stages1..13, including SQE/CP consuming PM4 through
CB2 and CP_MEM_WRITE landing at the CPU-visible fence. TRUST remained1, and
reserved CB FSR bit10 remained report-only. M4 adds **unverified on hardware**
pixel work. The offline model executes a small interpreted PM4 subset, not
SQE firmware, real SMMU translation, cache behavior or the rendering pipeline.

Stage12 now reads SECVID/TSB only: its legacy ignored TRUST=0 fallback write is
removed to leave secure mode alone. Page6[43]=0 means no write was attempted;
[44]/[45] are two raw read samples. No secure mode packet, zap/PAS/SCM call or
SID2 mapping is added. Init/fence command sizes remain7/11 and stage11 still
uses3 page-table pages. New mappings are installed only when their stage runs.

Local Mesa was found before using external references: revision
`875160ed32cc5bc32777fa04870a0a57ed38c987`, `src/gallium/drivers/freedreno/a6xx/`
`fd6_blitter.cc`, `fd6_emit.{cc,h}`, `src/freedreno/vulkan/tu_clear_blit.cc`,
`common/fd6_gmem_cache.h`, `freedreno_devices.py`, `fdl/fd6_format_table.c`,
and `registers/adreno/{a6xx.xml,a6xx_enums.xml}`. Linux reference remains the
supplied kernel-guest msm tree. `$MESA`, `$MSM`, `$FW`, `$LLVM`, `$PAYLOAD` below
are local input placeholders, never tracked machine paths.

## Stage14: 256x256 scratch BGRA copy

Stage14 requires13 PASS, retained GPU_SET, idle CP with expected rptr/wptr,
no defined faults and intact old fence guards. It adds two NC mappings to the
existing GPU table pool, with AF/shareability and permissions matching M3,
then publishes a barrier, invalidates the owned ASID and polls TLBSYNC at most
1000x1us. Neither context control nor stream ownership is reprogrammed; faults
are not cleared. Mapping/setup failure is one-shot, with no rollback or retry.

| Surface | GPU VA | Physical address | Mapped bytes / AP |
|---|---:|---:|---|
| Source | `0x40201000` | `0x8f1401000` | `0x40000`, read-only AP3 |
| Destination | `0x40401000` | `0x8f1501000` | `0x40000`, RW AP1 |
| Panel, stage15 only | `0x41000000` | `0xb8000000` | `0x9e4000`, RW AP1 |

Both scratch surfaces have a CPU-written 4KiB guard page immediately before
and after them, outside their GPU mappings. All guards and all source/dest
pixels are checked after completion. Pattern `(x^y)<<24 | x<<16 | y<<8 |
((13*x+7*y)&255)` varies every channel, including alpha; destination starts
as `deadbeef`. Expected and actual checksums are FNV-style **u32-word** folds:
start2166136261, `(hash ^ pixel)*16777619 mod2^32` in row-major order. Counts
and first bad x/y/expected/actual are primary evidence, not checksum alone.

Submission follows fd6/Turnip's linear 2D path: WFI, RM6_BLIT2DSCALE, A7xx CCU
clean/invalidate and UCHE invalidate events, sysmem CCU configuration, matching
RB/GRAS_2D_BLIT_CNTL (format30 UNORM8, maskf), A7xx SP_2D_DST_FORMAT at `a9bf`,
SP_PS_2D_SRC_* at `b2c0` (not A6xx `b4c0`), TPL1_A2D_BLT_CNTL at `b2d2`
(TEX_2D), BGRA WXYZ swap1, nearest, linear, single sample, no UBWC/sRGB.
Mesa's corrected SP output field has float type0 and half_precision0; do not
set the older Linux XML's misleading NORM bit0. Source pitch is encoded in
64-byte units at bit9, destination pitch in64-byte units at bit0. GRAS source
coordinates are shifted8; source/dest BR coordinates are inclusive.

A740's six CCUs x256KiB depth cache put the sysmem color cache at GMEM offset
`0x180000`: RB_CCU_CNTL2=`c0000000`, full caches; RB_CCU_CNTL=0 disables
concurrent resolve. Protected RB/TPL1/SP/GRAS NC registers use safe **CPU MMIO**
with Linux's A740 HBB16 configuration, after the idle gate. They never appear
as protected PKT4 writes. The PM4 model audits every PKT4 against the existing
NORDWR protection ranges. No 3D shaders, draw state, preemption or GMEM resolve
is enabled, and no bulk unrelated Mesa 3D magic is copied. Whether this minimal
cold 2D setup suffices on retail A740 remains a device question.

Each copy emits LABEL then `CP_BLIT / BLIT_OP_SCALE`, followed by WFI. The job
ends with CCU_CLEAN_COLOR/DEPTH and A7xx `CP_EVENT_WRITE7` CACHE_CLEAN49,
WRITE_ENABLED bit27, user32 payload to its own fence. This is Linux
`a7xx_submit`'s A7xx event encoding, not A6xx timestamp bit31 or a bare
CP_MEM_WRITE. A final WFI follows. CPU completion requires expected fence,
rptr=wptr and idle RBBM, with CP/RBBM/global/both-bank faults checked every16
polls and at completion. Wait is at most5000x10us plus MMIO cost. All new MMIO
and verification reads use abort-recovering wrappers; CPU-owned scratch fills
use the existing uncached stores. Failure retains power, mappings and ring.

Expected stage14 ring: starts11, appends63 words, ends74; tables3→5;
fence `0x40029010`=`a740b014`, all65536 pixels match, no guard/source damage.

## Stage15: visible bars and CPU comparison, gated on14 PASS

Before mapping or touching scanout, safe reads check the same source pipe as
request4e: PA=`b8000000`, stride4320, size=`09600438` (1080x2400),
format=`237ff`. A mismatch/abort stops with no panel submission. Never read
MDSS/MDP top, program display registers, or issue manual START.

The region is x64..319, y256..319: 256x64 BGRA pixels /65536 bytes. Eight
32-pixel bars are red, green, blue, white, yellow, cyan, magenta, dark grey.
Source is rebuilt in the same scratch surface. The destination's4320-byte
stride is **not** a multiple of64 and cannot be encoded directly. Submit64
one-row blits: base=`align_down(panelVA+row*4320+x*4,64)`, destination x phase
=`(byteOffset&63)/4`, padded single-row pitch1088; source y selects the row
from the256-wide scratch image. Pitch is never used to advance to another
panel row. WFI serializes coordinate/base changes. The 32KiB ring is never
wrapped or reused, and worst-case capacity is checked before writing new words.

Expected stage15 ring: starts74, appends1512 words, ends1586; tables5→10;
fence `0x40029020`=`a740b015`. Compare all16384 output/source pixels before any
CPU benchmark. Compare saved pixels immediately left/right of every row and
above/below the region, plus all scratch guards and old fence guards. This is
a perimeter check, not a proof that every other framebuffer pixel is unchanged.

Only after GPU verification succeeds, time a CPU `memcpy` of the exact same
64 rows/source/target and verify the region and perimeter again. The comparator
is probe.c's **volatile byte memcpy with MMU/caches off**, not an optimized XNU
memcpy. `cntpct_el0` GPU time includes doorbell, polling, fence and idle, excludes
mapping, pattern creation, packet construction and pixel verification. CPU time
includes64 memcpy calls plus final `dsb sy`. Both are u64 ticks with CNTFRQ in
Hz; polling granularity/MMIO overhead can dominate this small job. No speedup
or physical-panel visibility is claimed offline. CPU readback cannot establish
panel scanout/tearing; coordinator checks the actual bars.

## Report v6: fourteen512-byte pages

Selectors100..10d (7168 bytes); page1/page6/extended signatures=`a7400006`.
Updated host accepts `--stage 14/15`, fetches all14 pages, and still decodes
legacy v2/v3/v4/v5 sizes. `--read-reports` copies frozen RAM only. Stage12/13's
frozen records are preserved. Pages12/13 hold one128-u32 record for14/15:

| Words (local indices) | Meaning |
|---|---|
| 0/1/2/3 | signature / attempted stage (0 absent) / result / step |
| 4..8 | width,height,source pitch,logical destination pitch,bytes copied |
| 9/10 | source/destination GPU base VA |
| 11..14 | source PA low/high, destination PA low/high |
| 15/16 | destination x/y within logical surface |
| 17/18 | event fence VA / expected magic |
| 19/20/21 | expected checksum / actual checksum / mismatch count |
| 22..25 | first bad x,y,expected,actual; x/y=`ffffffff` when none |
| 26/27 | scratch guard or panel perimeter mismatch count / old fence low guard |
| 28..30 | table pages / last TLBSTATUS / TLB polls |
| 31..37 | ring start,end,count / completion polls / rptr,wptr / fence value |
| 38..42 | GPU ticks low/high, CPU ticks low/high, CNTFRQ Hz |
| 43/44 | reserved0 |
| 45..49 | scratch guard mismatches / source pixel mismatches / CPU pixel mismatches / old fence high guard / CPU perimeter mismatches |
| 50..53 | stage15 raw display PA,stride,size,format |
| 54..63 | reserved0 |
| 64..71 | frozen rptr,wptr,RBBM_STATUS,CP_INT,CP_HW_FAULT,CP_PROTECT_STATUS,RBBM_INT,GFSR |
| 72..79 | GPU CB,FSR,FAR low/high,FSYNR0/1,CBFRSYNRA,SCTLR |
| 80..87 | GMU bank, same fields |
| 88..91 | TRUST read-only / RBBM_STATUS1/2/3 |
| 92..95 | cumulative safe access faults / last PA low/high / ESR; page6 retains FAR/op/first fault |
| 96..127 | first32 command words of this stage |

Steps:1 prerequisites,2 map/TLB,3 pattern/guards,4 render setup/encode,
5 submit/wait,6 GPU verification,7 CPU benchmark/verification,8 complete.
Unreached fields are0; checksum, counts and timing are valid only once the
corresponding step ran. Snapshot reads are sequential, not atomic. Guard/source
failures stop before CPU overwrites. Diagnostic abort converts a tentative PASS
to failure. Result1=PASS,0=not run. Shared M4 failure codes:

| Code | Meaning |
|---|---|
| e01 | safe register/memory access abort (see page6 and record92..95) |
| e02 | defined CP/RBBM/global or either-bank fault |
| e03 | live mapping TLB sync timeout |
| e04 | OOB/previous stage/ring/idle/capacity precondition lost |
| e05 | event fence/rptr/wptr/idle timeout |
| e06 | destination or source pixel mismatch |
| e07 | scratch guard, old fence guard or panel perimeter damage |
| e08 | live scanout geometry mismatch |
| e09 | page-table mapping failure |
| e0a | CPU benchmark readback/perimeter mismatch |

## Validation and coordinator order

Offline synthetic and exact sheng GMU/SQE native UBSan models pass with
`-Wall -Wextra -Werror`. The model interprets PKT4/PKT7, walks real emitted PTEs
and enforces write permissions; it copies every pixel using decoded pitches,
base alignment and inclusive coordinates. It verifies full surface copies,
panel row phases, event fences, frozen records, one-shot failure/no next stage,
corrupt destination/source/guards/perimeter, missing fence, stall, CP error,
safe-access failures, TLB timeout, geometry refusal and ring capacity refusal.
It does not emulate SQE, caches, physical addresses or real rendering. Source
audit checks54 HWCG pairs,48 protection entries and36 CP/2D offsets against
Linux, plus cold RUN order and firmware copy. v6/legacy v5 decoders pass offline.

Reproduce without a device:
```
python3 tools/test-fuxi-gpu-probe.py
python3 tools/test-fuxi-gpu-probe.py --linux-msm "$MSM" \
  --firmware "$FW/gmu_gen70200.bin" --sqe "$FW/a740_sqe.fw"
```

1. Fresh boots: regression7,8a ownership review,10; then11,12,13. On12 expect
   TRUST1 with **write_ok0**, CP init rptr7; on13 fence/rptr11, no defined faults.
2. Fresh stage14 (`--raw --save <private-report>`). Require14 result1/step8,
   ring74, event magic, checksum equality, mismatch/source/guard counts0,
   no defined CP/RBBM/SMMU faults or safe aborts. Keep both bank FSR bit10 raw.
3. Only after14 passes, fresh stage15. Require region/geometry/fence/ring1586,
   all GPU/CPU mismatch and perimeter counts0; retain raw timings and visually
   check bars. Do not infer visibility solely from CPU memory.
4. Same boot11→12→13→14→15, report fetches and delays between stages. Repeated
   stages must preserve records/no new submission. Fetch all14 pages with the
   lane decoder; an old12-page runner loses M4 evidence. Keep request49 refusal.
5. Any failure/reset: stop climbing; retain all available reports, stage/step,
   exact boot outcome and image hash. No retries/recovery/handoff in that boot.

Risk by stage:1..13 retain proven hardware sequence apart from12's removal of
the ineffective TRUST write and report format;14 first exercises the cold2D
pipeline and can hang/fault despite bounded CPU polling;15 additionally permits
GPU writes to the active framebuffer and can damage output if address/stride
hardware semantics differ. None of M4 has been tested on the phone here.

Firmware-enabled loader (`-Werror`), explicitly empty-firmware loader, and
standalone probe build with clang-r547379: PASS, **zero warnings**. C relocation,
link, Image header/size guards pass. Independent inspection verifies boot v4,
zero ramdisk/padding, exact embedded sheng GMU/SQE and unchanged integ1 payload,
and no relocations in the saved firmware ELF. Firmware hashes match prior
milestones. Payload SHA256:
`800b9ea17ce18f1872c0533d5ef13f916c490eb7b1524e677fc2555378f00d66`.

Final ignored artifact: `shim/out/lane-gpu/fuxi-gpu-m4-boot.img`, **20,881,408 B**.
SHA256:
`00fc43ebbba2d0b3b01fa2e3d42a4c0a5aae9b46e9cbf1ba2a39272807041782`.
Kernel Image:20,873,296 B. Saved firmware ELF: GMU `[0x138c8,0x23de4)` (66844 B),
SQE `[0x23de4,0x36458)` (75380 B), BSS `[0x38000,0x68000)`,
payload start`0x78000`, Image end`0x13e8050`. The lane image/ELF/Image were
saved before the empty-firmware build; the lane artifact contains both blobs.
A `.sha256` file sits beside it. No firmware, dump, ELF, boot image or local
absolute input path is committed. Commit: the commit containing this handoff.

For the post-handoff driver, see [gpu-xnu-plan.md](gpu-xnu-plan.md).

---

# Lane: GPU A740 — milestone 3c, 2026-10-20

Current branch remains `codex-gpu-m3b`, continuing merged commit `86753f3`.
This M3c handoff supersedes the M3b instructions below. No phone access.

## M3c diagnosis: cold SQE control ordering

The coordinator's fresh M3b boot (`c645f6d7...a53`) passed stages1–11, then
stage12 returned `c06`, step7, **phase1, SQE_started=0**, wptr0, polls0.
Offline decoding of the supplied private report confirms CP_INT `80`
(BR_ILLEGAL_INSTR), GPU CB2 FSR `402`, FAR0, FSYNR `b0041/0`, SID0/MID0.
All programmed pointers read back correctly; frozen SQE_CNTL is1. The CPU
payload matches the source firmware, but instruction debug words are0.

The important correction is that this failure precedes the intended final
`SQE_CNTL=1` write, not just the PM4 doorbell. M3b wrote **SQE_CNTL=3 before
hw_init and INSTR_BASE**. Linux uses3 only to halt an already-running SQE in
`a6xx_recover`; cold `hw_init` writes the base/ring/shadows first, then1.
The supplied XML does not document the control bits or latch timing, so the
precise effect of cold-writing3 is not established. The snapshot is compatible
with an early enable/reset and a latched zero-base instruction fetch; later
base readback cannot prove what address was used for that earlier fetch.

Ranked against the supplied Linux source and exact sheng firmware:

1. **Our recovery command used during cold initialization.** Strongest new
   lead: phase1/started0, only prior control write3, frozen control1, FAR0 and
   BR illegal instruction. M3c removes that write. It makes one control write,
   value1, after programming/verifying all pointers and a `dsb sy` barrier.
   This is a candidate hardware fix, not a claimed device success.
2. **Instruction state/base latched by that early control write.** Closely
   related to1. Linux prescribes no CP reset between INSTR_BASE and RUN in
   cold `hw_init`. Adding SW_RESET after the base would depart from Linux and
   disturb the experiment. M3c reads SW_RESET/MISC/CHICKEN at three checkpoints
   but performs no speculative reset or new halt sequence.
3. **Multi-section layout / missing BV or LPAC pointer.** The image has
   repeated section-like headers (table below), but Linux `a6xx_ucode_load`
   calls `adreno_fw_create_bo` unchanged for A7xx. That helper allocates
   `fw->size-4` with WC/GPU_READONLY, copies `fw->data[4:]` at BO offset0 and
   never patches section pointers. `msm_gem_vma_init` requests PAGE_SIZE IOVA
   alignment; there is no SQE-specific 64KiB rule in this path. Our IOVA and PA
   are already 64KiB aligned. No separate BV instruction-base register is
   defined in the supplied XML; cold A740 writes only the shared base830/831.
   BR, rather than BV/LPAC, reports illegal instruction. Keep the full copied
   payload and unmapped IOVA0; new full BV/AC sweeps can test this residual lead.
4. **Other missing Linux setup, firmware/platform or translation behavior.**
   The prior complete hw_init comparison remains applicable. Linux A740 does
   not cold-write CP_MISC, CP_CHICKEN, CP_CONTEXT_SWITCH_SMMU_INFO, AQE/LPAC
   instruction bases, BV ring base, or an IFPC/power-up list before RUN.
   Preemption is not enabled here. Rendering/performance setup is still omitted.
   Neither the source audit nor native model proves real GPU DMA/cache state.

Reference locations in the supplied `kernel-guest-3a7d1771d492` msm tree:
`adreno/adreno_gpu.c:554` (BO copy), `adreno/a6xx_gpu.c:780` (ucode),
`:880` (cold hw_init), `:1137` (base), `:1170` (RUN), `:1266` (recovery halt),
`:1822` (SW reset helper), `msm_gem_vma.c:137` (alignment),
`adreno/a6xx_gpu_state.h:411` (BR/BV/AC indexed window bounds), and
`registers/adreno/a6xx.xml` (register offsets). The offline audit now checks
cold ordering and firmware-copy expressions in addition to 54 HWCG pairs,
48 protection slots and 21 named offsets.

### Firmware inspection

Same sheng SQE SHA256 `6d38046654333a6a5a4ee77ab67abc24e3c0c5c68237513afe98e94da8fc9fcc`,
75380 file bytes /75376 copied bytes (`499c` dwords). File word0 is0, and
payload word1 is `0100499c`: its low24 bits equal the complete copied word
count. These are raw observations, not a newly assumed loader format.
Repeated `0174xxxx` prefixes and following words in the copied payload:

| Payload dword index | First words | Interpretation limit |
|---|---|---|
| 0 | `01740168 0100499c` | Outer prefix / total size match |
| 2 | `01740139 01002458 01013fa0 b8020000` | First section-like header |
| 24d8 | `01740139 01001b78 01000000 b8020000` | Second section-like header |
| 40d0 | `01740055 01000848 b8020000 70030007` | Third section-like header |

This supports a composite image, but does not establish which internal engine
owns each section or how it locates it. Linux does no section splitting.
The newer linux-firmware image also starts with zero plus a total-size prefix
and comparable repeated headers. M3c keeps the exact tested sheng inputs;
no firmware substitution, header reinsertion or guessed section-base writes.

### M3c report v5 and bounded diagnostics

**Use the updated host tool: 12 pages /6144 bytes, selectors100..10b.**
Page1/page6/extended signatures become `a7400005`; the decoder still accepts
v2/v3/v4 saved reports. Page6[97..108] holds up to12 raw payload prefix words,
[109] their count; [119] is checkpoint-valid mask (bits0/1/2). File word0
remains at page6[19]. Existing packet sizes stay7 /11 dwords for init/fence.

Page7 stage12 record now uses its previously reserved words34..63 for three
10-word checkpoints: OOB held before CP setup, configured before RUN, immediately
after RUN. Each is:
```
SQE_CNTL, INSTR_BASE low/high, CP_INT, RBBM_STATUS, CP_HW_FAULT,
RBBM_SW_RESET_CMD, CP_MISC_CNTL, CP_CHICKEN_DBG, GPU_CB_FSR
```
The first checkpoint has phase4 (cold entry). A defined fault there stops with
`c06`; unreadable MMIO stops with `c05`; nonzero initial SQE_CNTL stops with
new `c0c`, without taking over or resetting the engine. Pointer verification
still uses phase1, RUN/wptr0 phase2, doorbell phase3. Set phase2 immediately
before the sole RUN write; `SQE_started=1` means that write succeeded. A
checkpoint records sequential samples, not an atomic hardware snapshot.

New pages10/11 are frozen stage12/stage13 pipe diagnostics,128 u32 each:
```
[0] a7400005 [1] stage [2:3] BV/AC SQE_STAT word counts [4] MMIO abort count
[5:8] BV raw UCODE_DBG indices0..3 [9:12] AC raw UCODE_DBG indices0..3
[13:15] reserved0
[16:66] BV SQE_STAT indices0..50 [67:117] AC SQE_STAT indices0..50
[118:121] raw AQE_INSTR_BASE_0/1 low/high pairs (not enabled/programmed)
[122:124] CP_MISC / BR_CHICKEN / BV_CHICKEN
[125] APERTURE_CNTL_CD [126:127] CONTEXT_SWITCH_SMMU_INFO low/high
```
All added MMIO uses abort-recovering wrappers. New indexed sweeps stop their
window on an abort and restore selectors; each is capped at Linux's0x33 words.
Unsupported/sentinel register reads remain raw evidence. No fault clear, SMMU
resume, IOVA0 mapping, additional engine enable, or error-path retry is added.
Stage13 preserves the stage12 checkpoints; report fetches copy frozen RAM only.

### M3c coordinator test order

1. Fresh boots: stage7, stage8a ownership review, stage10 regression.
2. Fresh stage11: PASS, unchanged buffers/context,3 tables, no defined faults.
3. Fresh stage12, save **all12 pages** with the updated tool. On `c0c` or
   phase4 failure inspect OOB/pre-setup before any CP control write. On phase1
   failure compare configured/pre-RUN with OOB/pre-setup; RUN has not occurred.
   On step7/phase2 compare the immediate post-RUN checkpoint and BR/BV/AC
   PCs/status/debug words. No PM4 doorbell has occurred. Do not retry that boot.
   PASS requires result1/step6, rptr/wptr7, trust0 and no defined faults.
4. Only after12 passes, fresh stage13: fence `a740f00d`, rptr/wptr11, guards
   intact, both frozen stage records. Stop climbing on failure/reset.
5. Same-boot11->12->13 with report fetches and a delay before13; repeated13
   and `--read-reports` preserve results without new MMIO. Retain the permanent
   request49 handoff-refusal check. Device runs belong to the coordinator.

### M3c build handoff

Firmware-enabled loader, explicitly empty-firmware loader and standalone probe:
**PASS, zero warnings**, clang-r547379. Loader `-Werror`, C relocation checks,
link and Image size/header checks pass; final firmware ELF has no relocations.
Native UBSan model passes with synthetic inputs and the exact sheng GMU/SQE.
The model covers the old early-control fault sequence, sole delayed RUN,
preexisting live SQE refusal, checkpoint MMIO abort, full BV/AC sweeps and a
bounded diagnostic read abort, alongside existing ownership/HFI/fence checks.
Linux source audit and twelve-page decoder pass. The supplied private M3b
v4 report still decodes offline. These checks do not execute SQE instructions
or establish the hardware effect of control3. No phone access or M3c device run.

Artifact (gitignored), **20,881,408 B**:
`shim/out/lane-gpu/fuxi-gpu-m3c-boot.img`.
SHA256: `943bb50e54012f6c0c997e2d4322b302eb98c9eb421deb94ae578ca0313f08be`.
Kernel Image: 20,873,296 B. Independent artifact inspection verifies boot
header v4, Image geometry, zero ramdisk/padding, exact embedded GMU/SQE bytes
and unchanged integ1 payload SHA256:
`800b9ea17ce18f1872c0533d5ef13f916c490eb7b1524e677fc2555378f00d66`.

Firmware ELF geometry: `_gpu_fw_start/end=0x11534/0x21a50` (66844 B),
`_sqe_fw_start/end=0x21a50/0x340c4` (75380 B),
`_bss_start/end=0x38000/0x68000`, `_payload_start=0x78000`,
`_image_end=0x13e8050`. The image and firmware-enabled ELF were saved in the
ignored lane output before the empty-firmware verification build. The saved
lane artifact contains both exact firmware inputs. No firmware, dump, ELF,
boot image or local absolute input path is committed. The handoff commit is
the commit containing this section on `codex-gpu-m3b`.

Next action: coordinator follows the M3c test order above using the updated
12-page host tool. The leading cold-control hypothesis still needs device
confirmation.

---

# Lane: GPU A740 — milestone 3b, 2026-10-20

Branch `codex-gpu-m3b`, based on published main containing M3 `7ed3102`.
This lane uses no phone, USB, adb, fastboot or CI. The coordinator owns device
runs. `NOTES.md`, `FUXI.md` and `AGENTS.md` are untouched by explicit instruction.
The M3b handoff below supersedes M3 packet sizes, report version and test targets;
the remaining M2/M3 material is retained as implementation history.

## M3b evidence and diagnosis

**Observed on the coordinator's original M3 image**
(`12b193ecf2ae55f26fe9ca553dca496528a8abcfb9ab047f41207baa856a4cdd`):
stages1–10 PASS, fresh stage11 PASS, fresh stage12 `c06` at step4, stage13 not
run. Offline decode of the supplied private `gpu-m2/stage-12m3.bin` confirms:
GPU CB2 FSR `402`, FAR `0`, FSYNR0/1 `b0041/0`; CP rptr/wptr `0/10`,
CP_INTERRUPT_STATUS `80`, CP_HW_FAULT/protection/global FSR zero,
RBBM_STATUS `800005`, no CPU MMIO aborts. SQE CPU copy/readback completed;
this does **not** prove successful SQE instruction fetching/execution.

Source-backed interpretation (`a6xx_gpu.c`, `a6xx.xml`, `arm-smmu.h`):

- FSYNR1 low byte 0 identifies **CP**. FSYNR0: read, level1, IND=1,
  PNU=0, PTWF=0. IND is the reported instruction attribute; it does not alone
  distinguish every internal CP DMA path. CBFRSYNRA (SID) was missing in M3.
- CP_INT bit7 is **BR illegal instruction**, not the opcode/HW-fault interrupt.
  SQE_STAT[1]=0 is a sampled packet/header field, not a successful firmware PC.
- RBBM `800005` reports GPU_BUSY_IGN_AHB, CP_BUSY and CP_AHB_BUSY_CX_MASTER
  under Linux's shared XML bit definitions. It does not identify UCHE activity.
- The M3 capture has no instruction/ring base register readbacks or SQE PC;
  it cannot establish whether the first failing read preceded the doorbell.

**Ranked hypotheses, not a confirmed hardware root cause:**

1. SQE instruction fetch/startup uses a zero/stale base or stale instruction
   state. FAR0 + BR illegal instruction + IND + no rptr progress makes this
   the leading hypothesis. No incorrect instruction-base offset or reversed
   word order was found: Linux uses low word then high word at `830/831` too.
   M3b verifies both base words before RUN and captures raw UCODE debug words
   alongside the CPU firmware words. Step7 with wptr0 isolates startup faults.
2. CP_ME_INIT's enabled ordinal8 register list dereferences the supplied null
   pointer. M3 copied Linux's `TODO` sequence: mask `14b`, pointer0, length0.
   The SQE may special-case length0; the source comparison cannot prove that
   for this external firmware. M3b removes ordinal8 **and its three payload
   words**, since the probe implements no IFPC/preemption list. This is the
   candidate fix being tested, not a claim that the capture proves this reader.
   Qualcomm's [Gen7 CP init](https://github.com/qualcomm-linux/kgsl/blob/master/adreno_gen7.c)
   labels bit8 REGISTER_INIT_LIST_WITH_SPINLOCK and supplies an allocated list;
   the supplied drm/msm source instead sends zeros. No IOVA0 mapping is added.
3. Another CP path (BV/LPAC, context restore, MRB/IB) starts unexpectedly.
   Linux A740 hw_init does not supply BV/LPAC instruction/ring bases or write
   CP_CONTEXT_SWITCH_SMMU_INFO. Inventing these pointers would hide the cause.
   M3b records LPAC enable/base, BR/BV/LPAC APRIV, aperture, MRB/IB state and
   BV/LPAC PCs/headers. A fault's MID/SID plus SQE PC/header narrows the path.
4. Omitted render/performance setup or firmware/platform mismatch. Lower rank:
   no graphics work was submitted, SQE bytes match the reference blob, and no
   matching missing pointer write appears in the supplied Linux A740 path.

### Complete Linux A740 hw_init / cp_init comparison

Reference is the supplied `kernel-guest-3a7d1771d492` tree, chip `43050a01`,
ADRENO_7XX_GEN2. This tree contains XML and generated snapshot range headers,
but no generated `a6xx.xml.h`/Gen7 register define header. XML is the offset
source; A6xx-only branches must not be added to A740. The companion current
[Mesa register XML](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/61b01302910401cbb4ad25555fd4c23e88d23272/src/freedreno/registers/adreno/a6xx.xml)
also retains the same A6xx–A7xx SQE/base offsets.

| Linux writes on this A740 path | Probe comparison / disposition |
|---|---|
| GPU_SET OOB | Held through stage12/13, including failure |
| GBIF_HALT `3c45`, RBBM_GBIF_HALT `16` =0; TSB control/base/size `f803/f800/f802` =0 | Same writes; safe reads in M3b flush both halt clears |
| GMU CGC `9309..930b`; all 54 A740 HWCG entries | Same offsets/values; automated source audit PASS |
| GBIF QSB `3c03..3c06` =71620, client QoS `11` =2120212 | Same |
| UCHE GX config `e3a`=10240e0; TRAP/WRITE_THRU `e09/e07`; GMEM `e0b/e0d`=16MiB..19MiB-1; CACHE_WAYS `e17`=BIT23 | Same; 64-bit writes low then high |
| PERFCTR_GPU_BUSY_MASKED, PERFCTR_CNTL, IFPC counter select, CP_PERFCTR_CP_SEL0; GMU busy mask/power counter select/enable | Omitted performance accounting; no pointer registers |
| ROQ thresholds / MEM_POOL_SIZE / LPAC FIFO size / CP_ADDR_MODE_CNTL | **Not written by Linux on A740**; A6xx branches are excluded |
| PC_DBG_ECO_CNTL primitive FIFO threshold | A740 catalog has no prim_fifo_threshold; not written |
| CP_AHB_CNTL `98d`=1; hang `1f`=40cfffff; UCHE_CLIENT_PF `e19`=81 | Same |
| RB/TPL1/SP/GRAS UBWC NC controls; UCHE/RBBM NC `e01/534` | UCHE/RBBM same; render pipeline controls omitted (no rendering) |
| CP_PROTECT_CNTL `84f`=b; 48 A730 slots starting850, skipping45/46 | Same; automated source audit checks every encoded slot |
| UCHE_CMDQ_CONFIG `e3c`=6690e | Same |
| BR/BV/LPAC APRIV `844/ad0/b31`=6f/f/f | Same; programming/readback gate before RUN |
| RB_CMP_DBG_ECO_CNTL bit11 | Omitted rendering optimization |
| RBBM_INT_0_MASK | Probe writes0 (poll raw status, no GIC handler) |
| adreno_hw_init | Linux resets software ring pointers/shadows; probe zeroes its fixed buffers and requires reset hardware pointers |
| SQE base `830/831`, RB base `800/801`, RB_CNTL `802`=20c | Same; new pre-RUN readback gate and frozen readbacks |
| BR RPTR `804/805`, BV RPTR `a98/a99` | Same register pairs, own mapped shadow words; no separate BV ring required |
| SQE_CNTL `808`=1 | Same; probe first halts with3 as Linux recovery does; new 32x10us wptr0 fault check before submission |
| THREAD_CONTROL BIT27; ME_INIT ordinals0/1/3/6/8 with seven payload words | M3b retains0/1/3/6, omits absent list ordinal8; four payload words |
| Zap then fallback TRUST_CNTL `f400`=0 | Same no-zap fallback after CP init; no secure SID2 or SCM calls |

Reproduce the register/table audit and native UBSan model with:
```
python3 tools/test-fuxi-gpu-probe.py --linux-msm "$MSM" \
  --firmware "$FW/gmu_gen70200.bin" --sqe "$FW/a740_sqe.fw"
```
`$MSM` denotes `drivers/gpu/drm/msm` in the supplied Linux tree. This checks
54 HWCG offset/value pairs, all48 protection slots and15 critical CP offsets.
It does not execute firmware or validate GPU DMA.

### M3b behavior and report additions (v4)

Stages and one-shot/OOB/handoff locking remain unchanged. Stage12 now validates
critical pointer/control readbacks before starting SQE (`c0b`, page6[94] is
1-based failed index). After starting SQE it enters **step7**, holds wptr0 for
32 polls x10us and checks CP/RBBM/both-bank/global faults before the doorbell.
A step7 failure proves that CP_ME_INIT has not been submitted. A successful
320us startup sample does not prove no later asynchronous startup fault.
Then step4 submits **7 dwords**, expects rptr/wptr7, and the trust fallback is
still step5. Stage13 adds4 dwords and expects **11**, fence `a740f00d`, intact
guards. New init packet (no null list):
```
70970001 08000000 70c80004 0000004b 00000003 20000000 00000002
```

Reports are now **10 pages /5120 B**, selectors `100..109`.
Page1[0] and page6[0] are `a7400004`. Existing v3 fields retain their positions;
page6[80..86] contains the7 init words, [87..89] remain0; [94] is the readback
failure index, [95:96] retain GBIF_HALT/RBBM_GBIF_HALT clear readbacks.
Legacy v2/v3 saved reports still decode (including the original
M3 capture). Pages8/9 are frozen extended stage12/stage13 snapshots; stage13
copies the original pre-RUN samples from page8. Each is128 little-endian u32:
```
[0] a7400004 [1] stage [2] substep at capture
[3] phase: 1 pointer verification, 2 SQE RUN/wptr0, 3 doorbell attempted
[4] completed startup sample polls [5] last pre-doorbell CP_INT
[6] last pre-doorbell GPU FSR [7] last pre-doorbell RBBM_STATUS
[8:23] pointers/controls before RUN (indices below)
[24:39] same registers at frozen capture
[40:41] GPU/GMU GR1 CBFRSYNRA (SID in bits15:0)
[42] UCHE_CLIENT_PF [43:45] RBBM_STATUS1/2/3
[46] CP_STATUS_1 [47] CP_CP2GMU_STATUS
[48:50] IB1 base low/high, remaining [51:53] IB2 same
[54] ROQ_RB_STAT [55] ROQ_MRB_STAT [56:58] MRB base low/high, remaining
[59] BV_HW_FAULT [60] BV_PROTECT_STATUS [61] cumulative safe MMIO abort count
[62] reserved0 [63] SQE_STAT words sampled
[64:114] BR SQE_STAT indices0..50 (Linux a7xx_indexed_reglist length33 hex)
[115:116] BV SQE_STAT indices0/1 [117:118] LPAC indices0/1 [119] reserved0
[120:123] raw CP_SQE_UCODE_DBG words0..3 (interpretation not assumed)
[124:127] CPU SQE buffer words0..3
```
Pointer/control indices0..15 (page6[94] uses index+1):
`SQElo, SQEhi, RBlo, RBhi, RB_CNTL, RPTRlo, RPTRhi, BV_RPTRlo, BV_RPTRhi,
BR_APRIV, BV_APRIV, LPAC_APRIV, APERTURE_HOST, LPAC_SQE_CNTL, LPAC_SQElo,
LPAC_SQEhi`. Only indices0..12 are compared to programmed/reset expectations;
LPAC state is evidence, not an invented enable/reset requirement.

All added MMIO uses safe read/write wrappers. SQE index sweeps are bounded;
selectors return to known indices afterwards. No fault is cleared/resumed by
diagnostics and page fetches still copy frozen RAM only. On error, first/last
CPU abort addresses remain in page6; the extended count includes the entire
snapshot. A diagnostic abort after an otherwise successful stage still fails
that stage. No IOVA0 mapping or diagnostic dummy page is enabled or added.
FSYNR1 low-byte MID follows Linux's A7xx client decoder (CP, CCU, flag cache,
CDP, GMU, UCHE BR/BV/LPAC using UCHE_CLIENT_PF); stale bank syndromes without a
defined FSR fault are not treated as a valid fault client. RBBM bit names use
Linux's shared XML definition; raw words remain authoritative.

### M3b verification and coordinator test order

Build results, exact image hash and geometry are recorded at the end of this
file. No M3b device result exists. Model additions cover dropped SQE-base
programming (RUN/doorbell refused), the exact observed fault signature injected
at SQE start (step7, no doorbell), all51 SQE indices, and existing fence,
timeout, ownership, abort and one-shot failure paths. These are CPU-side model
checks, not confirmation of the candidate CP_INIT fix.

1. Fresh boots: stage7, stage8a (review ownership), stage10 regression.
2. Fresh stage11: unchanged expected context/mappings,3 tables, defined faults0.
3. Fresh stage12: save all10 pages. If `c0b`, use page6[94] and page8[8..23].
   If step7/phase2, the fault precedes PM4: inspect MID/SID, base readbacks,
   BR SQE PC/header, UCODE debug and CPU words. Do not blame the init list.
   If step4/phase3, inspect BR PC/header/MRB/IB and fault attributes; compare
   against this exact SQE blob. PASS requires rptr/wptr7, step6, trust0, no faults.
4. Fresh stage13 only after12 passes: expect fence magic, rptr/wptr11, guards
   intact, both frozen records. Stop climbing on any failure/reset.
5. Same boot11->12->13 with report fetches and a delay before13; repeat13 and
   `--read-reports` must preserve frozen results without new hardware access.
   Keep the permanent request0x49 refusal check. Coordinator performs temporary
   boots only; this lane performs no device operation.

## M2/M3 implementation history

Stages 1–7 passed on Fuxi (coordinator runs gpu-01, gpu-03..07): CX on,
PLL1 locked after 6 polls, GMU reset=1, TCM retention=0, RSC idle, HFI not ready.
SMMU ID0/1/2 = `0x4c017e18 / 0x40000016 / 0x5511` (24 SMRs, 22 CBs,
4K register pages, CB base +0x20000, 36-bit OAS); sCR0 `0x002f0406`.
These are observations copied from the handoff, not new device results.

Reference: the supplied Linux msm tree under
`linux-6.12/drivers/gpu/drm/msm/`,
`adreno/a6xx_gmu.c` (memory_probe/fw_load/fw_start/start/rpmh_init),
`a6xx_hfi.c` + `.h` (queue layouts, messages, ARC votes, A740 BW table),
`a6xx_gpu.c` (OOB lifetime), `registers/adreno/a6xx_gmu.xml`, plus
`drivers/iommu/arm/arm-smmu/arm-smmu.{c,h}` and `drivers/soc/qcom/cmd-db.c`.
The supplied source has A740 support beyond vanilla upstream v6.12; its local
register definitions are authoritative for this port. Upstream comparison:
[Linux v6.12 GMU](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/adreno/a6xx_gmu.c).
UniXNU-sheng is a secondary register cross-check, not a device result for Fuxi.

## 1. Fuxi-verified GPU facts (facts vs. guesses)

Source of "live" facts: `research/device/probe-09/live.dtb` (the booted Fuxi
DTB) read with `tools/fuxi-inspect-dtb.py`. Register maths: pinned Linux
`42f3b40c`. "sheng" = the reference tablet; differences are called out.

### Identity / memory map (FACT, live DT)
| Item | Value | Source |
|------|-------|--------|
| GPU node | `qcom,kgsl-3d0@3d00000`, compatible `qcom,adreno-gpu-gen7-2-1` (A740 v2) | live DT |
| chip_id | `0x43050a01` ("C510v2") | live DT + a6xx_catalog.c (same as sheng) |
| GPU reg window | `0x03d00000` size `0x40000` | live DT |
| gpucc | `0x03d90000` size `0xa000` (qcom,kalama-gpucc) | live DT |
| GCC | `0x00100000` (kalama-gcc-v2); gpu branch page `0x171000`, gpll0 vote `0x152000` | live DT + gcc-kalama.c |
| adreno SMMU | `kgsl-smmu@3da0000` size `0x40000`, `qcom,qsmmu-v500`+`qcom,adreno-smmu`, dma-coherent, 3-lvl | live DT |
| GMU node | `qcom,gmu`, reg `gmu`=`0x3d68000/0x37000`, `gmu_pdc`=`0xb280000/0x10000`, `gmu_ao_blk_dec0`=`0x3d40000/0x10000` | live DT |
| GMU core base used by probe | `0x3d6a000` (upstream/sheng); Fuxi's `0x3d68000/0x37000` node **contains** it — same registers, same absolute addresses | note |
| rscc | `0x3d50000`; cx_misc/cx_mem `0x3d9e000` | live DT / sheng |
| cmd-db reserved region | `aop_cmd_db_region@81c60000` reg `0x81c60000` size `0x20000`, `no-map` | **live DT (FACT)** — matches sheng `0x81c60000/0x20000` |
| zap reserved region | `gpu_micro_code_region@9b09a000` size `0x2000`, `no-map` | live DT (same as sheng) |

### SMMU stream IDs / banks (FACT, live DT)
- `kgsl-iommu` context banks: `gfx3d_user` SID 0, `gfx3d_lpac` SID 1,
  `gfx3d_secure` SID 2 (mask 0). GMU: `iommus = <… SID 5>`.
- `qcom,actlr = <0 0x3ff 0x32b>`; QTB `gpu_qtb@3de8000` stream-id-range `0 0x400`.
- Driver-chosen (from sheng, GUESS-but-matches-arm-smmu-qcom): GPU uses **bank 0,
  ASID 2**, SMR slots 0/1 → SIDs 0/1; GMU uses **bank 1, ASID 1, SMR slot 2 → SID 5**.
  This was a reference assumption, not a device observation. Milestone 2
  selects an unused disabled bank, a free SMR and an unused ASID from stage 8a;
  bank 0 is deliberately left for the GPU. Existing SID5 matches cause refusal.

### IRQs / clocks (FACT, live DT)
- GPU IRQ SPI 300 level. GMU: hfi SPI 304, gmu SPI 305.
- gpucc GDSCs: `gpu_cc_cx_gdsc` gdscr `0x9108` (gds_hw_ctrl `0x953c`, VOTABLE,
  RETAIN_FF), `gpu_cc_gx_gdsc` gdscr `0x905c` (owned by the GMU — never touched
  by CPU here). Branch/PLL/RCG offsets confirmed against gpucc-sm8550.c:
  ahb `0x911c`, cx_gmu `0x913c`, hlos smmu vote `0x7000`, cxo_aon `0x9004`,
  demet `0x900c`, cxo `0x9144`, hub_cx_int `0x9148`, gmu RCG `0x9318`,
  hub RCG `0x93ec`, PLL0 `0x0`, PLL1 `0x1000`.
- GCC gpu branches (page `0x71000`): cfg_ahb `+0x004` (critical, hwcg), memnoc
  `+0x010`, snoc_dvm `+0x018`, ddrss_gpu_axi `+0x154`; gpll0 votes in `0x52000` bits 15/16.

### Power levels (FACT, live DT `gpu-pwrlevels-0`, initial level 8 = 220 MHz)
9 levels, freq/level(ARC): 719.6 MHz/0xe0, 680/0xc0, 615/0x90, 550/0x80,
475/0x50, 401/0x40, 348/0x3c, 295/0x38 (corrected 2026-10-20 from live DT; bin not yet confirmed), 220/0x34. GMU opp table: 500 MHz/SVS,
200 MHz/LOW_SVS. gmu-freq-table: 220 MHz vote 0x40, 550 MHz vote 0x80.
(Upstream sm8550.dtsi GPU opp table differs slightly in top bins; Fuxi's own
`gpu-pwrlevels-0` above is authoritative and is what the perf table must use.)

### Firmware (stored, NOT committed — under `research/firmware/gpu/`, gitignored)
| File | SHA256 | Bytes | Source |
|------|--------|-------|--------|
| a740_sqe.fw (sheng, **default**) | `6d38046654333a6a5a4ee77ab67abc24e3c0c5c68237513afe98e94da8fc9fcc` | 75380 | github ianchb/sheng-firmware master |
| gmu_gen70200.bin | `1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98` | 66844 | ianchb/sheng-firmware |
| a740_sqe.fw (linux-firmware, newer) | `96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473` | 76724 | gitlab kernel-firmware main `30979b11` |
| gmu_gen70200.bin (linux-firmware) | identical to sheng | 66844 | " |

- SQE is copied into the ring's SQE BO **without its first dword**
  (`adreno_fw_create_bo`). GMU fw is the block format `{addr,size,type,value}`
  + payload: ITCM `{0,0x4000}`, icache `{0x4000,0xffc000}`, DTCM
  `{0x10004000,0x4000}`; the file is 11 zero-size prealloc headers + 4 data blocks (ITCM 0x0 0x1dc, icache 0x4000 0x10210, DTCM 0x10007f98 0x28, 0x10007fdc 0x18); prealloc needs the whole icache window backed (corrected 2026-10-20).
- Fuxi's own vendor blobs (`a740_sqe.fw`, `gmu_gen70200.bin`, and the signed
  `a740_zap.*`) are **not available locally** — no Fuxi vendor image was
  extracted. Any future zap step would need a **Fuxi-signed** zap; sheng
  reached the desktop *without* the zap (`sheng-gpu-zap=0`), so milestone 1 and
  the first userspace attempt can skip it. GUESS: Fuxi's SQE/GMU blobs are
  byte-identical to the generic a740 ones (same chip_id), so the sheng/
  linux-firmware copies are expected to work; confirm by SHA against a Fuxi
  vendor image when one is dumped.

### Differences from sheng
- GMU DT node base `0x3d68000/0x37000` (Fuxi) vs `0x3d6a000/0x35000`
  (sheng/upstream) — same block, Fuxi's starts 0x2000 lower; absolute register
  addresses are identical, so the `dword*4 - 0x6a000` offset math is unchanged.
- cmd-db region, zap region, chip_id, SIDs: identical to sheng (confirmed FACT).
- Fuxi `gpu-pwrlevels-0` is the downstream 9-level table (above); build the HFI
  perf table from it, not from the upstream dtsi opp table.

---

## 2. Stages and USB interface

GPU code stays in `shim/fuxi-probe/gpu.c`, included by `probe.c`. The dispatch
changes only add page selection and the XNU handoff guard. Loader MMU/caches
are off, VA==PA. CPU accesses are uncached; every publication has barriers.
New/possibly secure MMIO reads use `probe_safe_rd32`. M3 also uses
`probe_safe_wr32` for every new MMIO write. Both recover local synchronous
aborts and record ESR/FAR; interconnect hangs or asynchronous SError can still
reset the SoC, and a software timeout cannot contain them.

| Stage / wIndex | Operation | Risk / gating |
|---|---|---|
| 1 | Existing gated GCC/GPUCC/SMMU/GPU snapshot | Read-only, verified |
| 2 | Enable GCC cfg_ahb | Low, verified |
| 3 | CX GDSC + AHB/SMMU vote clocks | Medium, verified |
| 4 | Powered SMMU globals + CB0/1 | Read-only, verified |
| 5 | GMU clock/reset snapshot | Read-only, verified |
| 6 | PLL/GMU/hub clocks, cold prerequisites | Medium, verified |
| 7 | cmd-db snapshot/resource lookup | Read-only, verified |
| **8a / 0x008a** | Snapshot all 24 SMR/S2CR pairs and all 22 CBs with CX on | Low additional risk: new register reads; preceding 2/3/6 still write |
| **8** | Validate firmware/prealloc extents, select unused context/slot/ASID, build 4K tables in fixed RAM, map SID5, FAL veto + TCM retention, load/read back ITCM/DTCM/icache | High: first SMMU programming and about 17 MiB of scratch writes; no CM3 start |
| **9** | Build cmd-db ARC votes, RSC/PDC init, HFI table, cold GMU config/start, init wait, HFI ready, PERF -> BW -> CORE_FW_START -> START ACKs | High: firmware runs and accesses RAM; permanent handoff lock |
| **10** | Blocking low-frequency vote, GPU_SET OOB, wait GX, read RBBM_STATUS, release OOB | High, device-verified |
| **11** | Snapshot/live ownership checks; separate GPU bank/ASID, SMRs for SIDs0/1, GPU page tables and buffers | High: new SMMU context, no SQE start; requires10 PASS |
| **12** | Hold GPU_SET, A740 CP/UCHE/GBIF/HWCG setup, SQE fetch, a7xx CP init, no-zap TRUST_CNTL fallback | Highest: first GPU DMA/CP execution and potentially protected writes; requires11 PASS |
| **13** | Append CP_MEM_WRITE, poll CPU fence, rptr/wptr, idle and faults | High: GPU write confined by mappings to scratch; requires12 PASS |

Request: type `0xc0`, request `0x56`, wValue `0x4753`. Stages1..13 and `0x8a`
run through the requested limit and return page0 (512 bytes). Invalid indices
stall. **Stage8a requires stage7 valid**, and 8–13 require its powered snapshot.
A failed stage never advances. After 8a the snapshot is frozen; each mutating
stage8..13 is attempted at most once. Later limits may advance the sequence;
repeating a failed stage requires a fresh boot. Requests for stages1..7 after
8a return the existing snapshot, avoiding PLL/TCM reinitialization.

Read-only report fetches: wIndex **0x0100..0x0107** select pages0..7. They only
copy RAM, do not run stages or refresh MMIO. All unused words are zero. Snapshot
pages2..5 are from immediately before the first stage8 write. Page1 live fields
are sampled at the end of each stage8a/8..13 request, not on page fetch.

**XNU request 0x49 is refused (USB stall) from immediately before CM3 reset is
released onward, including init timeout, ACK failure and subsequent requests.**
There is no unload/reset operation that clears this latch. Retain tables and
buffers until the normal loader reset. M3 adds GPU user SIDs, SQE, a fixed ring and a single fence command. No zap
shader, secure command submission, rendering or XNU GPU driver is included.
After stage12 obtains GPU_SET it is retained until reset, including errors: no
IFPC/power collapse is allowed to destroy CP state between stage12 and13.
Diagnostic snapshots are frozen at stage12/13 completion (or failure while GX
is confirmed on); report-page fetches never touch hardware.

Timeouts: TLB sync 1000 polls × 1 us; firmware/HFI/GX init each 1000 × 10 us;
each HFI ACK has one 50 ms counter deadline, including all receive handling.
MMIO overhead adds elapsed time; all loops remain bounded. Firmware is validated
in full before any buffer/context write, and every data dword is read back.
Malformed responses, unsolicited ACKs, firmware errors and queue corruption stop
startup rather than being drained without a bound.

## 3. Fixed loader-only DRAM ownership

Reserve **[0x8f0000000, 0x8f2000000)** (32 MiB), inside the already proven
`[0x8c0000000, 0x900000000)` XNU RAM window. The loader has not copied XNU into
this window while these probes run. The runtime checks this reservation against
`_head.._end` (including embedded payload/stacks/USB state) and the incoming FDT
before writing it. No storage or partition operation is involved. It is a fixed
loader scratch reservation, **not a Linux DT reserved-memory allocation**.

| Use | GMU VA | Physical base | Bytes |
|---|---:|---:|---:|
| SMMU table pool | not mapped | 0x8f0000000 | 0x100000 reserved; 13 pages used |
| icache (privileged RW/execute) | 0x00004000 | 0x8f0100000 | 0xffc000 |
| dummy (privileged RW) | 0x60000000 | 0x8f1100000 | 0x2000 |
| log (RW, unprivileged allowed) | 0x60002000 | 0x8f1102000 | 0x4000 |
| HFI (RW, unprivileged allowed) | 0x60006000 | 0x8f1106000 | 0x4000 |
| debug (privileged RW) | 0x60400000 | 0x8f110a000 | 0x7000 |

Unmapped holes stay unmapped. No large `.bss` buffers are added (the linker puts
`.bss` before `.payload`). The existing 128 KiB cmd-db copy remains in `.bss`.
Three-level 4K tables cover 32-bit IOVA/36-bit PA. Leaves use normal noncacheable
MAIR index0=0x44 with inner sharing and AF; walks are noncacheable/outer shared.
CBAR=0x1f300 (stage1 translation, stage2 bypass), CBA2R=1,
TCR2=0x38011, TCR=0x80802020, MAIR0=0xff44, SCTLR=0x1067.
Readback permits only the reference-observed TCR2 TBI bits5/6 reading one and
SCTLR ASIDPNE bit12 reading zero; all other configured bits must match.
TTBR0 = root PA | ASID<<48; TTBR1 disabled. TLBASID + bounded TLBSYNC precedes
M=1, then S2CR translate/default privilege and exact SMR SID5 are published.
GR0 and all unrelated banks/streams are preserved. Bank0 is excluded; the first
unreferenced disabled stage1 bank (starting at1), invalid SMR and unused nonzero
ASID are selected. Any valid SMR whose mask matches SID5 causes refusal.
The ID0 ATOSNS bit is set: no SMMU hardware translation-op test is attempted.
The actual DMA translation proof must come from firmware boot/HFI on the phone.

Firmware matches Linux `a6xx_gmu_fw_load`: 11 zero-size prealloc headers and
4 data blocks, ITCM 0/0x1dc, icache 0x4000/0x10210, DTCM
0x10007f98/0x28 and 0x10007fdc/0x18. Prealloc extents inside ITCM/DTCM/icache/
dummy are checked but do not copy payload. Total data bytes=0x1042c (66604).
No firmware bytes are tracked or copied into tracked paths.

## 4. New report word map

Each page has 128 little-endian u32. Page0 preserves every old word (below),
adding **[2] latest stage entered**, **[3] permanent GMU-start latch**.
Stage8a is encoded as 0x8a in page0[1]/[2].

Page1 (selector 0x101), indices local to that page:
```
[0] 0xa7400003 (report format v3; existing fields unchanged)
[1] snapshot status (1 complete) [2] stage8 result [3] stage9 result
[4] GMU-start/handoff-lock latch
[5] SMR count [6] CB count [7] register page stride [8] CB region offset
[9] pre-stage8 GFSR
[10] chosen SMR slot [11] CB [12] ASID [13:14] table PA low/high
[15] context programmed (configuration issued; not itself success)
[16] last TLBSTATUS [17] firmware bytes [18] data block count
[19] zero-size/prealloc count [20] data bytes [21] bad address [22] bad size
[23] CBAR [24] CBA2R [25] TCR2 [26] TCR [27:28] TTBR0 low/high
[29] MAIR0 [30] SCTLR [31] published SMR [32] published S2CR
[33] table pages used
[34] DTCM firmware version word [35] expected init result
[36] observed FW_INIT_RESULT [37] HFI_CTRL_STATUS [38] HFI_VERSION_INFO
[40:43] PERF_TABLE ACK record
[44:47] BW_TABLE ACK record
[48:51] CORE_FW_START ACK record
[52:55] START ACK record
[56:59] GX_BW_PERF_VOTE ACK record (stage10)
  each: sent header / response header / returned command header / error
[60] stage10 result [61] OOB interrupt sample [62] SPTPRAC status
[63] GX GDSCR [64] RBBM_STATUS [65] RBBM read valid
[84] GMU2HOST_INTR_INFO [85] AO_HOST_INTERRUPT_STATUS [86] AHB_FENCE_STATUS
[87] live global SMMU GFSR [88] chosen CB FSR [89:90] FAR low/high
[91:92] FSYNR0/1
[93:94] command queue read/write [95:96] response queue read/write
[100:109] GPU ARC votes, OFF then 220/295/348/401/475/550/615/680/719 MHz
[110:112] GMU ARC votes, OFF/200/500 MHz
```

Page2 (0x102): `[2*i] SMR(i), [2*i+1] S2CR(i)`, i<count (Fuxi24,
geometry limit32). Additional context controls: `[64+b] ACTLR(b)` and
`[88+b] CONTEXTIDR(b)`, b<CB count. ACTLR is observed and preserved.
Snapshot fault accounting lives in **page2[48..52]**, not page1 (where those
indices hold HFI ACKs): count, first ESR, first SMMU byte offset, stream-slot
fault mask, bank fault mask.

Pages3..5 (0x103..105): eight banks per page; bank b starts at
`page=3+b/8, word=16*(b%8)`, with these 16 fields:
```
CBAR, CBA2R, SCTLR, TCR2, TTBR0lo, TTBR0hi, TTBR1lo, TTBR1hi,
TCR, MAIR0, MAIR1, FSR, FARlo, FARhi, FSYNR0, FSYNR1
```

Healthy expectations for stages8–10 (now confirmed except FSR; see Device results):
- 8a: page1[1]=1, [5:8]=24/22/0x1000/0x20000. Review **all** streams and
  enabled banks before the first stage8 run. Existing SID5 match means stop.
- 8: [2]=1, firmware 66844 B/4 blocks/11 prealloc/66604 data B, 13 table pages;
  SMR=0x80000005, S2CR=selected CB, TTBR=0x8f0000000 | ASID<<48,
  TCR etc as above; chosen FSR has no defined fault bits. GMU latch remains zero.
- 9: [3]=1, [4]=1. Init is `0xbabeface` for DTCM version <=0x20010004,
  otherwise `(result & 0x1ff)==0x100`. HFI status bit0=1. Four ACKs must have
  id126 **or echoed command id**, type1, exact returned command headers and error0. Sent headers are
  `0x00103b04 / 0x0020a003 / 0x0030020e / 0x0040010a`.
- 10: [60]=1, vote ACK error0 (`sent=0x0050041e`), OOB sample bit31=1;
  `(SPTPRAC & 0xc3)==0` (Linux GX-off bits6/7 plus transition bits0/1),
  GX GDSCR bit31=1, [65]=1. RBBM_STATUS is evidence only: no exact idle
  value is promised before GPU hw_init. CB1 FSR=0x400 was observed, unexplained; defined fault bits stay clear.

Results: 0=not run, 1=success. Codes (hex):

| Code | Failure |
|---|---|
| 801 | Unsupported SMMU geometry/capability/client-disabled state |
| 802 | No externally embedded GMU firmware |
| 803 | Malformed/truncated/alignment/unmapped data or prealloc extent |
| 804 | Existing exact or masked SMR matches SID5 |
| 805 | No unused disabled CB/free SMR/unused ASID, or bank became active |
| 806 | Stage8 cold prerequisites not met |
| 807 | Scratch overlaps runtime image or incoming FDT |
| 808 | SMMU TLB sync timeout |
| 809 | SMMU context/stream readback mismatch |
| 80a | Firmware dword readback mismatch |
| 80b | Page-table pool exhausted/overlapping mapping |
| 80c | Chosen CB fault after firmware load |
| 901 / 902 | Firmware init / HFI init timeout or CM3 fault |
| 903 | HFI ACK deadline expired |
| 904 | Corrupt indices, queue full, bad/partial/oversize response |
| 905 | Firmware error packet or CM3 fault |
| 906 | ACK id/type/returned header/sequence mismatch |
| 907 | ACK nonzero error (record holds firmware error) |
| 908 | Missing/malformed cmd-db aux data, uncovered ARC level or missing BW resource |
| 909 | Cold state lost before stage9 start |
| a01 / a02 / a03 | GPU_SET timeout/fault / GX status timeout/fault / GX GDSC still off |
| a04 | Chosen CB fault before GX request |

HFI failures in stage10 use 903..907 in [60]. No destructive recovery is
attempted after failures; reset before changing firmware/tables or retrying.

### Legacy page0 word map (128 × u32, little-endian)
```
[0]  magic 0xa740     [1]  stage_limit
Stage 1/read-only:
[4] cfg_ahb [5] memnoc [6] snoc_dvm [7] gpll0_vote
[8] cx_gdscr [9] gx_gdscr [10] ahb_clk [11] smmu_vote [12] cx_gmu [13] gpucc_read
[14] gpu_read [15] smmu_read
[16] scr0 [17] id0 [18] id1 [19] id2 [20] gfsr
[21] smr_valid|(first<<16) [22] smr_first [23] s2cr_first [24] rbbm_status
Stage 2: [26] wrote_enable [27] cfg_ahb_after
Stage 3: [28] cx_gdscr_before [29] gds_hw_ctrl [30] cx_gdscr_after
         [31] ahb_clk [32] smmu_vote [33] smmu_on
Stage 4: [35] done [36] scr0 [37] id0 [38] id1 [39] id2
         [40..43] CB0 cbar/sctlr/fsr/tcr   [44..47] CB1 cbar/sctlr/fsr/tcr
Stage 5: [48] pll1_mode [49] pll1_opmode [50] pll1_L
         [51] gmu_rcg_cmd [52] gmu_rcg_cfg [53] hub_rcg_cmd [54] hub_rcg_cfg
         [55] cx_gmu [56] cxo [57] hub_cx_int [58] demet [59] axi [60] gpll0_vote
         [61] gmu_sysreset [62] cxmisc_tcm_ret [63] rscc_seq_busy [64] hfi_status
         [79] gmu_regs_read (1 = GMU core was readable in stage 5)
Stage 6: [65] result (0 not-run, 7 all-pass; 3 PLL1 lock t/o, 4 hub RCG t/o,
             5 gmu RCG t/o, 6 cx_gmu CLK_OFF t/o)
         [66] gpll0_voted [67] pll1_lock_polls [68] cx_gmu [69] cx_gmu_polls
         [70] pll1_mode [71] pll1_opmode [72] gmu_cfg(&0x7ff) [73] hub_cfg(&0x7ff)
         [74] gmu_sysreset(want 1) [75] tcm_ret(want 0) [76] rsc_busy(want 0)
         [77] hfi_status(want bit0=0)
Stage 7: [80] cmddb_version [81] cmddb_magic(want 0x0c0330db) [82] slaves
         [83..90] addresses: gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV, CN0
```
A healthy stage-6 result is `[65]=7` with `[74..77] = 1/0/0/even`, i.e. the GMU
is powered, clocked and held in reset, RSC idle, HFI not yet ready — exactly the
cold-start precondition `ShengGmuLoadAndStartHfi` checks.

## 5. HFI and implementation assumptions

The supplied Linux uses **two** queues (command0, response1), not the three in
the old lane plan. HFI BO: 16 KiB, header at0 (six u32), two 12-u32 queue
headers at24/72 bytes, 4 KiB rings at+4096/+8192. Table `size=120` follows the
Linux implementation's bytes, despite its header comment saying dwords. Rings
have 1024 words, leave one slot empty, align packets to four words and pad
commands with 0xfafafafa. Header size counts unpadded words. Only matching
id126/type1 ACKs with error0 count as success.

The perf table contains OFF plus the nine requested live-DT GPU frequencies in
ascending order (719 MHz represented as 719000 kHz per task; handoff calls top
719.6 MHz). ARC levels = 0/0x34/0x38/0x3c/0x40/0x50/0x80/0x90/0xc0/0xe0.
GMU OFF/200/500 MHz with levels0/0x40/0x80 follows its Linux OPP table. Primary
indices and levels come from gfx.lvl/cx.lvl aux data; secondary from gmxc.lvl
if present, otherwise mx.lvl. Encoding is `primary_level<<16 | secondary_idx<<8
| primary_idx`, including Linux's highest-nonzero secondary fallback.
Every unpopulated GX ACD field is zero, each populated entry uses 0xffffffff,
as Linux does. A740 BW table has one DDR off entry for SH0/MC0/ACV and two CN0
entries off/on; stage10 requests **BW index0**, the only DDR entry, with GPU
perf index1 (220 MHz). It never requests the high GPU frequencies.

Stage9 cold RSC/PDC sequence uses the A740 sequence-memory offset0x154,
hidden data0x80000021 and AOP-resident PDC (enable/start only). No warm-resume
path. GMU chip ID config=0x07020100. It sets FAL veto/TCM retention, boot2,
QTBL, fence range, CM3_CFG=0x4052 and log address as Linux fw_start does.
SYS_BUS/ICACHE/DCACHE config=1. GBIF QSB registers are **not** touched before
GX: Linux sets those during later GPU hw_init. IRQs are masked and polled.
No host GIC setup, zap, or secure-mode calls.

Unverified assumptions/limits: the generic GMU blob matches Fuxi; selected CBs
are writable in this boot's hypervisor environment; the fixed high RAM window
is reachable from the GMU SMMU; the documented GPU-pwrlevels-0 bin is the active
one. No QMP AOSS message to disable ACD is sent; firmware defaults and Linux's
ACD vote fields are used. No special IFPC override is added (Linux A7xx returns
early from power_config). These require device evidence, especially stage9
startup and stage10's live GX status. Offline mocks do not prove power or DMA.

## 6. Build, validation, and coordinator test order

Optional environment inputs: `FUXI_GMU_FIRMWARE=/absolute/local/path` and
`FUXI_SQE_FIRMWARE=/absolute/local/a740_sqe.fw`. The build
writes an `.incbin` assembly file **only in ignored shim/out/fuxi-loader**;
the input remains external. Omitting the variable emits empty symbols, builds
normally, and stage8 reports802 without touching scratch or the SMMU context.
With GMU embedded but SQE omitted, stage11 is available and stage12 returns
`c01` before GPU_SET/hw_init. Standalone fuxi-probe builds remain supported
without embedded firmware (stage8 returns802).

```sh
FUXI_GMU_FIRMWARE=$FW/gmu_gen70200.bin \
FUXI_SQE_FIRMWARE=$FW/a740_sqe.fw \
CLANG=$LLVM/clang \
LLD=$LLVM/ld.lld \
OBJCOPY=/usr/bin/llvm-objcopy-18 READELF=/usr/bin/llvm-readelf-18 \
NM=/usr/bin/llvm-nm-18 MKBOOTIMG=$MKBOOTIMG_DIR/mkbootimg.py \
bash shim/fuxi-loader/build.sh shim/out/fuxi-integ1-payload.bin
```

The M2 artifact/history remains in Device results and git history. Current M3
artifact, size, hash and ELF geometry are recorded in the final handoff below.
The supplied integ1 payload and firmware inputs remain external/read-only;
no firmware or boot image is tracked. Loader builds enforce `-Werror`.

Offline checks completed: firmware-enabled loader, no-firmware loader and
standalone probe all build with **zero warnings**; relocation/Image-size guards
pass. Native C memory model under UBSan and `-Wall -Wextra -Werror` passes with
synthetic firmware and the exact external blob; checks all icache PTEs and holes,
privilege attributes, firmware format bounds/readback, bank/ASID ownership,
masked SID collision, TLB timeout/readback failure, HFI startup order, ACK
errors/sequence/size, ring wrap/full/invalid index, bounded init/ACK timeouts,
and permanent GMU-start latch after init failure. M3 adds separate GPU table
leaves/permissions/holes, two-stream ownership, changed/unreadable ownership
refusal, TLB timeout/readback, CP packet contents, SQE header removal/readback,
rptr/idle/fence completion, fence guards, contained SECVID write abort, defined
FSR faults vs reserved bit10, and no retry/no next stage after failure. Python decodes all eight model
report pages offline. It does **not** execute firmware or emulate power/SMMU.

```sh
python3 tools/test-fuxi-gpu-probe.py
python3 tools/test-fuxi-gpu-probe.py --firmware /absolute/local/gmu_gen70200.bin \
  --sqe /absolute/local/a740_sqe.fw
python3 tools/fuxi-gpu-probe.py --decode /private/captured-report.bin
```

Coordinator test order is in the M3 handoff below. The main checkout's
`research/run-gpu.py` is read-only for this lane and unchanged. It mounts the
**main checkout's tool**, so integrate this lane's decoder before using that
runner, or use this worktree's tool against an already temporarily booted probe.
Pass the explicit M3 image path. The runner was not executed here.

## 7. Remaining work

Run coordinator tests 11, 12, 13 with fresh boots between the initial bisects,
then prove same-boot 11→12→13 advancement. Hardware outcomes are unknown.
Rendering, XNU DMA ownership, an XNU GPU driver and userspace acceleration remain
future work. The GMU-start handoff lock must stay: this scratch still overlaps
XNU's RAM window, and both processors retain DMA access until reset.

## Device results (2026-10-20, coordinator)
- 7 PASS (m2). 8a: first m2 boot reset the SoC (sync abort -> fatal -> PSCI reset), not
  reproduced on m2b, which reads the snapshot through `probe_safe_rd32` (0 faults; **page2[48..52]**, corrected from the old handoff).
  Every SMR invalid, S2CR 0x20000, CBAR 0, no bank enabled.
- 8 PASS (slot0 -> CB1, ASID1, 13 tables, fw read back).
- 9: m2b 0x906 - the gen70200 GMU ACK header echoes the command id (0x00010304), not 126.
  m2c accepts either (Linux checks only ret_header seq). m2c 9 PASS, all ACK errors 0.
- 0x49 after GMU start: STALL. 10 PASS: GX_GDSCR 0xf8222800, RBBM_STATUS 0x1.

## Milestone 3: layout, source choices and expected results

The following are implementation facts and predictions, **not new Fuxi device
observations**. The table pool helper is shared with M2 but receives an explicit
root/pool cursor; the native model checks that M3 leaves the GMU root unchanged.
The fixed reservation overlap check in stage8 is a prerequisite of stage11.

| Use | GPU VA | Physical base | Bytes / permission |
|---|---:|---:|---|
| GPU table pool | unmapped | `0x8f1200000` | 1 MiB reserved, 3 × 4K pages used |
| SQE code | `0x40000000` | `0x8f1300000` | 128 KiB, GPU read-only (both privilege levels) |
| Ring | `0x40020000` | `0x8f1320000` | 32 KiB, RW |
| BR/BV rptr shadows | `0x40028000` | `0x8f1328000` | 4 KiB, privileged RW; offsets 0/4 |
| Fence/scratch | `0x40029000` | `0x8f1329000` | 4 KiB, RW; magic at 0, guards at 4/0xffc |

All are inside `[0x8f0000000,0x8f2000000)`. GMU-owned memory ends at
`0x8f1111000`; M3 does not touch it. M3 uses TTBR0 only, 32-bit GPU VAs,
36-bit PA, 4K granules, NC memory and walks, AF/inner-shareable leaves.
All other GPU VAs are unmapped, including the GMU buffers/tables. SQE's leaf AP
is 3 (read-only); ring/fence AP 1; rptr AP 0. This is a small loader address space,
not Linux's split TTBR0/TTBR1 16 GiB process address-space design.

Stage11 checks **both** SID0 and SID1 for exact/masked collisions: supplied
`arch/arm64/boot/dts/qcom/sm8550.dtsi` attaches both to the GPU device. There is
no LPAC submission/start. Two invalid readable SMRs are selected. Snapshot and
live stream entries must agree, except the expected M2 SID5 entry. A snapshot
stream fault refuses selection because a hidden masked match cannot be ruled
out. Banks must be readable, disabled in the snapshot and live, unreferenced by
any live translating stream, at/after the stage1 banks, and neither bank0 nor
the GMU bank. ASIDs exclude the GMU ASID and every readable enabled bank's two
live TTBR ASIDs. Faulted banks are not candidates. ACTLR/CONTEXTIDR are retained
and reported; M3 does not guess a replacement implementation ACTLR value.
Stage11 writes/validates the same context format as verified stage8, invalidates
by the chosen ASID, waits for TLB sync, enables M, then publishes S2CR/SMR last.
Expected with the observed empty 8a: **slots1/2, CB2, ASID2**, root
`0x8f1200000`, TTBR0 `0x00020008f1200000`; this selection is not hardcoded.

Stage12 ports the CP/GBIF/UCHE subset of supplied `adreno/a6xx_gpu.c:hw_init`,
`a6xx_ucode_load`, `a7xx_cp_init`. `gpu-a740.h` holds source A740 HWCG values
and the A730 protection table used by Linux's A740 catalog (slots45/46 remain
untouched). It clears GBIF halts/trusted range, applies A740 HWCG/GMU CGC,
GBIF QoS, UCHE no-bypass/trap/GMEM range (16 MiB + 3 MiB), cache ways, AHB error
response, hang detection, UCHE queue/NC mode and expanded BR/BV/LPAC APRIV.
No GIC/interrupt delivery: `RBBM_INT_0_MASK=0`, raw status is polled. Graphics
pipeline UBWC setup, LLCC optimization, performance counter routing, preemption
and IFPC restore lists are omitted: no rendering or preemption is submitted.
Whether the remaining reset defaults suffice is a **device-test hypothesis**.

`FUXI_SQE_FIRMWARE` embeds a local SQE blob in the ignored output. Stage12
checks size/alignment/capacity and copies/readbacks all bytes **after dropping
the first dword**, as `adreno_fw_create_bo` does. The supplied file is 75380 B,
copy 75376 B / 18844 dwords. The 32 KiB ring uses `CP_RB_CNTL=0x20c`; BR rptr
shadow is at `0x40028000`, BV at `+4`. It requires reset rptr/wptr=0, starts
`CP_SQE_CNTL=1`, publishes a barrier then wptr 10, and waits for rptr=wptr=10
and `(RBBM_STATUS & ~1)==0`. Command dwords:
```
70970001 08000000                       CP_THREAD_CONTROL, disable binning
70c80007 0000014b 00000003 20000000       CP_ME_INIT (7 payload words)
00000002 00000000 00000000 00000000
```
No zap is loaded. Linux's A740 catalog specifies `a740_zap.mdt`; its generic
`hw_init` attempts zap **after** CP init, then handles `-ENODEV` by writing
`RBBM_SECVID_TRUST_CNTL=0`. This probe explicitly takes that no-zap fallback,
with safe read-before/write/read-after and raw A7xx TSB status. It sends no
`CP_SET_SECURE_MODE`, calls no SCM/PAS, and never maps secure SID2. A contained
permission fault is `c09`, nonzero trust readback is `c0a`; raw RBBM interrupt,
CP protection/HW fault, trust and TSB bits are retained. **Fuxi permission and
result bits are unknown until stage12 is run.** Safe writes only contain local
synchronous aborts; asynchronous violations/hangs can still reset the SoC.

Stage13 appends exactly four dwords (no wrap or buffer reuse):
```
703d8003 40029000 00000000 a740f00d       CP_MEM_WRITE, fence VA, magic
```
Expected wptr/rptr 14 and CPU fence 0→`0xa740f00d`, both guards `0x5ac3e17b`, idle
RBBM (`status & ~1` zero), CP interrupt error bits/global FSR/defined CB FSR
bits clear. Shadow rptr 10/14 is useful evidence but register rptr is the gate;
there is no guessed SQE_STAT[1] value. `CP_HW_FAULT`/protection should have no
active error indicated by CP_INTERRUPT_STATUS; raw values are reported.
Defined CB fault mask is `0xc00001fe` (same as M2). The observed CB1 FSR 0x400
is preserved in reports and ignored as a reserved SMMUv2 bit; its cause is still
unknown. Any defined fault in **either** bank stops submission/completion.

Every stage is attempted once and only after the previous PASS. There is no
rollback or recovery: partial tables/context/SQE state is retained for diagnosis.
GPU_SET ACK and GX transition waits are each 1000×10 us; GPU TLB sync 1000×1 us;
CP init and fence waits each 5000×10 us (50 ms nominal delay budget plus MMIO
cost), checking CP/RBBM/both-bank/global faults every 16 polls and at completion.
All loops are bounded. After stage12 obtains the OOB ACK, GPU_SET stays asserted
through later requests/errors until normal loader reset. Request0x49 still
stalls from stage9 onward, independent of any M3 failure or missing SQE blob.

### M3 report pages and failures

The v3 decoder accepts legacy 512-byte and v2 six-page 3072-byte captures too.
New captures have eight 512-byte pages (4096 B), selected by 0x100..0x107.
Pages0..5 retain their M2 fields; page1[0] becomes `0xa7400003`.
Page6 (0x106), each index a local little-endian u32:
```
[0] 0xa7400003 once 11 entered [1] stage11 attempted
[2] stage11 result [3] stage12 result [4] stage13 result
[5] stage12 attempted [6] stage13 attempted [7] GPU_SET ACK/held latch
[8] GMU CB [9] GMU ASID [10] SID0 slot [11] GPU CB [12] GPU ASID
[13:14] table PA low/high [15] context config issued [16] TLBSTATUS
[17] SQE file bytes [18] copied bytes [19] skipped file dword
[20] first copied dword [21] copied/readback dword count [22] bad file offset
[23] CBAR [24] CBA2R [25] TCR2 [26] TCR [27:28] TTBR0 low/high
[29] MAIR0 [30] SCTLR [31] SID0 SMR [32] SID0 S2CR [33] table pages
[34] SID1 slot [35] SID1 SMR [36] SID1 S2CR
[37:40] SQE/ring/rptr/fence GPU VAs [41] ring bytes [42] initial CP wptr
[43] TRUST_CNTL write completed [44] trust before [45] trust after
[46:47] A7xx TSB_STATUS low/high
[48] init rptr before [49] after [50] submitted/readback init wptr [51] polls
[52] CPU fence before [53] last polled value [54] expected magic
[55] fence rptr before [56] after [57] submitted/readback fence wptr [58] polls
[59] SQE start issued [60] stage12 substep (1 SQE,2 OOB,3 hw_init,
     4 SQE start/CP init,5 TRUST fallback,6 complete)
[61] OOB interrupt [62] SPTPRAC [63] GX_GDSCR
[64:71] SQE/ring/rptr/fence physical low/high pairs
[72] preserved GPU ACTLR [73] CONTEXTIDR
[74:75] GMU FSR before/after11 [76:77] GPU FSR before/after11
[80:89] ten init command dwords [90:93] four fence command dwords
[110:111] last faulting MMIO PA [112] ESR [113:114] FAR
[115] last operation (1 read,2 write) [116] safe MMIO fault count
[117:118] first faulting MMIO PA
```

Page7 (0x107): two frozen64-word records, stage12 at 0 and stage13 at 64.
Snapshot only runs while GX power-on/transition predicates were confirmed.
A failure before that leaves the record zero; page6's substep/OOB/GX/fault
fields remain available. Within each record:
```
[0] stage captured (12/13;0 absent) [1] CP rptr [2] wptr
[3] CPU BR shadow [4] BV shadow [5] CPU fence [6] RBBM_STATUS
[7] CP_SQE_CNTL [8] CP_SQE_STAT_DATA with STAT_ADDR=1
[9] CP_HW_FAULT [10] CP_INTERRUPT_STATUS [11] CP_PROTECT_STATUS
[12] RBBM_INT_0_STATUS [13] SECVID_TRUST_CNTL [14:15] TSB_STATUS
[16] global SMMU GFSR
[17] GPU CB [18] FSR [19:20] FAR [21:22] FSYNR0/1
[23] GMU CB [24] FSR [25:26] FAR [27:28] FSYNR0/1
[29] GMU2HOST_INTR_INFO [30] AO_HOST_INTERRUPT_STATUS
[31] cumulative safe MMIO fault count [32:33] fence guards
```

Results 0=not run, 1=PASS. M3 failure codes (hex):

| Code | Meaning |
|---|---|
| b01 | Safe SMMU MMIO fault or unreadable snapshot stream ownership |
| b02 | Snapshot/live ownership changed, bank became active, slot not free, or GMU stream changed |
| b03 | Existing exact/masked SID0/1 match |
| b04 | No free readable GPU bank, two free SMRs or unused nonzero ASID |
| b05 | GPU table capacity/overlap |
| b06 | GPU context TLB sync timeout |
| b07 | GPU context/stream readback mismatch |
| b08 | Defined fault on either bank at stage11 |
| c01/c02/c03 | Missing SQE / invalid size / payload copy readback mismatch |
| c04 | GPU_SET/GX timeout or MMIO fault |
| c05 | hw_init, CP or diagnostic MMIO fault |
| c06 | CP/RBBM/global SMMU/defined GPU or GMU CB fault |
| c07 | CP init rptr/wptr/idle timeout |
| c08 | Initial CP ring was not reset (rptr or wptr nonzero) |
| c09/c0a | SECVID/TSB access fault / TRUST_CNTL failed to clear |
| d01/d02/d03 | Fence MMIO fault / CP-RBBM-SMMU fault / fence-rptr-idle timeout |
| d04/d05 | Lost ring/fence precondition / guard overwritten |

Coordinator test order (serial, temporary boot only, save private binary/logs):

1. Fresh M3 boot stage7 regression, then fresh stage8a; review all streams/banks
   and page2 fault masks. Fresh stage10 regression, expect known M2 PASSs.
2. Fresh boot **stage11**. Require result 1, GPU CB≠0/GMU CB, different ASID,
   slots1/2 as predicted only if snapshot is unchanged, 3 table pages, context
   readbacks correct, both defined FSR masks zero, no safe MMIO faults. No SQE
   started; request0x49 must still stall because stage9 ran.
3. Fresh boot **stage12**. Require result 1, substep 6, copied 18844 dwords,
   rptr 0→10/wptr 10, OOB held, TRUST write_ok 1/readback 0, no active CP/RBBM/SMMU
   faults. Record actual TRUST-before and TSB status bits; no prior expectation
   of their reset values. Inspect page7's stage12 snapshot.
4. Fresh boot **stage13**. Require stage11/12 PASS and fence result 1,
   CPU0→magic, rptr 10→14/wptr 14 and guards intact. Inspect both frozen snapshots.
5. After fresh-boot bisects pass, same boot **11→12→13**, fetching reports between
   each and delaying between 12/13 to exercise the retained GPU_SET. Repeating 13
   must return existing results without a new submission; `--read-reports`
   returns RAM only. Keep the 0x49 refusal check.
6. Any failure/reset: stop climbing, capture available report pages, record exact
   boot/reset outcome, and wait for normal loader reset. No retries within a
   failed boot; no handoff, partition changes or firmware replacement on device.

Use `tools/fuxi-gpu-probe.py --stage 11` (then12/13 as appropriate), `--raw`,
`--save research/device/<private-run>/gpu.bin`. The new decoder fetches all
8 pages for v3. `--read-reports` never reruns hardware setup.

### M3 build handoff

Offline only: firmware-enabled loader, no-firmware loader, standalone probe
builds: **PASS, zero warnings**, linker/Image/relocation guards PASS. Native
UBSan C model and Python eight-page decode: PASS with synthetic and both exact
external firmware blobs. These checks model MMIO completion; they do not run
SQE/GMU code, translate real DMA, test protected-register permissions or prove
GPU power/CP execution. **No M3 device run, boot result or CI run exists yet.**

Image (this worktree, gitignored):
`shim/out/lane-gpu/fuxi-gpu-m3-boot.img`, **20,865,024 B**.
SHA256: `12b193ecf2ae55f26fe9ca553dca496528a8abcfb9ab047f41207baa856a4cdd`.
Payload SHA256:
`800b9ea17ce18f1872c0533d5ef13f916c490eb7b1524e677fc2555378f00d66`.
ELF `_gpu_fw_start/end=0x1028c/0x207a8` (66844 B),
`_sqe_fw_start/end=0x207a8/0x32e1c` (75380 B),
`_bss_start/end=0x34000/0x64000` (192 KiB),
`_payload_start=0x74000`, `_image_end=0x13e4050`.
No firmware bytes or dumps are committed. Next concrete action: coordinator
stage7/8a/10 regression with this image, then fresh-boot stage11.

### M3b build handoff (current artifact)

Offline firmware-enabled loader, explicitly empty-firmware loader and standalone
probe builds: **PASS, zero warnings** with clang-r547379. Loader `-Werror`,
relocation checks, Image size/header checks and link: PASS. Final native UBSan
model with the exact external sheng GMU/SQE inputs: PASS. Linux source register/
table audit: PASS. Ten-page decoder and client/interrupt labels: PASS; original
private M3 eight-page report still decodes offline. These checks do not execute
SQE code or prove the CP_INIT candidate fix. No phone access or M3b boot test.

Artifact in this worktree (gitignored):
`shim/out/lane-gpu/fuxi-gpu-m3b-boot.img`, **20,865,024 B**.
SHA256: `c645f6d7ffb8740aaead35699adc9c2ea7c5d2721d39db88ff364acd80768a53`.
Kernel Image: 20,856,912 B. Payload unchanged:
`800b9ea17ce18f1872c0533d5ef13f916c490eb7b1524e677fc2555378f00d66`.
Firmware hashes unchanged from the sheng table above. Build uses `$FW`, `$LLVM`,
`$MKBOOTIMG_DIR` and the supplied integ1 payload, per section6. The absolute
input payload can be passed as the build script argument; it need not be copied
into this worktree. No firmware, dump, boot image or local path is committed.

Firmware build ELF geometry: `_gpu_fw_start/end=0x10ec4/0x213e0` (66844 B),
`_sqe_fw_start/end=0x213e0/0x33a54` (75380 B),
`_bss_start/end=0x34000/0x64000`, `_payload_start=0x74000`,
`_image_end=0x13e4050`. Both extra report pages fit existing BSS alignment.
The lane image was copied before the empty-firmware verification build, so the
lane artifact contains **both** blobs. Commit: the commit containing this
handoff (`git log -1` on `codex-gpu-m3b`). Next action: coordinator fresh stage12
after regressions/stage11, using the new phase/PC/SID fields before further
pointer changes. The hardware root cause remains unconfirmed.

## Device results, milestone 3 (2026-10-20)
- m3 (first build): 11 PASS (SID0/1 -> CB2, ASID2). 12: CP instruction fetch at IOVA 0
  (FSYNR0 IND=1, CP_INT BR_ILLEGAL_INSTR). m3b removed the null CP_INIT list: same fault, now
  before the doorbell. m3c root cause: SQE_CNTL was set before CP_SQE_INSTR_BASE, so the SQE
  started fetching from 0. Linux cold ordering (bases verified, then RUN) fixes it.
- m3c 12: CP_INIT consumed (rptr 0 -> 7 = wptr), CP_INT 0, SMMU clean. RBBM_SECVID_TRUST_CNTL
  stays 0x1 after writing 0 (FACT; GUESS: TZ-owned on retail, needs the zap shader via PAS).
  m3d treats this as a report value, not a failure.
- **m3d 13 PASS: CP_MEM_WRITE of 0xa740f00d to the fence page (VA 0x40029000) landed, read by the
  CPU; rptr 7 -> 11, no faults, CP_INT 0, TRUST still 0x1.** The GPU executes PM4 from our ring
  through our SMMU context.
- Open: CB FSR bit 10 (0x400) on both banks from programming time; secure-mode exit; no draw yet.
