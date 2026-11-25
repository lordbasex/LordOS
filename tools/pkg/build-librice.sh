#!/bin/sh
# build-librice.sh — librice (rice-proto and rice-io, the ICE library WPE WebKit runs in its network process for
# WebRTC) as a Debian 13 package built by LordOS (docs/WPEWEBKIT.md, "librice"). Debian has no librice.
#
#   tools/pkg/build-librice.sh [lordos-revision]          ->  librice 0.4.3-1~lordos13+<revision> (default 1)
#
# 1. fetch    upstream's tag tarball (github.com/ystreet/librice, v$UPV), checked against its pinned SHA-256
#             (tools/pkg/librice.sums)
# 2. vendor   in a clean trixie chroot with cargo: `cargo vendor --locked`, every crate upstream's Cargo.lock names, at
#             the version and checksum it pins (cargo checks each download against the lock file), as the second
#             orig tarball (librice_<v>.orig-vendor.tar.xz, made reproducibly: sorted, fixed time and owner), checked
#             against its pinned SHA-256 too; debian/copyright is written from the crates' own license fields
#             (tools/pkg/librice-chroot.sh). Only this step uses the network. VENDOR=1 makes the tarball again (then
#             compare and pin its SHA-256: a different one means the crates changed)
# 3. build    in a clean trixie chroot (mmdebstrap --variant=buildd): the source package (Debian's 3.0 (quilt) with the
#             two tarballs and tools/pkg/librice/debian), its build dependencies from trixie, dpkg-buildpackage -sa as
#             an unprivileged user with cargo offline and frozen, lintian
# 4. test     in a second clean trixie chroot: the packages install, pkg-config finds rice-proto and rice-io at the
#             version, and a C program built against both libraries runs (tools/pkg/librice-test.c)
#
# Output: $LORDOS_WPE_WORK/librice/<version>/<arch>/out/ (default /var/tmp/lordos-wpewebkit): the .debs, the source
# package (.dsc, both orig tarballs, .debian.tar.xz: publish it next to the binaries, MIT or Apache-2.0 and the crates'
# licenses), .changes, .buildinfo, lintian.txt, test/report.txt. Nothing is signed or published here.
# ARCH=arm64 builds under qemu-user as tools/pkg/build-wpewebkit.sh does. JOBS, TMPFS, KEEP as there.
set -eu

