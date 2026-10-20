# Wallpaper lane: black lock/home wallpaper, black Settings > About

Scope: on the iOS 16.7 (20H392) boots the lock screen and home screen wallpaper
is black while icons and UI render; Settings > General > About is a black page.
CoreAnimation renders on the CPU; there is no MTLDevice.

Evidence used (local, not in git): `research/device/ios-0097-gpu/boot.log`,
`research/device/ios-0097-bsgpu/boot.log` (boot-args added
`sweetsample=backboardd,Preferences,PosterBoard sweetscan=PosterBoard,Preferences`),
older `research/device/ios-*/boot.log`, the 20H392 shared cache
(`ipsw dyld a2s / disass / class-dump / str`), and a read-only 7z listing of the
staged root image.

## 1. Wallpaper

### Facts (observed)

- Every boot since ios-0088 has exactly one PosterBoard crash, then launchd
  starts PosterBoard again and the second instance stays up. Same PC in every
  log: `CRASH PosterBoard[...] sig 5`, `reason ns 2 code 0x5` (SIGTRAP, `brk #0`).
- Symbolicated (unslid addresses, both boots identical):

  ```
  pc 0x1c1778268 -[PBFPosterExtensionDataStore _stateLock_setupSwitcherConfigurationForFirstTime] + 2040   (brk #0)
     0x1c177e304 -[PBFPosterExtensionDataStore _stateLock_updateExtensions:refreshDescriptors:powerLogReason:galleryUpdateOptions:queuedUpOperations:] + 5308
     0x1c1764c5c -[PBFPosterExtensionDataStore executeUpdate:refreshStrategy:galleryUpdateOptions:powerLogReason:completion:] + 1508
     0x1c1764644 -[PBFPosterExtensionDataStore prewarmWithCompletion:] + 144
     0x1c17e6204 -[PBFPosterExtensionDataStoreXPCServiceGlue server:prewarmWithCompletion:] + 64
     0x1a36b9318 -[PRSServer prewarmWithCompletion:] + 164
     0x19b9b372c +[BSXPCServiceConnectionProxy invokeMethod:onTarget:withMessage:forConnection:] + 1288
     ... BoardServices XPC dispatch
  ```

- The `brk` is an NSAssert at PBFPosterExtensionDataStore.m:3431. The function
  starts with `ldr x8, [x0, #0x98]; cbnz x8, assert` where ivar 0x98 is
  `_stateLock_switcherConfiguration` (class dump). The assert text is in the
  sweetscan heap dump of ios-0097-bsgpu:
  `Invalid condition not satisfying: !_stateLock_switcherConfiguration`.
  The second assert of the function (`!_stateLock_initialExtensionsLoaded`,
  line 3432) did not fire.
- Only two places store ivar 0x98 (whole-framework disassembly):
  `_stateLock_setupSwitcherConfigurationForFirstTime` itself (after the
  assert) and the lazy getter `-[PBFPosterExtensionDataStore _stateLock_switcherConfiguration]`,
  which builds a switcher configuration from whatever poster configurations
  are on disk when the ivar is nil.
- Same heap dump also has `Switcher configuration yielded no home or lock screen poster paths`
  and `Prewarm session '.../prewarm' initiated from XPC`. The first string occurs only in
  `-[PBFPosterExtensionDataStoreXPCServiceGlue server:fetchActivePosterConfiguration:]`,
  which calls `switcherConfiguration` → the lazy getter.
- `_stateLock_updateExtensions` calls `_stateLock_setupSwitcherConfigurationForFirstTime`
  when `_stateLock_initialExtensionsLoaded` (ivar 0xe9, in memory only) is 0, and sets
  it to 1 afterwards. The setup function is the one that installs the default poster
  (`shouldInstallCollectionsPosterAsDefaultLockScreenWallpaper`,
  `defaultWallpaperIdentifier`, `storeConfiguredPropertiesForPath:`) and builds the
  switcher configuration.
- No poster extension process is ever started in any log: no `exec CollectionsPoster`,
  `LegacyPoster`, `PhotosPosterProvider`, `GradientPosterExtension`, ... although all are
  in the image and in launchd.plist `AppExtensions` (extension point
  `com.apple.posterkit.provider`). `SBRendererService` is not started either.
