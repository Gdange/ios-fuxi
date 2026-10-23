# WLAN rail audit and ON-only experiment

## OBSERVED

The supplied W17 try16 result, with `fuxiwlanqrtr=5 fuxiwlanw16=3`, reaches
AMSS, FW_READY, successful WLAN_INI and WLAN_CFG replies. Firmware consumes
WLAN_MODE (`0x22`, channel 20 completion advances), but never replies.
BHI_ERRDBG2/3 are `0x14feeb3/0x496`; SYSERR is zero. Those debug words have no
established RF/power interpretation here. IPCR capacity, doorbell redrive,
shadow v3 and MLO have already been investigated; this experiment does not
reopen them. No new device measurement was made for this change.

**The normal path already votes the six external rails.**
[The XNU driver](../xnu/fuxi-wlan/fuxi_wlan.c) includes
[the shared M3 sequence](../shim/fuxi-probe/wlan.c) as `wlan.c`; patch 0099
carries both files. `fw_run` calls `wlan_m3(s)` for steps 1–13. Step 1 resolves
and checks the cmd-db resources. Step 2 calls `wl_vrm`, which sends voltage
then enable=1 through `rh_send`, for all six rails. Step 3 raises WLAN_EN
GPIO80 and waits 50 ms. The supplied log reports `step 2 ok code 0x1002`
with zero faults, before the later M6o cycle that explicitly retains all six
rails. Thus the premise that only `wl_m6o_rails` votes them is contradicted by
both the source and that step result. Completion is transport evidence, not
an analog measurement at the WCN pins.

## External resource map

Names, addresses and CNSS supply associations below come from the recorded
live-DT/cmd-db audit in [WLAN/BT topology and step table](wlan-bt.md), the M3
source, and the [M6n supply-order comparison](../docs/wlan-m6n.md). The table
uses the existing M6o/downstream order; report indices are absolute indices
in `wl3_report`. B/E/G identify PMIC instances, not WCN-internal LDO numbers.
The repository evidence does not establish a full PMIC silicon part number
for each instance, so none is inferred from the letter alone.

| cmd-db base | Resource / PMIC regulator | Report | M6o and new vote (mV) | Existing normal M3 (mV) | Recorded board supply / PMU input | Voltage assessment |
| --- | --- | --- | --- | --- | --- | --- |
| `0x40a00` | `ldob15`, B LDO15 (L15B) | 20 | 1800 | 1800 | `vdd-wlan-io`, PMU `vddio` | Sensible 1.8 V IO input; strong board evidence. Not a proven direct feed to endpoint `vddpcie1p8`. |
| `0x44000` | `smpg5`, G SMPS5 (S5G) | 24 | 1000 | 1000 | WLAN `vdd`, PMU `vdd` | Matches the recorded 1.0 V WLAN input; exact internal downstream domains unproven. |
| `0x40400` | `smpg2`, G SMPS2 (S2G) | 22 | 980 | 980 | WLAN `aon`, PMU `vddaon` | Matches the recorded 0.98 V AON input; plausible headroom for the internal AON regulator. |
| `0x40200` | `smpe4`, E SMPS4 (S4E) | 21 | 950 | 952 | WLAN `dig`, PMU `vdddig` | Matches the recorded 0.95 V request. M3 uses the documented 952 mV init value; see quantization caveat below. |
| `0x40600` | `smpg6`, G SMPS6 (S6G) | 25 | 1900 | 1900 | WLAN `rfa1`, PMU `vddrfa1p8` | Sensible parent for nominal 1.8 V RF; recorded 1.9 V external input supplies regulator headroom. |
| `0x40500` | `smpg4`, G SMPS4 (S4G) | 23 | 1350 | 1350 | WLAN `rfa2`, PMU `vddrfa1p2` | Sensible parent for nominal 1.2 V RF; recorded 1.35 V external input supplies regulator headroom. |

The legacy M3 order is **S5G, L15B, S2G, S4E, S4G, S6G**, with 1 ms after
each voltage/enable pair. M6o and this experiment use **L15B, S5G, S2G,
S4E, S6G, S4G**. The `rfa1`/`rfa2` board labels must not be interpreted as
literal 1.2/1.8 V output names: the recorded voltages and existing ordering
associate S6G with the higher RF input and S4G with the lower one.

