#!/bin/sh
# apt/build.sh — the curated app catalog (apt/catalog.json) of the LordOS APT repository
# (apt/README.md, docs/UPDATES.md "Mirrored apps (catalog)").
#
#   apt/build.sh check [id…]    validate the catalog; for every "apt" entry verify the vendor's
#                               repository (pinned key, signed InRelease, index hash, package per arch)
#   apt/build.sh fetch [id…]    download and verify (and build, from apt/recipes/) every app that goes
#                               into lordos.org/apt's pool/catalog; prints the resulting .deb paths
#   apt/build.sh list           one line per app: id, package, source type, delivery, version
#
# Only stdout of "fetch" is the list of .debs (tools/release/apt-repo.sh build --catalog reads it);
# everything else goes to stderr. Files are kept in $WORKDIR/catalog-cache (WORKDIR as apt-repo.sh:
# $LORDOS_APT_DIR or ~/.local/share/lordos-apt): a verified download or a built package is reused,
# never rebuilt for the same version, so a version that vanished upstream can still be republished
# and the append-only pool never sees two different files with the same name.
#
# Trust: nothing is used unless it matches what apt/catalog.json pins.
#   apt     the key file's only primary key must have the catalog's fingerprint; InRelease must carry a
#           good signature by it (gpgv, VALIDSIG); Packages must match the SHA-256 in InRelease; the
#           newest version per architecture is taken (dpkg --compare-versions); its Filename must stay
#           inside pool/; the .deb must match the index's SHA-256 and size. delivery "direct" (installed
#           from the vendor on the user's machine) is only checked, never mirrored.
#   deb     the vendor .deb must match the pinned SHA-256 and its Package/Version/Architecture.
#   build   the source tarball (and every extra input) must match its pinned SHA-256; the recipe runs
#           in a debian:trixie container of the target architecture with --network none.
#   binary  the same for the vendor's prebuilt archive; a sigstore bundle, when pinned, is verified
#           with cosign (keyless: the catalog's identity and OIDC issuer).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CAT=${LORDOS_CATALOG:-$ROOT/apt/catalog.json}
WORKDIR=${LORDOS_APT_DIR:-$HOME/.local/share/lordos-apt}
CACHE=$WORKDIR/catalog-cache
TOOLS_IMAGE=lordos-apt-tools:trixie                 # the same image as tools/release/apt-repo.sh
BUILD_IMAGE=lordos-catalog-build:trixie-v2          # + "-<arch>"; bump the tag when the packages below change
COSIGN_IMAGE=gcr.io/projectsigstore/cosign:v2.4.1@sha256:b03690aa52bfe94054187142fba24dc54137650682810633901767d8a3e15b31
export LC_ALL=C

die() { echo "apt/build.sh: $*" >&2; exit 1; }
say() { echo "$*" >&2; }
usage() { sed -n '5,10p' "$0" >&2; exit 2; }

