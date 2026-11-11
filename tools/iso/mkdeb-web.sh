#!/bin/sh
# mkdeb-web.sh — builds lordos-web_<version>_<arch>.deb, the LordOS Web Runtime (docs/WEBAPPS.md): lord-web, the only
# binary that links WPE WebKit, at /usr/lib/lordos/bin/lord-web next to lord-server (the one path lord-server starts it
# from). Optional, apart from the lordos package: without it the rest of LordOS works unchanged and the web app
# features say how to get it. Its dependencies are WebKit itself, GStreamer's plugins for media, and what WebKit's
# sandbox needs (bubblewrap, xdg-dbus-proxy): the sandbox is never turned off, so they are Depends, not Recommends.
#   tools/iso/mkdeb-web.sh <output folder>     (from the repository root, after make -C server; needs dpkg-deb)
set -eu
OUTDIR=${1:?usage: mkdeb-web.sh <output folder>}
[ -f package.json ] && [ -d tools/iso ] || { echo "mkdeb-web.sh: run from the repository root" >&2; exit 1; }
[ -x server/build/lord-web ] || { echo "missing server/build/lord-web (make -C server with libwpewebkit-2.0-dev, libwpebackend-fdo-1.0-dev and libegl-dev; docs/VM-SETUP.md)" >&2; exit 1; }
V=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' package.json | head -1)
ARCH=$(dpkg --print-architecture)
STRIP=${STRIP:-strip}
# the libraries lord-web links itself (WebKit brings GStreamer, libsoup and the rest), then WebKit's media and sandbox
DEPS="libwpewebkit-2.0-1 (>= 2.48), libwpe-1.0-1 (>= 1.16), libwpebackend-fdo-1.0-1 (>= 1.16), libegl1, libwayland-client0, libwayland-server0, libxkbcommon0, libglib2.0-0t64, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, bubblewrap, xdg-dbus-proxy"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/DEBIAN" "$T/usr/lib/lordos/bin" "$T/usr/share/doc/lordos-web"
install -m 755 server/build/lord-web "$T/usr/lib/lordos/bin/lord-web"
"$STRIP" --strip-unneeded "$T/usr/lib/lordos/bin/lord-web" 2>/dev/null || true
cat > "$T/usr/share/doc/lordos-web/copyright" <<C
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Files: *
License: MIT
 See the LordOS repository. lord-web links WPE WebKit (LGPL-2.1+ and BSD), installed
 by Debian's own packages, unmodified.
C
cat > "$T/DEBIAN/control" <<C
Package: lordos-web
Version: $V
Architecture: $ARCH
Maintainer: LordOS <lord.basex@gmail.com>
Section: web
Priority: optional
Depends: lordos (= $V), $DEPS
Description: LordOS Web Runtime: web apps as LordOS apps
 lord-web runs each installed web app and profile in its own process, in a
 window of the LordOS desktop, with WebKit's sandbox always on.
C
dpkg-deb --root-owner-group --build "$T" "$OUTDIR/lordos-web_${V}_$ARCH.deb" >/dev/null
echo "$OUTDIR/lordos-web_${V}_$ARCH.deb"