These six requested voltages are supported by the existing board audit;
none is a new voltage inferred solely from a generic WCN7850 binding. The
new experiment deliberately preserves M6o's raw **950 mV** request for S4E.
M3's **952 mV** was chosen to cover the 950 mV nominal request with the
regulator's init voltage. A raw VRM request is not Linux regulator-selector
quantization: acceptance does not establish which physical voltage results.
This is an experimental difference to record, not a demonstrated undervoltage
or a claimed fix. These are shared resources; neither completion nor an ON
vote proves exclusive ownership or pin-level power integrity.

## WCN PMU outputs and gaps

The external inputs above and the WLAN endpoint's supply names are different
layers. The Linux bindings are cited by name only, without reproduced binding
code: `regulator/qcom,qca6390-pmu.yaml` (the `qcom,wcn7850-pmu` compatible) and
`net/wireless/qcom,ath12k.yaml`. They distinguish host-fed PMU inputs from
internal LDO outputs feeding WLAN/BT. The six required WCN7850 PMU inputs
are `vdd`, `vddio`, `vddaon`, `vdddig`, `vddrfa1p2`, `vddrfa1p8`.
`pwrseq-qcom-wcn` also names the optional `vddio1p2` input. All six required
input names have board-associated votes above. **No `vddio1p2` resource is
identified or voted by this table**; this audit cannot establish whether Fuxi
needs a separate external 1.2 V IO feed or supplies it another way.

| WLAN endpoint supply | WCN PMU output in the binding model | Connection to the six external votes |
| --- | --- | --- |
| `vddaon` | LDO1, nominal AON 0.59 V | S2G is the board's external AON input; an internal AON feed is plausible. The identical supply name at two layers does not make 980 mV the endpoint output voltage. |
| `vddwlcx` | LDO2, nominal WLCX 0.8 V | S4E DIG is a plausible upstream feed; exact internal connection is unverified. |
| `vddwlmx` | LDO3, nominal WLMX 0.85 V | S4E DIG is a plausible upstream feed; exact internal connection is unverified. |
| `vddrfacmn` | LDO0, RF common | No separate resource or proven external-parent assignment in this table. |
| `vddrfa0p8` | LDO5, RF 0.8 V | No separate resource or proven external-parent assignment in this table. |
| `vddrfa1p2` | LDO6, RF 1.2 V | S4G 1.35 V is the board-associated external lower RF input; internal routing is unverified. |
| `vddrfa1p8` | LDO7, RF 1.8 V | S6G 1.9 V is the board-associated external higher RF input; internal routing is unverified. |
| `vddpcie0p9` | LDO8, endpoint PCIe 0.9 V | No separate resource or proven external-parent assignment in this table. |
| `vddpcie1p8` | LDO9, endpoint PCIe 1.8 V | No separate resource or proven external-parent assignment in this table; L15B IO is not sufficient evidence of this connection. |

**None of these nine internal outputs has an independent RPMh vote in our
six-entry table.** That does not prove nine missing external rails: the PMU
model exposes internal outputs, with sequencing controlled through the PMU
inputs and enable GPIO. In particular, RF common, RF 0.8 V and the two
endpoint PCIe supplies have no separately identified external vote here.
A schematic or additional board evidence would be needed to assign their
parents. This change adds no guessed addresses.

M3 step 4 separately votes **`ldoe1`/E LDO1 `0x43900` at 880 mV** and
**`ldoe3`/E LDO3 `0x43a00` at 1200 mV** for the **host SoC PCIe QMP PHY**
(`vreg-0p9`/`vreg-1p2`, recorded as PM_V6E). These are outside the six WCN
inputs and must not be relabeled as the endpoint's `vddpcie0p9`/`vddpcie1p8`.

## GUESS

WLAN_MODE plausibly initiates RF/PHY work that exercises power conditions
not exercised by the working digital/QMI path. A rail state, regulator mode,
voltage quantization, shared vote interaction or retention problem could
therefore explain the symptom. The existing successful initial votes weaken
the simpler missing-votes theory. There is no measurement proving an RF rail
is off, no decoded debug-word evidence for that cause, and no proven mapping
from every external input to each internal RF/PCIe output.

## Implemented experiment

`fuxiwlanrails` is numeric and independent of the QRTR/W16 selector. Zero or
absence adds no rail commands, mode commands, waits or rail-experiment logs;
the existing normal M3 votes and legacy explicit `cyclerails` behavior remain.
Values outside 0–2 are refused before M3 power writes. A selected variant
combined with `fuxiwlanseq=cyclerails` is also refused before power writes;
the legacy helper additionally refuses its votes when ON-only is selected.
Plain `fuxiwlanseq=cycle` remains compatible.

