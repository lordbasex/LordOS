# apt/recipes/lib.sh — helpers shared by the recipes (sourced inside the debian:trixie build container).
#
# A recipe runs as /recipes/<name>/build.sh with:
#   /in        read-only: the verified inputs (source.tar.gz or the vendor archive named in /in/ARCHIVE,
#              every "inputs" file by its name, and app.json, the app's catalog entry)
#   /out       where the .deb files go (and nothing else)
#   PKG VERSION UPSTREAM_VERSION ARCH SOURCE_DATE_EPOCH JOBS   from apt/build.sh
# The container has no network (--network none) and runs as the invoking user.

MAINTAINER="LordOS <lord.basex@gmail.com>"

die() { echo "recipe: $*" >&2; exit 1; }

# shlibs_depends <root> <libdir-in-root>… -- <file>…: the Depends that dpkg-shlibdeps computes for the
# ELF files (libraries in the given private folders of <root> are part of the package itself)
shlibs_depends() {
    sd_root=$1; shift
    sd_l=""
    while [ $# -gt 0 ] && [ "$1" != -- ]; do sd_l="$sd_l -l$sd_root$1"; shift; done
    [ "${1:-}" = -- ] && shift
    sd_w=$(mktemp -d)
    mkdir -p "$sd_w/debian"
    printf 'Source: lordos-catalog\n\nPackage: lordos-catalog\nArchitecture: any\n' > "$sd_w/debian/control"
    # shellcheck disable=SC2086
    (cd "$sd_w" && dpkg-shlibdeps -O --ignore-missing-info $sd_l "$@" 2>/dev/null) | sed -n 's/^shlibs:Depends=//p'
    rm -rf "$sd_w"
}

# elf_files <dir>…: the ELF executables and shared objects below the folders
elf_files() {
    find "$@" -type f | sort | while read -r ef; do
        case "$(file -b "$ef")" in ELF*) echo "$ef" ;; esac
    done
}

# installed_size <root>: kibibytes, as dpkg counts them (without DEBIAN/)
installed_size() { du -sk --exclude=DEBIAN "$1" | cut -f1; }

# build_deb <root> : normalises permissions and times, writes /out/<Package>_<Version>_<Architecture>.deb
build_deb() {
    bd_root=$1
    [ -f "$bd_root/DEBIAN/control" ] || die "no DEBIAN/control in $bd_root"
    bd_p=$(sed -n 's/^Package: //p' "$bd_root/DEBIAN/control")
    bd_v=$(sed -n 's/^Version: //p' "$bd_root/DEBIAN/control")
    bd_a=$(sed -n 's/^Architecture: //p' "$bd_root/DEBIAN/control")
    find "$bd_root" -type d -exec chmod 0755 {} +
    find "$bd_root" -type f ! -perm -u+x -exec chmod 0644 {} +
    find "$bd_root" -type f -perm -u+x -exec chmod 0755 {} +
    for s in preinst postinst prerm postrm; do [ -f "$bd_root/DEBIAN/$s" ] && chmod 0755 "$bd_root/DEBIAN/$s"; done
    [ -f "$bd_root/DEBIAN/conffiles" ] && chmod 0644 "$bd_root/DEBIAN/conffiles"
    # md5sums, as dpkg-deb's users expect
    (cd "$bd_root" && find . -path ./DEBIAN -prune -o -type f -print | sed 's|^\./||' | LC_ALL=C sort | xargs -r md5sum) > "$bd_root/DEBIAN/md5sums"
    chmod 0644 "$bd_root/DEBIAN/md5sums"
    find "$bd_root" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
    dpkg-deb --root-owner-group -Zxz -z6 --build "$bd_root" "/out/${bd_p}_${bd_v#*:}_${bd_a}.deb" >&2
}

# copyright_mit <file> <upstream-name> <source-url> <license-file>: a DEP-5 copyright file
copyright_file() {
    cf_out=$1; cf_name=$2; cf_src=$3; cf_lic=$4; cf_licfile=$5
    mkdir -p "$(dirname "$cf_out")"
    {
        printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
        printf 'Upstream-Name: %s\nSource: %s\n' "$cf_name" "$cf_src"
        printf 'Comment: Packaged for LordOS from the verified upstream release (apt/catalog.json in the LordOS\n repository). The upstream licence text follows.\n\n'
        printf 'Files: *\nCopyright: see below\nLicense: %s\n' "$cf_lic"
        sed -e 's/^$/./' -e 's/^/ /' "$cf_licfile"
    } > "$cf_out"
}
