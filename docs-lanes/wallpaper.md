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