- **`fuxiwlanrails=1`: early.** Re-vote all six after normal M3 step 2 and
  immediately before step 3 raises WLAN_EN. With M6o `cycle`, repeat while
  WLAN_EN is low, after its low dwell and before it rises again.
- **`fuxiwlanrails=2`: late.** No added early/cycle votes. Re-vote only after
  successful WLAN_INI/CFG, the existing pre-MODE snapshot/redrive and MODE
  encoding, immediately before the WLAN_MODE send. If MODE is never reached
  (for example QRTR selectors 0–4 or an earlier failure), no added votes run.

Each invocation first checks **all six** cmd-db results against the table;
any mismatch or existing access fault logs `rails skip ... expected ...` and
returns failure without submitting a vote. Failure blocks the corresponding
WLAN_EN rise or MODE request; it does not silently continue the experiment.
For each rail, send **voltage mV at base**, then **enable=1 at base+4**, then
**mode=7 at base+8** (HPM for LDO, PWM for SMPS), followed by the existing
M3-style 1 ms settling margin. There are 18 additional commands and 6 ms of
explicit dwell per invocation. Mode register offset and HPM/PWM wire value
are protocol facts; the DT mode enumeration is a different namespace and must
not be sent as the hardware value. The existing `rh_send` supports these
waited address/data requests through the same ACTIVE TCS; no transport is added.
The mode write is a new experimental delta, not a claim about inherited mode.

Every command logs resource, kind, address and value before submission, then
address, value and completion afterward, including failed voltage/mode votes.
Variant/stage begin and completion logs identify which path actually ran.
The existing transport retains its busy-slot checks, completion timeout and
poison/quarantine rules. The first failure stops submission. There is no retry,
rollback, OFF vote, or release of the new votes on success/failure/containment.
WLAN_EN/PERST containment continues independently of retained rail votes.
“ON-only” describes enable commands: the voltage re-vote can replace M3's
952 mV S4E request with 950 mV; it is not a claim that voltage never decreases.

## Host validation and next device comparisons

`tools/test-fuxi-wlan-rails.py`, wired into `tools/ci/build.sh`, compiles
production C under ASan/UBSan in two existing host harnesses. It covers normal
M3 voltage/enable/GPIO ordering through the actual TCS transport; all 18 early
completion failures; poisoned/busy/faulted transport; all six stale-address
positions; early bisect stops; numeric selectors and cyclerails conflict;
M6o re-votes before the cycle rise and no OFF on containment; late ordering
against the independent exact INI/CFG/MODE peer; all W16 variants; all 18 late
failures; MODE silence and retained allocations until containment. Existing
host suites additionally check patch/source byte identity and legacy behavior.
Host results establish software sequencing, not regulator voltage or RF success.

Validation result: all 17 runnable `tools/test-fuxi-wlan-*.py` suites passed,
including every WLAN host check invoked by the kernel build script. Shell
syntax, the WLAN CI dry-run/plan, and both patch 0099 new-file hunks were also
checked; the hunks apply and reconstruct their canonical sources byte-for-byte.
`test-fuxi-wlan-m6-artifacts.py` requires existing packed kernel/boot artifacts
and was not run; those inputs are absent and were not created or fetched.
No full macOS kernel build or device test was run.

Use fresh boots and retain the otherwise identical known try16 arguments,
including `fuxiwlanqrtr=5 fuxiwlanw16=3` and the existing sequence selectors.
Do not add `cyclerails`. Priority order:

1. Append **`fuxiwlanrails=1`**. Confirm the early complete log before WLAN_EN;
   if `cycle` is selected, confirm its second complete log before the cycle rise.
   Check all voltage/enable/mode completions, then MODE reply/timeout and MHI
   state. A successful MODE reply supports a power-related delta but does not
   isolate order, S4E request, timing, or mode as its cause.
2. If the early variant misbehaves, append **`fuxiwlanrails=2`** instead.
   Confirm no added early/cycle votes, CFG success, late complete, then one MODE.
3. Compare with **`fuxiwlanrails=0`** (or omit the argument) as the unchanged
   baseline. Neither failing experiment identifies a missing internal output
   or justifies adding an unproven external resource.