- The second PosterBoard instance is idle: all 8 samples (ios-0097-bsgpu) show t0 in
  `mach_msg2_trap` ← `CFRunLoopRunSpecific` ← `GSEventRunModal` ← `UIApplicationMain`,
  t1 in the UIEventFetcher run loop.
- SpringBoard stderr has no wallpaper error; the only related line is
  `IOServiceMatchingfailed for: AppleM2ScalerCSCDriver` (IOSurfaceAccelerator scaler).
- Metal: PosterBoard does not call `MTLCreateSystemDefaultDevice` on the data-store path
  (only `PBFHomeScreenPreviewImageBuilder`). `PBUICALocalRenderer initWithColorSpace:`
  (PaperBoardUI) returns nil without a device. `+[SBFCARendererImpl _actuallyRenderImage:...]`
  (SpringBoardFoundation, legacy wallpaper luminance/dimming treatment, run in the XPC
  service `com.apple.springboard.SBRendererService` when called from SpringBoard) has no
  nil check: with a nil device it builds `@{kCARendererColorSpace: cs, kCARendererMetalCommandQueue: nil}`
  with `dictionaryWithObjects:forKeys:count:`, which throws on the nil object.
- `Autobahn` (feature flag, domain SpringBoard, FeatureComplete) gates the iOS 16
  poster lock screen. `server:fetchActivePosterConfiguration:` returns
  "Unsupported on this platform" when Autobahn is off or the idiom is iPad
  (`idiom & ~4 == 1`), i.e. the iPad iOS 16 path is the legacy wallpaper path.
  libsystem_featureflags reads `/System/Library/FeatureFlags` and then
  `/Library/Preferences/FeatureFlags` (`Domain/<name>.plist`, also
  `FEATUREFLAGS_ENABLED` / `FEATUREFLAGS_DISABLED` env vars).

### Conclusion (inference from the facts above)

The wallpaper is black because no poster configuration exists, so SpringBoard has
nothing to host and never asks for a poster extension. On each boot:

1. SpringBoard asks the new PosterBoard for the active poster before the prewarm
   update has run. The lazy getter builds an empty switcher configuration from the
   empty data store ("yielded no home or lock screen poster paths").
2. The prewarm update then calls the first-time setup, whose entry assert sees the
   already-built configuration and traps. The default poster is never written.
3. The respawned PosterBoard is never prewarmed again (it does not crash and never
   installs a poster or starts an extension), so the state stays empty, and
   step 1 repeats on the next boot.

The race is probably timing: on this slow CPU-only system the prewarm update waits
(extension discovery) longer than SpringBoard waits before it calls
fetchActivePosterConfiguration. This is a guess; the exact ordering and what the
prewarm waits for are not in the logs because PosterBoard logs only through os_log.

Metal is not the first blocker. It becomes relevant later: once a poster exists the
extension has to render (PosterKit/WallpaperKit link Metal; not checked), and the
legacy path's SBRendererService will throw on the nil Metal command queue if
SpringBoard asks it for a treated wallpaper image.

### Fix options

| Option | What | Status |
| --- | --- | --- |
| A (diagnostic) | `--posterboard-oslog` + `sweetnull` with PosterBoard: PosterBoard's os_log on stderr in the kernel log | implemented, opt-in |
| B (experiment) | `--legacy-wallpaper`: `SpringBoard/Autobahn` off → iPad-style legacy wallpaper, no posters | implemented, opt-in, reversible with `--no-legacy-wallpaper` |
| C | Get one first-time setup to finish so that a default poster is persisted; later boots then return it even if the prewarm still traps | needs A's log first (what the prewarm waits for); maybe PosterBoard RunAtLoad, or a hand-made data store |
| D | Patch the assert / lazy getter (dylib or shared-cache patch) | invasive; not proposed |

B is the least invasive change that might give a visible wallpaper, but it may move
the failure to SBRendererService (nil Metal command queue, see facts). A comes first
because it costs nothing and tells whether C is easy.

Tool: `tools/sweet-root-tweaks.py` (header documents the steps). `--only-optional`
applies just the named opt-in steps to an already tweaked root:

