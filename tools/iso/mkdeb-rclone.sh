#!/bin/sh
# mkdeb-rclone.sh — builds lordos-rclone_<version>_<arch>.deb: upstream rclone, unpatched, built from its release
# tag's source, at /usr/lib/lordos/rclone/rclone, so it never fights Debian's /usr/bin/rclone (docs/CLOUD.md 3.1).
# Cloud (server/src/cloud) and Files' Connect to Server (server/src/lan/rconnect.c) run this binary.
#
#   tools/iso/mkdeb-rclone.sh <output folder> [amd64] [arm64]     (from the repository root; default: both)
#
# Needs Go GO_VERSION (GO=<path to the go binary>, else `go` on PATH; any other version is refused unless
# ALLOW_OTHER_GO=1), dpkg-deb, curl, sha256sum and the network for the Go modules (go.sum checks every one).
# RCLONE_SRC=<folder> uses an unpacked source tree of that tag instead of downloading it; its VERSION must match.
# Prints only the paths of the packages on standard output.
#
# The build is upstream's own (bin/cross-compile.go): CGO_ENABLED=0, -trimpath, -ldflags "-s -X …fs.Version=",
# no build tags, plus -buildid= so the same source and Go give the same bytes. File times, the changelog date and
# the archive come from the tag's date (the source tree's VERSION file), so apt-repo.sh accepts the package again
# unchanged and refuses a different one under the same version (bump PKG_REV for a rebuild that differs).
#
# copyright: rclone's MIT license, then each Go module linked in (go version -m on the binary) with the license
# file it carries in the module cache.
set -eu
RCLONE_VERSION=1.75.2
RCLONE_SHA256=68afd7f68c84bd978966feae2116339aa7bf454b39c573910c462e87c3774d5a   # github.com/rclone/rclone/archive/refs/tags/v1.75.2.tar.gz
GO_VERSION=go1.26.10
PKG_REV=${PKG_REV:-1}
OUTDIR=${1:?usage: mkdeb-rclone.sh <output folder> [amd64] [arm64]}
shift
ARCHES=${*:-amd64 arm64}
[ -f package.json ] && [ -d tools/iso ] || { echo "mkdeb-rclone.sh: run from the repository root" >&2; exit 1; }
[ -d "$OUTDIR" ] || { echo "no such folder: $OUTDIR" >&2; exit 1; }
OUTDIR=$(cd "$OUTDIR" && pwd)
GO=${GO:-go}
command -v "$GO" >/dev/null || { echo "missing Go ($GO_VERSION): set GO=<path to go>" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "missing dpkg-deb" >&2; exit 1; }
gv=$("$GO" env GOVERSION)
[ "$gv" = "$GO_VERSION" ] || [ "${ALLOW_OTHER_GO:-0}" = 1 ] || { echo "Go is $gv, this package is built with $GO_VERSION (ALLOW_OTHER_GO=1 to build anyway)" >&2; exit 1; }
for a in $ARCHES; do case $a in amd64|arm64) ;; *) echo "unknown architecture: $a (amd64, arm64)" >&2; exit 1 ;; esac; done

W=$(mktemp -d)
trap 'chmod -R u+w "$W" 2>/dev/null; rm -rf "$W"' EXIT
if [ -n "${RCLONE_SRC:-}" ]; then
    SRC=$(cd "$RCLONE_SRC" && pwd)
else
    command -v curl >/dev/null || { echo "missing curl" >&2; exit 1; }
    curl -fsSL -o "$W/src.tgz" "https://github.com/rclone/rclone/archive/refs/tags/v$RCLONE_VERSION.tar.gz" >&2
    got=$(sha256sum "$W/src.tgz" | cut -c1-64)
    [ "$got" = "$RCLONE_SHA256" ] || { echo "the source tarball's sha256 is $got, not $RCLONE_SHA256" >&2; exit 1; }
    tar -xzf "$W/src.tgz" -C "$W"
    SRC=$W/rclone-$RCLONE_VERSION
fi
[ "$(cat "$SRC/VERSION")" = "v$RCLONE_VERSION" ] || { echo "$SRC is not rclone v$RCLONE_VERSION" >&2; exit 1; }
EPOCH=$(stat -c %Y "$SRC/VERSION")
STAMP=$(date -u -d "@$EPOCH" +%Y%m%d%H%M.%S)
RFC=$(LC_ALL=C date -u -R -d "@$EPOCH")
VERSION="$RCLONE_VERSION-$PKG_REV"
MODCACHE=$("$GO" env GOMODCACHE)

