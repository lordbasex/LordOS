#!/bin/sh
# mkdeb-hwdata.sh — builds lordos-hwdata_<version>_all.deb: the signed tested-hardware snapshot for System
# Information's Compatibility panel (docs/BENCHMARK.md, "Newer snapshots between releases").
#
#   tools/iso/mkdeb-hwdata.sh <output folder>      (from the repository root; needs dpkg-deb)
#
# Prints only the path of the package on standard output. Contents, nothing else:
#   /usr/lib/lordos/assets/hwreport/snapshot.json       assets/hwreport/snapshot.json (tools/hwreport/catalog.mjs)
#   /usr/lib/lordos/assets/hwreport/snapshot.json.sig   its Ed25519 signature with the release key
#                                                       (tools/release/sign-hwsnapshot.sh, the maintainer's machine)
#   /usr/share/doc/lordos-hwdata/                       copyright, changelog
#
# lord-server reads the two files on every lord.sysinfo.community() call and verifies the signature in C before
# parsing (server/src/sysreport/hwsnap.c), so the package is only a way to deliver them: the lordos package
# Depends on it, and a newer one published alone in the signed APT repository reaches every system through the
# daily update check and Store → Updates, between LordOS releases (docs/RELEASING.md, "A newer hardware snapshot").
#
# Version: the snapshot's "generated" date (its newest report) as YYYY.MM.DD, plus .N from HWDATA_SERIAL=N
# (1..99) for a second, different snapshot with the same date. The package is reproducible: the same snapshot
# and signature give the same bytes (file times, the changelog date and the archive times come from that date),
# so apt-repo.sh accepts it again unchanged and refuses a different one under the same version.
#
# REQUIRE_SIGNED_CODE=1 (a release) refuses a missing signature and checks it with openssl against
# tools/release/lordos-release.pub.pem (CODE_PUB=<public.pem> for a throwaway test key).
#
# The format rule: while any supported lordos reads format 1, this package carries format 1. The build refuses
# any other format until someone adds the matching Breaks: lordos (<< first version that reads it) here.
set -eu
OUTDIR=${1:?usage: mkdeb-hwdata.sh <output folder>}
SNAP=assets/hwreport/snapshot.json
SIG=assets/hwreport/snapshot.json.sig
[ -f package.json ] && [ -d assets ] || { echo "mkdeb-hwdata.sh: run from the repository root" >&2; exit 1; }
[ -f "$SNAP" ] || { echo "missing $SNAP (node tools/hwreport/catalog.mjs, or tools/release/sign-hwsnapshot.sh)" >&2; exit 1; }
[ -d "$OUTDIR" ] || { echo "no such folder: $OUTDIR" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "missing dpkg-deb" >&2; exit 1; }

# the snapshot's own header, as catalog.mjs writes it (compact JSON, these keys first)
head=$(head -c 200 "$SNAP")
case "$head" in
    '{"format":"lordos-hwsnapshot","version":1,"generated":"'*) ;;
    *) echo "$SNAP is not a lordos-hwsnapshot of format 1: add Breaks for older lordos versions first (see the header)" >&2; exit 1 ;;
esac
GEN=$(printf %s "$head" | sed -n 's/^{"format":"lordos-hwsnapshot","version":1,"generated":"\([0-9]\{4\}-[0-9][0-9]-[0-9][0-9]\)".*/\1/p')
[ -n "$GEN" ] || { echo "$SNAP: no generated date (YYYY-MM-DD)" >&2; exit 1; }
VERSION=$(printf %s "$GEN" | tr - .)
case "${HWDATA_SERIAL:-}" in
    "") ;;
    [1-9]|[1-9][0-9]) VERSION="$VERSION.$HWDATA_SERIAL" ;;
    *) echo "HWDATA_SERIAL must be a number from 1 to 99" >&2; exit 1 ;;
esac