command -v jq >/dev/null 2>&1 || die "needs jq"
[ -f "$CAT" ] || die "no catalog at $CAT"

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
fsize() { wc -c < "$1" | tr -d ' '; }
is_sha() { printf %s "$1" | grep -Eq '^[0-9a-f]{64}$'; }
is_https() { case "$1" in https://*) case "$1" in *[!A-Za-z0-9._~:/?=\&%+@-]*) return 1 ;; esac; return 0 ;; *) return 1 ;; esac; }
is_name() { printf %s "$1" | grep -Eq '^[a-z0-9][a-z0-9.+-]+$'; }
is_version() { printf %s "$1" | grep -Eq '^([0-9]+:)?[0-9][A-Za-z0-9.+~-]*$'; }
j() { jq -r "$@" "$CAT"; }
# a field of app $1 (an id): jf <id> <jq filter on the app object>
jf() { jq -r --arg id "$1" ".apps[] | select(.id == \$id) | $2" "$CAT"; }

# download $1 to $2 unless $2 already has SHA-256 $3; verify; atomically rename
fetch_pinned() {
    fp_url=$1; fp_dest=$2; fp_want=$3
    if [ -f "$fp_dest" ] && [ "$(sha256 "$fp_dest")" = "$fp_want" ]; then return 0; fi
    mkdir -p "$(dirname "$fp_dest")"
    say "  downloading $fp_url"
    curl -fsSL --proto '=https' --proto-redir '=https' --retry 3 -o "$fp_dest.part" "$fp_url" || { rm -f "$fp_dest.part"; die "cannot download $fp_url"; }
    fp_got=$(sha256 "$fp_dest.part")
    [ "$fp_got" = "$fp_want" ] || { rm -f "$fp_dest.part"; die "$fp_url: SHA-256 is $fp_got, the catalog pins $fp_want"; }
    mv "$fp_dest.part" "$fp_dest"
}

# dpkg-deb and dpkg --compare-versions: this machine's, or the apt-repo.sh tools container's
tools_image() {
    docker image inspect "$TOOLS_IMAGE" >/dev/null 2>&1 && return 0
    printf 'FROM debian:trixie\nRUN apt-get update -q && apt-get install -y -q --no-install-recommends apt-utils dpkg-dev xz-utils && rm -rf /var/lib/apt/lists/*\n' |
        docker build -q --platform linux/amd64 -t "$TOOLS_IMAGE" - >/dev/null || die "could not build the $TOOLS_IMAGE container"
}
# deb_field <file.deb> <Field>
deb_field() {
    if command -v dpkg-deb >/dev/null 2>&1; then dpkg-deb -f "$1" "$2"; return; fi
    command -v docker >/dev/null 2>&1 || die "needs dpkg-deb or docker"
    tools_image
    df_d=$(cd "$(dirname "$1")" && pwd)
    docker run --rm --platform linux/amd64 --network none -v "$df_d:/d:ro" "$TOOLS_IMAGE" dpkg-deb -f "/d/$(basename "$1")" "$2"
}
# stdin: "<version> <rest…>" lines; stdout: the line with the highest Debian version
newest() {
    script='best=""; line=""; while read -r v rest; do if [ -z "$best" ] || dpkg --compare-versions "$v" gt "$best"; then best=$v; line="$v $rest"; fi; done; printf "%s\n" "$line"'
    if command -v dpkg >/dev/null 2>&1; then sh -c "$script"; return; fi
    tools_image
    docker run --rm -i --platform linux/amd64 --network none "$TOOLS_IMAGE" sh -c "$script"
}
# check that a .deb is the package, version and architecture we expect
check_deb() {
    cd_f=$1; cd_pkg=$2; cd_ver=$3; cd_arch=$4
    cd_p=$(deb_field "$cd_f" Package); cd_v=$(deb_field "$cd_f" Version); cd_a=$(deb_field "$cd_f" Architecture)
    [ "$cd_p" = "$cd_pkg" ] || die "$cd_f is package $cd_p, expected $cd_pkg"
    [ -z "$cd_ver" ] || [ "$cd_v" = "$cd_ver" ] || die "$cd_f is version $cd_v, expected $cd_ver"
    [ "$cd_a" = "$cd_arch" ] || [ "$cd_a" = all ] || die "$cd_f is for $cd_a, expected $cd_arch"
}

# seconds since the epoch of an RFC 2822 date (Release "Date"/"Valid-Until"), GNU or BSD date
epoch_of() {
    date -u -d "$1" +%s 2>/dev/null && return
    d=$(printf %s "$1" | sed -e 's/ UTC$/ +0000/' -e 's/ GMT$/ +0000/' -e 's/ Z$/ +0000/')
    date -j -u -f '%a, %d %b %Y %H:%M:%S %z' "$d" +%s 2>/dev/null
}

# ---------------------------------------------------------------------------------------------------
# the catalog itself
validate() {
    [ "$(j .schema)" = 1 ] || die "catalog schema must be 1"
    dup=$(j '.apps[].id' | sort | uniq -d)
    [ -z "$dup" ] || die "duplicate ids: $dup"
    dup=$(j '.apps[] | .package, (.extra_packages // [])[]' | sort | uniq -d)
    [ -z "$dup" ] || die "duplicate package names: $dup"
    for id in $(j '.apps[].id'); do
        is_name "$id" || die "bad id $id"
        for f in package name license vendor homepage kind summary.en summary.es summary.pt source.type; do
            v=$(jf "$id" ".$f // empty")
            [ -n "$v" ] || die "$id: missing $f"
        done
        is_name "$(jf "$id" .package)" || die "$id: bad package name"
        for p in $(jf "$id" '(.extra_packages // [])[]'); do is_name "$p" || die "$id: bad extra package $p"; done
        is_https "$(jf "$id" .homepage)" || die "$id: homepage must be https"
        arches=$(jf "$id" '.arches[]')
        [ -n "$arches" ] || die "$id: no arches"
        for a in $arches; do case "$a" in amd64|arm64) ;; *) die "$id: unsupported arch $a" ;; esac; done
        type=$(jf "$id" .source.type)
        case "$type" in
            apt)
                case "$(jf "$id" '.source.delivery // "mirror"')" in mirror|direct) ;; *) die "$id: delivery must be mirror or direct" ;; esac
                is_https "$(jf "$id" .source.url)" || die "$id: source.url must be https"
                is_name "$(jf "$id" .source.suite)" || die "$id: bad suite"
                [ -n "$(jf "$id" '.source.components[]?')" ] || die "$id: no components"
                for c in $(jf "$id" '.source.components[]'); do is_name "$c" || die "$id: bad component $c"; done
                for a in $(jf "$id" '.source.arches[]'); do printf ' %s ' $arches | grep -q " $a " || die "$id: source arch $a not in arches"; done
                k=$(jf "$id" .source.key)
                case "$k" in keys/*.asc) ;; *) die "$id: key must be keys/<name>.asc" ;; esac
                case "$k" in *..*|*[!A-Za-z0-9._/-]*) die "$id: bad key path" ;; esac
                [ -f "$ROOT/apt/$k" ] || die "$id: no key file apt/$k"
                printf %s "$(jf "$id" .source.fingerprint)" | grep -Eq '^[0-9A-F]{40}$' || die "$id: fingerprint must be 40 upper-case hex digits"
                is_name "$(jf "$id" .source.package)" || die "$id: bad source.package"
                [ "$(jf "$id" .source.package)" = "$(jf "$id" .package)" ] || die "$id: source.package differs from package"
                ;;
            deb|binary)
                is_version "$(jf "$id" .source.version)" || die "$id: bad version"
                for a in $arches; do
                    is_https "$(jf "$id" ".source.files.\"$a\".url")" || die "$id: files.$a.url must be https"
                    is_sha "$(jf "$id" ".source.files.\"$a\".sha256")" || die "$id: files.$a.sha256"
                    if [ -n "$(jf "$id" ".source.files.\"$a\".sigstore // empty")" ]; then
                        is_https "$(jf "$id" ".source.files.\"$a\".sigstore.url")" || die "$id: sigstore url"
                        is_sha "$(jf "$id" ".source.files.\"$a\".sigstore.sha256")" || die "$id: sigstore sha256"
                        printf %s "$(jf "$id" ".source.files.\"$a\".sigstore.blob")" | grep -Eq '^[A-Za-z0-9._-]+$' || die "$id: sigstore blob"
                        [ -n "$(jf "$id" '.source.sigstore.identity // empty')" ] && [ -n "$(jf "$id" '.source.sigstore.issuer // empty')" ] || die "$id: sigstore identity/issuer"
                    fi
                done
                ;;
            build)
                is_version "$(jf "$id" .source.version)" || die "$id: bad version"
                is_https "$(jf "$id" .source.url)" || die "$id: source.url must be https"
                is_sha "$(jf "$id" .source.sha256)" || die "$id: source.sha256"
                ;;
            *) die "$id: unknown source type $type" ;;
        esac
        case "$type" in
            build|binary)
                r=$(jf "$id" .source.recipe)
                is_name "$r" && [ -f "$ROOT/apt/recipes/$r/build.sh" ] || die "$id: no recipe apt/recipes/$r/build.sh"
                printf %s "$(jf "$id" .source.revision)" | grep -Eq '^[0-9][A-Za-z0-9.+~]*$' || die "$id: bad revision"
                printf %s "$(jf "$id" .source.source_date_epoch)" | grep -Eq '^[0-9]{9,10}$' || die "$id: bad source_date_epoch"
                n=$(jf "$id" '(.source.inputs // []) | length')
                i=0
                while [ "$i" -lt "$n" ]; do
                    printf %s "$(jf "$id" ".source.inputs[$i].name")" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._-]*$' || die "$id: bad input name"
                    is_https "$(jf "$id" ".source.inputs[$i].url")" || die "$id: input url must be https"
                    is_sha "$(jf "$id" ".source.inputs[$i].sha256")" || die "$id: input sha256"
                    i=$((i + 1))
                done
                ;;
        esac
    done
}

# ---------------------------------------------------------------------------------------------------
# vendor APT repositories

# apt_release <id> <dir>: verified $dir/Release of the vendor's repository
apt_release() {
    id=$1; t=$2
    key=$ROOT/apt/$(jf "$id" .source.key); fpr=$(jf "$id" .source.fingerprint)
    url=$(jf "$id" .source.url); suite=$(jf "$id" .source.suite)
    # exactly one primary key in the file, and it is the pinned one
    k=$(mktemp -d)
    prim=$(GNUPGHOME=$k gpg --batch --with-colons --show-keys "$key" 2>/dev/null | awk -F: '/^pub/ { p = 1; next } p && /^fpr/ { print $10; p = 0 }')
    GNUPGHOME=$k gpg --batch --quiet --yes --dearmor -o "$t/key.gpg" "$key" 2>/dev/null || { rm -rf "$k"; die "$id: cannot read apt/$(jf "$id" .source.key)"; }
    rm -rf "$k"
    [ "$prim" = "$fpr" ] || die "$id: apt/$(jf "$id" .source.key) holds key(s) $(echo $prim), the catalog pins $fpr"
    curl -fsSL --proto '=https' --proto-redir '=https' --retry 3 -o "$t/InRelease" "$url/dists/$suite/InRelease" || die "$id: cannot download $url/dists/$suite/InRelease"
    gpgv --status-fd 3 --keyring "$t/key.gpg" --output "$t/Release" "$t/InRelease" 3> "$t/status" 2>/dev/null \
        || die "$id: InRelease does not verify with apt/$(jf "$id" .source.key)"
    awk -v f="$fpr" '$2 == "VALIDSIG" && ($3 == f || $NF == f) { ok = 1 } END { exit !ok }' "$t/status" \
        || die "$id: InRelease is not signed by $fpr"
    # the suite: Suite, or Codename when Suite is empty (Cursor)
    s=$(sed -n 's/^Suite: *//p' "$t/Release" | head -n 1); c=$(sed -n 's/^Codename: *//p' "$t/Release" | head -n 1)
    [ "$s" = "$suite" ] || [ "$c" = "$suite" ] || die "$id: InRelease is for suite '$s' / codename '$c', not $suite"
    vu=$(sed -n 's/^Valid-Until: *//p' "$t/Release" | head -n 1)
    if [ -n "$vu" ]; then
        e=$(epoch_of "$vu") || die "$id: cannot read Valid-Until '$vu'"
        [ "$e" -gt "$(date -u +%s)" ] || die "$id: InRelease expired ($vu)"
    fi
}

