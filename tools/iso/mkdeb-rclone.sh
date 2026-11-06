#!/bin/sh
# mkdeb-rclone.sh — builds lordos-rclone_<version>_<arch>.deb: rclone's official release binary, unmodified, at
# /usr/lib/lordos/rclone/rclone, so it never fights Debian's /usr/bin/rclone (docs/CLOUD.md 3.1 and 17.2 item 7).
# Cloud (server/src/cloud) and Files' Connect to Server (server/src/lan/rconnect.c) run this binary.
#
#   tools/iso/mkdeb-rclone.sh <output folder> [amd64] [arm64]     (from the repository root; default: both)
#
# Needs curl, gpg, unzip, sha256sum and dpkg-deb; no Go. RCLONE_DIST=<folder> takes the release files from there
# (SHA256SUMS and rclone-v<version>-linux-<arch>.zip) instead of downloads.rclone.org. Prints only the paths of the
# packages on standard output.
#
# Trust: SHA256SUMS is clearsigned by rclone's release key, Nick Craig-Wood's, whose fingerprint is pinned below; the
# key itself is packaging/keys/rclone-release-<fingerprint>.asc (fetched once from keys.openpgp.org and checked
# against the fingerprint; rclone.org/KEYS lists the same key). The signature must be good and made by exactly that
# key, then the zip's SHA-256 must be the one in the signed file, then the binary in it must say it is v<version>.
# Nothing in the binary is changed.
#
# copyright: rclone's MIT license and the license of every Go module linked in (packaging/rclone/copyright-v<version>,
# generated once from the module cache of a build of that tag). File times, the changelog date and the archive come
# from the binary's own time in the zip, so the same release gives the same package bytes.
set -eu
RCLONE_VERSION=1.75.2
RCLONE_FPR=FBF737ECE9F8AB18604BD2AC93935E02FF3B54FA
PKG_REV=${PKG_REV:-1}
OUTDIR=${1:?usage: mkdeb-rclone.sh <output folder> [amd64] [arm64]}
shift
ARCHES=${*:-amd64 arm64}
[ -f package.json ] && [ -d tools/iso ] || { echo "mkdeb-rclone.sh: run from the repository root" >&2; exit 1; }
[ -d "$OUTDIR" ] || { echo "no such folder: $OUTDIR" >&2; exit 1; }
OUTDIR=$(cd "$OUTDIR" && pwd)
KEY=packaging/keys/rclone-release-$RCLONE_FPR.asc
LIC=packaging/rclone/copyright-v$RCLONE_VERSION
for t in gpg unzip sha256sum dpkg-deb; do command -v "$t" >/dev/null || { echo "missing $t" >&2; exit 1; }; done
[ -f "$KEY" ] || { echo "missing $KEY" >&2; exit 1; }
[ -f "$LIC" ] || { echo "missing $LIC" >&2; exit 1; }
for a in $ARCHES; do case $a in amd64|arm64) ;; *) echo "unknown architecture: $a (amd64, arm64)" >&2; exit 1 ;; esac; done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fetch() {     # file name in the release
    if [ -n "${RCLONE_DIST:-}" ]; then cp "$RCLONE_DIST/$1" "$W/$1"
    else command -v curl >/dev/null || { echo "missing curl" >&2; exit 1; }
         curl -fsSL -o "$W/$1" "https://downloads.rclone.org/v$RCLONE_VERSION/$1"; fi
}

# the key: exactly the pinned fingerprint, nothing else in the file
G=$W/gnupg
mkdir -m 700 "$G"
fprs=$(gpg --homedir "$G" --batch --show-keys --with-colons "$KEY" 2>/dev/null | awk -F: '$1 == "fpr" { print $10 }')
case "$fprs" in "$RCLONE_FPR"*) ;; *) echo "$KEY is not the key $RCLONE_FPR" >&2; exit 1 ;; esac
[ "$(gpg --homedir "$G" --batch --show-keys --with-colons "$KEY" 2>/dev/null | grep -c '^pub:')" = 1 ] || { echo "$KEY holds more than one key" >&2; exit 1; }
gpg --homedir "$G" --batch --dearmor < "$KEY" > "$W/key.gpg"

