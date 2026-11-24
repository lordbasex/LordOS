#!/bin/bash
# The Storage window's figures on the file systems that share extents (docs/VIRTUALIZATION.md 6.16): the e2e flows
# premade, clone and storage, each on a scratch loop file system: btrfs, btrfs with compression (the figures must say
# "about": MH_EXPECT_APPROX) and XFS. Needs root only to make, mount and hand over the empty file systems (sudo asks);
# everything else runs as the person, in the harness's own namespaces. The file systems' tools come from Debian's
# btrfs-progs and xfsprogs packages: installed, or unpacked with `apt-get download` + `dpkg -x` into MKFS_ROOT.
#   bash tools/vm/e2e/machines/storage-fs.sh [btrfs] [btrfs-zstd] [xfs]      (from the repository; ~15 min)
set -u
REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
W=${MH_FS_WORK:-/var/tmp/lordos-m100-fs}
X=${MKFS_ROOT:-}
mkdir -p "$W"
ME=$(id -u):$(id -g)
mkfs_of() { if [ -n "$X" ]; then echo "$X/usr/sbin/$1"; else command -v "$1" || echo "/usr/sbin/$1"; fi; }
fails=0
run_one() {
    local kind=$1 mkfs opts approx img mnt
    case $kind in
        btrfs) mkfs=$(mkfs_of mkfs.btrfs); opts=""; approx=0 ;;
        btrfs-zstd) mkfs=$(mkfs_of mkfs.btrfs); opts="compress-force=zstd"; approx=1 ;;
        xfs) mkfs=$(mkfs_of mkfs.xfs); opts=""; approx=0 ;;
        *) echo "unknown: $kind"; return 2 ;;
    esac
    [ -x "$mkfs" ] || { echo "skipped $kind: needs $mkfs"; return 0; }
    img=$W/$kind.img; mnt=$W/mnt-$kind
    mkdir -p "$mnt"
    rm -f "$img"; truncate -s 12G "$img"
    "$mkfs" -q "$img" >"$W/$kind.mkfs.log" 2>&1 || "$mkfs" "$img" >"$W/$kind.mkfs.log" 2>&1 || { echo "$kind: mkfs failed"; cat "$W/$kind.mkfs.log"; return 1; }
    sudo mount -o "loop${opts:+,$opts}" "$img" "$mnt" || { echo "$kind: mount failed"; return 1; }
    sudo chown "$ME" "$mnt"
    echo "== $kind: $(stat -f -c %T "$mnt") ($(findmnt -n -o OPTIONS "$mnt"))"
    (cd "$REPO" && MH_SCRATCH="$mnt/e2e" MH_EXPECT_APPROX=$approx python3 -I tools/vm/e2e/machines/run.py --out "$W/shots-$kind" --json "$W/$kind.json" premade clone storage)
    local rc=$?
    [ "$rc" = 0 ] || fails=$((fails + 1))
    sudo umount "$mnt"; rm -f "$img"; rmdir "$mnt"
}
[ $# -gt 0 ] || set -- btrfs btrfs-zstd xfs
for k in "$@"; do run_one "$k"; done
echo "== storage on shared-extent file systems: $fails failed (the figures: $W/*.json)"
[ "$fails" = 0 ]