# apt_packages <id> <dir> <component> <arch>: verified, decompressed $dir/Packages.<component>.<arch>
apt_packages() {
    id=$1; t=$2; comp=$3; a=$4
    url=$(jf "$id" .source.url); suite=$(jf "$id" .source.suite)
    out=$t/Packages.$comp.$a
    for ext in .xz .gz ""; do
        p=$comp/binary-$a/Packages$ext
        line=$(awk -v p="$p" '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 } s && $3 == p { print $1, $2; exit }' "$t/Release")
        [ -n "$line" ] || continue
        want=${line%% *}; size=${line#* }
        is_sha "$want" || die "$id: bad SHA-256 for $p in InRelease"
        curl -fsSL --proto '=https' --proto-redir '=https' --retry 3 -o "$t/raw" "$url/dists/$suite/$p" || die "$id: cannot download $p"
        [ "$(sha256 "$t/raw")" = "$want" ] && [ "$(fsize "$t/raw")" = "$size" ] || die "$id: $p does not match InRelease"
        case "$ext" in .xz) xz -dc "$t/raw" > "$out" ;; .gz) gzip -dc "$t/raw" > "$out" ;; *) mv "$t/raw" "$out" ;; esac
        rm -f "$t/raw"
        return 0
    done
    die "$id: InRelease lists no $comp/binary-$a/Packages"
}

