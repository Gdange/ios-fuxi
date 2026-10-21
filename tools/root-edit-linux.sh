#!/bin/bash
# Edit the iOS root image (case-sensitive journaled HFS+) on Linux, replacing the
# macOS hdiutil/mount_hfs step. Runs in the fuxi-hfs container (tools/docker/fuxi-hfs)
# because loop-mounting needs root. The image is mounted with the kernel hfsplus
# driver (-o force). That driver ignores the journal, and iOS never unmounts the root
# cleanly (the phone is reset), so committed transactions are first replayed with
# tools/hfs-journal-replay.py. The volume is a little smaller than the image file and
# its alternate volume header sits at the end of the volume, so the loop device is
# limited to the volume size. fsck.hfsplus -n checks the volume before and after;
# only open-unlinked files left by the reset are accepted, anything else fails.
# Linux mounts a volume without the unmounted bit read-only, so after replay and a
# clean fsck the bit is set; after the Linux unmount lastMountedVersion is put back
# to HFSJ (the journal is still empty) so XNU mounts it as a clean journaled volume.
#
#   tools/root-edit-linux.sh IMAGE INPUT_DIR -- COMMAND...
#
# Inside COMMAND: $MNT is the mounted root, $IN the read-only inputs, $TOOLS the
# read-only repository tools/ directory. Example:
#   tools/root-edit-linux.sh ios-root-mod.img new -- python3 '$TOOLS/stage-fuxi-metal.py' '$MNT' ...
set -euo pipefail
[ $# -ge 4 ] && [ "$3" = "--" ] || { echo "usage: $0 IMAGE INPUT_DIR -- COMMAND..." >&2; exit 2; }
img=$(realpath "$1"); in=$(realpath "$2"); shift 3
tools=$(realpath "$(dirname "$0")")
[ -f "$img" ] && [ -d "$in" ] || { echo "missing image or input dir" >&2; exit 2; }
docker image inspect fuxi-hfs:1 >/dev/null 2>&1 || docker build -q -t fuxi-hfs:1 "$tools/docker/fuxi-hfs" >/dev/null
docker run --rm --privileged -v /lib/modules:/lib/modules:ro \
  -v "$(dirname "$img")":/work -v "$in":/in:ro -v "$tools":/tools:ro \
  -e IMG="/work/$(basename "$img")" -e MNT=/mnt/root -e IN=/in -e TOOLS=/tools \
  fuxi-hfs:1 bash -c '
set -euo pipefail
modprobe hfsplus 2>/dev/null || grep -qw hfsplus /proc/filesystems
sig=$(dd if="$IMG" bs=1 skip=1024 count=2 2>/dev/null)
[ "$sig" = HX ] || { echo "not an HFSX volume (signature $sig)" >&2; exit 3; }
check() {  # fsck problems other than orphaned open-unlinked files fail
  fsck.hfsplus -n -f "$dev" >/tmp/fsck-$1.txt 2>&1 || true
  if grep -q "appears to be OK" /tmp/fsck-$1.txt; then echo "fsck $1: OK"; return; fi
  # Accepted: open-unlinked files left by the reset; the iOS hfs driver removes them at
  # its next read-write mount.
  bad=$(grep -E "^\s+\S" /tmp/fsck-$1.txt | grep -vE "Executing fsck_hfs|The volume name is|Orphaned open unlinked file" || true)
  if [ -n "$bad" ]; then cat /tmp/fsck-$1.txt; echo "fsck $1 NOT clean: $bad" >&2; exit 4; fi
  echo "fsck $1: OK apart from $(grep -c "Orphaned open unlinked" /tmp/fsck-$1.txt) orphaned open-unlinked files"
}
python3 -I /tools/hfs-journal-replay.py "$IMG" --apply
vol=$(python3 -I -c "import struct,sys;f=open(sys.argv[1],\"rb\");f.seek(1064);b,n=struct.unpack(\">II\",f.read(8));print(b*n)" "$IMG")
# The container gets a snapshot of /dev: a loop node the host creates later is missing.
dev=$(losetup -f); [ -e "$dev" ] || mknod "$dev" b 7 "${dev#/dev/loop}"
losetup --sizelimit "$vol" "$dev" "$IMG"; trap "umount $MNT 2>/dev/null || true; losetup -d $dev" EXIT
echo "volume $vol bytes on $dev"
check before
python3 -I /tools/hfs-journal-replay.py "$IMG" --mark-unmounted
mkdir -p "$MNT"; mount -t hfsplus -o force,rw "$dev" "$MNT"
grep -q " $MNT hfsplus rw" /proc/mounts || { echo "mount is not read-write" >&2; exit 5; }
echo "mounted $dev rw"
eval "$*"
rm -rf "$MNT/.fseventsd" "$MNT/.Spotlight-V100"
sync; umount "$MNT"
check after
python3 -I /tools/hfs-journal-replay.py "$IMG" --mark-journaled
python3 -I /tools/hfs-journal-replay.py "$IMG" | grep -q JOURNAL_EMPTY || { echo "journal not empty after edit" >&2; exit 7; }
losetup -d "$dev"; trap - EXIT
echo ROOT_EDIT_DONE
' fuxi-root-edit "$@"
