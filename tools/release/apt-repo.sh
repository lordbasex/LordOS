#!/bin/sh
# apt-repo.sh — the signed LordOS APT repository on lordos.org (docs/UPDATES.md, docs/RELEASING.md).
#
#   tools/release/apt-repo.sh build   <target> [--notes VERSION=SECTION]… [--rpi] [--catalog] <file.deb>…
#   tools/release/apt-repo.sh publish <target>
#   tools/release/apt-repo.sh verify  <target>
#   tools/release/apt-repo.sh rpi-fetch <target>     (only fetches and verifies the --rpi packages)
#
# <target> is "staging" (https://lordos.org/apt-staging, for tests) or "live" (https://lordos.org/apt,
# what every LordOS system reads). Both have one suite, "stable", component "main", for amd64 and arm64.
#
#   build    copies the published pool down from S3, adds the .debs, writes the indexes with Debian's
#            apt-ftparchive (in a debian:trixie container when this machine has none), the release notes
#            of each lordos version from CHANGELOG.md (main/notes/ReleaseNotes), and signs Release
#            with the LordOS archive key: InRelease and Release.gpg. Then it checks the signatures.
#   publish  uploads the pool first, then the indexes, the signed files last; the archive keyring to
#            lordos.org/keys/ and lordos.sources next to the repository; invalidates the CDN.
#   verify   downloads InRelease over https AND http (the Store's package engine has no TLS of its
#            own and reads https sources over http), checks the signature and every index hash.
#
# --rpi also adds the Raspberry Pi packages pinned in tools/release/rpi-mirror.list (rpi-eeprom and
# what it needs; arm64 only, under pool/rpi/), fetched from Raspberry Pi's archive and accepted only
# when Raspberry Pi's InRelease verifies with tools/release/raspberrypi-archive-keyring.pgp (its
# fingerprint checked), the index's SHA-256 matches InRelease, and the file's SHA-256 matches both
# the index and the pinned line. Verified files are kept in $WORKDIR/rpi-cache, so a pinned version
# can still be published after Raspberry Pi's archive has moved on (docs/UPDATES.md). Pinned source
# packages (MPL-2.0 kms++) go to pool/rpi/source/<letter>/<name>/, checked the same way against the
# signed Sources index, so their source is offered next to the binaries.
#
# --catalog also adds the curated apps of apt/catalog.json that LordOS mirrors (apt/README.md): it runs
# `apt/build.sh fetch`, which downloads every pinned vendor file, verifies it (pinned SHA-256, sigstore,
# a vendor's signed APT index) and builds LordOS's own packages from apt/recipes/ in debian:trixie
# containers (amd64 and arm64), keeping everything in $WORKDIR/catalog-cache. Those .debs go to
# pool/catalog/<letter>/<name>/ and into the indexes of their architectures, like pool/main. Apps whose
# vendor has its own signed APT repository ("delivery": "direct") are only checked, never mirrored: the
# user's system installs them from the vendor (docs/UPDATES.md, "Mirrored apps (catalog)").
#
# lordos-hwdata (0.8.7, docs/BENCHMARK.md "Newer snapshots between releases"): the signed tested-hardware
# snapshot, an Architecture: all package (tools/iso/mkdeb-hwdata.sh) that lordos depends on. It is added like any
# .deb, with a release or alone between releases (`build live lordos-hwdata_<date>_all.deb`, docs/RELEASING.md),
# and only after its snapshot's Ed25519 signature verifies with tools/release/lordos-release.pub.pem
# (LORDOS_HWDATA_PUB=<public.pem> names a throwaway test key, for the staging target only). A lordos that
# depends on it is refused while the pool has no lordos-hwdata.
#
# --notes VERSION=SECTION takes the notes of lordos VERSION from CHANGELOG.md section SECTION (for
# example 0.8.1~staging1=Unreleased). By default the section is "VERSION" or "VERSION-<suffix>".
#
# The private key lives ONLY on the maintainer's machine, next to the release key:
#   ~/.config/lordos-release/apt-gnupg   (a GnuPG home, mode 0700; LORDOS_APT_GNUPGHOME overrides)
# Never copy it to a VM, a server or the repository. The public key is
# packaging/keys/lordos-archive-keyring.gpg (shipped in the lordos package) and the script refuses to
# sign with any other key. Uploads use only the AWS profile "lordos" (tools/web/deploy.sh's).
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PROFILE=lordos
BUCKET=lordos.org
DISTRIBUTION=E35DXDFJ83X7Z8
SUITE=stable
ARCHES="amd64 arm64"
KEYRING=$ROOT/packaging/keys/lordos-archive-keyring.gpg
GNUPGHOME=${LORDOS_APT_GNUPGHOME:-$HOME/.config/lordos-release/apt-gnupg}
WORKDIR=${LORDOS_APT_DIR:-$HOME/.local/share/lordos-apt}
IMAGE=lordos-apt-tools:trixie
RPI_LIST=$ROOT/tools/release/rpi-mirror.list
RPI_KEYRING=$ROOT/tools/release/raspberrypi-archive-keyring.pgp
RPI_KEY_FPR=CF8A1AF502A2AA2D763BAE7E82B129927FA3303E
RPI_ARCHIVE=${RPI_ARCHIVE:-http://archive.raspberrypi.com/debian}
export GNUPGHOME

die() { echo "apt-repo.sh: $*" >&2; exit 1; }
usage() { sed -n '3,7p' "$0" >&2; exit 2; }
aws() { command aws --profile "$PROFILE" "$@"; }

[ $# -ge 2 ] || usage
cmd=$1; target=$2; shift 2
case "$target" in
    staging) PREFIX=apt-staging ;;
    live) PREFIX=apt ;;
    *) die "target must be staging or live, not $target" ;;