UPV=0.4.3
REV=${1:-1}
case "$REV" in *[!0-9]*|'') echo "build-librice.sh: the LordOS revision is a number" >&2; exit 1 ;; esac
NEWVER="$UPV-1~lordos13+$REV"
HERE=$(cd "$(dirname "$0")" && pwd)
SUMS=$HERE/librice.sums
ARCH=${ARCH:-$(dpkg --print-architecture)}
case "$ARCH" in amd64|arm64) ;; *) echo "build-librice.sh: ARCH is amd64 or arm64" >&2; exit 1 ;; esac
WORK=${LORDOS_WPE_WORK:-/var/tmp/lordos-wpewebkit}/librice/$NEWVER/$ARCH
MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
SECURITY=${DEBIAN_SECURITY:-http://security.debian.org/debian-security}
SUITE=trixie
SOURCES1="deb $MIRROR $SUITE main"
SOURCES2="deb $MIRROR $SUITE-updates main"
SOURCES3="deb $SECURITY $SUITE-security main"
JOBS=${JOBS:-$( [ "$ARCH" = "$(dpkg --print-architecture)" ] && nproc || echo 8 )}
TMPFS=${TMPFS:-}
die() { echo "build-librice.sh: $*" >&2; exit 1; }
command -v mmdebstrap >/dev/null || die "needs mmdebstrap"
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
LOW="nice -n 19 ionice -c 3"
EMULATED=; [ "$ARCH" = "$(dpkg --print-architecture)" ] || EMULATED=1

drop_dir() {
    for m in $(findmnt -rn -o TARGET | awk -v d="$1" 'index($0, d "/") == 1 || $0 == d' | sort -r); do $SUDO umount -l "$m"; done
    [ -z "$(findmnt -rn -o TARGET | awk -v d="$1" 'index($0, d "/") == 1 || $0 == d')" ] || die "$1 still has mounts"
    $SUDO rm -rf --one-file-system "$1"
}
fresh_dir() {
    drop_dir "$1"
    mkdir -p "$1"
    [ -z "$TMPFS" ] || $SUDO mount -t tmpfs -o "size=$TMPFS,mode=0755" lordos-librice "$1"
}
pinned() { awk -v f="$1" '$2 == f { print $1 }' "$SUMS"; }

mkdir -p "$WORK/in" "$WORK/out"
echo "== librice $UPV -> $NEWVER ($ARCH) in $WORK"

# 1. fetch
ORIG=librice_$UPV.orig.tar.gz
if [ ! -s "$WORK/in/$ORIG" ]; then
    curl -fsSL -o "$WORK/in/$ORIG.part" "https://github.com/ystreet/librice/archive/refs/tags/v$UPV.tar.gz" || die "cannot download librice v$UPV"
    mv "$WORK/in/$ORIG.part" "$WORK/in/$ORIG"
fi
want=$(pinned "$ORIG")
[ -n "$want" ] || die "$ORIG has no pinned SHA-256 in $SUMS"
[ "$(sha256sum < "$WORK/in/$ORIG" | cut -d' ' -f1)" = "$want" ] || die "$ORIG doesn't match its pinned SHA-256"
echo "upstream tarball: $want (pinned)"

# 2. vendor (the network, once)
VEND=librice_$UPV.orig-vendor.tar.xz
if [ -n "${VENDOR:-}" ] || [ ! -s "$WORK/in/$VEND" ]; then
    fresh_dir "$WORK/vendor-chroot"
    $SUDO $LOW mmdebstrap --variant=apt --mode=root --architectures="$(dpkg --print-architecture)" --dpkgopt=force-unsafe-io \
        --include=ca-certificates,cargo,rustc,python3,xz-utils \
        --customize-hook='mkdir -p "$1/build/in" "$1/build/out"' \
        --customize-hook="upload $WORK/in/$ORIG /build/in/$ORIG" \
        --customize-hook="upload $HERE/librice-chroot.sh /build/librice-chroot.sh" \
        --customize-hook="chroot \"\$1\" sh /build/librice-chroot.sh vendor $UPV || echo FAILED > \"\$1/build/out/FAILED\"" \
        --customize-hook="sync-out /build/out $WORK/vendor-out" \
        "$SUITE" "$WORK/vendor-chroot" "$SOURCES1" "$SOURCES2" "$SOURCES3"
    $SUDO chown -R "$(id -u):$(id -g)" "$WORK/vendor-out"
    [ ! -e "$WORK/vendor-out/FAILED" ] || die "vendoring failed"
    cp "$WORK/vendor-out/$VEND" "$WORK/vendor-out/copyright" "$WORK/in/"
    [ -n "${KEEP:-}" ] || drop_dir "$WORK/vendor-chroot"
fi
got=$(sha256sum < "$WORK/in/$VEND" | cut -d' ' -f1)
want=$(pinned "$VEND")
[ -n "$want" ] || die "$VEND is $got and has no pinned SHA-256 in $SUMS: check it (the crates Cargo.lock names) and pin it"
[ "$got" = "$want" ] || die "$VEND is $got, not the pinned $want: the vendored crates differ"
echo "vendored crates: $got (pinned)"

# 3. build
built() { [ -s "$WORK/out/librice_${NEWVER}_$ARCH.changes" ] && [ -s "$WORK/out/lintian-summary.txt" ] && [ ! -e "$WORK/out/FAILED" ]; }
if [ -z "${TEST_ONLY:-}" ] && { [ -n "${REBUILD:-}" ] || ! built; }; then
    fresh_dir "$WORK/build-chroot"
    $SUDO find "$WORK/out" -maxdepth 1 -type f -delete
    tar -C "$HERE/librice" -cf "$WORK/debian.tar" debian
    $SUDO $LOW mmdebstrap --variant=buildd --mode=root --architectures="$ARCH" --dpkgopt=force-unsafe-io \
        --include=ca-certificates,devscripts,lintian,equivs,fakeroot \
        --customize-hook='mkdir -p "$1/build/in" "$1/build/out"' \
        --customize-hook="sync-in $WORK/in /build/in" \
        --customize-hook="upload $WORK/debian.tar /build/debian.tar" \
        --customize-hook="upload $HERE/librice-chroot.sh /build/librice-chroot.sh" \
        --customize-hook="mount --bind \"$WORK/out\" \"\$1/build/out\"" \
        --customize-hook="chroot \"\$1\" env JOBS=$JOBS sh /build/librice-chroot.sh build $UPV $NEWVER || { echo FAILED > \"\$1/build/out/FAILED\"; }; sync; umount \"\$1/build/out\"" \
        "$SUITE" "$WORK/build-chroot" "$SOURCES1" "$SOURCES2" "$SOURCES3"
    $SUDO chown -R "$(id -u):$(id -g)" "$WORK/out"
    [ ! -e "$WORK/out/FAILED" ] || die "the build failed (the chroot is $WORK/build-chroot)"
else
    built || die "no finished build in $WORK/out"
fi

# 4. test
rm -rf "$WORK/debs" "$WORK/out/test" && mkdir -p "$WORK/debs" "$WORK/out/test"
cp "$WORK/out/"*.deb "$WORK/debs/" && rm -f "$WORK/debs/"*-dbgsym_*.deb
fresh_dir "$WORK/test-chroot"
$SUDO $LOW mmdebstrap --variant=apt --mode=root --architectures="$ARCH" --dpkgopt=force-unsafe-io \
    --include=ca-certificates,gcc,libc6-dev,pkgconf \
    --customize-hook='mkdir -p "$1/build/debs" "$1/build/test"' \
    --customize-hook="sync-in $WORK/debs /build/debs" \
    --customize-hook="upload $HERE/librice-chroot.sh /build/librice-chroot.sh" \
    --customize-hook="upload $HERE/librice-test.c /build/librice-test.c" \
    --customize-hook="chroot \"\$1\" env EMULATED=$EMULATED sh /build/librice-chroot.sh test $UPV || echo FAILED > \"\$1/build/test/FAILED\"" \
    --customize-hook="sync-out /build/test $WORK/out/test" \
    "$SUITE" "$WORK/test-chroot" "$SOURCES1" "$SOURCES2" "$SOURCES3"
$SUDO chown -R "$(id -u):$(id -g)" "$WORK/out/test"
[ ! -e "$WORK/out/test/FAILED" ] || die "the test failed: $WORK/out/test/report.txt"
if [ -z "${KEEP:-}" ]; then drop_dir "$WORK/build-chroot"; drop_dir "$WORK/test-chroot"; fi
(cd "$WORK/out" && sha256sum ./*.deb ./*.dsc ./*.tar.* > SHA256SUMS)
echo "== done: $WORK/out"
cat "$WORK/out/lintian-summary.txt" "$WORK/out/test/report.txt"
