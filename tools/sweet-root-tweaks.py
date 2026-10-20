#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py [OPTIONS] MOUNTED_ROOT

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
turned the plain mkiosroot.sh output into the images that reach the home
screen (image versions v8-v14 in NOTES.md):

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); CommCenter jobs removed (no baseband: it
                  crash-loops and Setup blocks on its CoreTelephony XPC)
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark seeded; the actual Activated state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py

Opt-in steps (off unless named; docs-lanes/wallpaper.md has the evidence):

  --posterboard-oslog   diagnostic: OS_ACTIVITY_DT_MODE=YES in PosterBoard's
                        launchd job, so its os_log output also goes to stderr
                        (= /dev/null, which the sweetnull boot-arg tees into
                        the kernel log as "|PosterBoard| ...")
  --legacy-wallpaper    experiment: feature-flag override SpringBoard/Autobahn
                        off in /Library/Preferences/FeatureFlags, i.e. the
                        iPad-style iOS 16 wallpaper path without PosterBoard
                        posters
  --no-legacy-wallpaper remove that override again
  --only-optional       apply just the named opt-in steps, not the base set
"""
import argparse, glob, os, plistlib, shutil, sys

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.CommCenter']
    for k in removed:
        del jobs[k]
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} CommCenter jobs removed')


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    save(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('data ark', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


POSTERBOARD_JOB = '/System/Library/LaunchAngels/com.apple.PosterBoard.plist'
FEATURE_OVERRIDE = 'Library/Preferences/FeatureFlags/Domain/SpringBoard.plist'


def posterboard_oslog(root):
    """PosterBoard's os_log to stderr. launchd reads the job from the
    launchd.plist cache (launchd_unsecure_cache), so the cached entry is
    changed, not the LaunchAngels plist."""
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    job = plist['LaunchDaemons'].get(POSTERBOARD_JOB)
    if job is None or job.get('Label') != 'com.apple.PosterBoard':
        sys.exit('launchd.plist: no com.apple.PosterBoard job at ' + POSTERBOARD_JOB)
    job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('launchd.plist: PosterBoard OS_ACTIVITY_DT_MODE=YES')


def legacy_wallpaper(root, enable):
    """libsystem_featureflags merges /Library/Preferences/FeatureFlags over
    /System/Library/FeatureFlags. Autobahn is the iOS 16 poster lock screen;
    with it off PosterBoard answers "Unsupported on this platform" (its iPad
    branch) and SpringBoard keeps the legacy wallpaper."""
    path = os.path.join(root, FEATURE_OVERRIDE)
    if not enable:
        if os.path.exists(path):
            data = load(path)
            data.pop('Autobahn', None)
            if data:
                save(path, data, 0, 0, 0o644)
            else:
                os.remove(path)
        print('feature flags: SpringBoard/Autobahn override removed')
        return
    d = os.path.dirname(path)
    for sub in (os.path.dirname(d), d):
        if not os.path.isdir(sub):
            os.makedirs(sub)
            os.chown(sub, 0, 0)
            os.chmod(sub, 0o755)
    data = load(path) if os.path.exists(path) else {}
    data['Autobahn'] = {'Enabled': False}
    save(path, data, 0, 0, 0o644, plistlib.FMT_XML)
    print('feature flags: SpringBoard/Autobahn disabled (legacy wallpaper)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root')
    ap.add_argument('--posterboard-oslog', action='store_true')
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--legacy-wallpaper', action='store_true')
    group.add_argument('--no-legacy-wallpaper', action='store_true')
    ap.add_argument('--only-optional', action='store_true')
    args = ap.parse_args()
    root = args.root
    if not os.path.isdir(os.path.join(root, 'System/Library/xpc')):
        ap.error('not a mounted iOS root: ' + root)
    if args.posterboard_oslog:
        posterboard_oslog(root)
    if args.legacy_wallpaper or args.no_legacy_wallpaper:
        legacy_wallpaper(root, args.legacy_wallpaper)
    if args.only_optional:
        return
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()
