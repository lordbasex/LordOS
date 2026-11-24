#!/bin/sh
# wpewebkit-pivot.sh — run a command inside the chroot folder <root> as the root of a private mount namespace, for
# tools/pkg/build-wpewebkit.sh's test (docs/WPEWEBKIT.md). The kernel refuses to create user namespaces from inside a
# chroot, and WebKit's bubblewrap sandbox needs them; after pivot_root the folder is the namespace's real root, so the
# sandbox works as on a desktop. Fresh /proc, /sys, /dev (only null, zero, full, random, urandom, tty: no GPU, no
# disks), /dev/pts, /dev/shm and /tmp. Run as root under `unshare --mount --pid --fork`; nothing leaks to the host.
#
#   unshare --mount --pid --fork sh wpewebkit-pivot.sh <root> <command> [args…]
set -eu
R=${1:?usage: wpewebkit-pivot.sh <root> <command> [args...]}; shift
# never in the host's own mount namespace
[ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)" ] || { echo "wpewebkit-pivot.sh: run it under unshare --mount" >&2; exit 1; }
[ -x "$R/bin/sh" ] || { echo "wpewebkit-pivot.sh: $R is not a system folder" >&2; exit 1; }
mount --make-rprivate /
mount --bind "$R" "$R"
mount -t proc proc "$R/proc"
mount -t sysfs sysfs "$R/sys"
mount -t tmpfs -o mode=0755 dev "$R/dev"
for d in null zero full random urandom tty; do touch "$R/dev/$d"; mount --bind /dev/$d "$R/dev/$d"; done
mkdir -p "$R/dev/pts" "$R/dev/shm"
mount -t devpts -o newinstance,ptmxmode=0666 devpts "$R/dev/pts"
ln -s pts/ptmx "$R/dev/ptmx"
mount -t tmpfs tmpfs "$R/dev/shm"
mount -t tmpfs tmpfs "$R/tmp"
cd "$R"
mkdir -p .oldroot
pivot_root . .oldroot
# the chroot's own umount (the mount package is in its --include): the host's is another architecture's binary
# under ARCH=arm64, and its loader is not in this root
/usr/bin/umount -l /.oldroot
rmdir /.oldroot
exec "$@"
