# WLAN W9: POR INTVEC initial value and refusal diagnostics

## Findings

The supplied try8 excerpt reaches steps 17–19 with zero access faults, then
ends step 20 at this breadcrumb, without the helper's write/read line:

```text
FUXI WLAN: M6o begin POR INTVEC zero offset 0x60400120 value 0 mask 0xffffffff
FUXI WLAN: step 20 FAILED code 0x1401 (11 ms, faults 0)
```

The existing helper has three pre-write rejection conditions. The fault count
is latched and remains zero. Counting the successfully logged host writes
against the journal predicate gives **36 entries**, not 96. Therefore the
old-value read returned **0xffffffff**. This is a deduction from the trace
and production control flow, rather than a directly printed register value.
Whether that initial value represents KIWI reset state or another endpoint
read behavior remains unproven; it does not establish that INTVEC is write-only.

The actual discovery line is:

```text
FUXI WLAN: MHI CHDBOFF 0x300 ERDBOFF 0x700 BHIOFF 0x100 BHIEOFF 0x224 DEBUGOFF 0xb00
```

These match the earlier M5/M6 observations in [wlan-bt.md](wlan-bt.md).
Step 16 reads BHIOFF and BHIEOFF at BAR0 `+0x28/+0x2c`; step 17 validates
their alignment and mapped extents, and the cycle checks their equality
again after restoring endpoint config. BHI INTVEC is BHI `+0x20`, hence BAR0
`+0x120`, physical `0x60400120`. BHIE RX low/high/size/doorbell are BHIE
`+0x60/+0x64/+0x68/+0x70`, hence BAR0 `+0x284/+0x288/+0x28c/+0x294`.
There is no evidence of a mistaken base or missing BAR addition.

The short status line's `BHIE 0x6aa00c7d` comes from **BHIE TXSTATUS
at +0x44**, physical `0x60400268`, stored in report word 72. It is not
BHIEOFF. No BHIe TX doorbell has been issued on this path, so that raw value
does not describe a completed owned TX transfer and cannot diagnose the
base. The earlier host fixture already models undefined pre-doorbell
TXSTATUS. W9 tests the supplied value without treating it as an offset.