```sh
sudo python3 tools/sweet-root-tweaks.py --only-optional --posterboard-oslog /mnt/iosroot      # step 1
sudo python3 tools/sweet-root-tweaks.py --only-optional --legacy-wallpaper /mnt/iosroot       # step 2
sudo python3 tools/sweet-root-tweaks.py --only-optional --no-legacy-wallpaper /mnt/iosroot    # undo 2
```

`--posterboard-oslog` adds `EnvironmentVariables = {OS_ACTIVITY_DT_MODE = YES}` to the
`com.apple.PosterBoard` job in `/System/Library/xpc/launchd.plist` (the cache launchd
uses with `launchd_unsecure_cache=1`). `--legacy-wallpaper` writes
`/Library/Preferences/FeatureFlags/Domain/SpringBoard.plist` = `{Autobahn = {Enabled = false}}`
(root:wheel 0644). Not verified on device: that release libsystem_trace honours
`OS_ACTIVITY_DT_MODE` (the string is in libsystem_trace), that RunningBoard's launch of
PosterBoard applies the job's environment, and that the `/Library/Preferences`
override is honoured on a customer build.

## 1b. Boot 0098i: Autobahn off, wallpaper still black

Boot `research/device/ios-0098-igpu/boot.log` (local), root with `--legacy-wallpaper`
and `--posterboard-oslog`; boot-args had `sweetsample=backboardd,Preferences,PosterBoard
sweetscan=PosterBoard,Preferences` and no `sweetnull`; SpringBoard/backboardd stderr is teed
anyway, PosterBoard's is not (0 `|PosterBoard|` lines), so `--posterboard-oslog` showed nothing.

### Facts (observed)

- No `CRASH PosterBoard` (one `exec PosterBoard`, pid 242, stays up). No `exec
  SBRendererService`, no poster extension, no `CRASH SpringBoard`/`backboardd`.
- All 14 PosterBoard samples (slide 0x35688000) are idle: t0 `mach_msg2_trap` <-
  `__CFRunLoopServiceMachPort` <- `__CFRunLoopRun` <- `CFRunLoopRunSpecific` <-
  `GSEventRunModal` <- `-[UIApplication _run]` <- `UIApplicationMain` <- `start`;
  the other thread is the UIEventFetcher run loop (`-[UIEventFetcher threadMain]`) or a
  parked workqueue thread (`__workq_kernreturn`).
- The 413 `|SpringBoard|` lines are the same set as in ios-0097-* (poster mode): CATransformLayer
  property warnings, 20x QuartzCore "trying to set state of nil layer." (also 20x in every
  0097 log, so not wallpaper-mode related), UICollectionViewFlowLayout size warnings, the
  `AppleM2ScalerCSCDriver` match failure. Nothing about wallpaper, image decode or
  CARenderServer. backboardd stderr is only GameController class-duplicate warnings.
- Root image (read-only): `/var/mobile/Library/SpringBoard` has IconState, RecentAppLayouts,
  SBLastRestoreIdentifier, TodayViewArchive only - no `LockBackground*`/`HomeBackground*`.
  `com.apple.springboard.plist` has `SBLockScreenWallpapers`/`SBHomeScreenWallpapers` =
  `{none = {parallax 1, zoom 1, portrait, no video, no cropping, procedural not user set}}`:
  geometry only, no color, gradient, procedural identifier or image hash.
  `MappedImageCache/Wallpaper` is empty.
- `/Library/Wallpaper/DefaultWallpapers~iphone.plist` default (no iPhone10,3 entry) is
  collection iOS 16 / wallpaper 7400 "WWDC 2022", type `LayeredAnimation`: three CoreAnimation
  packages whose assets are HEIC (HEVC) only. Every `.wallpaperCollection` asset in the
  image is HEIC (48 files, no PNG/JPEG). Heritage stripes (`Stills/Any/3xx`) are procedural.
