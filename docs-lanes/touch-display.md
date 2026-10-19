# Touch + display lane (Fuxi, 2026-10-20)

Patch: `xnu/patches/0093-fuxi-touch-display-polish.patch` (on top of 0091; touches
only `bsd/dev/fuxi_touch.c` and `iokit/Kernel/arm/SweetFramebuffer.cpp`).
Built rc=0 in the Mac tree `ci-lane-touchdisp` (log `logs/kernel-lane-touchdisp.log`).
NOT booted yet.

## Findings (from code and the ios-0091d-01 log)

Observed facts:
- **The display was capped at 10 Hz.** `AppleCLCD::start` defaults `sweetfb_hz` to 10
  (a Sweet-era value from when one A55 core was too slow for the copy). The Fuxi
  cmdline does not set `sweetfb_hz`, so the panel copy ran every 6th 60 Hz tick.
  That adds 50 ms on average (up to 100 ms) between a rendered frame and the panel,
  and is the largest part of touch-to-photon latency.
- The vsync tick re-armed itself with `setTimeoutUS(16667)` *after* the copy, so the
  copy time made the period longer.
- backboardd never enables vsync notifications (no `notifications type` line,
  selector 72 is never called), so CA does not pace on our vsync. The tick matters only
  as the present clock.
- Swaps happen only when content changes: about 2000 swaps over the whole ios-0091d-01
  session (`SWEET FB: 1000/2000 swaps`). The rect is always full-screen
  (`a 0,0 1125x2436 b 0,0 1125x2436`), so it gives no dirty-rect information.
- The panel memory is mapped write-combined (`SweetSurface.cpp`, kIOMapWriteCombineCache);
  ABL autorefresh scans it out on every TE (patch 0087), so no START kick is needed.
- Display off (selector 12, patch 0079) fades out by read-modify-write of every
  pixel in the write-combined memory, 7 times (uncached reads on the workloop).
- Touch mapping is consistent: SweetHIDTouch maps panel (x+22, y+18) into
  1125x2436, the same centered crop the framebuffer copy uses (offsets 22/18). The
  edge flag (48 px, 0071) works: `touch 0 down at 632,2398 edge` → swipe-up worked.
- The touch driver polled every 8 ms (relative sleep, default timer coalescing
  leeway). It read the FIFO one 8-byte event per SPI transaction. It treated hover
  (type 5) as touching. It released fingers only after 10 consecutive errors and
  never recovered from errors. No FIFO errors were seen in ios-0091d-01.
- No brightness path: backboardd looks for a service with `backlight-control`
  (`IOK backboardd: matching ... backlight-control ... -> 0 found`).

Guesses (not measured):
- The FTS scan rate is probably 120-240 Hz, so 8 ms polling (125 Hz) may drop
  samples during fast swipes.
- The 1080x2400 copy into write-combined memory should take about 1-3 ms on the
  SM8550 big cores. Patch 0093 logs the real number.

## Changes in 0093, by expected impact

1. **Present up to 60 Hz on FUXI** (default; `sweetfb_hz=N` still overrides,
   `sweetfb_hz=10` gives the old behaviour for A/B). The tick now uses absolute
   deadlines with zero leeway. A stats line is printed after every 600 presented frames:
   `SWEET FB: stats N frames in T ms, copy avg A us max M us, S swaps, D not shown`
   (D = swaps replaced before they were shown).
2. **Touch polling 4 ms while active.** The poll is 4 ms while a finger is down or
   the IRQ was seen in the last 150 ms, 8 ms when idle and 32 ms while the display
   is off. Deadlines are absolute with critical urgency (no coalescing). The thread
   runs at priority 92. Boot-args: `fuxitouch_poll_us`, `fuxitouch_idle_us`.
3. **The FIFO is drained in 2 SPI reads** (one event, then the queued count from
   byte 7 in one read, as Lineage `fts_event_handler`) instead of one read per event.
4. **Display off:** a single black fill (write-only) instead of the uncached fade.
   Touches are released and ignored while the display is off (`sweet_fb_display_off`),
   so nothing can be tapped on a black screen; the power button wakes the phone.
   `fuxitouch_offsuppress=0` turns this off.
5. **Robustness:**
   - After 2 consecutive FIFO errors, any held finger is released.
   - After 3 errors, and then every 50, the GPII rings are re-initialised.
   - A controller-ready event (0x03) while sensing means the IC reset itself:
     fingers are released and sense-on is sent again.
   - Hover (type 5) counts as lifted, and invalid-type (0) events are ignored.
