# Fuxi buttons lane (Power / Volume Up / Volume Down)

Status 2026-10-20: patch 0092 written and built. Not booted yet (this lane has no
device access).

## Why buttons did not work

`bsd/dev/sweet_keys.c` is compiled on FUXI (FUXI builds also define
`ARM64_BOARD_CONFIG_SWEET`), but `sweet_ufs.c` calls `sweet_keys_init()` only on
the `#else` (Sweet) path. It is also Sweet-specific anyway: arbiter v5 offsets,
PM6150 PON at 0x810, PM6150L GPIO2. backboardd already opens `SweetHIDButtons`
on Fuxi (ios-0091d-01 boot.log), so the HID side is ready. Only the PMIC reader was
missing.

## Hardware facts

Source: live DT `research/device/probe-09/live.dtb`. Register meanings come from
Linux v6.12 `drivers/spmi/spmi-pmic-arb.c`, `drivers/input/misc/pm8941-pwrkey.c`
and `drivers/pinctrl/qcom/pinctrl-spmi-gpio.c`.

SPMI arbiter `/soc/qcom,spmi@c42d000`, compatible `qcom,spmi-pmic-arb`,
`qcom,ee = 0`, `qcom,channel = 0`, `qcom,bus-id = 0`:

| reg-name | base | size | v7 use |
|---|---|---|---|
| cnfg   | 0x0c42d000 | 0x4000   | APID owner table: `cnfg + 4*n`, bits[2:0] = owner EE |
| core   | 0x0c400000 | 0x3000   | VERSION @0 (v7 >= 0x70000000), FEATURES @4 bits[10:0] = APID count (bus 0), APID map `0x2000 + 4*n`: bits[19:8] PPID, bit 24 IRQ owner |
| chnls  | 0x0c500000 | 0x400000 | RW channel `0x1000*apid` (writes; only for the opt-in GPIO config) |
| obsrvr | 0x0c440000 | 0x80000  | read channel `0x8000*ee + 0x20*apid`: CMD +0, STATUS +8 (bit0 done, bits1-3 fail/denied/dropped), RDATA0 +0x18 |
| intr   | 0x0c4c0000 | 0x10000  | not used |

Command word (v2..v7): `opc<<27 | (addr&0xff)<<4 | bc`, EXT_READL opc = 1,
EXT_WRITEL opc = 0. PPID = `sid<<8 | addr>>8`.

Keys:

| key | PMIC / SID | register | pressed when | DT source |
|---|---|---|---|---|
| Power | PMK8550, SID 0 | pon_hlos@1300, RT_STS 0x1310 bit 7 (KPDPWR_N_SET) | bit = 1 | `pon_hlos@1300/pwrkey`, compatible `qcom,pmk8350-pwrkey`, irq `<0 0x13 7>`, code 116 |
| Volume down | PMK8550, SID 0 | RT_STS 0x1310 bit 6 (RESIN_N_SET) | bit = 1 | `pon_hlos@1300/resin`, `qcom,pmk8350-resin`, irq `<0 0x13 6>`, code 114 |
| Volume up | PM8550, SID 1 | GPIO6 = 0x8800 + 5*0x100 = 0x8d00, RT_STS 0x8d10 bit 0 | bit = 0 (active low, pull-up) | `/soc/gpio_keys/vol_up` gpios `<pm8550_gpios 6 1>`, code 115, debounce 32 ms; pinctrl `key_vol_up_default`: function normal, input-enable, bias-pull-up, power-source 1 |

PON peripheral: second reg 0x800 = pon_pbs, not needed for status. PPIDs used:
0x013 (PON) and 0x18d (GPIO6). GPIO registers: TYPE 0x04 (expect 0x10),
SUBTYPE 0x05 (LV/MV 0x10..0x13 expected), MODE_CTL 0x40 (LV/MV: bits[1:0] dir,
0 = input), DIG_VIN_CTL 0x41, DIG_PULL_CTL 0x42 (0 = pull-up 30 uA), EN_CTL 0x46
bit 7.

Facts and guesses:
- Facts (DT and Linux source): all addresses, SIDs, bits and polarity above.
- Guess: the APID count is read from FEATURES on the device and is not checked
  here. Linux reads the same register.
- Guess: ABL/PBS leaves GPIO6 enabled as an input. If it does not, the driver
  withholds volume up and prints the registers. Boot-arg `fuxikeys_gpiocfg=1` then
  writes the Linux pinctrl config (VIN 1, pull-up 30 uA, input, enable) on the
  pin's own RW channel, and only if EE 0 owns that APID. That PMIC config is
  volatile. It is off by default.