The [MHI register definitions](https://android.git.googlesource.com/kernel/common/+/e4f2c4dfeeaeb70164967aa4abfa1190f7e61781/drivers/bus/mhi/common.h)
confirm these layout facts. The existing lane's
[power-up reference](https://github.com/torvalds/linux/blob/v6.6/drivers/bus/mhi/host/pm.c)
initializes INTVEC to zero in POR without depending on its previous value.
Only layout and sequencing facts are used here; no Linux implementation is
copied. Earlier M6e preflight observations read INTVEC zero after its zero
write. Together these support accepting an unknown **initial** value for
this full overwrite, while retaining readback verification. They do not
justify skipping readback on KIWI.

[Arm IHI 0062D.c, section 16.5.9, pages 16-299–300](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb)
defines FSR.Format at bits `[10:9]`; encoding 2 denotes AArch64 and is
read-only. Thus `0x400` alone is not a fault. The existing `0xc00001fe` mask
excludes both format bits while including MULTI, SS and fault bits `[8:1]`.
FAR/syndrome values without a fault flag are not evidence of current DMA
failure. The mask and SMMU programming are unchanged.

## Change and safety

`wl_m6o_write` prints a refusal with the operation, address, old value,
latched access-fault count, undo count/capacity, journal eligibility and reason.
If conditions overlap, reason priority is access fault, old all ones, undo
full; the numeric fields remain visible. Write/read lines now also include
the old value, fault count and undo usage, exposing post-write failures.

The sole new all-ones exception requires validated MHI state, the POR
selector, the exact discovered BHI INTVEC address, value zero and a full
32-bit mask. It covers initial POR and the existing SYS_ERR reset branch.
No old bits are retained, and endpoint registers never enter the host undo
journal. Faults still reject the write. A readback other than zero still
fails before the BHI image doorbell; W9 introduces no write-only path.

Before installing RX addresses, prepare re-reads both discovery registers
and prints live/saved offsets, INTVEC offset, undo usage and faults. Any
drift or access fault stops prepare. This adds diagnostics without inventing
a KIWI-specific base.

The journal remains bounded at 96, with one shared capacity constant.
The try8 Gen3 setup uses 18 entries initially and 18 on replay. The maximum
supported Gen4 setup adds a second EQ bank and Gen4 preset: 23 initially
and 23 on replay, **46 total**, verified over all 511 selector combinations.
Endpoint config/MSI/RX/INTVEC writes do not consume entries. Capacity is now
checked only when a host entry will actually be appended; overflow still
refuses before writing, and the final valid slot remains usable.

PERST/BME containment, reverse host undo (including banked EQ), MSI restore,
quarantine on failed containment/undo, and the existing one-shot BHI
doorbell guard remain in place. There are no ITS/GIC changes or added
doorbells. No device operation, firmware access, kernel image build, push
or CI run was performed for W9.

## Expected next boot

For the same Gen3 selectors, prepare should print:

```text
FUXI WLAN: W9 prepare BHIOFF 0x100/0x100 BHIEOFF 0x224/0x224 (live/saved) INTVEC offset 0x120 undos 36/96 faults 0
FUXI WLAN: M6o POR INTVEC zero offset 0x60400120 write 0 read 0 mask 0xffffffff old 0xffffffff faults 0 undos 36/96
```

The first line confirms live offsets match the validated saved values. The
second confirms that the old-value check caused the previous abort and that
the zero write/readback succeeds. A different old value with zero readback
is also success. Step 20 can then finish; step 21 retains its preflight and
one-image-doorbell limit, and step 22 retains its diagnostic stop/containment.
Passing prepare alone does not prove firmware execution.

Other outcomes are unambiguous:

| Log outcome | Meaning / next investigation |
| --- | --- |
| Live/saved offset mismatch or nonzero faults in `W9 prepare` | Discovery changed or MMIO access failed; RX installation is stopped. Investigate config/BAR/link state. |
| `M6o refused ... reason access fault` | The initial read faulted, or a prior access fault is latched; no write attempted. Use the printed fault count and existing ESR/FAR diagnostics. |
| `M6o refused ... reason old all ones` | An operation outside the narrow full POR INTVEC-zero exception read all ones; no write attempted. |
| `M6o refused ... undos 96/96 journal 1 reason undo full` | A host write would overflow the journal; no write attempted. This is unexpected on the measured 36/46-entry paths. |
| INTVEC `write 0 read 0xffffffff ... faults 0` | Initial all ones was tolerated, but zero readback failed. Readability after writing is still unresolved; contain without an image doorbell. This would be new evidence for evaluating a KIWI write-only policy. |
| INTVEC `write 0 read` nonzero, or nonzero faults | Readback mismatch or write/read access failure; contain without an image doorbell. |
| INTVEC zero readback, later failure | The original gate is cleared; use the later step/preflight/firmware diagnostics, without a second download attempt. |

## Host validation

`tools/test-fuxi-wlan-m6o.py` compiles the production C under ASan/UBSan.
W9 adds the observed Gen3/cap/status reproduction through the single BHI
doorbell, exact maximum journal usage, last-slot/overflow/reverse-undo checks,
full-zero-only exception checks, access faults, persistent all-ones/nonzero
readback failures, offset drift, SYS_ERR reset with an unknown INTVEC old
value, and FSR format-versus-fault checks. Existing selector, write-fault,
containment, MSI and quarantine coverage remains active.

Patch 0099's new-file hunk is regenerated from canonical `fuxi_wlan.c`;
the existing M6/M6o sync assertions compare its line count and reconstructed
bytes, including the unchanged embedded shim. This checkout has no dedicated
WLAN sync command; the other `sync-fuxi-*` generators target other lanes.

All eight source-only suites pass: M5, M6, M6i, M6k, M6l, M6m, M6n and
M6o/W9. These include the patch/source equality checks, packing-input
rejection and log-decoder coverage. `git diff --check` also passes. The separate
`test-fuxi-wlan-m6-artifacts.py` requires prebuilt lane ADT/kernel/loader/image
outputs. Its attempted run cannot start because those inputs are absent in
this worktree; it is not a source-only host test. No replacement images or
blobs are fabricated or downloaded.