6. **Log trim:** the first-16 FIFO dumps and the every-50th report lines are gone. There
   is one summary per contact instead:
   `FUXI TOUCH: contact N: R reports in T ms (I us apart), P polls, FIFO read avg A us max M us`.

## Not done / next ideas

- **Interrupt-driven touch.** GPIO25 maps to PDC 57, and the TLMM summary IRQ is SPI
  208. XNU's GIC code (pe_fiq.c, SWEET) only handles PPIs/SGIs today, so this needs
  three pieces: GICD SPI setup, PDC/TLMM interrupt configuration (PDC may be
  secure-only) and an IRQ dispatch path. That is too risky without the device;
  4 ms polling gives most of the benefit.
- **Real panel off and brightness.** Both need DCS commands over DSI0: 0x28/0x10 for off, 0x51 for brightness.
  With INTF1 autorefresh on, a command-DMA transfer would have to be interleaved
  with autorefresh (disable AR, send, re-enable), and that was the cause of the static in
  ar-01. Read the Lineage panel driver (dsi-panel-m3 / mi_dsi) for the exact
  sequence before trying it. A software brightness option is to scale pixels during the
  copy (NEON). iOS would also need an IOService with `backlight-control`
  (AppleARMBacklight-like) for the slider to do anything.
- **Copy cost.** If the stats show copy >4 ms: copy only the rows that differ from
  the previous swap's surface (both are cacheable), or split the copy across 2 threads.
- **Cropped edges.** The centered crop hides 22 px left/right and 18 px top/bottom of iOS
  (the bottom of the home indicator). A scaled present would show all of it, but each
  pixel would cost more CPU and look softer.
- SweetHIDTouch sends one collection per changed finger. Two fingers moving give two HID
  events per poll; one combined event would be cleaner (SweetHID.cpp, not changed here).

## Artifacts (local, not in git)

- Kernel `xnu/out-lane-touchdisp/kernel.development.fuxi.unstripped` sha256 9741f309…
- Payload `shim/out/lane-touchdisp/fuxi-0093-payload.bin` sha256 538aa1db… (0091d cmdline, ADT fuxi-0090.adt)
- Boot image `shim/out/lane-touchdisp/fuxi-0093-boot.img` sha256 4917082d… (payload verified inside)
- Runner `research/run-ios-0093-touchdisp.py` (copy of run-ios-0091d.py with these paths; keeps `--touch-power`)

## Test plan

Run it the same way as 0091d: phone in OrangeFox with /tmp/f2fs_io, then
`sudo python3 research/run-ios-0093-touchdisp.py research/device/ios-0093td-01`.

Boot checks (log):
- `SWEET FB: present up to 60 Hz (every 1 ticks)`
- `FUXI TOUCH: starting in 30 s (poll 4000 us active, 8000 us idle)`, then
  `reset err=0 event 03`, `chip id ... 3640`, `sensing`.

On the phone (user):
1. Unlock with a swipe up. Scroll the home screen pages left and right, and open
   and scroll Settings for about 30 s. Expected: motion visibly smoother than 0091d.
   Proof: `SWEET FB: stats` lines. The frames/T ratio is the real fps. Copy avg should
   be well under 16 ms. "not shown" should be a small part of swaps; at 10 Hz most
   swaps would be dropped.
2. Drag one finger slowly, then quickly, for a few seconds. Proof:
   `FUXI TOUCH: contact` lines with "us apart" ≈ 4000-8000 (it was ≥8000 before), and
   FIFO read avg/max (cost of one poll).
3. Two-finger pinch on a photo/Settings or a two-finger tap. Check that nothing is
   left stuck down: every `SWEET HID: touch N down` has a matching `up`.
4. Press power (lock). Expected: screen goes black at once (no fade), `SWEET FB:
   display off`. Tap and swipe the black screen: nothing happens and no `SWEET HID:
   touch` lines appear. Press power again: `display on`, the lock screen comes back,
   and touch works.
5. Optional A/B in the same image is not possible (the cmdline is in the payload).
   For an A/B, rebuild the payload with `sweetfb_hz=10` or
   `fuxitouch_poll_us=8000 fuxitouch_offsuppress=0`.

Failure signs: `FUXI TOUCH: FIFO read error`, `GPI re-init`, `controller reset
event`, or a panic in AppleCLCD::vsyncFired / present. Fall back to the 0091d image.
