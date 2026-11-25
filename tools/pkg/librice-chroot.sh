#!/bin/sh
# librice-chroot.sh — the steps of tools/pkg/build-librice.sh that run inside its chroots (docs/WPEWEBKIT.md, "librice").
#   vendor <upstream-version>                   the crates upstream's Cargo.lock pins, as a reproducible tarball, and
#                                               debian/copyright from their license fields (the only step with network)
#   build  <upstream-version> <debian-version>  the source package and the binary packages, offline
#   test   <upstream-version>                   the packages install and work from C
set -eu
export LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
die() { echo "librice-chroot: $*" >&2; exit 1; }
DEBEMAIL_LORDOS="LordOS <lord.basex@gmail.com>"
# the release date of librice 0.4.3 (its tag), the tarballs' and the changelog's fixed time
EPOCH=1778457600

cmd_vendor() {
    up=$1
    mkdir -p /build/src && tar -xzf "/build/in/librice_$up.orig.tar.gz" -C /build/src
    cd "/build/src/librice-$up"
    # every crate of the lock file, at its pinned version; cargo checks each one against the lock file's checksum
    CARGO_HOME=/build/cargo cargo vendor --locked --versioned-dirs /build/vendor > /dev/null
    # the import libraries of the target-specific crates for another operating system's linker are prebuilt binaries
    # that never take part in a Linux build: removed, so the source package holds none (their checksum list emptied
    # so cargo accepts the crate; the crate's own checksum, the lock file's, stays)
    for d in /build/vendor/windows_*_gnu* /build/vendor/windows_*_msvc* /build/vendor/winapi-*-pc-windows-gnu*; do
        [ -d "$d" ] || continue
        find "$d" -type f \( -name '*.a' -o -name '*.lib' -o -name '*.dll' \) -delete
        python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["files"]={}; json.dump(d,open(p,"w"))' "$d/.cargo-checksum.json"
    done
    n=$(ls /build/vendor | wc -l)
    echo "vendored $n crates"
    cd /build && find vendor -exec touch -h -d "@$EPOCH" {} +
    tar --sort=name --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner --format=gnu -cf - vendor | xz -T1 -9 > "/build/out/librice_$up.orig-vendor.tar.xz"
    python3 -I - "/build/src/librice-$up" /build/vendor > /build/out/copyright <<'PY'
import os, re, sys, tomllib
src, vend = sys.argv[1], sys.argv[2]
def lic(expr):
    expr = (expr or "").replace("/", " OR ")
    return re.sub(r"\s+", " ", re.sub(r"\bOR\b", "or", re.sub(r"\bAND\b", "and", expr))).strip()
seen, stanzas = set(), []
for name in sorted(os.listdir(vend)):
    with open(os.path.join(vend, name, "Cargo.toml"), "rb") as f:
        p = tomllib.load(f).get("package", {})
    l = p.get("license")
    if not l:
        lf = p.get("license-file")
        l = "LicenseRef-%s" % name if lf else "UNKNOWN"
    l = lic(l)
    for t in re.split(r"[ ()]+", l):
        if t and t not in ("or", "and", "WITH", "with"):
            seen.add(t)
    authors = p.get("authors") or ["the %s authors" % p.get("name", name)]
    stanzas.append("Files: vendor/%s/*\nCopyright: %s\nLicense: %s\n" % (name, "\n ".join(a.replace("\n", " ") for a in authors), l))
seen |= {"MIT", "Apache-2.0"}
out = ["Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\nUpstream-Name: librice\n"
       "Source: https://github.com/ystreet/librice\nComment: The crates in vendor/ are the ones upstream's Cargo.lock pins,\n"
       " fetched by cargo vendor at those versions and checksums (tools/pkg/build-librice.sh in the LordOS\n"
       " repository); the license of each comes from its own Cargo.toml.\n",
       "Files: *\nCopyright: 2020-2026 Matthew Waters <matthew@centricular.com>\nLicense: MIT or Apache-2.0\n",
       "Files: debian/*\nCopyright: 2026 LordOS\nLicense: MIT or Apache-2.0\n"] + stanzas
common = {"Apache-2.0", "MPL-2.0", "CC0-1.0", "GPL-2", "GPL-3", "LGPL-2.1", "BSD"}
def text_for(lid):
    # the text from upstream (MIT) or from a vendored crate that ships it
    cands = {"MIT": ["LICENSE-MIT", "LICENSE.MIT"], "ISC": ["LICENSE-ISC", "LICENSE"], "Zlib": ["LICENSE-ZLIB", "LICENSE-Zlib", "LICENSE"],
             "BSD-3-Clause": ["LICENSE-BSD", "LICENSE"], "BSD-2-Clause": ["LICENSE-BSD", "LICENSE"], "Unicode-3.0": ["LICENSE-UNICODE", "LICENSE"],
             "Unlicense": ["UNLICENSE"], "BSL-1.0": ["LICENSE-BOOST", "LICENSE"], "0BSD": ["LICENSE-0BSD", "LICENSE"]}.get(lid, ["LICENSE-" + lid])
    if lid == "MIT":
        return open(os.path.join(src, "LICENSE-MIT")).read()
    for name in sorted(os.listdir(vend)):
        with open(os.path.join(vend, name, "Cargo.toml"), "rb") as f:
            l = lic(tomllib.load(f).get("package", {}).get("license", ""))
        if lid not in re.split(r"[ ()]+", l):
            continue
        for c in cands:
            p = os.path.join(vend, name, c)
            if os.path.isfile(p):
                return open(p, errors="replace").read()
    return None
for lid in sorted(seen):
    if lid in common:
        out.append("License: %s\n On Debian systems, the complete text of this license is in\n /usr/share/common-licenses/%s.\n" % (lid, lid))
        continue
    t = text_for(lid)
    if t is None:
        out.append("License: %s\n The text is in the vendored crate that names it (vendor/*/).\n" % lid)
        continue
    body = "\n".join((" " + l) if l.strip() else " ." for l in t.strip("\n").splitlines())
    out.append("License: %s\n%s\n" % (lid, body))
print("\n".join(out))
PY
    echo "licenses: $(grep -c '^License: ' /build/out/copyright) stanzas"
}

