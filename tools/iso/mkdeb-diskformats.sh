#!/bin/sh
# mkdeb-diskformats.sh — builds lordos-disk-formats_<version>_<arch>.deb, the Store's "Disk Formats" item: what lets
# Files and Disks mount and format NTFS, exFAT, FAT, HFS+ and APFS volumes. All of it is free software; the name is
# Commercial because the formats are. The marker file is what disk-mount checks (disk/disk_admin.c).
#   tools/iso/mkdeb-diskformats.sh <output folder>     (needs dpkg-deb)
set -eu
OUTDIR=${1:?usage: mkdeb-diskformats.sh <output folder>}
V=$(sed -n 's/.*"version": *"\([0-9.]*\).*/\1/p' package.json | head -1)
ARCH=$(dpkg --print-architecture)
DEPS="ntfs-3g, exfatprogs, dosfstools"
# APFS's open driver is a module built for the running kernel, with compilers and headers: hundreds of MB, so it is
# the Store page's own choice (`optional` in apt/catalog.json), never a dependency. Debian's kernel only: amd64.
SUGGESTS=
[ "$ARCH" = amd64 ] && SUGGESTS="Suggests: apfs-dkms, linux-headers-amd64"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/DEBIAN" "$T/usr/lib/lordos" "$T/usr/share/doc/lordos-disk-formats"
echo "1" > "$T/usr/lib/lordos/disk-formats.enabled"
cat > "$T/usr/share/doc/lordos-disk-formats/copyright" <<C
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Files: *
License: MIT
 See the LordOS repository. This package holds a marker file and depends on the drivers.
C
cat > "$T/DEBIAN/control" <<C
Package: lordos-disk-formats
Version: $V
Architecture: $ARCH
Maintainer: LordOS <lord.basex@gmail.com>
Section: admin
Priority: optional
Depends: lordos, $DEPS
Description: disk formats for LordOS Files and Disks
 Mounts and formats NTFS, exFAT, FAT and HFS+ volumes on the desktop; APFS with
 the optional apfs-dkms driver.
C
[ -z "$SUGGESTS" ] || sed -i "/^Depends:/a $SUGGESTS" "$T/DEBIAN/control"
dpkg-deb --root-owner-group --build "$T" "$OUTDIR/lordos-disk-formats_${V}_$ARCH.deb" >/dev/null
echo "$OUTDIR/lordos-disk-formats_${V}_$ARCH.deb"
