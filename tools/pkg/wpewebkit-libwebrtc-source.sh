#!/bin/sh
# wpewebkit-libwebrtc-source.sh — the libwebrtc component of LordOS's WPE WebKit source package (docs/WPEWEBKIT.md,
# "LibWebRTC (+5)"): WebKit's own copy of Google's libwebrtc, Source/ThirdParty/libwebrtc, which WebKit's release
# tarballs leave out. Made from WebKit's git at the release tag and pinned in tools/pkg/wpewebkit-libwebrtc.sums.
#
#   tools/pkg/wpewebkit-libwebrtc-source.sh <upstream-version> <folder>
#       -> <folder>/wpewebkit_<upstream-version>.orig-libwebrtc.tar.xz
#
# 1. fetch     WebKit's git (github.com/WebKit/WebKit) at the pinned commit only, sparse (Source/ThirdParty/libwebrtc and
#              Source/cmake), no history: about 400 MB
# 2. verify    the release tag resolves to the pinned tag object and commit; the subtree's git tree hash is the pinned
#              one (git's hashes cover every byte below it); and Source/cmake at that commit is byte for byte the
#              signed release tarball's, so the commit is the one WebKit released (the caller gives the tarball)
# 3. pack      git archive of the subtree under libwebrtc/ (every date is the commit's), without the prebuilt files it
#              carries (test fixtures: object files, static libraries, an Android .jar), xz -9 single-threaded:
#              the same tarball every time on Debian 13; its SHA-256 must be the pinned one
#
# The orig tarball for the check of step 2: ORIG=<path to wpewebkit_<version>.orig.tar.xz> (already verified by
# its signature, tools/pkg/wpewebkit-chroot.sh). Needs git, curl, xz, tar.
set -eu
UP=${1:?usage: wpewebkit-libwebrtc-source.sh <upstream-version> <folder>}
DEST=${2:?usage: wpewebkit-libwebrtc-source.sh <upstream-version> <folder>}
HERE=$(cd "$(dirname "$0")" && pwd)
SUMS=$HERE/wpewebkit-libwebrtc.sums
die() { echo "wpewebkit-libwebrtc-source.sh: $*" >&2; exit 1; }
val() { awk -v k="$1" '$1 == k { print $2; exit }' "$SUMS"; }
[ "$(val upstream)" = "$UP" ] || die "$SUMS pins libwebrtc for WebKit $(val upstream), not $UP: pin the new release first"
TAG=$(val tag) TAGOBJ=$(val tag-object) COMMIT=$(val commit) TREE=$(val tree) SHA=$(val sha256)
NAME=wpewebkit_$UP.orig-libwebrtc.tar.xz
REPO=${WEBKIT_GIT:-https://github.com/WebKit/WebKit.git}
mkdir -p "$DEST"
if [ -s "$DEST/$NAME" ] && [ "$(sha256sum < "$DEST/$NAME" | cut -d' ' -f1)" = "$SHA" ]; then
    echo "$NAME: already made, SHA-256 is the pinned one"; exit 0
fi
G=$(mktemp -d)
trap 'rm -rf "$G"' EXIT
git init -q "$G/webkit"
git -C "$G/webkit" remote add origin "$REPO"
# the tag on the server must still be the one pinned (a moved tag stops here)
got=$(git -C "$G/webkit" ls-remote origin "refs/tags/$TAG" "refs/tags/$TAG^{}")
echo "$got" | grep -q "^$TAGOBJ	refs/tags/$TAG\$" || die "tag $TAG is not the pinned tag object $TAGOBJ: $got"
echo "$got" | grep -q "^$COMMIT	refs/tags/$TAG^{}\$" || die "tag $TAG does not point at the pinned commit $COMMIT: $got"
git -C "$G/webkit" sparse-checkout set --no-cone /Source/ThirdParty/libwebrtc/ /Source/cmake/
git -C "$G/webkit" fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
git -C "$G/webkit" -c advice.detachedHead=false checkout -q FETCH_HEAD
[ "$(git -C "$G/webkit" rev-parse HEAD)" = "$COMMIT" ] || die "fetched commit is not $COMMIT"
[ "$(git -C "$G/webkit" rev-parse HEAD:Source/ThirdParty/libwebrtc)" = "$TREE" ] || die "Source/ThirdParty/libwebrtc's tree is not the pinned $TREE"
echo "WebKit $TAG: tag object $TAGOBJ, commit $COMMIT, libwebrtc tree $TREE"
if [ -n "${ORIG:-}" ]; then
    mkdir "$G/rel"
    tar -xJf "$ORIG" -C "$G/rel" "wpewebkit-$UP/Source/cmake"
    # every file of the tarball's Source/cmake (a release tarball is one port's: other ports' Options*.cmake are left out)
    n=0
    for f in $(cd "$G/rel/wpewebkit-$UP" && find Source/cmake -type f); do
        cmp -s "$G/rel/wpewebkit-$UP/$f" "$G/webkit/$f" || die "$f at $COMMIT differs from the release tarball's: not the released commit"
        n=$((n + 1))
    done
    [ "$n" -gt 50 ] || die "only $n files in the release tarball's Source/cmake"
    echo "Source/cmake at that commit is the signed release tarball's, byte for byte ($n files)"
fi
# prebuilt files in the subtree (test fixtures), never in the source package
EXCL=$(git -C "$G/webkit" ls-tree -r --name-only HEAD:Source/ThirdParty/libwebrtc | grep -E '\.(o|a|lib|jar|so|dll|dylib|exe)$' || true)
set --
for f in $EXCL; do set -- "$@" ":(exclude)$f"; done
echo "left out (prebuilt): $(echo "$EXCL" | grep -c . || true) files"
# a tree (not a commit) would get the current time as every file's date: the commit's date, so the tarball is the same
# every time
git -C "$G/webkit" archive --format=tar --mtime="@$(git -C "$G/webkit" log -1 --format=%ct)" --prefix=libwebrtc/ "HEAD:Source/ThirdParty/libwebrtc" -- . "$@" > "$G/l.tar"
! tar -tf "$G/l.tar" | grep -E '\.(o|a|lib|jar|so|dll|dylib|exe)$' || die "a prebuilt file is still in the tarball"
xz -9 -T1 -c "$G/l.tar" > "$G/$NAME"
got=$(sha256sum < "$G/$NAME" | cut -d' ' -f1)
if [ "$SHA" = "-" ]; then
    echo "$NAME: $got (nothing pinned yet: write it in $SUMS)"
elif [ "$got" != "$SHA" ]; then
    die "$NAME: SHA-256 $got, pinned $SHA"
fi
mv "$G/$NAME" "$DEST/$NAME"
echo "$NAME: $(du -h "$DEST/$NAME" | cut -f1), SHA-256 $got"
