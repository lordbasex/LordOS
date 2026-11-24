#!/bin/sh
# wpewebkit-chroot.sh — the part of tools/pkg/build-wpewebkit.sh that runs INSIDE a clean Debian 13 chroot made by
# mmdebstrap (docs/WPEWEBKIT.md). Never run it on a real system: it installs packages and adds a user.
#
#   build <debian-version> <lordos-version>   verify Debian's source, make the LordOS backport, build it
#                                            (source + every binary), lintian; reads /build/in, writes /build/out
#   test                                     install /build/debs/*.deb, check pkg-config, the ABI against
#                                            trixie's 2.48 and render a page with WPEPlatform's headless backend
#
# The changes to Debian's packaging are all here, in backport(), so a new WebKit release needs no edit unless one
# of them stops applying (then the build stops and says which).
set -eu
export LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

die() { echo "wpewebkit-chroot: $*" >&2; exit 1; }

# The keys that sign WebKitGTK and WPE WebKit releases (https://wpewebkit.org/release/verify/). Debian ships them in
# debian/upstream/signing-key.asc; a signature by any other key in that file is refused.
UPSTREAM_FPRS="5AA3BC334FD7E3369E7C77B291C559DBE4C9123B 013A0127AC9C65B34FFA62526C1009B693975393"
DEBEMAIL_LORDOS="LordOS <lord.basex@gmail.com>"

# the .dsc is signed by a Debian Developer (debian-keyring) and every file matches its checksums; the upstream
# tarball's own signature verifies with one of the pinned release keys, and wpewebkit.org's copy of that signature is
# the same file (so Debian's .orig is the very tarball WebKit released)
verify_source() {
    dsc=$1 up=$2
    dscverify --keyring /usr/share/keyrings/debian-keyring.gpg "$dsc" || die "$dsc: the Debian signature or a checksum does not verify"
    t=$(mktemp -d)
    dpkg-source --no-check -x "$dsc" "$t/src" >/dev/null
    gpg --batch --quiet --homedir "$t" --dearmor -o "$t/upstream.gpg" "$t/src/debian/upstream/signing-key.asc"
    orig=/build/in/wpewebkit_$up.orig.tar.xz
    [ -f "$orig.asc" ] || die "no $orig.asc next to the .orig tarball"
    gpgv --status-fd 3 --keyring "$t/upstream.gpg" "$orig.asc" "$orig" 3> "$t/status" >/dev/null 2>&1 || die "$orig: the upstream signature does not verify"
    fpr=$(awk '$2 == "VALIDSIG" { print $3 }' "$t/status")
    case " $UPSTREAM_FPRS " in *" $fpr "*) ;; *) die "$orig is signed by $fpr, not by a pinned WebKit release key" ;; esac
    echo "upstream signature: good, key $fpr"
    if [ -f "/build/in/wpewebkit-$up.tar.xz.asc" ]; then
        cmp -s "/build/in/wpewebkit-$up.tar.xz.asc" "$orig.asc" || die "wpewebkit.org's signature differs from Debian's .orig.tar.xz.asc"
        echo "wpewebkit.org's signature is the same file as Debian's"
    fi
    rm -rf "$t"
}