if [ "${REQUIRE_SIGNED_CODE:-0}" = 1 ]; then
    [ -f "$SIG" ] || { echo "missing $SIG: sign it on the maintainer's machine (tools/release/sign-hwsnapshot.sh)" >&2; exit 1; }
    command -v openssl >/dev/null || { echo "openssl is needed to check $SIG" >&2; exit 1; }
    pub=${CODE_PUB:-tools/release/lordos-release.pub.pem}
    t=$(mktemp)
    base64 -d < "$SIG" > "$t" 2>/dev/null || { rm -f "$t"; echo "$SIG is not base64" >&2; exit 1; }
    openssl pkeyutl -verify -pubin -inkey "$pub" -rawin -in "$SNAP" -sigfile "$t" >/dev/null 2>&1 || {
        rm -f "$t"; echo "$SIG does not verify $SNAP with $pub: sign the snapshot again after rebuilding it" >&2; exit 1; }
    rm -f "$t"
elif [ ! -f "$SIG" ]; then
    echo "warning: $SIG missing: System Information will say the snapshot is not verified" >&2
fi

# GNU date (Debian) or BSD date
EPOCH=$(date -u -d "$GEN" +%s 2>/dev/null || date -u -j -f %Y-%m-%d "$GEN" +%s)
STAMP=$(printf %s "$GEN" | tr -d -)0000
RFC=$(LC_ALL=C date -u -R -d "@$EPOCH" 2>/dev/null || LC_ALL=C date -u -j -r "$EPOCH" '+%a, %d %b %Y %H:%M:%S +0000')

PKG=$(mktemp -d)
trap 'rm -rf "$PKG"' EXIT
H="$PKG/usr/lib/lordos/assets/hwreport"
D="$PKG/usr/share/doc/lordos-hwdata"
install -d "$H" "$D" "$PKG/DEBIAN"
install -m 644 "$SNAP" "$H/snapshot.json"
[ -f "$SIG" ] && install -m 644 "$SIG" "$H/snapshot.json.sig"
cat > "$D/copyright" <<'C'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: LordOS
Source: https://github.com/lordbasex/LordOS

Files: *
Copyright: 2026 iPERFEX and the LordOS contributors
License: Expat
 The snapshot is built by tools/hwreport/catalog.mjs from the hardware reports
 accepted in the LordOS repository (web/content/hardware/reports); it holds
 device keys, LordOS versions and counts, no personal data.
 .
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
C
printf 'lordos-hwdata (%s) trixie; urgency=medium\n\n  * The tested-hardware snapshot of %s, built by tools/iso/mkdeb-hwdata.sh.\n\n -- LordOS <lord.basex@gmail.com>  %s\n' \
    "$VERSION" "$GEN" "$RFC" | gzip -9n > "$D/changelog.gz"
chmod 644 "$D/copyright" "$D/changelog.gz"
(cd "$PKG" && find usr -type f | LC_ALL=C sort | xargs md5sum) > "$PKG/DEBIAN/md5sums"
SIZE=$(du -sk "$PKG/usr" | awk '{print $1}')
cat > "$PKG/DEBIAN/control" <<C
Package: lordos-hwdata
Version: $VERSION
Architecture: all
Multi-Arch: foreign
Maintainer: LordOS <lord.basex@gmail.com>
Installed-Size: $SIZE
Section: misc
Priority: optional
Homepage: https://github.com/lordbasex/LordOS
Description: tested-hardware snapshot for LordOS System Information
 The signed snapshot of the hardware reports accepted for LordOS: for each
 device and LordOS version, how many reports say it works, works with limits
 or doesn't work. System Information's Compatibility panel shows it only after
 checking its Ed25519 signature with the LordOS release key. Updated between
 LordOS releases through the LordOS repository.
C
chmod 644 "$PKG/DEBIAN/control" "$PKG/DEBIAN/md5sums"
find "$PKG" -type d -exec chmod 755 {} +
# every time from the snapshot's date, the archive's own times too (dpkg-deb reads SOURCE_DATE_EPOCH)
find "$PKG" -exec env TZ=UTC touch -h -t "$STAMP" {} +
OUT="$OUTDIR/lordos-hwdata_${VERSION}_all.deb"
SOURCE_DATE_EPOCH=$EPOCH dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT" >/dev/null
echo "$OUT"
