#!/bin/sh
# build-wpewebkit.sh — LordOS's backport of Debian's WPE WebKit to Debian 13 (trixie), with WPEPlatform on
# (docs/WPEWEBKIT.md). Redo it for every WebKit Security Advisory (WSA) and Debian security update.
#
#   tools/pkg/build-wpewebkit.sh <debian-version> [lordos-revision]
#   tools/pkg/build-wpewebkit.sh 2.54.3-1          ->  wpewebkit 2.54.3-1~lordos13+1
#
# 1. fetch    Debian's source package (.dsc, .orig.tar.xz and its .asc, .debian.tar.xz) from deb.debian.org's pool,
#             or from snapshot.debian.org once the pool has moved on, and wpewebkit.org's signature of the tarball
# 2. build    in a clean trixie chroot (mmdebstrap --variant=buildd): dscverify against debian-keyring (the
#             uploader's signature and every checksum), the upstream signature against the pinned WebKit release
#             keys, LordOS's changes (tools/pkg/wpewebkit-chroot.sh, backport()), the build dependencies from trixie
#             only, dpkg-buildpackage -sa (source package + every binary package), lintian
# 3. test     in a second clean trixie chroot: install the .debs, pkg-config of wpe-webkit-2.0, wpe-platform-2.0 (+ drm,
#             headless, wayland), wpe-1.0 and wpebackend-fdo-1.0, SONAME and exported symbols against trixie's 2.48,
#             the copyright files, and a WPEPlatform headless program that renders a page into a buffer
#
# Output: $LORDOS_WPE_WORK/<version>/out/ (default /var/tmp/lordos-wpewebkit): the .debs, the source package (.dsc,
# .orig.tar.xz, .orig.tar.xz.asc, .debian.tar.xz — publish it next to the binaries, LGPL-2.1+), .changes, .buildinfo,
# build-time.txt, lintian.txt, lintian-summary.txt; out/test/ has the test report and the rendered frame.
# Nothing is signed or published here: the maintainer adds the packages with tools/release/apt-repo.sh.
#
# Needs: Debian with mmdebstrap and root through sudo (mmdebstrap's root mode, for real mounts in the chroot); the
# host's own packages are never touched. JOBS=<n> limits the build (default: every core). KEEP=1 keeps the chroots.
# TEST_ONLY=1 runs step 3 again on the packages already in out/. Interrupted (a power cut while building in a tmpfs):
# run it again; a build that had finished (its .changes and lintian summary in out/) isn't redone, REBUILD=1 redoes it.
# out/ is on disk from the start: the build chroot writes its results straight into it (a bind mount), not at the
# end; the test (short, and run again by a rerun) copies its report out when it ends.
# Everything runs at the lowest CPU and I/O priority (nice 19, ionice idle): the build machine is a desktop in use.
# JOBS defaults to every core natively and to 8 under qemu-user (ARCH other than the machine's).
# TMPFS=<size> (e.g. 64G) builds in a tmpfs mounted on the chroot folders: on a busy hard disk, dpkg's syncs while the
# chroots are made take far longer than the build; the build needs about 30 GB. The chroots are throwaway, so dpkg also
# runs with force-unsafe-io in them.
# ARCH=amd64|arm64 (default: this machine's). Another architecture than the machine's runs under qemu-user (its binfmt
# handler must be registered with the F flag, as Debian's qemu-user-static does): slow, but the same chroots, checks and
# tests (docs/WPEWEBKIT.md "arm64").
# EXTRA_DEBS=<folder> (since +4): LordOS's own packages the build needs that trixie lacks (librice0 and librice-dev,
# tools/pkg/build-librice.sh), installed in the build chroot before the build dependencies and in the test chroot with
# the new packages; never anything else (the folder's *.deb only, no -dbgsym).
# LORDOS_SRC=<LordOS tree> (default: the tree this script is in) adds lord-web to the test: built on both of its view
# backends (WPEPlatform and legacy) against the new packages, then its web-backend-test and probe test; LORDOS_SRC=none
# skips it.
set -eu