- Guess: the reads through the observer channel need no SMMU or clock setup. Linux
  on this device uses the same path, and Sweet v5 worked the same way.

## Patch 0092 (`xnu/patches/0092-fuxi-buttons.patch`)

- New `bsd/dev/fuxi_keys.c` (`#if defined(FUXI)`). A kernel thread maps
  core/cnfg/obsrvr and checks VERSION in [0x70000000, 0x80000000). It finds the
  APIDs for PPIDs 0x013 and 0x18d (preferring the one owned by EE 0), logs the PON
  and GPIO6 registers, then polls every 20 ms. A change needs two equal samples in
  a row. On each change it logs and calls `sweet_hid_key(k, down)` (0 power,
  1 vol+, 2 vol-).
- `sweet_ufs.c` FUXI branch: `fuxi_keys_init()` after `fuxi_touch_init()`.
- Boot-args: `fuxikeys=0` disables it, `fuxikeys_gpiocfg=1` is the opt-in GPIO6
  config described above.

## Build and artifacts (2026-10-20 05:47)

- Mac tree `ci-lane-buttons/xnu` (= ci-0081 + patches through 0091, then 0092
  applied with `patch -p1`). `tools/build-fuxi-native.sh` returned rc=0 with no
  errors and no fuxi_keys warnings (log `logs/kernel-lane-buttons.log`).
- Kernel `xnu/out-lane-buttons/kernel.development.fuxi.unstripped`, sha256 906a98e7...
- Payload `shim/out/lane-buttons/fuxi-0092-payload.bin`, sha256 0977e7fc... It
  uses ADT `shim/out/fuxi-0090.adt` and the 0091 cmdline unchanged.
- Boot image `shim/out/lane-buttons/fuxi-0092-boot.img`, sha256 2a55dc6a...,
  built with the main repo loader (no loader changes).
- Runner `research/run-ios-lane-buttons.py`: a copy of run-ios-0091d.py with these
  paths, still with `--touch-power`. It needs OrangeFox + /tmp/f2fs_io, like 0091d.
- No loader probe was added: the driver reads only DT-listed SPMI blocks and logs
  every value it reads at init.

## Test plan

1. Boot exactly as for 0091d (phone in OrangeFox), with
   `python3 research/run-ios-lane-buttons.py research/device/ios-0092-01` from the
   repo root.
2. Wait for the lock screen (the touch log line `FUXI TOUCH: sensing` comes
   around the same time).
3. Check the init lines below, then press each key once, slowly: Power, Vol+,
   Vol-. Then press Power again to wake the screen.

Expected log lines, in order:

```
FUXI KEYS: arb=0x7xxxxxxx apids=N pon-apid=A/ee0 gpio6-apid=B/ee0 pon ok=1 type=0x?? subtype=0x9 rt=0x0 st=0x1   (subtype 0x09 = PON GEN3 HLOS)
FUXI KEYS: boot gpio6 ok=1 type=0x10 subtype=0x1? mode=0x0 vin=0x1 pull=0x0 en=0x80 rt=0x1
FUXI KEYS: polling every 20 ms (power=1 volume-up=1 volume-down=1)
```

Then each press gives a pair of lines:

```
FUXI KEYS: power down (pon=0x80 gpio6=0x1)   ... power up (pon=0 ...)
FUXI KEYS: volume-down down (pon=0x40 ...)   ... volume-down up
FUXI KEYS: volume-up down (pon=0 gpio6=0)    ... volume-up up
```

Expected effect in iOS: power locks the screen (SweetFramebuffer logs
`SWEET FB: display off` and fades the panel to black), and a second press logs
`display on`. The volume keys show the volume HUD.

Failure branches:
- `unsupported arbiter version` means the v7 assumption is wrong. Capture the value.
- `pon ok=0 ... st=0x..` means the observer read failed. bit2 means denied
  (apps EE not allowed), and a missing done bit means a timeout.
- `gpio6 not an enabled input` means ABL did not configure the pin. Re-run with
  `fuxikeys_gpiocfg=1` added to the cmdline, after the user confirms the PMIC
  GPIO write.
- No `FUXI KEYS` lines at all: the thread never started, or the SoC reset on the
  first MMIO access. In that case the last lines are before the root mount.