# apt_newest <id> <Packages file> <arch>: "<version> <sha256> <size> <filename>" of the newest $package
apt_newest() {
    id=$1; f=$2; a=$3; pkg=$(jf "$id" .source.package)
    awk -v n="$pkg" -v a="$a" 'BEGIN { RS = "" } {
        pn = pv = pa = ps = pz = pf = ""; k = split($0, L, "\n")
        for (i = 1; i <= k; i++) {
            if (L[i] ~ /^Package: /) pn = substr(L[i], 10); else if (L[i] ~ /^Version: /) pv = substr(L[i], 10)
            else if (L[i] ~ /^Architecture: /) pa = substr(L[i], 15); else if (L[i] ~ /^SHA256: /) ps = substr(L[i], 9)
            else if (L[i] ~ /^Size: /) pz = substr(L[i], 7); else if (L[i] ~ /^Filename: /) pf = substr(L[i], 11)
        }
        if (pn == n && (pa == a || pa == "all")) print pv, ps, pz, pf }' "$f" > "$f.cand"
    [ -s "$f.cand" ] || die "$id: no $pkg for $a in the vendor's index"
    while read -r v s z p; do
        is_version "$v" || die "$id: bad version '$v' in the vendor's index"
        is_sha "$s" || die "$id: $pkg $v has no SHA-256 in the index"
        printf %s "$z" | grep -Eq '^[0-9]+$' || die "$id: $pkg $v has no Size"
        case "$p" in pool/*.deb) ;; *) die "$id: Filename $p is outside pool/" ;; esac
        case "$p" in *..*|*//*|*[!A-Za-z0-9._+~/-]*) die "$id: unsafe Filename $p" ;; esac
    done < "$f.cand"
    newest < "$f.cand"
}

# do_apt <id> [fetch]: verify the vendor repository; with "fetch", also download the newest .deb
# of each architecture into the cache and print its path
do_apt() {
    id=$1; mode=${2:-check}
    t=$(mktemp -d)
    apt_release "$id" "$t"
    pkg=$(jf "$id" .source.package); url=$(jf "$id" .source.url)
    for a in $(jf "$id" '.source.arches[]'); do
        found=0
        for comp in $(jf "$id" '.source.components[]'); do
            apt_packages "$id" "$t" "$comp" "$a"
            awk -v n="$pkg" 'BEGIN { RS = "" } $0 ~ ("(^|\n)Package: " n "\n") { f = 1 } END { exit !f }' "$t/Packages.$comp.$a" || continue
            found=1
            line=$(apt_newest "$id" "$t/Packages.$comp.$a" "$a")
            set -- $line; v=$1; s=$2; z=$3; p=$4
            say "  ok $id: $pkg $v ($a) signed by $(jf "$id" .source.fingerprint), $comp, $(printf '%s' "$z" | awk '{ printf "%.1f MB", $1 / 1e6 }')"
            if [ "$mode" = fetch ]; then
                dest=$CACHE/apt/$id/${pkg}_${v#*:}_$a.deb
                case "$p" in *_all.deb) dest=$CACHE/apt/$id/${pkg}_${v#*:}_all.deb ;; esac
                fetch_pinned "$url/$p" "$dest" "$s"
                [ "$(fsize "$dest")" = "$z" ] || die "$id: $dest has the wrong size"
                check_deb "$dest" "$pkg" "$v" "$a"
                echo "$dest"
            fi
            break
        done
        [ "$found" = 1 ] || die "$id: no $pkg for $a in any component"
    done
    rm -rf "$t"
}

# ---------------------------------------------------------------------------------------------------
# vendor .debs
do_deb() {
    id=$1
    pkg=$(jf "$id" .package); ver=$(jf "$id" .source.version)
    for a in $(jf "$id" '.arches[]'); do
        u=$(jf "$id" ".source.files.\"$a\".url"); s=$(jf "$id" ".source.files.\"$a\".sha256")
        dest=$CACHE/deb/$id/${pkg}_${ver#*:}_$a.deb
        fetch_pinned "$u" "$dest" "$s"
        check_deb "$dest" "$pkg" "$ver" "$a"
        say "  ok $id: $pkg $ver ($a), SHA-256 $s"
        echo "$dest"
    done
}

# ---------------------------------------------------------------------------------------------------
# packages LordOS builds (apt/recipes/<recipe>/build.sh)

build_image() {
    a=$1; img=$BUILD_IMAGE-$a
    docker image inspect "$img" >/dev/null 2>&1 && return 0
    say "  building the $img container"
    printf '%s\n' 'FROM debian:trixie' \
        'RUN apt-get update -q && apt-get install -y -q --no-install-recommends build-essential cmake ninja-build pkg-config libssl-dev libsdl2-dev libvulkan-dev glslc spirv-headers dpkg-dev xz-utils zstd file jq ca-certificates && rm -rf /var/lib/apt/lists/*' |
        docker build -q --platform "linux/$a" -t "$img" - >/dev/null || die "could not build the $img container"
}

cosign_verify() {
    id=$1; bundle=$2; blob=$3
    ident=$(jf "$id" .source.sigstore.identity); issuer=$(jf "$id" .source.sigstore.issuer)
    if [ -n "${LORDOS_CATALOG_NO_COSIGN:-}" ]; then say "  WARNING: sigstore check of $(basename "$blob") skipped (LORDOS_CATALOG_NO_COSIGN)"; return 0; fi
    if command -v cosign >/dev/null 2>&1; then
        cosign verify-blob --bundle "$bundle" --certificate-identity "$ident" --certificate-oidc-issuer "$issuer" "$blob" >/dev/null 2>&1 \
            || die "$id: sigstore bundle does not verify $(basename "$blob")"
    else
        command -v docker >/dev/null 2>&1 || die "$id: needs cosign or docker for the sigstore check"
        d=$(dirname "$blob")
        cp "$bundle" "$d/bundle.sigstore"
        docker run --rm -v "$d:/x:ro" "$COSIGN_IMAGE" verify-blob --bundle /x/bundle.sigstore \
            --certificate-identity "$ident" --certificate-oidc-issuer "$issuer" "/x/$(basename "$blob")" >/dev/null 2>&1 \
            || die "$id: sigstore bundle does not verify $(basename "$blob")"
    fi
    say "  ok $id: sigstore signature of $(basename "$blob") by $ident"
}

do_recipe() {
    id=$1
    type=$(jf "$id" .source.type); recipe=$(jf "$id" .source.recipe)
    pkg=$(jf "$id" .package); up=$(jf "$id" .source.version); ver=$up-$(jf "$id" .source.revision)
    allowed=" $pkg $(jf "$id" '(.extra_packages // []) | join(" ")') "
    src=$CACHE/src/$id
    # inputs shared by every architecture
    n=$(jf "$id" '(.source.inputs // []) | length'); i=0
    while [ "$i" -lt "$n" ]; do
        fetch_pinned "$(jf "$id" ".source.inputs[$i].url")" "$src/inputs/$(jf "$id" ".source.inputs[$i].name")" "$(jf "$id" ".source.inputs[$i].sha256")"
        i=$((i + 1))
    done
    if [ "$type" = build ]; then
        fetch_pinned "$(jf "$id" .source.url)" "$src/source.tar.gz" "$(jf "$id" .source.sha256)"
        say "  ok $id: source $(jf "$id" .source.tag) SHA-256 $(jf "$id" .source.sha256)"
    fi
    for a in $(jf "$id" '.arches[]'); do
        out=$CACHE/out/$id/$ver/$a
        if [ -f "$out/.done" ]; then
            say "  ok $id: $ver ($a) already built (cache)"
            for f in "$out"/*.deb; do echo "$f"; done
            continue
        fi
        in=$CACHE/work/$id-$a
        rm -rf "$in" "$out"; mkdir -p "$in" "$out"
        [ -d "$src/inputs" ] && cp "$src/inputs/"* "$in/"
        if [ "$type" = build ]; then
            cp "$src/source.tar.gz" "$in/source.tar.gz"
        else
            u=$(jf "$id" ".source.files.\"$a\".url"); s=$(jf "$id" ".source.files.\"$a\".sha256")
            name=$(basename "$u")
            fetch_pinned "$u" "$src/$a/$name" "$s"
            say "  ok $id: $name SHA-256 $s"
            if [ -n "$(jf "$id" ".source.files.\"$a\".sigstore // empty")" ]; then
                fetch_pinned "$(jf "$id" ".source.files.\"$a\".sigstore.url")" "$src/$a/$name.sigstore" "$(jf "$id" ".source.files.\"$a\".sigstore.sha256")"
                blob=$(jf "$id" ".source.files.\"$a\".sigstore.blob")
                v=$(mktemp -d)
                tar -xzf "$src/$a/$name" -C "$v" "$blob" || die "$id: $name has no $blob"
                cosign_verify "$id" "$src/$a/$name.sigstore" "$v/$blob"
                rm -rf "$v"
            fi
            cp "$src/$a/$name" "$in/$name"
            printf '%s\n' "$name" > "$in/ARCHIVE"
        fi
        jq --arg id "$id" '.apps[] | select(.id == $id)' "$CAT" > "$in/app.json"
        build_image "$a"
        # parallel jobs: all CPUs for the native architecture; 3 under emulation, where the compilers
        # use several times the memory (16 jobs ran an 8 GB Docker VM out of memory on llama.cpp's
        # Vulkan shaders). LORDOS_CATALOG_JOBS overrides.
        case "$(uname -m)" in x86_64|amd64) native=amd64 ;; arm64|aarch64) native=arm64 ;; *) native="" ;; esac
        if [ "$a" = "$native" ]; then jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4); else jobs=3; fi
        jobs=${LORDOS_CATALOG_JOBS:-$jobs}
        say "  building $pkg $ver for $a (apt/recipes/$recipe; this can take a long time under emulation)"
        docker run --rm --platform "linux/$a" --network none -u "$(id -u):$(id -g)" \
            -e HOME=/tmp -e PKG="$pkg" -e VERSION="$ver" -e UPSTREAM_VERSION="$up" -e ARCH="$a" \
            -e SOURCE_DATE_EPOCH="$(jf "$id" .source.source_date_epoch)" -e JOBS="$jobs" \
            -v "$ROOT/apt/recipes:/recipes:ro" -v "$in:/in:ro" -v "$out:/out" \
            "$BUILD_IMAGE-$a" sh -eu "/recipes/$recipe/build.sh" >&2 || die "$id: recipe $recipe failed for $a"
        ls "$out"/*.deb >/dev/null 2>&1 || die "$id: recipe $recipe produced no .deb for $a"
        for f in "$out"/*.deb; do
            p=$(deb_field "$f" Package)
            case "$allowed" in *" $p "*) ;; *) die "$id: recipe produced $p, not one of$allowed" ;; esac
            check_deb "$f" "$p" "$ver" "$a"
        done
        : > "$out/.done"
        rm -rf "$in"
        for f in "$out"/*.deb; do say "  built $(basename "$f") ($(fsize "$f" | awk '{ printf "%.1f MB", $1 / 1e6 }'))"; echo "$f"; done
    done
}

# ---------------------------------------------------------------------------------------------------
selected() {
    if [ $# -eq 0 ]; then j '.apps[].id'; return; fi
    for id in "$@"; do [ -n "$(jf "$id" .id)" ] || die "no app $id in the catalog"; echo "$id"; done
}

[ $# -ge 1 ] || usage
cmd=$1; shift
case "$cmd" in
    check)
        validate
        for id in $(selected "$@"); do
            case "$(jf "$id" .source.type)" in
                apt) do_apt "$id" check ;;
                *) say "  ok $id: $(jf "$id" .source.type) $(jf "$id" .source.version), pins valid" ;;
            esac
        done
        say "catalog OK"
        ;;
    fetch)
        validate
        for id in $(selected "$@"); do
            case "$(jf "$id" .source.type)" in
                apt) if [ "$(jf "$id" '.source.delivery // "mirror"')" = mirror ]; then do_apt "$id" fetch; else
                         say "  $id: delivery direct (installed from $(jf "$id" .source.url)), checking only"; do_apt "$id" check; fi ;;
                deb) do_deb "$id" ;;
                build|binary) do_recipe "$id" ;;
            esac
        done
        ;;
    list)
        jq -r '.apps[] | [.id, .package, .source.type, (.source.delivery // "mirror"), (.source.version // "(vendor index)")] | @tsv' "$CAT"
        ;;
    *) usage ;;
esac