DVER=${1:?usage: build-wpewebkit.sh <debian-version, e.g. 2.54.3-1> [lordos-revision]}
REV=${2:-1}
case "$DVER" in *[!0-9.a-z+~-]*|*-*-*|[!0-9]*) echo "build-wpewebkit.sh: bad Debian version $DVER" >&2; exit 1 ;; esac
case "$DVER" in *-*) ;; *) echo "build-wpewebkit.sh: $DVER has no Debian revision (2.54.3-1)" >&2; exit 1 ;; esac
case "$REV" in *[!0-9]*|'') echo "build-wpewebkit.sh: the LordOS revision is a number" >&2; exit 1 ;; esac
UP=${DVER%-*}
NEWVER="$DVER~lordos13+$REV"
HERE=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-$(dpkg --print-architecture)}
case "$ARCH" in amd64|arm64) ;; *) echo "build-wpewebkit.sh: ARCH is amd64 or arm64, not $ARCH" >&2; exit 1 ;; esac
WORK=${LORDOS_WPE_WORK:-/var/tmp/lordos-wpewebkit}/$NEWVER/$ARCH
MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
SECURITY=${DEBIAN_SECURITY:-http://security.debian.org/debian-security}
SUITE=trixie
# both chroots read trixie with its point-release updates and its security archive, like an installed LordOS
SOURCES1="deb $MIRROR $SUITE main"
SOURCES2="deb $MIRROR $SUITE-updates main"
SOURCES3="deb $SECURITY $SUITE-security main"
JOBS=${JOBS:-$( [ "$ARCH" = "$(dpkg --print-architecture)" ] && nproc || echo 8 )}

die() { echo "build-wpewebkit.sh: $*" >&2; exit 1; }
command -v mmdebstrap >/dev/null || die "needs mmdebstrap (sudo apt-get install mmdebstrap)"
command -v curl >/dev/null || die "needs curl"
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
# mmdebstrap and everything it starts (dpkg, the compilers, the tests) inherit this priority
LOW="nice -n 19 ionice -c 3"
if [ "$ARCH" != "$(dpkg --print-architecture)" ]; then
    q=$(case "$ARCH" in arm64) echo aarch64 ;; amd64) echo x86_64 ;; esac)
    grep -qs '^enabled' "/proc/sys/fs/binfmt_misc/qemu-$q" && grep -qs '^flags:.*F' "/proc/sys/fs/binfmt_misc/qemu-$q" ||
        die "building $ARCH here needs qemu-user's binfmt handler for $q with the F flag (apt-get install qemu-user-static)"
fi
EMULATED=; [ "$ARCH" = "$(dpkg --print-architecture)" ] || EMULATED=1
LORDOS_SRC=${LORDOS_SRC:-$(cd "$HERE/../.." && pwd)}
[ "$LORDOS_SRC" = none ] || [ -f "$LORDOS_SRC/server/mk/web.mk" ] || die "LORDOS_SRC=$LORDOS_SRC is not a LordOS tree (or say none)"
TMPFS=${TMPFS:-}
case "$TMPFS" in ''|[0-9]*[GM]) ;; *) die "TMPFS is a size like 64G" ;; esac

# remove a chroot folder: first whatever is still mounted in it (a run that was killed leaves /proc, /dev… there),
# and never across a file system boundary, so a mount that stayed can't take the host's files with it
drop_dir() {
    for m in $(findmnt -rn -o TARGET | awk -v d="$1" 'index($0, d "/") == 1 || $0 == d' | sort -r); do
        $SUDO umount -l "$m"
    done
    [ -z "$(findmnt -rn -o TARGET | awk -v d="$1" 'index($0, d "/") == 1 || $0 == d')" ] || die "$1 still has mounts"
    $SUDO rm -rf --one-file-system "$1"
}
# a fresh, empty folder for a chroot (a tmpfs with TMPFS=<size>)
fresh_dir() {
    drop_dir "$1"
    mkdir -p "$1"
    [ -z "$TMPFS" ] || $SUDO mount -t tmpfs -o "size=$TMPFS,mode=0755" lordos-wpewebkit "$1"
}