- PaperBoardUI (20H392 disassembly):
  - `-[PBUIWallpaperConfigurationManager init]`: data stores = `PBUIWallpaperDirectoryDataStore`
    (`CPSharedResourcesDirectory()/Library/SpringBoard`) + `PBUIWallpaperUserDefaultsDataStore`
    (single-user mode).
  - `wallpaperConfigurationForVariant:includingValuesForTypes:wallpaperMode:` tries, per
    store, procedural info -> color -> gradient -> static image (`hasWallpaperImageForVariant:`)
    -> video; with none it returns `fallbackWallpaperConfigurationForVarient:`, which is a
    color configuration with `+[UIColor blackColor]`.
  - `restoreDefaultWallpaperForAllVariantsAndNotify:` only runs for `idiom & ~4 == 1` (iPad);
    otherwise it logs "Hello NOT iPad, trying to restoreDefaultWallpaperForAllVariantsAndNotify?
    not today." Even on iPad, `setWallpaperBundle:appearance:` only takes
    `fileBasedWallpaperForLocation:andAppearance:` (still/live image URLs), not LayeredAnimation.
  - Static image file = `LockBackground.cpbitmap` (variant 0) / `HomeBackground.cpbitmap`
    (variant 1); for wallpaper mode 2 the name gets `PBUIStringForWallpaperMode(2)` appended and
    a missing file falls back to the mode-none one ("Defaulting to PBUIWallpaperModeNone image"), read with `CPBitmapCreateImagesFromData` (raw pixels, no ImageIO decode).
  - AppSupport `CPBitmapCreateImagesFromData` (disassembly, matches SpringBoard's own
    `MappedImageCache/*.cpbitmap` in the image): rows of BGRA (format 0, `0x2006`) padded to
    16 pixels, then an optional colorspace plist, an "extra" binary plist (SpringBoard
    writes the scale `3.0`, 50 bytes), then a 28-byte trailer
    `<cs len, extra len, width, height, format, count, 0xdcb543a2>` (u32 LE); total length
    must equal `stride*height + cs + extra + 28`.

### Hypotheses (ranked)

1. **Most likely (inference from the disassembly + empty data stores):** the legacy path is
   active and shows its black fallback. On iPhone nothing ever installs a default wallpaper
   (restore refuses on non-iPad), the stores hold no image/color/gradient/procedural entry,
   so `fallbackWallpaperConfigurationForVarient:` = black color. This matches "no crash,
   everything else renders, wallpaper black". Not verified at runtime: that SpringBoard
   really took the legacy branch (no os_log from SpringBoard in the console).
2. Even with a default installed, the stock default would not render here: 7400 is a
   LayeredAnimation (poster-only type, not handled by `setWallpaperBundle:`) with HEVC HEIC
   assets, and there is no hardware HEVC decoder. Matters for the poster path later.
3. Next possible failure once an image exists (guess): a treated/blurred variant requested
   through SBRendererService (`+[SBFCARendererImpl _actuallyRenderImage:...]`, nil Metal queue,
   see section 1), or CABackdrop-based treatment on the CPU renderer.

### Fix: `--legacy-wallpaper-image [PNG]`

Writes `/private/var/mobile/Library/SpringBoard/LockBackground.cpbitmap` and
`HomeBackground.cpbitmap` (mobile 501:501, 0644; directory created 0755 if missing) in the
format above. Without an argument it uses a built-in 1125x2436 blue-purple vertical
gradient (screen size UIKit sees, patch 0061); with a PNG (8-bit RGB/RGBA, non-interlaced,
1125x2436 recommended) it converts that. No preference changes are needed: the type comes
from the file being reachable. Requires `--legacy-wallpaper` (Autobahn off). Undo with
`--no-legacy-wallpaper-image`. Host test: `python3 tools/test-sweet-root-tweaks.py`
(checks the trailer/extra plist byte-for-byte against SpringBoard's own 3x7 cpbitmap, the
length rule, pixel order, PNG decode, file modes).

```sh
sudo python3 tools/sweet-root-tweaks.py --only-optional --legacy-wallpaper-image <root>
```

Next boot, expected: lock screen and home screen show the gradient (lock first: blue at the
top, purple at the bottom). If it is still black, boot with
`sweetnull=SpringBoard sweetsample=SpringBoard sweetscan=SpringBoard` and look for
`exec SBRendererService` / `CRASH SBRendererService`, a `CRASH SpringBoard`, and the heap-scan
strings "Cannot find image for variant", "Wallpaper file URL", "CPBitmapErrorDomain",
"Invalid magic in trailer", "Length of file does not match expected length".