# the LordOS changes to Debian's packaging
backport() {
    newver=$1
    # 1. WPEPlatform (wpe-platform-2.0 with its headless and Wayland backends) and the legacy libwpe API both on,
    #    explicitly: they are upstream's defaults today, and a default that changes must stop the build, not ship.
    #    WPEPlatform's DRM backend off (maintainer's decision, 2026-11-24): it is the only part that links libinput and
    #    libudev, which LordOS's policy keeps out (ARCHITECTURE.md), and lord-web never drives DRM itself
    grep -q '^	   -DUSE_LIBBACKTRACE=OFF$' debian/rules || die "debian/rules changed: the cmake flags anchor is gone"
    sed -i 's/^	   -DUSE_LIBBACKTRACE=OFF$/	   -DUSE_LIBBACKTRACE=OFF \\\
	   -DENABLE_WPE_PLATFORM=ON \\\
	   -DENABLE_WPE_PLATFORM_DRM=OFF \\\
	   -DENABLE_WPE_PLATFORM_HEADLESS=ON \\\
	   -DENABLE_WPE_PLATFORM_WAYLAND=ON \\\
	   -DENABLE_WPE_LEGACY_API=ON/' debian/rules
    #    The DRM backend's own API (WPEDisplayDRM, WPEScreenDRM, WPEToplevelDRM, WPEViewDRM: wpe_display_drm_* and the
    #    _get_type of the others) isn't in the library then, so it leaves Debian's symbols file too; WPEPlatform's core
    #    DRM device API (wpe_drm_device_*, used for dmabuf) stays
    n=$(grep -cE '^ wpe_(display|screen|toplevel|view)_drm_' debian/libwpewebkit-2.0-1.symbols) || true
    [ "${n:-0}" -gt 0 ] || die "debian/libwpewebkit-2.0-1.symbols has no DRM backend symbols: check what changed"
    sed -i -E '/^ wpe_(display|screen|toplevel|view)_drm_/d' debian/libwpewebkit-2.0-1.symbols
    echo "symbols file: $n symbols of the DRM backend removed"
    # 2. LordOS maintains this build: bugs come to LordOS, not to Debian's WebKit team, who stay credited in the
    #    changelog and debian/copyright (no XSBC-Original-Maintainer: Debian's lintian doesn't know the field)
    grep -q '^Maintainer: ' debian/control || die "debian/control has no Maintainer"
    sed -i "s/^Maintainer: .*$/Maintainer: $DEBEMAIL_LORDOS/; /^Uploaders:/d" debian/control
    # 3. the changelog entry, dated one second after Debian's own entry it follows (a later date, as Debian policy
    #    asks, but a fixed one): the date is SOURCE_DATE_EPOCH for the build, so the same version built again gives
    #    the same .debian.tar.xz (and the same binaries, as far as WebKit's build allows)
    debdate=$(TZ=UTC date -R -d "@$(( $(dpkg-parsechangelog -S Timestamp) + 1 ))")
    DEBEMAIL=$DEBEMAIL_LORDOS dch --force-bad-version --newversion "$newver" --distribution trixie --force-distribution \
        "Backport to Debian 13 (trixie) for LordOS (docs/WPEWEBKIT.md in the LordOS repository)."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Build WPEPlatform (wpe-platform-2.0: headless, Wayland) and keep the legacy libwpe API, both explicitly."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Don't build WPEPlatform's DRM backend, so libinput and libudev aren't linked; its symbols leave debian/libwpewebkit-2.0-1.symbols."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Maintainer: LordOS (the packaging is the Debian WebKit Maintainers')."
    sed -i "0,/^ -- /s/^\( -- .*>  \).*$/\1$debdate/" debian/changelog
    [ "$(dpkg-parsechangelog -S Date)" = "$debdate" ] || die "could not date the changelog entry"
}

cmd_build() {
    dver=$1 newver=$2
    up=${dver%-*}
    dsc=/build/in/wpewebkit_$dver.dsc
    verify_source "$dsc" "$up"

    id -u builder >/dev/null 2>&1 || useradd -m builder
    mkdir -p /build/work /build/out
    cp /build/in/wpewebkit_$up.orig.tar.xz /build/in/wpewebkit_$up.orig.tar.xz.asc /build/work/
    dpkg-source --no-check -x "$dsc" /build/work/wpewebkit-$up
    cd /build/work/wpewebkit-$up
    backport "$newver"
    # the build dependencies, from trixie only (a missing one stops here and is named)
    mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control
    rm -f wpewebkit-build-deps_*
    chown -R builder: /build/work
    start=$(date +%s)
    runuser -u builder -- env DEB_BUILD_OPTIONS="parallel=${JOBS:-$(nproc)}" dpkg-buildpackage -us -uc -sa
    end=$(date +%s)
    echo "build time: $((end - start)) s" | tee /build/out/build-time.txt
    cd /build/work
    cp ./*.deb ./*.dsc ./*.debian.tar.xz ./*.orig.tar.xz ./*.orig.tar.xz.asc ./*.changes ./*.buildinfo /build/out/
    # the source package alone, re-checked: every file the .dsc names is in /build/out and matches
    (cd /build/out && dscverify --no-sig-check "wpewebkit_${newver}.dsc")
    lintian --info --display-info --pedantic --profile debian /build/out/wpewebkit_${newver}_*.changes > /build/out/lintian.txt 2>&1 || true
    echo "lintian: $(grep -c '^E:' /build/out/lintian.txt) errors, $(grep -c '^W:' /build/out/lintian.txt) warnings," \
         "$(grep -c '^I:' /build/out/lintian.txt) info, $(grep -c '^P:' /build/out/lintian.txt) pedantic" | tee /build/out/lintian-summary.txt
    sed -i '/^$/d' /build/out/lintian-summary.txt
}

cmd_test() {
    out=/build/test
    mkdir -p "$out/old"
    # trixie's own 2.48 first, for the ABI comparison (never installed)
    (cd "$out/old" && apt-get download libwpewebkit-2.0-1 >/dev/null)
    dpkg-deb -x "$out"/old/libwpewebkit-2.0-1_*.deb "$out/old/root"
    apt-get install -y --no-install-recommends /build/debs/*.deb
    {
        echo "== installed"
        dpkg-query -W 'libwpewebkit*' wpewebkit-webdriver
        echo "== pkg-config"
        for m in wpe-webkit-2.0 wpe-web-process-extension-2.0 wpe-platform-2.0 wpe-platform-headless-2.0 \
                 wpe-platform-wayland-2.0 wpe-1.0 wpebackend-fdo-1.0; do
            v=$(pkg-config --modversion "$m") || die "pkg-config does not find $m"
            printf '%s %s\n' "$m" "$v"
        done
        echo "== SONAME"
        old=$(ls "$out"/old/root/usr/lib/*/libWPEWebKit-2.0.so.1.*)
        new=$(ls /usr/lib/*/libWPEWebKit-2.0.so.1.*)
        printf 'trixie 2.48: %s %s\nbackport:    %s %s\n' "${old##*/}" "$(objdump -p "$old" | awk '/SONAME/ { print $2 }')" \
            "${new##*/}" "$(objdump -p "$new" | awk '/SONAME/ { print $2 }')"
        nm -D --defined-only "$old" | awk '{ print $NF }' | sort -u > "$out/old.syms"
        nm -D --defined-only "$new" | awk '{ print $NF }' | sort -u > "$out/new.syms"
        echo "exported symbols: 2.48 $(wc -l < "$out/old.syms"), backport $(wc -l < "$out/new.syms")," \
             "removed $(comm -23 "$out/old.syms" "$out/new.syms" | wc -l), added $(comm -13 "$out/old.syms" "$out/new.syms" | wc -l)"
        comm -23 "$out/old.syms" "$out/new.syms" > "$out/removed.syms"
        echo "== NEEDED of the backport's library (readelf -d)"
        readelf -d "$new" | awk '/NEEDED/ { gsub(/[][]/, "", $5); print $5 }' | tr '\n' ' '; echo
        echo "== copyright files"
        for p in libwpewebkit-2.0-1 libwpewebkit-2.0-dev wpewebkit-webdriver libwpewebkit-doc; do
            if dpkg -s "$p" >/dev/null 2>&1; then ls -l "/usr/share/doc/$p/copyright"; fi
        done
        echo "== trixie packages that depend on libwpewebkit-2.0-1"
        apt-cache rdepends --installed=false libwpewebkit-2.0-1 | sed '1,2d' | sort -u
        echo "== WPEPlatform headless render"
    } > "$out/report.txt" 2>&1
    cat "$out/report.txt"
    # what the maintainer's decisions require: no DRM backend, so no libinput or libudev in WebKit's NEEDED
    ! pkg-config --exists wpe-platform-drm-2.0 || die "wpe-platform-drm-2.0 is installed: the DRM backend was built"
    lib=$(ls /usr/lib/*/libWPEWebKit-2.0.so.1.*)
    ! readelf -d "$lib" | grep -E 'NEEDED.*\[lib(input|udev)\.so' || die "libWPEWebKit-2.0 links libinput or libudev"
    echo "no libinput, no libudev in libWPEWebKit-2.0's NEEDED" | tee -a "$out/report.txt"
    gcc -Wall -Wextra -O1 -o "$out/wpe-headless-render" /build/wpewebkit-headless-test.c \
        $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-2.0 wpe-platform-headless-2.0)
    id -u tester >/dev/null 2>&1 || useradd -m tester
    chown tester: "$out"
    grep -q 'WEBKIT_DISABLE_SANDBOX' /proc/self/environ && die "WebKit's sandbox must stay on in the test"
    # a user like any desktop user: WebKit starts its web and network processes in its bubblewrap sandbox
    runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 120 "$out/wpe-headless-render" "$out/render.ppm" >> "$out/report.txt" 2>&1 ||
        { tail -n 30 "$out/report.txt"; die "the headless render failed"; }
    tail -n 5 "$out/report.txt"
    rm -rf "$out/old/root"
    [ ! -f /build/lordos-src.tar ] || lordweb_check
}

# today's lord-web against this WebKit, on both of its view backends (WPEPlatform and the legacy libwpe one): built
# with -Wall -Wextra, then its own headless backend test (server/tests/web_backend_test.c: render, input, resize,
# scale, visibility, cursor) and its probe test (server/tests/web_probe_test.py), both with WebKit's sandbox on
lordweb_check() {
    out=/build/test
    rm -rf /build/src && mkdir -p /build/src && tar -xf /build/lordos-src.tar -C /build/src
    chown -R tester: /build/src
    echo "== lord-web (LordOS $(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' /build/src/package.json | head -1))" >> "$out/report.txt"
    for b in platform legacy; do
        runuser -u tester -- make -C /build/src/server -j"$(nproc)" LORD_WEB_BACKEND=$b build/lord-web build/web-backend-test \
            > "$out/lord-web-$b.log" 2>&1 || { tail -n 30 "$out/lord-web-$b.log"; die "lord-web ($b) does not build"; }
        ! grep -E 'warning:' "$out/lord-web-$b.log" || die "lord-web ($b) builds with warnings"
        (cd /build/src/server && runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 300 build/web-backend-test) \
            >> "$out/report.txt" 2>&1 || { tail -n 20 "$out/report.txt"; die "web-backend-test ($b) failed"; }
        (cd /build/src/server && runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 300 python3 -I tests/web_probe_test.py build/lord-web --pss) \
            >> "$out/report.txt" 2>&1 || { tail -n 20 "$out/report.txt"; die "web_probe_test ($b) failed"; }
        echo "lord-web ($b backend): builds without warnings, web-backend-test and web_probe_test pass" | tee -a "$out/report.txt"
    done
}

case "${1:-}" in
    build) shift; cmd_build "$@" ;;
    test) cmd_test ;;
    *) die "usage: wpewebkit-chroot.sh build <debian-version> <lordos-version> | test" ;;
esac