EXTRA_DEBS=${EXTRA_DEBS:-}
mkdir -p "$WORK/in" "$WORK/out" "$WORK/extra"
rm -f "$WORK/extra/"*.deb
if [ -n "$EXTRA_DEBS" ]; then
    ls "$EXTRA_DEBS"/*.deb >/dev/null 2>&1 || die "EXTRA_DEBS=$EXTRA_DEBS has no .deb"
    cp "$EXTRA_DEBS"/*.deb "$WORK/extra/" && rm -f "$WORK/extra/"*-dbgsym_*.deb
fi
rm -f "$WORK/out/FAILED" "$WORK/out/test/FAILED"
echo "== wpewebkit $DVER -> $NEWVER ($ARCH) in $WORK"

# 1. fetch: the .dsc from the pool, else from snapshot.debian.org; then each file it lists. Nothing is trusted here:
#    every byte is checked inside the chroot (verify_source) before it is used.
fetch() {
    f=$1
    [ -s "$WORK/in/$f" ] && return 0
    curl -fsSL -o "$WORK/in/$f.part" "$MIRROR/pool/main/w/wpewebkit/$f" 2>/dev/null || {
        # snapshot.debian.org's API: the files of this source version, name -> hash
        h=$(curl -fsSL "https://snapshot.debian.org/mr/package/wpewebkit/$DVER/srcfiles?fileinfo=1" 2>/dev/null |
            python3 -I -c 'import json,sys
d = json.load(sys.stdin)["fileinfo"]
print(next((h for h, v in d.items() if any(i["name"] == sys.argv[1] for i in v)), ""))' "$f" 2>/dev/null) || h=
        [ -n "$h" ] || die "$f is neither in $MIRROR nor on snapshot.debian.org"
        curl -fsSL -o "$WORK/in/$f.part" "https://snapshot.debian.org/file/$h" || die "cannot download $f"
    }
    mv "$WORK/in/$f.part" "$WORK/in/$f"
}
fetch "wpewebkit_$DVER.dsc"
for f in $(awk '/^Checksums-Sha256:/ { s = 1; next } /^[^ ]/ { s = 0 } s && NF == 3 { print $3 }' "$WORK/in/wpewebkit_$DVER.dsc"); do
    case "$f" in */*|.*) die "unexpected file name in the .dsc: $f" ;; esac
    fetch "$f"
done
curl -fsSL -o "$WORK/in/wpewebkit-$UP.tar.xz.asc" "https://wpewebkit.org/releases/wpewebkit-$UP.tar.xz.asc" ||
    { rm -f "$WORK/in/wpewebkit-$UP.tar.xz.asc"; echo "  (wpewebkit.org has no wpewebkit-$UP.tar.xz.asc: only Debian's copy is checked)"; }
