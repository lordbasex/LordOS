#!/bin/sh
# build-arm64.sh — builds the arm64 live ISO on an x86-64 host: tools/iso/build.sh takes the architecture
# from the host, so it runs here inside an arm64 chroot, emulated by qemu-user. See docs/ISO.md.
#
#   sudo tools/iso/build-arm64.sh [work folder]     (from the repository root; default /var/tmp/lordos-iso-arm64)
#
# BUILD_CHROOT (default /var/tmp/lordos-arm64-br) is an arm64 Debian 13 with the build packages, as
# tools/rpi/build-img.sh makes it. It is only read: an overlay in a private mount namespace takes every
# change, so it stays as it was. The tree's server/build must be for aarch64 or empty; it is compiled
# with GPU=1 in the chroot. RELEASE=1 and REQUIRE_SIGNED_CODE=1 pass through. On an arm64 host, run
# tools/iso/build.sh directly.
set -eu
[ "$(id -u)" = 0 ] || { echo "build-arm64.sh: run as root (sudo)" >&2; exit 1; }
[ -f package.json ] && [ -d server ] || { echo "build-arm64.sh: run from the repository root" >&2; exit 1; }
TREE=$(pwd)
WORK=${1:-/var/tmp/lordos-iso-arm64}
BR=${BUILD_CHROOT:-/var/tmp/lordos-arm64-br}
[ -x "$BR/usr/bin/gcc" ] || { echo "build-arm64.sh: no arm64 build chroot in $BR" >&2; exit 1; }
J=${JOBS:-4}
mkdir -p "$WORK/upper" "$WORK/ofs" "$WORK/merged" "$WORK/iso"

# the new namespace's mounts go away with this process, and none reaches the host's /dev or /run
exec unshare --mount --propagation private sh -eu -c '
    BR=$1; WORK=$2; TREE=$3; J=$4; REL=$5; SIGNED=$6
    M="$WORK/merged"
    mount -t overlay overlay -o "lowerdir=$BR,upperdir=$WORK/upper,workdir=$WORK/ofs" "$M"
    mount --rbind /dev "$M/dev"; mount -t proc proc "$M/proc"; mount -t sysfs sysfs "$M/sys"
    mkdir -p "$M/tree" "$M/work"
    mount --bind "$TREE" "$M/tree"; mount --bind "$WORK/iso" "$M/work"
    cp /etc/resolv.conf "$M/etc/resolv.conf"
    chroot "$M" env DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 PATH=/usr/sbin:/usr/bin:/sbin:/bin \
        RELEASE="$REL" REQUIRE_SIGNED_CODE="$SIGNED" J="$J" sh -eu -c "
        apt-get update -q
        apt-get install -y -q --no-install-recommends debootstrap squashfs-tools xorriso grub-common \
            grub-efi-arm64-bin mtools dosfstools
        cd /tree
        make -C server -j\$J GPU=1 RELEASE=\$RELEASE
        make -C server build/canvas-test && server/build/canvas-test
        chmod -R g-w server/build
        tools/iso/build.sh /work
    "
' build-arm64 "$BR" "$WORK" "$TREE" "$J" "${RELEASE:-0}" "${REQUIRE_SIGNED_CODE:-0}"