## 2. Settings > General > About

### Facts

- The page is `PSGAboutController` (GeneralSettingsUI) with `PSGAboutDataSource`;
  values are loaded by `_loadValues` (data source has `_threadRunning`, `_lock`,
  `_loading`; controller has `shouldDeferPushForSpecifierID:`).
- `_bluetoothMACAddress` reads `local-mac-address` of the IODeviceTree `bluetooth`
  node and is nil-safe; it does not talk to bluetoothd. `_macAddress` uses
  `SCNetworkInterfaceCopyAll`.
- `_loadValues` also uses MobileGestalt (`cellular-data`, `bluetooth`, `wi-fi`),
  LSApplicationWorkspace (app count), disk capacity of `/private/var`, media counts
  (songs/videos/photos), CoreTelephonyClient (carrier, ICCID; CommCenter jobs are
  removed by the base tweaks), SUManagerClient, NDOManager, SystemHealthUI.
- Preferences was started twice in ios-0097-gpu (prewarm, then the user's launch),
  no Preferences crash. No Preferences stderr, samples or IOKit trace exist yet.

### Guesses

- Bluetooth: the About data source does not need bluetoothd, so the sweetnoexec
  block of bluetoothd is probably not the cause (it may still matter through
  MobileGestalt or another framework).
- More likely a synchronous XPC wait in `_loadValues` or in the deferred push on a
  service that never answers (media library / photos counts, CoreTelephony,
  softwareupdate, fairplayd for app info). The samples will show which.

## Device test order (coordinator)

1. Diagnostic boot, no image change other than step A:
   `sudo python3 tools/sweet-root-tweaks.py --only-optional --posterboard-oslog <root>`
   and add to the boot-args:

   ```
   sweetnull=backboardd,SpringBoard,PosterBoard,Preferences sweetsample=PosterBoard,Preferences sweetscan=PosterBoard,Preferences,SpringBoard sweetiolog=PosterBoard,Preferences
   ```

   (`sweetiolog` has a 400-line global cap that backboardd alone fills, so this boot
   traces PosterBoard and Preferences instead.) After the home screen appears, open
   Settings > General > About and leave it open at least 5 minutes (samples come 90 s
   after the kernel thread starts, then every 120 s, 10 in total).

   Look for:
   - `|PosterBoard| ...` lines before the `CRASH PosterBoard` line: the order of
     `fetchActivePosterConfiguration`/"Switcher configuration yielded no home or lock
     screen poster paths", "Prewarm session ... initiated from XPC", and what the
     prewarm update waits on (extension discovery, "Extensions were updated",
     `_stateLock_updateExtensions` messages). If there are no `|PosterBoard|` lines at
     all, OS_ACTIVITY_DT_MODE did not reach the process.
   - `|PosterBoard|` lines of the second instance: whether it ever logs a prewarm.
   - `IOK PosterBoard: ...` / `IOK Preferences: ...` service lookups (failed matches).
   - `SMP Preferences[pid] t0 ...` while About is black: symbolicate t0 with
     `tools/sweet-crash.py` or `ipsw dyld a2s` (address minus the printed slide). A t0
     in `mach_msg2_trap` under `xpc_connection_send_message_with_reply_sync` or
     `dispatch_semaphore_wait` names the service it waits for.
   - `|Preferences| ...` stderr.
2. Experiment boot for the wallpaper (same boot-args):
   `sudo python3 tools/sweet-root-tweaks.py --only-optional --legacy-wallpaper <root>`
   Expected if it works: no `CRASH PosterBoard` (PosterBoard may not start at all) and
   a visible default wallpaper. Possible failure: `exec SBRendererService` followed by
   `CRASH SBRendererService` (NSInvalidArgumentException, nil Metal command queue), or
   still black. If SpringBoard crash-loops, undo with `--no-legacy-wallpaper`.
3. Depending on 1: option C (persist one poster).

No firmware is needed from the phone. No kernel change in this lane; the Mac tree
`ci-lane-wallpaper` was not used.
