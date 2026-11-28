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
[ -x server/build/lord-web ] || { echo "missing server/build/lord-web (make -C server with libwpewebkit-2.0-dev and libegl-dev, plus libwpebackend-fdo-1.0-dev for the legacy backend; docs/VM-SETUP.md)" >&2; exit 1; }
V=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' package.json | head -1)
ARCH=$(dpkg --print-architecture)
STRIP=${STRIP:-strip}
# the libraries lord-web links itself (WebKit brings GStreamer, libsoup and the rest), then WebKit's media and sandbox;
# which WebKit depends on the backend make built (mk/web.mk: build/gen/web_backend): WPEPlatform needs WPE WebKit 2.54
# built with it (LordOS's backport for Debian 13), the legacy backend Debian 13's 2.48 with libwpe and wpebackend-fdo
BACKEND=$(cat server/build/gen/web_backend 2>/dev/null || echo legacy)
if [ "$BACKEND" = platform ]; then
    # gstreamer1.0-nice: WebRTC's ICE (calls, docs/WEBAPPS.md "Calls"), with LordOS's build that has WebRTC (+3)
    DEPS="libwpewebkit-2.0-1 (>= 2.54), libegl1, libwayland-client0, libxkbcommon0, libglib2.0-0t64, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-nice, bubblewrap, xdg-dbus-proxy"
    ENGINE="LordOS's build of Debian's wpewebkit 2.54 for Debian 13 (WPEPlatform on)"
else
    DEPS="libwpewebkit-2.0-1 (>= 2.48), libwpe-1.0-1 (>= 1.16), libwpebackend-fdo-1.0-1 (>= 1.16), libegl1, libwayland-client0, libwayland-server0, libxkbcommon0, libglib2.0-0t64, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, bubblewrap, xdg-dbus-proxy"
    ENGINE="Debian's own packages, unmodified"
fi
# spelling dictionaries for the page's text fields (docs/WEBAPPS.md, "Spelling"): WebKit checks with enchant and
# hunspell's dictionaries, LordOS's three main languages; Recommends, so a system without them still installs
RECS="hunspell-en-us, hunspell-es, hunspell-pt-br"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/DEBIAN" "$T/usr/lib/lordos/bin" "$T/usr/share/doc/lordos-web"
install -m 755 server/build/lord-web "$T/usr/lib/lordos/bin/lord-web"
"$STRIP" --strip-unneeded "$T/usr/lib/lordos/bin/lord-web" 2>/dev/null || true
# lord-web's web process extension (Chrome's Client Hints, docs/WEBAPPS.md "Browser identity"): WebKit loads it only from
# this folder next to lord-web, which lord-web names itself
[ -f server/build/lord-web-ext/lord-web-ext.so ] || { echo "missing server/build/lord-web-ext/lord-web-ext.so (make -C server)" >&2; exit 1; }
mkdir -p "$T/usr/lib/lordos/bin/lord-web-ext"
install -m 644 server/build/lord-web-ext/lord-web-ext.so "$T/usr/lib/lordos/bin/lord-web-ext/lord-web-ext.so"
"$STRIP" --strip-unneeded "$T/usr/lib/lordos/bin/lord-web-ext/lord-web-ext.so" 2>/dev/null || true
cat > "$T/usr/share/doc/lordos-web/copyright" <<C
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Files: *
License: MIT
 See the LordOS repository. lord-web links WPE WebKit (LGPL-2.1+ and BSD), installed
 by $ENGINE.
C
cat > "$T/DEBIAN/control" <<C
Package: lordos-web
Version: $V
Architecture: $ARCH
Maintainer: LordOS <lord.basex@gmail.com>
Section: web
Priority: optional
Depends: lordos (= $V), $DEPS
Recommends: $RECS
Description: LordOS Web Runtime: web apps as LordOS apps
 lord-web runs each installed web app and profile in its own process, in a
 window of the LordOS desktop, with WebKit's sandbox always on.
C
dpkg-deb --root-owner-group --build "$T" "$OUTDIR/lordos-web_${V}_$ARCH.deb" >/dev/null
echo "$OUTDIR/lordos-web_${V}_$ARCH.deb"