esac
TREE=$WORKDIR/$target
URL=https://$BUCKET/$PREFIX

# apt-ftparchive and dpkg-deb: this machine's (a Debian build host) or a debian:trixie container's
debtool() {
    if command -v apt-ftparchive >/dev/null 2>&1 && command -v dpkg-deb >/dev/null 2>&1; then
        (cd "$TREE" && "$@")
        return
    fi
    command -v docker >/dev/null 2>&1 || die "needs apt-ftparchive and dpkg-deb (apt-utils, dpkg) or docker"
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        printf 'FROM debian:trixie\nRUN apt-get update -q && apt-get install -y -q --no-install-recommends apt-utils dpkg-dev xz-utils && rm -rf /var/lib/apt/lists/*\n' |
            docker build -q -t "$IMAGE" - >/dev/null || die "could not build the $IMAGE container"
    fi
    docker run --rm -u "$(id -u):$(id -g)" -v "$TREE:/repo" -w /repo "$IMAGE" "$@"
}

# the fingerprint of the one key in packaging/keys/lordos-archive-keyring.gpg
expected_fpr() {
    t=$(mktemp -d)
    GNUPGHOME=$t gpg --batch --quiet --import "$KEYRING" 2>/dev/null
    GNUPGHOME=$t gpg --batch --with-colons --list-keys | awk -F: '/^fpr/ { print $10; exit }'
    rm -rf "$t"
}

check_key() {
    [ -d "$GNUPGHOME" ] || die "no signing key in $GNUPGHOME (see docs/UPDATES.md, \"The keys\")"
    FPR=$(expected_fpr)
    [ -n "$FPR" ] || die "cannot read $KEYRING"
    gpg --batch --with-colons --list-secret-keys "$FPR" >/dev/null 2>&1 \
        || die "the secret key for $FPR (packaging/keys/lordos-archive-keyring.gpg) is not in $GNUPGHOME"
}

# CHANGELOG.md section → deb822 "Notes" lines (one leading space, " ." for an empty line)
notes_for() {
    version=$1; section=$2
    if [ -z "$section" ]; then
        section=$(sed -n 's/^## \[\([^]]*\)\].*/\1/p' "$ROOT/CHANGELOG.md" |
            awk -v v="$version" '$0 == v || index($0, v "-") == 1 { print; exit }')
    fi
    [ -n "$section" ] || return 0
    # the release body without its fixed footer (downloads, verification and links)
    "$ROOT/tools/release/release-notes.sh" "$section" 2>/dev/null |
        awk '/^---$/ { cut = NR } { line[NR] = $0 } END { n = cut ? cut - 1 : NR; while (n > 0 && line[n] == "") n--; for (i = 1; i <= n; i++) print line[i] }' |
        sed -e 's/^$/./' -e 's/^/ /'
}