(cd "$WORK/in" && sha256sum ./* > "$WORK/out/SHA256SUMS.debian-source")
cat "$WORK/out/SHA256SUMS.debian-source"

# 2. build in a fresh chroot (kept in $WORK/build-chroot until the next run, for a look after a failure); out/ is
#    bind-mounted into it, so each result is on disk as soon as it exists
built() { [ -s "$WORK/out/wpewebkit_${NEWVER}_$ARCH.changes" ] && [ -s "$WORK/out/lintian-summary.txt" ] && [ ! -e "$WORK/out/FAILED" ]; }
if [ -z "${TEST_ONLY:-}" ] && { [ -n "${REBUILD:-}" ] || ! built; }; then
    fresh_dir "$WORK/build-chroot"
    # what an interrupted run left: nothing of it is kept
    $SUDO find "$WORK/out" -maxdepth 1 -type f ! -name SHA256SUMS.debian-source -delete
    $SUDO $LOW mmdebstrap --variant=buildd --mode=root --architectures="$ARCH" --dpkgopt=force-unsafe-io \
        --include=ca-certificates,devscripts,debian-keyring,lintian,equivs,gnupg,gpgv,fakeroot \
        --customize-hook='mkdir -p "$1/build/in" "$1/build/out"' \
        --customize-hook="sync-in $WORK/in /build/in" \
        --customize-hook='mkdir -p "$1/build/extra"' \
        --customize-hook="sync-in $WORK/extra /build/extra" \
        --customize-hook="upload $HERE/wpewebkit-chroot.sh /build/wpewebkit-chroot.sh" \
        --customize-hook="mount --bind \"$WORK/out\" \"\$1/build/out\"" \
        --customize-hook="chroot \"\$1\" env JOBS=$JOBS sh /build/wpewebkit-chroot.sh build $DVER $NEWVER || { echo FAILED > \"\$1/build/out/FAILED\"; }; sync; umount \"\$1/build/out\"" \
        "$SUITE" "$WORK/build-chroot" "$SOURCES1" "$SOURCES2" "$SOURCES3"
    $SUDO chown -R "$(id -u):$(id -g)" "$WORK/out"
    [ ! -e "$WORK/out/FAILED" ] || die "the build failed (log above; the chroot is $WORK/build-chroot)"
else
    built || die "no finished build in $WORK/out (TEST_ONLY=1 needs one)"
    echo "== the build in $WORK/out is complete: not built again (REBUILD=1 rebuilds)"
fi

# 3. test in another fresh chroot, with only trixie and the new packages. It runs as the root of its own mount
#    namespace (tools/pkg/wpewebkit-pivot.sh, pivot_root): the kernel refuses user namespaces inside a chroot, and
#    WebKit's bubblewrap sandbox needs them, so this is how the test runs WebKit with its sandbox on, like a desktop
$SUDO rm -rf "$WORK/out/test" "$WORK/debs"
mkdir -p "$WORK/out/test" "$WORK/debs"
cp "$WORK/out/"*.deb "$WORK/debs/"
cp "$WORK/extra/"*.deb "$WORK/debs/" 2>/dev/null || true
rm -f "$WORK/debs/"*-dbgsym_*.deb "$WORK/debs/libwpewebkit-doc_"*.deb
fresh_dir "$WORK/test-chroot"
rm -f "$WORK/lordos-src.tar"
SRC_HOOK=true
if [ "$LORDOS_SRC" != none ]; then
    # the tree as it is on disk (its own build output left out)
    tar -C "$LORDOS_SRC" --exclude=server/build -cf "$WORK/lordos-src.tar" server third_party package.json
    SRC_HOOK="upload $WORK/lordos-src.tar /build/lordos-src.tar"
fi
$SUDO $LOW mmdebstrap --variant=apt --mode=root --architectures="$ARCH" --dpkgopt=force-unsafe-io \
    --include=ca-certificates,gcc,libc6-dev,pkgconf,binutils,libegl-mesa0,libgl1-mesa-dri,libwpebackend-fdo-1.0-dev,passwd,make,libwayland-dev,wayland-protocols,libxkbcommon-dev,libegl-dev,python3,mount,gstreamer1.0-nice,gstreamer1.0-plugins-base,gstreamer1.0-plugins-good,gstreamer1.0-plugins-bad \
    --customize-hook='mkdir -p "$1/build/debs" "$1/build/test"' \
    --customize-hook="sync-in $WORK/debs /build/debs" \
    --customize-hook="$SRC_HOOK" \
    --customize-hook="upload $HERE/wpewebkit-chroot.sh /build/wpewebkit-chroot.sh" \
    --customize-hook="upload $HERE/wpewebkit-headless-test.c /build/wpewebkit-headless-test.c" \
    --customize-hook="unshare --mount --pid --fork sh $HERE/wpewebkit-pivot.sh \"\$1\" /usr/bin/env EMULATED=$EMULATED /bin/sh /build/wpewebkit-chroot.sh test || echo FAILED > \"\$1/build/test/FAILED\"" \
    --customize-hook="sync-out /build/test $WORK/out/test" \
    "$SUITE" "$WORK/test-chroot" "$SOURCES1" "$SOURCES2" "$SOURCES3"
$SUDO chown -R "$(id -u):$(id -g)" "$WORK/out/test"
[ ! -e "$WORK/out/test/FAILED" ] || die "the test failed: $WORK/out/test/report.txt"
if [ -z "${KEEP:-}" ]; then drop_dir "$WORK/build-chroot"; drop_dir "$WORK/test-chroot"; fi

(cd "$WORK/out" && sha256sum ./*.deb ./*.dsc ./*.tar.xz ./*.asc > SHA256SUMS)
echo "== done: $WORK/out"
cat "$WORK/out/build-time.txt" "$WORK/out/lintian-summary.txt"
ls -l "$WORK/out"/*.deb "$WORK/out"/*.dsc