fetch SHA256SUMS
st=$(gpg --homedir "$G" --batch --no-default-keyring --keyring "$W/key.gpg" --status-fd 1 --verify "$W/SHA256SUMS" 2>/dev/null || true)
echo "$st" | grep -q "^\[GNUPG:\] VALIDSIG $RCLONE_FPR " || { echo "SHA256SUMS is not signed by $RCLONE_FPR" >&2; exit 1; }
# only what the signature covers: the clearsigned text, without the armor
gpg --homedir "$G" --batch --no-default-keyring --keyring "$W/key.gpg" --output "$W/sums" --decrypt "$W/SHA256SUMS" 2>/dev/null

for ARCH in $ARCHES; do
    Z=rclone-v$RCLONE_VERSION-linux-$ARCH.zip
    fetch "$Z"
    want=$(awk -v f="$Z" '$2 == f || $2 == "*" f { print $1 }' "$W/sums")
    [ -n "$want" ] || { echo "$Z is not in the signed SHA256SUMS" >&2; exit 1; }
    got=$(sha256sum "$W/$Z" | cut -c1-64)
    [ "$got" = "$want" ] || { echo "$Z: sha256 $got, the signed SHA256SUMS says $want" >&2; exit 1; }
    rm -rf "$W/x"
    mkdir "$W/x"
    unzip -q "$W/$Z" "rclone-v$RCLONE_VERSION-linux-$ARCH/rclone" -d "$W/x"
    BIN=$W/x/rclone-v$RCLONE_VERSION-linux-$ARCH/rclone
    EPOCH=$(stat -c %Y "$BIN")
    STAMP=$(date -u -d "@$EPOCH" +%Y%m%d%H%M.%S)
    RFC=$(LC_ALL=C date -u -R -d "@$EPOCH")
    VERSION="$RCLONE_VERSION-$PKG_REV"
    if [ "$ARCH" = "$(dpkg --print-architecture)" ]; then
        v=$("$BIN" version 2>/dev/null | head -1)
        [ "$v" = "rclone v$RCLONE_VERSION" ] || { echo "$Z: the binary says '$v'" >&2; exit 1; }
    fi
    PKG=$W/pkg-$ARCH
    D=$PKG/usr/share/doc/lordos-rclone
    install -d "$PKG/usr/lib/lordos/rclone" "$D" "$PKG/DEBIAN"
    install -m 755 "$BIN" "$PKG/usr/lib/lordos/rclone/rclone"
    {
        cat <<C
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: rclone
Upstream-Contact: https://rclone.org
Source: https://downloads.rclone.org/v$RCLONE_VERSION/$Z (sha256 $got, from SHA256SUMS signed by $RCLONE_FPR)
Comment: rclone's official release binary v$RCLONE_VERSION, unmodified, repackaged by LordOS's tools/iso/mkdeb-rclone.sh.
 The Go modules linked in follow rclone's own license, each with the license file it ships.

C
        cat "$LIC"
    } > "$D/copyright"
    printf 'lordos-rclone (%s) trixie; urgency=medium\n\n  * rclone v%s, the official release binary for linux-%s, repackaged by tools/iso/mkdeb-rclone.sh.\n\n -- LordOS <lord.basex@gmail.com>  %s\n' \
        "$VERSION" "$RCLONE_VERSION" "$ARCH" "$RFC" | gzip -9n > "$D/changelog.gz"
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
 rclone's official release binary v$RCLONE_VERSION, unmodified, at
 /usr/lib/lordos/rclone/rclone, apart from Debian's own rclone package. LordOS
 runs it for the cloud accounts of Cloud (rclone rcd on a Unix socket, for the
 session user only) and for the remote folders of Files' Connect to Server.
C
    chmod 644 "$PKG/DEBIAN/control" "$PKG/DEBIAN/md5sums"
    find "$PKG" -type d -exec chmod 755 {} +
    find "$PKG" -exec env TZ=UTC touch -h -t "$STAMP" {} +
    OUT="$OUTDIR/lordos-rclone_${VERSION}_$ARCH.deb"
    SOURCE_DATE_EPOCH=$EPOCH dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT" >/dev/null
    echo "$OUT"
done