# the pinned Raspberry Pi packages (rpi-mirror.list), verified, into $WORKDIR/rpi-cache; prints the paths
rpi_import() {
    cache=$WORKDIR/rpi-cache
    mkdir -p "$cache"
    t=$(mktemp -d)
    fpr=$(GNUPGHOME=$t gpg --batch --with-colons --show-keys "$RPI_KEYRING" 2>/dev/null | awk -F: '/^fpr/ { print $10; exit }')
    [ "$fpr" = "$RPI_KEY_FPR" ] || { rm -rf "$t"; die "$RPI_KEYRING has fingerprint $fpr, expected $RPI_KEY_FPR"; }
    have_index=0
    grep -v '^#' "$RPI_LIST" > "$t/list"
    while read -r name ver arch sum _; do
        [ -n "$name" ] || continue
        [ "$arch" = source ] && continue            # rpi_src_import
        case "$name$ver$arch" in *[!A-Za-z0-9.+~:_-]*) rm -rf "$t"; die "rpi-mirror.list: bad line for $name" ;; esac
        printf %s "$sum" | grep -Eq '^[0-9a-f]{64}$' || { rm -rf "$t"; die "rpi-mirror.list: bad sha256 for $name"; }
        f="$cache/${name}_${ver#*:}_$arch.deb"
        if [ -f "$f" ] && [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$sum" ]; then echo "$f"; continue; fi
        if [ "$have_index" = 0 ]; then
            curl -fsS -o "$t/InRelease" "$RPI_ARCHIVE/dists/trixie/InRelease" || die "cannot download Raspberry Pi's InRelease"
            gpgv --keyring "$RPI_KEYRING" --output "$t/Release" "$t/InRelease" 2>/dev/null || die "Raspberry Pi's InRelease does not verify with $RPI_KEYRING"
            want=$(awk '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 } s && $3 == "main/binary-arm64/Packages.gz" { print $1 }' "$t/Release")
            [ -n "$want" ] || die "Raspberry Pi's Release lists no main/binary-arm64/Packages.gz"
            curl -fsS -o "$t/Packages.gz" "$RPI_ARCHIVE/dists/trixie/main/binary-arm64/Packages.gz" || die "cannot download Raspberry Pi's index"
            [ "$(shasum -a 256 "$t/Packages.gz" | cut -d' ' -f1)" = "$want" ] || die "Raspberry Pi's Packages.gz does not match its InRelease"
            gzip -dc "$t/Packages.gz" > "$t/Packages"
            have_index=1
        fi
        # the stanza of exactly that package, version and architecture
        line=$(awk -v n="$name" -v v="$ver" -v a="$arch" 'BEGIN { RS = "" } {
            pn = pv = pa = ps = pf = ""; k = split($0, L, "\n")
            for (i = 1; i <= k; i++) {
                if (L[i] ~ /^Package: /) pn = substr(L[i], 10); else if (L[i] ~ /^Version: /) pv = substr(L[i], 10)
                else if (L[i] ~ /^Architecture: /) pa = substr(L[i], 15); else if (L[i] ~ /^SHA256: /) ps = substr(L[i], 9)
                else if (L[i] ~ /^Filename: /) pf = substr(L[i], 11)
            }
            if (pn == n && pv == v && pa == a) print ps, pf }' "$t/Packages")
        [ -n "$line" ] || die "$name $ver ($arch) is no longer in Raspberry Pi's archive and not in $cache: keep the cache or pin the new version"
        isum=${line%% *}; path=${line#* }
        [ "$isum" = "$sum" ] || die "$name $ver: Raspberry Pi's index says $isum, rpi-mirror.list pins $sum"
        case "$path" in pool/main/*.deb) ;; *) die "$name: unexpected Filename $path" ;; esac
        curl -fsS -o "$f.part" "$RPI_ARCHIVE/$path" || die "cannot download $path"
        [ "$(shasum -a 256 "$f.part" | cut -d' ' -f1)" = "$sum" ] || { rm -f "$f.part"; die "$path: SHA-256 does not match"; }
        mv "$f.part" "$f"
        echo "$f"
    done < "$t/list"
    rm -rf "$t"
}

# the pinned Raspberry Pi source packages ("source" lines): the .dsc (pinned SHA-256, also in the
# signed Sources index) and every file the index lists for it, into $WORKDIR/rpi-cache/src/<name>/;
# prints those folders
rpi_src_import() {
    t=$(mktemp -d)
    fpr=$(GNUPGHOME=$t gpg --batch --with-colons --show-keys "$RPI_KEYRING" 2>/dev/null | awk -F: '/^fpr/ { print $10; exit }')
    [ "$fpr" = "$RPI_KEY_FPR" ] || { rm -rf "$t"; die "$RPI_KEYRING has fingerprint $fpr, expected $RPI_KEY_FPR"; }
    have_index=0
    grep -v '^#' "$RPI_LIST" | awk '$3 == "source"' > "$t/list"
    while read -r name ver arch sum _; do
        [ -n "$name" ] || continue
        case "$name$ver" in *[!A-Za-z0-9.+~:_-]*) rm -rf "$t"; die "rpi-mirror.list: bad line for $name" ;; esac
        printf %s "$sum" | grep -Eq '^[0-9a-f]{64}$' || { rm -rf "$t"; die "rpi-mirror.list: bad sha256 for $name"; }
        dir=$WORKDIR/rpi-cache/src/$name
        dsc="$dir/${name}_${ver#*:}.dsc"
        mkdir -p "$dir"
        if [ "$have_index" = 0 ]; then
            curl -fsS -o "$t/InRelease" "$RPI_ARCHIVE/dists/trixie/InRelease" || die "cannot download Raspberry Pi's InRelease"
            gpgv --keyring "$RPI_KEYRING" --output "$t/Release" "$t/InRelease" 2>/dev/null || die "Raspberry Pi's InRelease does not verify with $RPI_KEYRING"
            want=$(awk '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 } s && $3 == "main/source/Sources.gz" { print $1 }' "$t/Release")
            [ -n "$want" ] || die "Raspberry Pi's Release lists no main/source/Sources.gz"
            curl -fsS -o "$t/Sources.gz" "$RPI_ARCHIVE/dists/trixie/main/source/Sources.gz" || die "cannot download Raspberry Pi's Sources index"
            [ "$(shasum -a 256 "$t/Sources.gz" | cut -d' ' -f1)" = "$want" ] || die "Raspberry Pi's Sources.gz does not match its InRelease"
            gzip -dc "$t/Sources.gz" > "$t/Sources"
            have_index=1
        fi
        # "<directory>" then "<sha256> <file>" for each file of exactly that source package and version
        awk -v n="$name" -v v="$ver" 'BEGIN { RS = "" } {
            pn = pv = pd = ""; k = split($0, L, "\n"); c = 0
            for (i = 1; i <= k; i++) {
                if (L[i] ~ /^Package: /) pn = substr(L[i], 10); else if (L[i] ~ /^Version: /) pv = substr(L[i], 10)
                else if (L[i] ~ /^Directory: /) pd = substr(L[i], 12)
                else if (L[i] ~ /^Checksums-Sha256:/) c = 1
                else if (L[i] !~ /^ /) c = 0
                else if (c) { split(L[i], F, " "); f[++m] = F[1] " " F[3] }
            }
            if (pn == n && pv == v) { print pd; for (j = 1; j <= m; j++) print f[j] }
            m = 0 }' "$t/Sources" > "$t/files"
        [ -s "$t/files" ] || die "source $name $ver is no longer in Raspberry Pi's archive: keep $dir or pin the new version"
        path=$(head -n 1 "$t/files")
        case "$path" in pool/main/*) ;; *) die "$name: unexpected Directory $path" ;; esac
        grep -qx "$sum $(basename "$dsc")" "$t/files" || die "source $name $ver: Raspberry Pi's index does not list the pinned .dsc ($sum)"
        tail -n +2 "$t/files" > "$t/want"
        while read -r fsum file; do
            case "$file" in "$name"_*) ;; *) die "$name: unexpected file $file" ;; esac
            case "$file" in *[!A-Za-z0-9.+~_-]*) die "$name: unexpected file $file" ;; esac
            f="$dir/$file"
            if [ -f "$f" ] && [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$fsum" ]; then continue; fi
            curl -fsS -o "$f.part" "$RPI_ARCHIVE/$path/$file" || die "cannot download $path/$file"
            [ "$(shasum -a 256 "$f.part" | cut -d' ' -f1)" = "$fsum" ] || { rm -f "$f.part"; die "$path/$file: SHA-256 does not match"; }
            mv "$f.part" "$f"
        done < "$t/want"
        echo "$dir"
    done < "$t/list"
    rm -rf "$t"
}

# a lordos-hwdata package (in $TREE/incoming): Architecture all, and its snapshot signed with the release key
check_hwdata() {
    [ "$2" = all ] || die "$1: lordos-hwdata must be Architecture: all, not $2"
    pub=$ROOT/tools/release/lordos-release.pub.pem
    if [ -n "${LORDOS_HWDATA_PUB:-}" ]; then
        [ "$target" = staging ] || die "LORDOS_HWDATA_PUB (a test key) is only for the staging target"
        pub=$LORDOS_HWDATA_PUB
    fi
    command -v openssl >/dev/null 2>&1 || die "openssl is needed to check the snapshot of $1"
    t=$(mktemp -d)
    debtool dpkg-deb --fsys-tarfile "$1" | tar -x -C "$t" -f - || { rm -rf "$t"; die "cannot read $1"; }
    h=$t/usr/lib/lordos/assets/hwreport
    [ -f "$h/snapshot.json" ] && [ -f "$h/snapshot.json.sig" ] || { rm -rf "$t"; die "$1 has no signed snapshot (tools/release/sign-hwsnapshot.sh)"; }
    { base64 -d < "$h/snapshot.json.sig" > "$t/sig.bin" || base64 -D < "$h/snapshot.json.sig" > "$t/sig.bin"; } 2>/dev/null
    openssl pkeyutl -verify -pubin -inkey "$pub" -rawin -in "$h/snapshot.json" -sigfile "$t/sig.bin" >/dev/null 2>&1 ||
        { rm -rf "$t"; die "$1: the snapshot's signature does not verify with $pub"; }
    rm -rf "$t"
    echo "  $1: snapshot signature verified"
}

# is this package one of the mirrored Raspberry Pi packages? (they go to pool/rpi, arm64 index only)
is_rpi_pkg() { grep -v '^#' "$RPI_LIST" | awk -v n="$1" '$1 == n { f = 1 } END { exit !f }'; }

do_build() {
    NOTES_MAP=""; RPI=0; CATALOG=0; cats=
    while [ $# -gt 0 ]; do
        case "$1" in
            --notes) [ $# -ge 2 ] || die "--notes needs VERSION=SECTION"; NOTES_MAP="$NOTES_MAP $2"; shift ;;
            --notes=*) NOTES_MAP="$NOTES_MAP ${1#*=}" ;;
            --rpi) RPI=1 ;;
            --catalog) CATALOG=1 ;;
            -*) die "unknown option $1" ;;
            *) break ;;
        esac
        shift
    done
    check_key
    if [ "$RPI" = 1 ]; then
        rpis=$(rpi_import) || exit 1
        echo "  Raspberry Pi packages verified: $(echo "$rpis" | wc -l | tr -d ' ')"
        # shellcheck disable=SC2086
        set -- "$@" $rpis
    fi
    if [ "$CATALOG" = 1 ]; then
        cats=$(LORDOS_APT_DIR=$WORKDIR "$ROOT/apt/build.sh" fetch) || die "apt/build.sh fetch failed"
        echo "  catalog packages verified: $(echo "$cats" | grep -c .)"
        # shellcheck disable=SC2086
        set -- "$@" $cats
        cats=" $(echo $cats) "
    fi
    mkdir -p "$TREE/pool/main"
    # the pool is append-only: start from what is published (old versions stay installable)
    if [ -z "${LORDOS_APT_OFFLINE:-}" ]; then
        aws s3 sync "s3://$BUCKET/$PREFIX/pool" "$TREE/pool" --only-show-errors || die "could not read s3://$BUCKET/$PREFIX/pool"
    fi
    for deb in "$@"; do
        [ -f "$deb" ] || die "no such file: $deb"
        mkdir -p "$TREE/incoming"
        cp "$deb" "$TREE/incoming/"
        b=$(basename "$deb")
        name=$(debtool dpkg-deb -f "incoming/$b" Package)
        ver=$(debtool dpkg-deb -f "incoming/$b" Version)
        arch=$(debtool dpkg-deb -f "incoming/$b" Architecture)
        case "$name" in lib?*) dir=$(printf %s "$name" | cut -c1-4) ;; *) dir=$(printf %s "$name" | cut -c1) ;; esac
        comp=main
        is_rpi_pkg "$name" && comp=rpi
        case "$cats" in *" $deb "*) comp=catalog ;; esac
        [ "$name" = lordos-hwdata ] && check_hwdata "incoming/$b" "$arch"
        dest="pool/$comp/$dir/$name/${name}_${ver#*:}_$arch.deb"
        if [ -f "$TREE/$dest" ] && ! cmp -s "$TREE/incoming/$b" "$TREE/$dest"; then
            die "$dest is already published with different contents: a version is never replaced (bump it)"
        fi
        mkdir -p "$(dirname "$TREE/$dest")"
        mv "$TREE/incoming/$b" "$TREE/$dest"
        echo "  $dest"
    done
    rm -rf "$TREE/incoming"
    # every lordos that depends on lordos-hwdata needs one in the pool, or no system could install it
    if [ -z "$(find "$TREE/pool/main/l/lordos-hwdata" -name 'lordos-hwdata_*_all.deb' 2>/dev/null | head -n 1)" ]; then
        for d in $(find "$TREE/pool/main/l/lordos" -name 'lordos_*.deb' 2>/dev/null | sed "s#^$TREE/##"); do
            case "$(debtool dpkg-deb -f "$d" Depends)" in
                *lordos-hwdata*) die "$d depends on lordos-hwdata and the pool has none: add tools/iso/mkdeb-hwdata.sh's package" ;;
            esac
        done
    fi
    # the sources of the mirrored copyleft packages, next to them and unindexed (append-only too)
    if [ "$RPI" = 1 ]; then
        for d in $(rpi_src_import) ; do
            name=$(basename "$d")
            dest="pool/rpi/source/$(printf %s "$name" | cut -c1)/$name"
            mkdir -p "$TREE/$dest"
            for f in "$d"/*; do
                if [ -f "$TREE/$dest/${f##*/}" ] && ! cmp -s "$f" "$TREE/$dest/${f##*/}"; then
                    die "$dest/${f##*/} is already published with different contents"
                fi
                cp "$f" "$TREE/$dest/"
                echo "  $dest/${f##*/}"
            done
        done
    fi

    D=dists/$SUITE
    rm -rf "${TREE:?}/dists"
    for a in $ARCHES; do
        mkdir -p "$TREE/$D/main/binary-$a"
        debtool apt-ftparchive --arch "$a" packages pool/main > "$TREE/$D/main/binary-$a/Packages"
        # the Raspberry Pi packages (pool/rpi) only in the arm64 index: "all" packages would show up on PCs
        if [ "$a" = arm64 ] && [ -d "$TREE/pool/rpi" ]; then
            debtool apt-ftparchive --arch "$a" packages pool/rpi >> "$TREE/$D/main/binary-$a/Packages"
        fi
        # the mirrored catalog apps (pool/catalog), for each architecture like pool/main
        if [ -d "$TREE/pool/catalog" ]; then
            debtool apt-ftparchive --arch "$a" packages pool/catalog >> "$TREE/$D/main/binary-$a/Packages"
        fi
        xz -9 -k -f "$TREE/$D/main/binary-$a/Packages"
        gzip -9 -n -k -f "$TREE/$D/main/binary-$a/Packages"
        printf 'Archive: %s\nOrigin: LordOS\nLabel: LordOS\nComponent: main\nArchitecture: %s\n' "$SUITE" "$a" > "$TREE/$D/main/binary-$a/Release"
    done

    # release notes of every lordos version in the pool (Store → Updates shows the candidate's)
    mkdir -p "$TREE/$D/main/notes"
    : > "$TREE/$D/main/notes/ReleaseNotes"
    versions=$(awk '/^Package: lordos$/ { p = 1; next } /^Package:/ { p = 0 } p && /^Version:/ { print $2 }' "$TREE"/$D/main/binary-*/Packages | sort -u)
    for v in $versions; do
        section=""
        for m in $NOTES_MAP; do [ "${m%%=*}" = "$v" ] && section=${m#*=}; done
        body=$(notes_for "$v" "$section")
        [ -n "$body" ] || { echo "  (no CHANGELOG section for lordos $v: no release notes)"; continue; }
        printf 'Package: lordos\nVersion: %s\nNotes:\n%s\n\n' "$v" "$body" >> "$TREE/$D/main/notes/ReleaseNotes"
        echo "  notes for lordos $v"
    done
    xz -9 -k -f "$TREE/$D/main/notes/ReleaseNotes"

    # the Store's banner (Store → Discover): apt/featured/featured.json and apt/featured/banner.png,
    # edited in the repository and signed here with the indexes (the engine reads them only from this
    # repository, docs/UPDATES.md "The Store's banner")
    rm -rf "$TREE/$D/main/featured"
    if [ -f "$ROOT/apt/featured/featured.json" ]; then
        python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); assert isinstance(d,dict) and d.get("schema")==1' "$ROOT/apt/featured/featured.json" ||
            die "apt/featured/featured.json is not a schema 1 JSON object"
        [ "$(wc -c < "$ROOT/apt/featured/featured.json")" -le 65536 ] || die "apt/featured/featured.json is over 64 KB"
        mkdir -p "$TREE/$D/main/featured"
        cp "$ROOT/apt/featured/featured.json" "$TREE/$D/main/featured/Featured"
        if [ -f "$ROOT/apt/featured/banner.png" ]; then
            [ "$(head -c 8 "$ROOT/apt/featured/banner.png" | od -An -tx1 | tr -d ' \n')" = 89504e470d0a1a0a ] || die "apt/featured/banner.png is not a PNG"
            [ "$(wc -c < "$ROOT/apt/featured/banner.png")" -le 2097152 ] || die "apt/featured/banner.png is over 2 MB"
            cp "$ROOT/apt/featured/banner.png" "$TREE/$D/main/featured/Banner"
        fi
        echo "  featured banner: $(python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("catalog") or d.get("package") or "?")' "$ROOT/apt/featured/featured.json")"
    fi

    debtool apt-ftparchive \
        -o APT::FTPArchive::Release::Origin=LordOS \
        -o APT::FTPArchive::Release::Label=LordOS \
        -o APT::FTPArchive::Release::Suite="$SUITE" \
        -o APT::FTPArchive::Release::Codename="$SUITE" \
        -o APT::FTPArchive::Release::Architectures="$ARCHES" \
        -o APT::FTPArchive::Release::Components=main \
        -o APT::FTPArchive::Release::Description="LordOS updates for Debian 13 (trixie)" \
        -o APT::FTPArchive::Release::Patterns::=ReleaseNotes -o APT::FTPArchive::Release::Patterns::=ReleaseNotes.xz \
        -o APT::FTPArchive::Release::Patterns::=Featured -o APT::FTPArchive::Release::Patterns::=Banner \
        release "$D" > "$TREE/Release.new"
    mv "$TREE/Release.new" "$TREE/$D/Release"

    # sign on this machine (the key never leaves it): clear-signed InRelease and detached Release.gpg
    gpg --batch --yes --local-user "$FPR" --digest-algo SHA512 --clearsign -o "$TREE/$D/InRelease" "$TREE/$D/Release"
    gpg --batch --yes --local-user "$FPR" --digest-algo SHA512 --armor --detach-sign -o "$TREE/$D/Release.gpg" "$TREE/$D/Release"
    check_signatures "$TREE/$D/InRelease" "$TREE/$D/Release" "$TREE/$D/Release.gpg"
    echo "built $TREE ($(find "$TREE/pool" -name '*.deb' | wc -l | tr -d ' ') packages); publish with: $0 publish $target"
}

# the signatures against the public keyring that the lordos package ships (not the signing home)
check_signatures() {
    t=$(mktemp -d)
    GNUPGHOME=$t gpg --batch --quiet --import "$KEYRING" 2>/dev/null
    GNUPGHOME=$t gpg --batch --quiet --verify "$1" 2>/dev/null || { rm -rf "$t"; die "InRelease does not verify with $KEYRING"; }
    GNUPGHOME=$t gpg --batch --quiet --verify "$3" "$2" 2>/dev/null || { rm -rf "$t"; die "Release.gpg does not verify with $KEYRING"; }
    rm -rf "$t"
    echo "  signatures verified with packaging/keys/lordos-archive-keyring.gpg"
}

do_publish() {
    [ -f "$TREE/dists/$SUITE/InRelease" ] || die "nothing built in $TREE: run $0 build $target first"
    check_signatures "$TREE/dists/$SUITE/InRelease" "$TREE/dists/$SUITE/Release" "$TREE/dists/$SUITE/Release.gpg"
    # 1. packages (never change once published), 2. indexes, 3. the signed files that point at them
    aws s3 sync "$TREE/pool" "s3://$BUCKET/$PREFIX/pool" --cache-control "public, max-age=31536000, immutable" \
        --exclude "rpi/source/*" --content-type application/vnd.debian.binary-package --only-show-errors
    if [ -d "$TREE/pool/rpi/source" ]; then
        aws s3 sync "$TREE/pool/rpi/source" "s3://$BUCKET/$PREFIX/pool/rpi/source" --cache-control "public, max-age=31536000, immutable" \
            --exclude "*" --include "*.dsc" --content-type "text/plain; charset=utf-8" --only-show-errors
        aws s3 sync "$TREE/pool/rpi/source" "s3://$BUCKET/$PREFIX/pool/rpi/source" --cache-control "public, max-age=31536000, immutable" \
            --exclude "*.dsc" --content-type application/octet-stream --only-show-errors
    fi
    aws s3 sync "$TREE/dists" "s3://$BUCKET/$PREFIX/dists" --delete --cache-control "no-cache" \
        --exclude "*/InRelease" --exclude "*/Release" --exclude "*/Release.gpg" --only-show-errors
    for f in Release Release.gpg InRelease; do
        aws s3 cp "$TREE/dists/$SUITE/$f" "s3://$BUCKET/$PREFIX/dists/$SUITE/$f" --cache-control "no-cache" \
            --content-type "text/plain; charset=utf-8" --only-show-errors
    done
    # the sources file of this target (live: byte for byte the one the package installs)
    sed "s#^URIs: https://lordos.org/apt\$#URIs: $URL#" "$ROOT/packaging/apt/lordos.sources" > "$TREE/lordos.sources"
    aws s3 cp "$TREE/lordos.sources" "s3://$BUCKET/$PREFIX/lordos.sources" --cache-control "no-cache" \
        --content-type "text/plain; charset=utf-8" --only-show-errors
    aws s3 cp "$KEYRING" "s3://$BUCKET/keys/lordos-archive-keyring.gpg" --cache-control "no-cache" \
        --content-type application/pgp-keys --only-show-errors
    aws s3 cp "${KEYRING%.gpg}.asc" "s3://$BUCKET/keys/lordos-archive-keyring.asc" --cache-control "no-cache" \
        --content-type "text/plain; charset=utf-8" --only-show-errors
    id=$(aws cloudfront create-invalidation --distribution-id "$DISTRIBUTION" \
        --paths "/$PREFIX/dists/*" "/$PREFIX/lordos.sources" "/keys/*" --query Invalidation.Id --output text)
    echo "published $URL (invalidation $id); check it with: $0 verify $target"
}

do_verify() {
    t=$(mktemp -d)
    trap 'rm -rf "$t"' EXIT
    GNUPGHOME=$t gpg --batch --quiet --import "$KEYRING" 2>/dev/null
    for scheme in https http; do
        u=$scheme://$BUCKET/$PREFIX/dists/$SUITE
        code=$(curl -s -o "$t/InRelease.$scheme" -w '%{http_code}' "$u/InRelease")
        [ "$code" = 200 ] || die "$u/InRelease: HTTP $code (http must be served as is, not redirected: docs/UPDATES.md)"
        GNUPGHOME=$t gpg --batch --quiet --verify "$t/InRelease.$scheme" 2>/dev/null || die "$u/InRelease: bad signature"
        echo "  $u/InRelease: signature good"
    done
    cmp -s "$t/InRelease.https" "$t/InRelease.http" || die "http and https serve different InRelease files (CDN cache? try again in a minute)"
    GNUPGHOME=$t gpg --batch --quiet --decrypt "$t/InRelease.https" 2>/dev/null > "$t/Release"
    awk '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 } s { print $1, $3 }' "$t/Release" | while read -r sum path; do
        case "$path" in */Release) continue ;; esac
        curl -fsS -o "$t/f" "http://$BUCKET/$PREFIX/dists/$SUITE/$path" || die "missing $path"
        got=$(shasum -a 256 "$t/f" | cut -d' ' -f1)
        [ "$got" = "$sum" ] || die "$path: SHA-256 does not match InRelease"
    done
    echo "  every index matches InRelease"
    curl -fsS "http://$BUCKET/$PREFIX/dists/$SUITE/main/binary-amd64/Packages" | awk '/^Package:|^Version:|^Architecture:/' | paste - - - | sed 's/^/  /'
    curl -fsS "http://$BUCKET/$PREFIX/dists/$SUITE/main/binary-arm64/Packages" | awk '/^Package:|^Version:|^Architecture:/' | paste - - - | sed 's/^/  /'
    echo "verified $URL"
}

case "$cmd" in
    build) do_build "$@" ;;
    publish) do_publish ;;
    verify) do_verify ;;
    rpi-fetch) rpi_import; rpi_src_import ;;   # only fetch and verify the pinned Raspberry Pi packages and sources
    *) usage ;;
esac