cmd_build() {
    up=$1 newver=$2
    id -u builder >/dev/null 2>&1 || useradd -m builder
    mkdir -p /build/work && cd /build/work
    cp "/build/in/librice_$up.orig.tar.gz" "/build/in/librice_$up.orig-vendor.tar.xz" .
    tar -xzf "librice_$up.orig.tar.gz"
    mkdir "librice-$up/vendor" && tar -xJf "librice_$up.orig-vendor.tar.xz" -C "librice-$up/vendor" --strip-components=1
    tar -xf /build/debian.tar -C "librice-$up"
    cp /build/in/copyright "librice-$up/debian/copyright"
    cd "librice-$up"
    date=$(TZ=UTC date -R -d "@$EPOCH")
    DEBEMAIL=$DEBEMAIL_LORDOS dch --create --package librice --newversion "$newver" --distribution trixie --force-distribution \
        "librice $up for LordOS (Debian 13): rice-proto and rice-io as C libraries, for WPE WebKit's WebRTC (docs/WPEWEBKIT.md in the LordOS repository)."
    sed -i "0,/^ -- /s/^\( -- .*>  \).*$/\1$date/" debian/changelog
    mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control
    rm -f librice-build-deps_*
    chown -R builder: /build/work
    runuser -u builder -- env DEB_BUILD_OPTIONS="parallel=${JOBS:-$(nproc)}" dpkg-buildpackage -us -uc -sa
    cd /build/work
    cp ./*.deb ./*.dsc ./*.debian.tar.xz ./*.orig*.tar.* ./*.buildinfo /build/out/
    cp ./*.changes /build/out/
    sync
    (cd /build/out && dscverify --no-sig-check "librice_${newver}.dsc")
    lintian --info --display-info --pedantic --profile debian /build/out/librice_${newver}_*.changes > /build/out/lintian.txt 2>&1 || true
    echo "lintian: $(grep -c '^E:' /build/out/lintian.txt) errors, $(grep -c '^W:' /build/out/lintian.txt) warnings," \
         "$(grep -c '^I:' /build/out/lintian.txt) info, $(grep -c '^P:' /build/out/lintian.txt) pedantic" > /build/out/lintian-summary.txt
    cat /build/out/lintian-summary.txt
}

cmd_test() {
    up=$1
    out=/build/test
    apt-get install -y --no-install-recommends /build/debs/*.deb
    {
        echo "== installed"; dpkg-query -W 'librice*'
        echo "== pkg-config"
        for m in rice-proto rice-io; do
            v=$(pkg-config --modversion "$m") || die "pkg-config does not find $m"
            [ "$v" = "$up" ] || die "$m is $v, not $up"
            echo "$m $v: $(pkg-config --cflags --libs "$m")"
        done
        echo "== files"; dpkg -L librice0 librice-dev | grep -E '\.(so|h|pc)' | sort
        echo "== NEEDED"; for l in /usr/lib/*/librice-*.so.*; do [ -L "$l" ] || echo "$l: $(readelf -d "$l" | awk '/NEEDED|SONAME/ { gsub(/[][]/, "", $5); printf "%s ", $5 }')"; done
        echo "== C"
    } > "$out/report.txt" 2>&1
    gcc -Wall -Wextra -o "$out/librice-test" /build/librice-test.c $(pkg-config --cflags --libs rice-proto rice-io) >> "$out/report.txt" 2>&1 || die "the C test doesn't build"
    if [ -n "${EMULATED:-}" ]; then echo "built; run under qemu-user" >> "$out/report.txt"; fi
    "$out/librice-test" >> "$out/report.txt" 2>&1 || { cat "$out/report.txt"; die "the C test failed"; }
    cat "$out/report.txt"
}

case "${1:-}" in
    vendor) shift; cmd_vendor "$@" ;;
    build) shift; cmd_build "$@" ;;
    test) shift; cmd_test "$@" ;;
    *) die "usage: librice-chroot.sh vendor <up> | build <up> <version> | test <up>" ;;
esac