for ARCH in $ARCHES; do
    BIN=$W/rclone-$ARCH
    (cd "$SRC" && CGO_ENABLED=0 GOOS=linux GOARCH=$ARCH GOFLAGS=-mod=readonly \
        "$GO" build -trimpath -ldflags "-s -buildid= -X github.com/rclone/rclone/fs.Version=v$RCLONE_VERSION" -o "$BIN" .) >&2
    PKG=$W/pkg-$ARCH
    D=$PKG/usr/share/doc/lordos-rclone
    install -d "$PKG/usr/lib/lordos/rclone" "$D" "$PKG/DEBIAN"
    install -m 755 "$BIN" "$PKG/usr/lib/lordos/rclone/rclone"
    {
        cat <<C
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: rclone
Upstream-Contact: https://rclone.org
Source: https://github.com/rclone/rclone/archive/refs/tags/v$RCLONE_VERSION.tar.gz (sha256 $RCLONE_SHA256)
Comment: Upstream rclone v$RCLONE_VERSION, unpatched, built by LordOS's tools/iso/mkdeb-rclone.sh with $GO_VERSION
 (CGO_ENABLED=0, -trimpath). The Go modules linked in follow, each with the license file it ships.

Files: *
Copyright: Nick Craig-Wood and the rclone contributors
License: Expat
C
        sed 's/^$/./; s/^/ /' "$SRC/COPYING"
        "$GO" version -m "$BIN" | awk '$1 == "dep" || $1 == "=>" { print $2, $3 }' | while read -r mod ver; do
            # the module cache's own spelling: capitals as !lowercase
            dir=$MODCACHE/$(printf %s "$mod" | sed 's/[A-Z]/!&/g' | tr 'A-Z' 'a-z')@$ver
            lic=
            for f in LICENSE LICENSE.txt LICENSE.md LICENCE COPYING LICENSE-MIT LICENSE.MIT License license; do
                [ -f "$dir/$f" ] && { lic=$dir/$f; break; }
            done
            printf '\nFiles: (Go module) %s %s\n' "$mod" "$ver"
            if [ -n "$lic" ]; then
                printf 'License: see below\n'
                sed 's/^[[:space:]]*$/./; s/^/ /' "$lic"
            else
                printf 'License: see https://pkg.go.dev/%s?tab=licenses\n' "$mod"
            fi
        done
    } > "$D/copyright"
    printf 'lordos-rclone (%s) trixie; urgency=medium\n\n  * Upstream rclone v%s, built from the tag by tools/iso/mkdeb-rclone.sh.\n\n -- LordOS <lord.basex@gmail.com>  %s\n' \
        "$VERSION" "$RCLONE_VERSION" "$RFC" | gzip -9n > "$D/changelog.gz"
    chmod 644 "$D/copyright" "$D/changelog.gz"
    (cd "$PKG" && find usr -type f | LC_ALL=C sort | xargs md5sum) > "$PKG/DEBIAN/md5sums"
    SIZE=$(du -sk "$PKG/usr" | awk '{print $1}')
    cat > "$PKG/DEBIAN/control" <<C
Package: lordos-rclone
Version: $VERSION
Architecture: $ARCH
Maintainer: LordOS <lord.basex@gmail.com>
Installed-Size: $SIZE
Depends: fuse3
Section: net
Priority: optional
Homepage: https://rclone.org
Description: rclone for LordOS Cloud and Connect to Server
 Upstream rclone v$RCLONE_VERSION, unpatched, at /usr/lib/lordos/rclone/rclone,
 apart from Debian's own rclone package. LordOS runs it for the cloud accounts
 of Cloud (rclone rcd on a Unix socket, for the session user only) and for the
 remote folders of Files' Connect to Server.
C
    chmod 644 "$PKG/DEBIAN/control" "$PKG/DEBIAN/md5sums"
    find "$PKG" -type d -exec chmod 755 {} +
    find "$PKG" -exec env TZ=UTC touch -h -t "$STAMP" {} +
    OUT="$OUTDIR/lordos-rclone_${VERSION}_$ARCH.deb"
    SOURCE_DATE_EPOCH=$EPOCH dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT" >/dev/null
    echo "$OUT"
done
