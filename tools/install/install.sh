#!/bin/sh
# LordOS installer for an existing Debian 13 (trixie) system.
#
#   curl -fsSL https://lordos.org/install.sh | sh
#   curl -fsSL https://lordos.org/install.sh | sh -s -- --uninstall
#   curl -fsSL https://lordos.org/install.sh | sh -s -- --remote-desktop    (servers and VPS)
#
# Canonical copy: tools/install/install.sh in https://github.com/lordbasex/LordOS.
# https://lordos.org/install.sh must stay byte-identical to the file in the release tag.
# Documentation: docs/INSTALL-DEBIAN.md. No telemetry: the only network requests are to
# api.github.com (to find the latest release), github.com (the release files) and the
# Debian mirrors that apt already uses. The package then adds the signed LordOS repository
# (lordos.org/apt), so later versions arrive through Store → Updates or apt (docs/UPDATES.md).
# Running it again on an installed LordOS (from the ISO, the Raspberry Pi image or this script)
# upgrades it in place: files, settings and the login setup are kept.
#
# Everything lives inside main(), called on the last line, so a truncated download runs nothing.
set -eu

INSTALLER_VERSION=1
REPO="lordbasex/LordOS"
API_URL="https://api.github.com/repos/$REPO"
DL_URL="https://github.com/$REPO/releases/download"
STATE_DIR=/var/lib/lordos-installer
STATE_FILE=$STATE_DIR/state
SESSION_CONF=/etc/lordos/session.conf
GREETER_USER=lordos-greeter
# groups of the live ISO's user (tools/iso/build.sh), added only when they exist; sudo only through
# ADMIN_GROUP, when the system would otherwise have no administrator (pick_admin)
USER_GROUPS="audio video input render lpadmin pipewire plugdev"
ADMIN_GROUP=""
# what the ISO adds beyond the package's Depends/Recommends for sound, fonts and the session bus
EXTRA_PKGS="sudo dbus-user-session pipewire pipewire-pulse pipewire-alsa wireplumber rtkit alsa-ucm-conf
 fonts-noto-core fonts-noto-color-emoji fonts-dejavu-core libthai-data shared-mime-info"
# locales the interface ships translations for (en is the default); only generated, never selected
LOCALES="en_US.UTF-8 es_ES.UTF-8 pt_BR.UTF-8"
KNOWN_DMS="gdm3 gdm sddm lightdm lxdm xdm slim nodm greetd ly"
# remote desktop (--remote-desktop): the file Settings > Sharing and the Raspberry Pi switch file
# write (server/src/remote/remote_admin.c, tools/rpi/rootfs/usr/lib/lordos/rpi/firstboot)
REMOTE_DIR=/etc/lordos/remote
REMOTE_CONF=$REMOTE_DIR/config
REMOTE_DEFAULT_PORT=5673

# Ed25519 public key that signs SHA256SUMS (SHA256SUMS.sig = base64 of the 64-byte signature).
# Fingerprint (SHA-256 of the DER key): c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e
# See docs/SIGNING.md, "Release files". A missing or invalid signature stops the install.
RELEASE_PUBKEY_PEM="-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEA8cqwDzH+ZVMdBqNjC3141dmo3gcH8PWt88I+RE9dOaA=
-----END PUBLIC KEY-----"

# ---------------------------------------------------------------- output

setup_colors() {
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
        C_B=$(printf '\033[1m'); C_R=$(printf '\033[31m'); C_G=$(printf '\033[32m')
        C_Y=$(printf '\033[33m'); C_C=$(printf '\033[36m'); C_0=$(printf '\033[0m')
    else
        C_B=""; C_R=""; C_G=""; C_Y=""; C_C=""; C_0=""
    fi
}
say()  { printf '%s\n' "$*"; }
step() { printf '%s==>%s %s%s%s\n' "$C_C" "$C_0" "$C_B" "$*" "$C_0"; }
ok()   { printf '  %sok%s  %s\n' "$C_G" "$C_0" "$*"; }
warn() { printf '%swarning:%s %s\n' "$C_Y" "$C_0" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$C_R" "$C_0" "$*" >&2; exit 1; }

CHANGES=""
changed() { CHANGES="$CHANGES
  - $*"; }

usage() {
    cat <<EOF
LordOS installer for Debian 13 (trixie), version $INSTALLER_VERSION

Usage:
  curl -fsSL https://lordos.org/install.sh | sh
  curl -fsSL https://lordos.org/install.sh | sh -s -- [options]
  sh install.sh [options]

Options:
  -y, --yes                 do not ask for confirmation (also LORDOS_YES=1)
  --version X               install release X (for example 0.8.0-beta); default: the latest
  --deb FILE                install a local lordos_<version>_<arch>.deb instead of downloading;
                            a SHA256SUMS file next to it is verified when present
  --user NAME               the user to set up (default: the user who ran sudo)
  --keep-display-manager    do not disable the current display manager (LordOS is not
                            enabled at boot then; see the final message)
  --no-extras               do not install the extra sound/font packages the ISO has
  --boot-splash             also set up the LordOS boot splash (Plymouth) without asking;
                            without it, it is only offered (and skipped with --yes)
  --no-boot-splash          do not offer the boot splash
  --no-systemd-check        continue without a running systemd (containers, chroots):
                            the systemd steps are skipped
  --remote-desktop          also turn on LordOS remote desktop (servers and VPS without a
                            screen): https://<this machine's IP>:5673 in a browser, TLS and a
                            user's password; asks first unless --yes
  --remote-port N           with --remote-desktop: the port, 1024-65535 (default 5673)
  --uninstall               remove LordOS and restore the previous login setup (also
                            removes the remote desktop configuration)
  --purge                   with --uninstall: purge the package and its /etc files
  -h, --help                this help
EOF
}

# ---------------------------------------------------------------- helpers

as_root() {
    if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

# yes/no question read from the terminal, so it works under curl | sh; $2 is the --yes answer
ask() {
    if [ "$ASSUME_YES" = 1 ]; then return "${2:-0}"; fi
    # a subshell: a failed redirection on ":" would end the whole script
    if ! (: </dev/tty) 2>/dev/null; then
        die "no terminal to ask \"$1\"; run again with --yes (or LORDOS_YES=1)"
    fi
    printf '%s%s%s [y/N] ' "$C_B" "$1" "$C_0" >/dev/tty
    read -r answer </dev/tty || answer=""
    case "$answer" in y|Y|yes|YES|Yes) return 0 ;; *) return 1 ;; esac
}

os_field() {
    sed -n "s/^$1=//p" /etc/os-release 2>/dev/null | head -n 1 | sed 's/^"//; s/"$//'
}

in_container() {
    if command -v systemd-detect-virt >/dev/null 2>&1 && systemd-detect-virt -cq 2>/dev/null; then return 0; fi
    [ -f /.dockerenv ] || [ -f /run/.containerenv ]
}

user_in_group() { id -nG "$1" 2>/dev/null | tr ' ' '\n' | grep -qx "$2"; }
regular_users() { getent passwd | awk -F: '$3 >= 1000 && $3 < 60000 && $7 !~ /(nologin|false)$/ { print $1 }'; }
has_admin_user() {
    for g in sudo admin wheel; do
        getent group "$g" 2>/dev/null | cut -d: -f4 | tr ',' '\n' | grep -qvxE 'root|' && return 0
    done
    return 1
}

state_get() {
    [ -r "$STATE_FILE" ] || return 0
    sed -n "s/^$1=//p" "$STATE_FILE" | tail -n 1
}

state_set() {
    as_root mkdir -p "$STATE_DIR"
    if [ -f "$STATE_FILE" ]; then
        as_root sed -i "/^$1=/d" "$STATE_FILE"
    fi
    printf '%s=%s\n' "$1" "$2" | as_root tee -a "$STATE_FILE" >/dev/null
}

has_systemd() { [ "$NO_SYSTEMD" = 0 ]; }

# the unit behind display-manager.service (gdm3.service, sddm.service, ...), or nothing
current_dm() {
    link=""
    if [ -L /etc/systemd/system/display-manager.service ]; then
        link=$(readlink /etc/systemd/system/display-manager.service)
    fi
    if [ -z "$link" ] && command -v systemctl >/dev/null 2>&1; then
        link=$(systemctl show -p Id --value display-manager.service 2>/dev/null || true)
        [ "$link" = display-manager.service ] && link=""
    fi
    [ -n "$link" ] && basename "$link"
    return 0
}

unit_exists() {
    # sh has no local variables: a name of its own so callers' loop variables survive
    for _ue_dir in /etc/systemd/system /usr/lib/systemd/system /lib/systemd/system; do
        [ -e "$_ue_dir/$1" ] && return 0
    done
    return 1
}

fetch() {
    # $1 url, $2 output file; https only unless LORDOS_BASE_URL points somewhere else (tests)
    if [ -n "${LORDOS_BASE_URL:-}" ]; then
        curl -fsSL --retry 3 -o "$2" "$1"
    else
        curl -fsSL --proto '=https' --tlsv1.2 --retry 3 -o "$2" "$1"
    fi
}

# ---------------------------------------------------------------- checks

check_os() {
    [ -r /etc/os-release ] || die "cannot read /etc/os-release: this does not look like Debian 13"
    os_id=$(os_field ID); like=$(os_field ID_LIKE); code=$(os_field VERSION_CODENAME)
    dcode=$(os_field DEBIAN_CODENAME); name=$(os_field PRETTY_NAME)
    if [ "$os_id" = debian ] && [ "$code" = trixie ]; then
        ok "system: $name"
        return
    fi
    case " $like " in
        *" debian "*)
            if [ "$code" = trixie ] || [ "$dcode" = trixie ]; then
                warn "$name is a Debian derivative, not Debian itself. LordOS is tested on Debian 13 (trixie) only; continuing."
                return
            fi ;;
    esac
    die "LordOS needs Debian 13 (trixie); this system is ${name:-unknown} (ID=$os_id, VERSION_CODENAME=${code:-none})."
}

check_arch() {
    command -v dpkg >/dev/null 2>&1 || die "dpkg is missing: this is not a Debian system"
    ARCH=$(dpkg --print-architecture)
    case "$ARCH" in
        amd64|arm64) ok "architecture: $ARCH" ;;
        *) die "LordOS is built for amd64 and arm64 only; this system is $ARCH." ;;
    esac
    model=""
    if [ -r /proc/device-tree/model ]; then model=$(tr -d '\000' </proc/device-tree/model); fi
    case "$model" in
        *"Raspberry Pi 5"*)
            warn "This is a $model. The LordOS Raspberry Pi image is the recommended way to run it there (it brings the tested kernel and boot configuration); continuing with the package." ;;
    esac
}

check_root() {
    if [ "$(id -u)" -eq 0 ]; then
        ok "running as root"
        return
    fi
    command -v sudo >/dev/null 2>&1 || die "run this as root, or install sudo and add your user to the sudo group"
    say "  Some steps need administrator rights; sudo may ask for your password."
    sudo true || die "sudo did not grant administrator rights"
    ok "administrator rights through sudo"
}

check_disk() {
    free_kb=$(df -Pk /var 2>/dev/null | awk 'NR == 2 { print $4 }')
    [ -n "$free_kb" ] || { warn "could not measure the free disk space"; return; }
    if [ "$free_kb" -lt 2097152 ]; then
        die "LordOS needs at least 2 GB free on /var (for the package and its dependencies); there are $((free_kb / 1024)) MB."
    fi
    ok "free disk space: $((free_kb / 1024)) MB"
}

check_systemd() {
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        ok "systemd is running"
        return
    fi
    if [ "$SKIP_SYSTEMD_CHECK" = 1 ] || in_container; then
        NO_SYSTEMD=1
        warn "systemd is not running (container or chroot): services, the display manager, getty and linger will NOT be configured."
        return
    fi
    die "LordOS needs systemd as the init system (/run/systemd/system is missing). Use --no-systemd-check only in containers or chroots."
}

pick_user() {
    if [ -z "$TARGET_USER" ]; then
        if [ "$(id -u)" -ne 0 ]; then TARGET_USER=$(id -un)
        elif [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then TARGET_USER=$SUDO_USER
        else
            # plain root, as after su or on a Debian installed with a root password: who logged in,
            # or the only regular user there is
            who=$(logname 2>/dev/null || true)
            if [ -n "$who" ] && [ "$who" != root ] && getent passwd "$who" >/dev/null 2>&1; then TARGET_USER=$who
            elif [ "$(regular_users | wc -l)" -eq 1 ]; then TARGET_USER=$(regular_users)
            fi
        fi
    fi
    if [ -n "$TARGET_USER" ]; then
        getent passwd "$TARGET_USER" >/dev/null 2>&1 || die "user $TARGET_USER does not exist"
        ok "user to set up: $TARGET_USER"
    else
        warn "running as root without sudo: no user's groups will be changed (use --user NAME)."
    fi
}

# Installing and updating need an administrator (a member of sudo, admin or wheel: is_admin in
# server/src/admin.c). A Debian installed with a root password has none, and LordOS has no way to
# name one from inside, so without this nobody could use the Store.
pick_admin() {
    has_admin_user && return 0
    if [ -z "$TARGET_USER" ]; then
        warn "this system has no administrator: the Store can't install or update anything until one exists (run again with --user NAME)."
        return 0
    fi
    getent group sudo >/dev/null 2>&1 || { warn "there is no sudo group: $TARGET_USER can't be made an administrator."; return 0; }
    ADMIN_GROUP=sudo
}

# ---------------------------------------------------------------- download and verification

cleanup_tmp() {
    rm -rf "$TMP"
    if [ -n "$ROOT_TMP" ]; then
        if [ "$(id -u)" -eq 0 ]; then rm -rf "$ROOT_TMP"; else sudo -n rm -rf "$ROOT_TMP" 2>/dev/null || true; fi
    fi
}

resolve_tag() {
    if [ -n "$WANT_VERSION" ]; then
        TAG="v${WANT_VERSION#v}"
    else
        TAG=""
        out="$TMP/release.json"
        # releases/latest skips pre-releases (the betas): fall back to the newest release of any kind
        if fetch "$API_URL/releases/latest" "$out" 2>/dev/null; then
            TAG=$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "$out" | head -n 1)
        fi
        if [ -z "$TAG" ] && fetch "$API_URL/releases?per_page=1" "$out" 2>/dev/null; then
            TAG=$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "$out" | head -n 1)
        fi
        [ -n "$TAG" ] || die "could not find the latest LordOS release on GitHub ($API_URL). Check the connection, or use --version X or --deb FILE."
    fi
    case "$TAG" in *[!A-Za-z0-9._-]*|"") die "invalid release name: $TAG" ;; esac
}

sha256_of() { sha256sum "$1" | awk '{ print $1 }'; }

# the expected hash of file $2 in SHA256SUMS $1 (accepts "hash  name" and "hash *name")
sum_for() {
    awk -v f="$2" '{ n = $2; sub(/^\*/, "", n); if (n == f) { print $1; exit } }' "$1"
}

verify_signature() {
    # $1 SHA256SUMS, $2 SHA256SUMS.sig (may be missing)
    pem=$RELEASE_PUBKEY_PEM
    if [ -n "${LORDOS_PUBKEY_FILE:-}" ]; then
        # testing only: lets the signature path be exercised with a throwaway key
        pem=$(cat "$LORDOS_PUBKEY_FILE")
        warn "using the release key from LORDOS_PUBKEY_FILE (testing)"
    fi
    # never skip the check: an empty or non-PEM key (e.g. a test override) is an error
    case "$pem" in
        "-----BEGIN PUBLIC KEY-----"*"-----END PUBLIC KEY-----"*) ;;
        *) die "the release-signing key is missing or not a PEM public key: refusing to install" ;;
    esac
    [ -s "$2" ] || die "the release has no SHA256SUMS.sig signature: refusing to install"
    if ! command -v openssl >/dev/null 2>&1; then
        say "  Installing openssl to check the signature..."
        as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -q openssl </dev/null >/dev/null \
            || die "openssl is needed to check the release signature"
    fi
    printf '%s\n' "$pem" > "$TMP/release.pub"
    base64 -d < "$2" > "$TMP/sums.sig.bin" 2>/dev/null || die "SHA256SUMS.sig is not valid base64"
    if openssl pkeyutl -verify -pubin -inkey "$TMP/release.pub" -rawin \
        -in "$1" -sigfile "$TMP/sums.sig.bin" >/dev/null 2>&1; then
        SIG_NOTE="valid Ed25519 signature of SHA256SUMS"
        ok "signature: $SIG_NOTE"
    else
        die "the SHA256SUMS signature is NOT valid: the release files may have been tampered with. Nothing was installed."
    fi
}

check_deb_file() {
    pkg=$(dpkg-deb -f "$1" Package 2>/dev/null || true)
    [ "$pkg" = lordos ] || die "$1 is not the LordOS package (Package: ${pkg:-unreadable})"
    darch=$(dpkg-deb -f "$1" Architecture 2>/dev/null || true)
    [ "$darch" = "$ARCH" ] || die "$(basename "$1") is built for $darch, but this system is $ARCH"
    DEB_VERSION=$(dpkg-deb -f "$1" Version)
}

get_package() {
    SIG_NOTE=""
    EXPECT_SHA=""
    # apt reads the file as the _apt user: a world-readable copy in our private-but-traversable dir
    chmod 755 "$TMP"
    if [ -n "$LOCAL_DEB" ]; then
        [ -f "$LOCAL_DEB" ] || die "no such file: $LOCAL_DEB"
        base=$(basename "$LOCAL_DEB")
        sums="$(dirname "$LOCAL_DEB")/SHA256SUMS"
        cp "$LOCAL_DEB" "$TMP/$base"
        DEB_PATH="$TMP/$base"
        if [ -f "$sums" ]; then
            want=$(sum_for "$sums" "$base")
            [ -n "$want" ] || die "$sums has no entry for $base"
            [ "$(sha256_of "$DEB_PATH")" = "$want" ] || die "SHA-256 mismatch for $base: the file is corrupted or not the released one"
            EXPECT_SHA=$want
            ok "SHA-256 matches $sums"
            verify_signature "$sums" "$sums.sig"
        else
            SIG_NOTE="not checked: local file without a SHA256SUMS next to it"
            warn "no SHA256SUMS next to $LOCAL_DEB: the checksum was not verified (you provided this file)."
        fi
    else
        command -v curl >/dev/null 2>&1 || die "curl is missing (apt-get install curl)"
        resolve_tag
        base_url="${LORDOS_BASE_URL:-$DL_URL/$TAG}"
        say "  Release $TAG from $base_url"
        fetch "$base_url/SHA256SUMS" "$TMP/SHA256SUMS" \
            || die "could not download SHA256SUMS of release $TAG (does the release exist? is the repository public yet?)"
        fetch "$base_url/SHA256SUMS.sig" "$TMP/SHA256SUMS.sig" 2>/dev/null || rm -f "$TMP/SHA256SUMS.sig"
        base=$(awk '{ n = $2; sub(/^\*/, "", n); print n }' "$TMP/SHA256SUMS" \
            | grep -E "^lordos_[A-Za-z0-9.+~-]+_${ARCH}\.deb\$" | head -n 1 || true)
        [ -n "$base" ] || die "release $TAG has no LordOS package for $ARCH"
        # check the signature before trusting anything else the file says
        verify_signature "$TMP/SHA256SUMS" "$TMP/SHA256SUMS.sig"
        say "  Downloading $base..."
        fetch "$base_url/$base" "$TMP/$base" || die "could not download $base"
        DEB_PATH="$TMP/$base"
        EXPECT_SHA=$(sum_for "$TMP/SHA256SUMS" "$base")
        [ "$(sha256_of "$DEB_PATH")" = "$EXPECT_SHA" ] \
            || die "SHA-256 mismatch for $base: the download is corrupted. Nothing was installed."
        ok "SHA-256 of $base matches SHA256SUMS"
    fi
    chmod 644 "$DEB_PATH"
    # Everything above ran on a file this user owns: any other process of the user could swap
    # it between the checks and apt (root would then run its maintainer scripts). Give root its
    # own copy in a root-owned folder and check that copy again; apt installs only that one.
    if [ "$(id -u)" -ne 0 ]; then
        ROOT_TMP=$(as_root mktemp -d) || die "could not create a temporary folder as root"
        as_root chmod 755 "$ROOT_TMP" || die "could not prepare $ROOT_TMP"
        as_root install -m 644 "$DEB_PATH" "$ROOT_TMP/$base" || die "could not copy $base for installation"
        DEB_PATH="$ROOT_TMP/$base"
    fi
    if [ -n "$EXPECT_SHA" ]; then
        [ "$(sha256_of "$DEB_PATH")" = "$EXPECT_SHA" ] \
            || die "SHA-256 mismatch for $base after copying it: refusing to install"
    fi
    check_deb_file "$DEB_PATH"
    ok "package: lordos $DEB_VERSION ($ARCH)"
}

# ---------------------------------------------------------------- install

apt_install() {
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@" </dev/null
}

install_package() {
    step "Installing the package and its dependencies from Debian"
    as_root env DEBIAN_FRONTEND=noninteractive apt-get update -q </dev/null \
        || warn "apt-get update failed; trying with the package lists already on the system"
    extras=""
    if [ "$NO_EXTRAS" = 0 ]; then
        for p in $EXTRA_PKGS; do
            if apt-cache show "$p" >/dev/null 2>&1; then extras="$extras $p"; fi
        done
    fi
    was=$(dpkg-query -W -f '${Version}' lordos 2>/dev/null || true)
    # an old (validly signed) release served as "latest" must not downgrade a newer install
    if [ -n "$was" ] && [ -z "$WANT_VERSION" ] && [ -z "$LOCAL_DEB" ] \
        && dpkg --compare-versions "$DEB_VERSION" lt "$was"; then
        die "the release offered ($DEB_VERSION) is older than the installed LordOS ($was): refusing to downgrade (use --version X to choose a version)"
    fi
    # shellcheck disable=SC2086 # $extras is a list of package names
    apt_install "$DEB_PATH" $extras || die "apt-get could not install LordOS (see the messages above). Nothing else was changed."
    if [ -n "$was" ]; then changed "package lordos $was replaced by $DEB_VERSION"
    else changed "package lordos $DEB_VERSION installed (with its Debian dependencies)"; fi
    if [ -n "$extras" ]; then changed "extra packages ensured:$extras"; fi
    state_set INSTALLED_VERSION "$DEB_VERSION"
}

setup_greeter_user() {
    if getent passwd "$GREETER_USER" >/dev/null 2>&1; then
        ok "system user $GREETER_USER already exists"
        return
    fi
    as_root useradd --system --home-dir /var/lib/lordos-greeter --create-home \
        --shell /usr/sbin/nologin --comment LordOS "$GREETER_USER"
    state_set GREETER_CREATED 1
    changed "system user $GREETER_USER created (the login screen's user)"
}

setup_groups() {
    [ -n "$TARGET_USER" ] || return 0
    added=""
    for g in $USER_GROUPS $ADMIN_GROUP; do
        getent group "$g" >/dev/null 2>&1 || continue
        user_in_group "$TARGET_USER" "$g" && continue
        as_root usermod -aG "$g" "$TARGET_USER"
        added="$added $g"
    done
    if [ -n "$added" ]; then
        prev=$(state_get GROUPS_ADDED)
        state_set GROUPS_USER "$TARGET_USER"
        state_set GROUPS_ADDED "$(printf '%s %s' "$prev" "$added" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ' | sed 's/ $//')"
        changed "user $TARGET_USER added to:$added (takes effect after a reboot: the user's services keep their old groups across a logout)"
    else
        ok "user $TARGET_USER already has the groups LordOS uses"
    fi
}

setup_locales() {
    if [ ! -f /etc/locale.gen ] || ! command -v locale-gen >/dev/null 2>&1; then
        warn "the locales package is missing: the interface works, but only the C locale is generated"
        return 0
    fi
    new=""
    for loc in $LOCALES; do
        if grep -qE "^[[:space:]]*${loc}[[:space:]]+UTF-8" /etc/locale.gen; then continue; fi
        if grep -qE "^#[[:space:]]*${loc}[[:space:]]+UTF-8" /etc/locale.gen; then
            as_root sed -i -E "s/^#[[:space:]]*(${loc}[[:space:]]+UTF-8)/\\1/" /etc/locale.gen
        else
            printf '%s UTF-8\n' "$loc" | as_root tee -a /etc/locale.gen >/dev/null
        fi
        new="$new $loc"
    done
    if [ -n "$new" ]; then
        as_root locale-gen </dev/null >/dev/null
        changed "locales generated:$new (the system language was not changed)"
    fi
}

setup_session_conf() {
    if [ -e "$SESSION_CONF" ]; then
        ok "$SESSION_CONF already exists (kept)"
        return
    fi
    as_root mkdir -p /etc/lordos
    printf '# LordOS: empty AUTOLOGIN shows the login screen. Settings > Users changes it.\nAUTOLOGIN=\n' \
        | as_root tee "$SESSION_CONF" >/dev/null
    state_set SESSION_CONF_CREATED 1
    changed "$SESSION_CONF written: the login screen, no automatic login"
}

setup_systemd() {
    if ! has_systemd; then
        warn "skipped without systemd: enabling lordos.service, lord-netperm and lord-fand, the display manager, getty@tty1 and linger"
        return
    fi
    step "Configuring the system services"
    as_root systemctl daemon-reload
    # user PipeWire (Debian's packages enable it already; just in case, like the ISO)
    as_root systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service >/dev/null 2>&1 || true
    as_root systemctl enable lord-netperm.service lord-fand.service >/dev/null 2>&1 \
        || warn "could not enable lord-netperm/lord-fand"
    changed "lord-netperm (per-app network permissions) and lord-fand (fans) enabled"

    DM_SWITCHED=0
    if [ -n "$DM_UNIT" ] && [ "$DM_UNIT" != lordos.service ]; then
        if [ "$KEEP_DM" = 1 ]; then
            warn "keeping $DM_UNIT as the display manager (--keep-display-manager): LordOS is not enabled at boot"
        elif ask "Disable $DM_UNIT so LordOS shows its own login screen at boot?" 0; then
            as_root systemctl disable "$DM_UNIT" >/dev/null 2>&1 || die "could not disable $DM_UNIT"
            state_set PREV_DM "$DM_UNIT"
            changed "display manager $DM_UNIT disabled (still installed; --uninstall turns it back on)"
            DM_SWITCHED=1
        else
            KEEP_DM=1
            warn "kept $DM_UNIT: LordOS is installed but not enabled at boot"
        fi
    fi
    if [ "$KEEP_DM" = 1 ] && [ -n "$DM_UNIT" ]; then
        return
    fi
    if [ "$(systemctl is-enabled lordos.service 2>/dev/null || true)" = enabled ]; then
        ok "lordos.service already enabled"
    else
        as_root systemctl enable lordos.service >/dev/null 2>&1 || die "could not enable lordos.service"
        changed "lordos.service enabled: LordOS starts on tty1 at boot"
    fi
    if [ "$(systemctl is-enabled getty@tty1.service 2>/dev/null || true)" != masked ]; then
        as_root systemctl mask getty@tty1.service >/dev/null 2>&1
        state_set GETTY_MASKED 1
        changed "getty@tty1 masked (text logins remain on tty2-tty6: Ctrl+Alt+F2)"
    fi
    if [ -n "$TARGET_USER" ] && [ ! -e "/var/lib/systemd/linger/$TARGET_USER" ] && command -v loginctl >/dev/null 2>&1; then
        if as_root loginctl enable-linger "$TARGET_USER" 2>/dev/null; then
            state_set LINGER_USER "$TARGET_USER"
            changed "linger enabled for $TARGET_USER (their PipeWire starts with the system, like on the ISO)"
        fi
    fi
}

# ---------------------------------------------------------------- boot splash (optional)

GRUB_DROPIN=/etc/default/grub.d/lordos.cfg
# themes a stock Debian may have selected; anything else is someone's choice and is asked about
STOCK_PLYMOUTH_THEMES="ceratopsian emerald futureprototype homeworld joy lines moonlight softwaves spacefun bgrt spinner text details tribar script fade-in glow solar spinfinity"

# the effective GRUB settings (/etc/default/grub plus its grub.d drop-ins), as "cmdline|theme|terminal"
grub_settings() {
    (
        GRUB_CMDLINE_LINUX_DEFAULT=""; GRUB_THEME=""; GRUB_TERMINAL=""; GRUB_TERMINAL_OUTPUT=""
        # shellcheck disable=SC1091,SC1090
        [ -r /etc/default/grub ] && . /etc/default/grub
        for f in /etc/default/grub.d/*.cfg; do
            # shellcheck disable=SC1090
            [ -r "$f" ] && [ "$f" != "$GRUB_DROPIN" ] && . "$f"
        done
        printf '%s|%s|%s\n' "$GRUB_CMDLINE_LINUX_DEFAULT" "$GRUB_THEME" "${GRUB_TERMINAL_OUTPUT:-$GRUB_TERMINAL}"
    ) 2>/dev/null
}

# The LordOS boot splash: Plymouth with the lordos theme (it ships in the package) and, when this
# system boots with an unmodified GRUB, "splash" on the kernel command line and the LordOS boot menu,
# through a drop-in of our own that --uninstall removes. A custom bootloader setup is never edited:
# then only the theme is set and the final message says what to add. See docs/INSTALL-DEBIAN.md.
setup_splash() {
    [ "$BOOT_SPLASH" = no ] && return 0
    [ -f /usr/share/plymouth/themes/lordos/lordos.plymouth ] || return 0
    if [ "$BOOT_SPLASH" != yes ]; then
        ask "Also show the LordOS boot splash (logo and progress bar) at boot? It installs Plymouth" 1 || {
            ok "boot splash: not set up (run again with --boot-splash to add it)"; return 0; }
    fi
    step "Setting up the boot splash"
    apt_install plymouth plymouth-label || { warn "could not install Plymouth; the boot splash was skipped"; return 0; }
    prev=$(plymouth-set-default-theme 2>/dev/null || true)
    set_theme=1
    # shellcheck disable=SC2086 # $STOCK_PLYMOUTH_THEMES: one theme name per line
    if [ "$prev" = lordos ]; then
        set_theme=0
    elif [ -n "$prev" ] && ! printf '%s\n' $STOCK_PLYMOUTH_THEMES | grep -qx "$prev"; then
        ask "Your boot splash theme is \"$prev\". Replace it with LordOS's?" 1 || set_theme=0
    fi
    if [ "$set_theme" = 1 ]; then
        state_set PREV_PLYMOUTH_THEME "${prev:-none}"
        if as_root plymouth-set-default-theme -R lordos >/dev/null 2>&1; then
            changed "boot splash theme set to lordos (was ${prev:-the default}; initramfs rebuilt)"
        else
            warn "plymouth-set-default-theme failed; the boot splash theme was not changed"
        fi
    fi
    # the bootloader: only a GRUB that nobody customised for these settings
    if ! command -v update-grub >/dev/null 2>&1 || [ ! -f /boot/grub/grub.cfg ]; then
        SPLASH_NOTE="this system does not boot with GRUB here: add \"quiet splash\" to the kernel command line to see the splash"
        return 0
    fi
    IFS='|' read -r g_cmd g_theme g_term <<EOF
$(grub_settings)
EOF
    add_splash=1; add_theme=1
    case " $g_cmd " in *" splash "*) add_splash=0 ;; esac
    case " $g_cmd " in *" nosplash "*) add_splash=0; SPLASH_NOTE="your kernel command line has \"nosplash\" and was left as it is" ;; esac
    [ -n "$g_theme" ] && add_theme=0
    case "$g_term" in *console*|*serial*) add_theme=0 ;; esac
    [ -f /usr/share/grub/themes/lordos/theme.txt ] || add_theme=0
    if [ "$add_splash" = 0 ] && [ "$add_theme" = 0 ]; then
        ok "bootloader: left as it is (it already has a splash setting or a theme of its own)"
        return 0
    fi
    as_root mkdir -p /etc/default/grub.d
    {
        echo "# LordOS boot splash and boot menu, written by the LordOS installer (install.sh)."
        echo "# Remove this file and run update-grub to undo (install.sh --uninstall does it)."
        # shellcheck disable=SC2016 # expanded by GRUB's own shell, not here
        if [ "$add_splash" = 1 ]; then echo 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT splash"'; fi
        echo 'GRUB_GFXPAYLOAD_LINUX=keep'
        if [ "$add_theme" = 1 ]; then echo 'GRUB_THEME=/usr/share/grub/themes/lordos/theme.txt'; fi
    } | as_root tee "$GRUB_DROPIN" >/dev/null
    state_set GRUB_DROPIN 1
    if as_root update-grub >/dev/null 2>&1; then
        changed "GRUB: $GRUB_DROPIN ($( [ "$add_splash" = 1 ] && printf 'splash' )$( [ "$add_splash" = 1 ] && [ "$add_theme" = 1 ] && printf ' and ' )$( [ "$add_theme" = 1 ] && printf 'the LordOS boot menu' )) and update-grub"
    else
        warn "update-grub failed; $GRUB_DROPIN was removed again"
        as_root rm -f "$GRUB_DROPIN"
        state_set GRUB_DROPIN 0
    fi
}

undo_splash() {
    if [ "$(state_get GRUB_DROPIN)" = 1 ] && [ -f "$GRUB_DROPIN" ]; then
        as_root rm -f "$GRUB_DROPIN"
        as_root update-grub >/dev/null 2>&1 || warn "update-grub failed; run it by hand"
        changed "GRUB: $GRUB_DROPIN removed and update-grub run"
    fi
    prev=$(state_get PREV_PLYMOUTH_THEME)
    if [ -n "$prev" ] && command -v plymouth-set-default-theme >/dev/null 2>&1 &&
       [ "$(plymouth-set-default-theme 2>/dev/null)" = lordos ]; then
        if [ "$prev" = none ]; then as_root plymouth-set-default-theme -R --reset >/dev/null 2>&1 || true
        else as_root plymouth-set-default-theme -R "$prev" >/dev/null 2>&1 || true; fi
        changed "boot splash theme restored ($prev); Plymouth itself is kept (sudo apt remove plymouth removes it)"
    fi
}

remote_note() {
    if [ -f "$REMOTE_CONF" ] && grep -q '^enabled=1' "$REMOTE_CONF" 2>/dev/null; then
        warn "remote desktop was already turned on in $REMOTE_CONF before this run; it was left as it was (Settings > Sharing)"
    else
        ok "remote desktop: off (port $REMOTE_DEFAULT_PORT closed; Settings > Sharing or --remote-desktop turns it on)"
    fi
}

# remote login (SSH): never changed here (on a server or VPS it may be how you are connected right
# now); only reported. Settings > Sharing > Remote login installs it if needed and turns it on or off.
ssh_note() {
    if [ ! -x /usr/sbin/sshd ]; then
        ok "remote login (SSH): not installed (Settings > Sharing installs it and turns it on)"
    elif systemctl is-enabled -q ssh.service 2>/dev/null || systemctl is-enabled -q ssh.socket 2>/dev/null; then
        ok "remote login (SSH): on, left as it was (Settings > Sharing turns it off)"
    else
        ok "remote login (SSH): installed but off, left as it was (Settings > Sharing turns it on)"
    fi
}

valid_port() {
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    [ "${#1}" -ge 4 ] && [ "${#1}" -le 5 ] && [ "$1" -ge 1024 ] && [ "$1" -le 65535 ]
}

# a value of the remote desktop configuration ("key=value" lines)
remote_value() {
    [ -r "$REMOTE_CONF" ] || return 0
    sed -n "s/^$1=//p" "$REMOTE_CONF" | tail -n 1 | tr -d '[:space:]'
}

# a person account (uid 1000-59999) with a password: the remote login refuses every other account
has_login_account() {
    getent passwd | awk -F: '$3 >= 1000 && $3 < 60000 { print $1 }' | while read -r u; do
        as_root getent shadow "$u" 2>/dev/null | cut -d: -f2 | grep -q '^\$' && echo "$u"
    done | grep -q .
}

# --remote-desktop: the same file and format as Settings > Sharing (remote-enable/remote-config in
# server/src/remote/remote_admin.c) and the Raspberry Pi image's remote-desktop switch file
# (tools/rpi/rootfs/usr/lib/lordos/rpi/firstboot); lord-sessiond (lordos.service) reads it
setup_remote() {
    if [ "$REMOTE" != 1 ]; then remote_note; return 0; fi
    step "Turning on remote desktop"
    # the current values (Settings > Sharing may have saved some), then the options
    port=$REMOTE_DEFAULT_PORT; allow=all; dev=0
    v=$(remote_value port); if [ -n "$v" ] && valid_port "$v"; then port=$v; fi
    [ "$(remote_value allow)" = group ] && allow=group
    [ "$(remote_value insecure_dev)" = 1 ] && dev=1
    [ -n "$REMOTE_PORT" ] && port=$REMOTE_PORT
    if [ "$allow" = group ] && ! getent group lordos-remote >/dev/null 2>&1; then
        as_root groupadd --system lordos-remote 2>/dev/null || true
    fi
    as_root install -d -m 755 /etc/lordos "$REMOTE_DIR"
    printf '# LordOS · remote desktop (Settings → Sharing). See docs/REMOTE.md\nenabled=1\nport=%s\nallow=%s\ninsecure_dev=%s\n' \
        "$port" "$allow" "$dev" | as_root tee "$REMOTE_CONF.lordos" >/dev/null
    as_root chmod 644 "$REMOTE_CONF.lordos"
    as_root mv -f "$REMOTE_CONF.lordos" "$REMOTE_CONF"
    state_set REMOTE_SET 1
    changed "remote desktop turned on in $REMOTE_CONF: port $port, allow=$allow (Settings > Sharing changes it)"
    REMOTE_NOTE_PORT=$port
    has_login_account || REMOTE_NO_ACCOUNT=1
    has_systemd || return 0
    # lord-sessiond reads the file when it starts, and again on reload (SIGHUP)
    if systemctl is-active -q lordos.service 2>/dev/null; then
        as_root systemctl reload lordos.service >/dev/null 2>&1 || warn "could not reload lordos.service: run sudo systemctl restart lordos"
        REMOTE_RUNNING=1
    elif [ "$(systemctl is-enabled lordos.service 2>/dev/null || true)" = enabled ]; then
        # start it now only when that can't take a screen from anyone: no display manager running
        # and nobody logged in on tty1 (lordos.service takes tty1 over)
        if [ "$dm_active" = 0 ] && ! who 2>/dev/null | awk '{ print $2 }' | grep -qx tty1; then
            if as_root systemctl start lordos.service >/dev/null 2>&1; then
                REMOTE_RUNNING=1
                changed "lordos.service started (the login screen runs headless if there is no screen)"
            else
                warn "could not start lordos.service; it starts at the next boot (journalctl -u lordos)"
            fi
        fi
    fi
    if [ "$REMOTE_RUNNING" = 1 ]; then
        i=0
        while [ $i -lt 30 ]; do
            if grep -q '"running":true' /run/lordos/status.json 2>/dev/null; then break; fi
            i=$((i + 1)); sleep 0.2
        done
        if grep -q '"running":true' /run/lordos/status.json 2>/dev/null; then ok "remote desktop is listening on port $port"
        else warn "remote desktop is not listening yet on port $port (journalctl -u lordos)"; fi
    fi
}

undo_remote() {
    [ -e "$REMOTE_CONF" ] || return 0
    as_root rm -f "$REMOTE_CONF" "$REMOTE_CONF.lordos"
    # a running lord-sessiond rereads it on reload: without the file, remote desktop is off
    if has_systemd && systemctl is-active -q lordos.service 2>/dev/null; then
        as_root systemctl reload lordos.service >/dev/null 2>&1 || true
    fi
    changed "remote desktop configuration $REMOTE_CONF removed (port closed)"
}

# the first IPv4 address of this machine, for the final message (empty if there is none)
host_ip() {
    if command -v hostname >/dev/null 2>&1; then
        hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1
    fi
    return 0
}

network_note() {
    nm=""; nd=""; ifu=""
    if [ -d /run/NetworkManager/devices ]; then nm=" NetworkManager"; fi
    if has_systemd && systemctl is-active -q systemd-networkd.service 2>/dev/null; then nd=" systemd-networkd"; fi
    if command -v ifup >/dev/null 2>&1 && cat /etc/network/interfaces /etc/network/interfaces.d/* 2>/dev/null \
        | grep -E '^[[:space:]]*(auto|allow-hotplug|iface)[[:space:]]' | grep -vqE '^[[:space:]]*[a-z-]+[[:space:]]+lo([[:space:]]|$)'; then
        ifu=" ifupdown"
    fi
    if [ -n "$nm$ifu$nd" ]; then
        ok "network: managed by$nm$ifu$nd and left untouched (LordOS shows those interfaces as system ones)"
    else
        ok "network: configuration untouched"
    fi
}

do_install() {
    step "Checking this system"
    check_os
    check_arch
    check_root
    check_disk
    check_systemd
    pick_user
    pick_admin
    DM_UNIT=""
    if has_systemd; then DM_UNIT=$(current_dm); fi
    dm_active=0
    if [ -n "$DM_UNIT" ] && systemctl is-active -q "$DM_UNIT" 2>/dev/null; then dm_active=1; fi
    installed=$(dpkg-query -W -f '${db:Status-Abbrev} ${Version}' lordos 2>/dev/null || true)
    LORDOS_RUNNING=0
    if has_systemd && systemctl is-active -q lordos.service 2>/dev/null; then LORDOS_RUNNING=1; fi

    say ""
    step "This will:"
    if [ -n "$LOCAL_DEB" ]; then say "  1. Use the local package $LOCAL_DEB"
    elif [ -n "$WANT_VERSION" ]; then say "  1. Download LordOS v${WANT_VERSION#v} for $ARCH from github.com/$REPO and verify its SHA-256"
    else say "  1. Download the latest LordOS release for $ARCH from github.com/$REPO and verify its SHA-256"; fi
    case "$installed" in ii*) say "     (LordOS ${installed#ii } is already installed: it will be upgraded or reinstalled)" ;; esac
    say "  2. Install it with apt-get; its dependencies come from your Debian mirrors"
    [ "$NO_EXTRAS" = 0 ] && say "     plus sudo, PipeWire, fonts and the user D-Bus session if missing"
    say "  3. Create the $GREETER_USER system user (if missing) and generate the en/es/pt locales"
    [ -n "$TARGET_USER" ] && say "  4. Add $TARGET_USER to the groups that exist of: $USER_GROUPS"
    [ -n "$ADMIN_GROUP" ] && say "     and make $TARGET_USER an administrator: this system has none, and the Store needs one"
    if has_systemd; then
        say "  5. Enable lordos.service, lord-netperm and lord-fand, and mask getty@tty1"
        if [ -n "$DM_UNIT" ] && [ "$KEEP_DM" = 0 ]; then
            say "  6. Offer to disable your current display manager, $DM_UNIT (you can switch back)"
        fi
    fi
    [ "$BOOT_SPLASH" != no ] && say "  Then it offers the LordOS boot splash (Plymouth; never changes a customised bootloader)"
    if [ "$REMOTE" = 1 ]; then
        plan_port=$REMOTE_PORT
        if [ -z "$plan_port" ]; then
            plan_port=$(remote_value port)
            valid_port "$plan_port" || plan_port=$REMOTE_DEFAULT_PORT
        fi
        say "  Remote desktop: turned on, port $plan_port (--remote-desktop)"
        say "  It does NOT touch your network configuration."
    else
        say "  It does NOT touch your network configuration, and remote desktop stays off."
    fi
    say ""
    if [ "$REMOTE" = 1 ]; then
        warn "with remote desktop on, this machine accepts remote-desktop logins from the network on port $plan_port (TLS and a user's password). Protect that port with a firewall or reach it through a VPN; don't leave it open to the whole internet."
        # asked on its own (a "no" keeps the rest of the install); --yes means the flag is the answer
        ask "Turn remote desktop on?" 0 || { REMOTE=0; warn "remote desktop stays off"; }
    fi
    ask "Continue?" 0 || die "cancelled; nothing was changed"
    if [ -n "$ADMIN_GROUP" ]; then
        # asked on its own: a "no" keeps the rest of the install
        ask "Make $TARGET_USER an administrator?" 0 || { ADMIN_GROUP=""; warn "$TARGET_USER stays a standard user: the Store can't install or update anything until an administrator exists."; }
    fi

    TMP=$(mktemp -d)
    ROOT_TMP=""
    trap cleanup_tmp EXIT INT TERM
    step "Getting the package"
    get_package
    install_package
    step "Setting up LordOS"
    setup_greeter_user
    setup_groups
    setup_locales
    setup_session_conf
    setup_systemd
    setup_splash
    setup_remote
    ssh_note
    network_note
    state_set INSTALLER_VERSION "$INSTALLER_VERSION"

    say ""
    step "${C_G}LordOS $DEB_VERSION is installed.${C_0}"
    say "What changed:$CHANGES"
    say ""
    if [ -n "$SIG_NOTE" ]; then say "Signature: $SIG_NOTE."; say ""; fi
    if [ -n "$SPLASH_NOTE" ]; then say "Boot splash: $SPLASH_NOTE."; say ""; fi
    if [ "$REMOTE" = 1 ]; then
        ip=$(host_ip)
        [ -n "$ip" ] || ip="<this machine's IP>"
        say "Remote desktop: open ${C_B}https://$ip:$REMOTE_NOTE_PORT${C_0} in a browser and log in with a user of this machine."
        say "  The certificate is self-signed: compare the fingerprint in /etc/lordos/remote/fingerprint."
        say "  Keep port $REMOTE_NOTE_PORT behind a firewall or a VPN."
        if [ "$REMOTE_NO_ACCOUNT" = 1 ]; then
            warn "no user with a password can log in yet: create one with  sudo adduser NAME"
        fi
        if [ "$REMOTE_RUNNING" = 0 ] && has_systemd; then
            if [ "$KEEP_DM" = 1 ] && [ -n "$DM_UNIT" ]; then
                warn "remote desktop runs inside lordos.service, which is not enabled while $DM_UNIT stays the display manager"
            else
                say "  It starts with lordos.service (at the next boot, or: sudo systemctl start lordos)."
            fi
        fi
        say ""
    fi
    if [ "$LORDOS_RUNNING" = 1 ]; then
        say "LordOS is running and was upgraded in place. The running session keeps working; restart"
        say "the computer when it suits you to start the new version:  ${C_B}sudo reboot${C_0}"
        say "(or Store → Updates → Restart). Files, settings and accounts were kept."
    elif ! has_systemd; then
        say "No systemd here, so nothing was set to start. On a real system run the installer again."
    elif [ "$KEEP_DM" = 1 ] && [ -n "$DM_UNIT" ]; then
        say "LordOS is installed but $DM_UNIT is still your display manager. To switch later:"
        say "  sudo systemctl disable $DM_UNIT"
        say "  sudo systemctl enable lordos.service && sudo systemctl mask getty@tty1.service"
        say "  sudo reboot"
    elif [ "$REMOTE_RUNNING" = 1 ]; then
        say "LordOS is running (lordos.service) and starts at every boot; without a screen, the login"
        say "screen and the sessions are reached through the remote desktop."
    else
        say "To start LordOS:"
        say "  - reboot:  ${C_B}sudo reboot${C_0}  (LordOS's login screen appears on the screen)"
        if [ "$dm_active" = 1 ]; then
            say "  - $DM_UNIT is still running this session, so rebooting is the safe way."
        else
            say "  - or, from a text console (not over SSH from inside a desktop):  ${C_B}sudo systemctl start lordos${C_0}"
        fi
        if [ "$DM_SWITCHED" = 1 ]; then
            say ""
            say "To go back to $DM_UNIT without uninstalling:"
            say "  sudo systemctl disable lordos.service && sudo systemctl unmask getty@tty1.service"
            say "  sudo systemctl enable $DM_UNIT && sudo reboot"
        fi
    fi
    say ""
    say "Updates: Store → Updates (or: sudo apt update && sudo apt upgrade). No need to reinstall."
    say "To uninstall:  curl -fsSL https://lordos.org/install.sh | sh -s -- --uninstall"
    say "Documentation: https://github.com/$REPO/blob/main/docs/INSTALL-DEBIAN.md"
}

# ---------------------------------------------------------------- uninstall

do_uninstall() {
    step "Checking this system"
    check_root
    check_systemd
    installed=$(dpkg-query -W -f '${db:Status-Abbrev}' lordos 2>/dev/null || true)
    prev_dm=$(state_get PREV_DM)
    if [ -z "$prev_dm" ] && has_systemd && [ -z "$(current_dm)" ]; then
        # no record (installed by hand?): offer the first display manager still installed
        for d in $KNOWN_DMS; do
            if unit_exists "$d.service"; then prev_dm="$d.service"; break; fi
        done
    fi
    say ""
    step "This will:"
    case "$installed" in
        ii*|iF*|iU*|rc*) if [ -n "$PURGE" ]; then say "  - purge the lordos package"; else say "  - remove the lordos package"; fi ;;
        *) say "  - (the lordos package is not installed)" ;;
    esac
    if has_systemd; then
        say "  - disable lordos.service and lord-netperm, and restore getty@tty1"
        [ -n "$prev_dm" ] && say "  - enable the display manager $prev_dm again"
    fi
    [ -n "$(state_get GROUPS_ADDED)" ] && say "  - remove $(state_get GROUPS_USER) from the groups the installer added: $(state_get GROUPS_ADDED)"
    [ "$(state_get GREETER_CREATED)" = 1 ] && say "  - delete the $GREETER_USER system user"
    [ "$(state_get GRUB_DROPIN)" = 1 ] && say "  - remove the LordOS GRUB settings ($GRUB_DROPIN) and run update-grub"
    [ -n "$(state_get PREV_PLYMOUTH_THEME)" ] && say "  - put back the previous boot splash theme"
    [ -e "$REMOTE_CONF" ] && say "  - remove the remote desktop configuration ($REMOTE_CONF)"
    say "  Your home folder and LordOS settings in it (~/.config/lordos) are kept."
    say ""
    ask "Continue?" 0 || die "cancelled; nothing was changed"

    if has_systemd; then
        as_root systemctl disable lordos.service lord-netperm.service >/dev/null 2>&1 || true
        as_root systemctl stop lord-netperm.service >/dev/null 2>&1 || true
        changed "lordos.service and lord-netperm disabled"
        if [ "$(systemctl is-enabled getty@tty1.service 2>/dev/null || true)" = masked ]; then
            as_root systemctl unmask getty@tty1.service >/dev/null 2>&1 || true
            changed "getty@tty1 unmasked"
        fi
        if [ "$(systemctl is-enabled getty@tty1.service 2>/dev/null || true)" = disabled ]; then
            as_root systemctl enable getty@tty1.service >/dev/null 2>&1 || true
        fi
        if [ -n "$prev_dm" ]; then
            if ask "Enable $prev_dm again as the display manager?" 0; then
                if as_root systemctl enable "$prev_dm" >/dev/null 2>&1 || as_root systemctl enable --force "$prev_dm" >/dev/null 2>&1; then
                    changed "display manager $prev_dm enabled again"
                else
                    warn "could not enable $prev_dm; run: sudo systemctl enable --force $prev_dm"
                fi
            fi
        fi
        lu=$(state_get LINGER_USER)
        if [ -n "$lu" ] && command -v loginctl >/dev/null 2>&1; then
            as_root loginctl disable-linger "$lu" 2>/dev/null || true
            changed "linger disabled for $lu"
        fi
    fi
    undo_splash
    undo_remote
    case "$installed" in
        ii*|iF*|iU*|rc*)
            step "Removing the package"
            if [ -n "$PURGE" ]; then
                as_root env DEBIAN_FRONTEND=noninteractive apt-get purge -y -q lordos </dev/null
                changed "package lordos purged"
            else
                as_root env DEBIAN_FRONTEND=noninteractive apt-get remove -y -q lordos </dev/null
                changed "package lordos removed (its /etc files are kept; --purge removes them)"
            fi ;;
    esac
    gu=$(state_get GROUPS_USER)
    for g in $(state_get GROUPS_ADDED); do
        if [ -n "$gu" ] && user_in_group "$gu" "$g"; then
            as_root gpasswd -d "$gu" "$g" >/dev/null 2>&1 || true
        fi
    done
    if [ -n "$(state_get GROUPS_ADDED)" ]; then changed "$gu removed from: $(state_get GROUPS_ADDED)"; fi
    if [ "$(state_get SESSION_CONF_CREATED)" = 1 ] && [ -f "$SESSION_CONF" ]; then
        as_root rm -f "$SESSION_CONF"
        as_root rmdir /etc/lordos 2>/dev/null || true
        changed "$SESSION_CONF removed"
    fi
    if [ "$(state_get GREETER_CREATED)" = 1 ] && getent passwd "$GREETER_USER" >/dev/null 2>&1; then
        if as_root userdel -r "$GREETER_USER" >/dev/null 2>&1 || as_root userdel "$GREETER_USER" >/dev/null 2>&1; then
            changed "system user $GREETER_USER deleted"
        else
            warn "could not delete $GREETER_USER (still logged in?); after a reboot: sudo userdel -r $GREETER_USER"
        fi
    elif getent passwd "$GREETER_USER" >/dev/null 2>&1; then
        say "  The $GREETER_USER system user was not created by this installer and was kept (sudo userdel -r $GREETER_USER removes it)."
    fi
    as_root rm -rf "$STATE_DIR"

    say ""
    step "LordOS was uninstalled."
    say "What changed:${CHANGES:-
  - nothing}"
    say ""
    say "Reboot to get back to your previous login screen: sudo reboot"
    say "Packages pulled in as dependencies can be removed with: sudo apt autoremove"
}

# ---------------------------------------------------------------- main

main() {
    setup_colors
    ASSUME_YES=0; [ "${LORDOS_YES:-0}" = 1 ] && ASSUME_YES=1
    WANT_VERSION=""; LOCAL_DEB=""; TARGET_USER=""; MODE=install; PURGE=""
    KEEP_DM=0; NO_EXTRAS=0; SKIP_SYSTEMD_CHECK=0; NO_SYSTEMD=0; DM_SWITCHED=0
    BOOT_SPLASH=ask; SPLASH_NOTE=""
    REMOTE=0; REMOTE_PORT=""; REMOTE_NOTE_PORT=$REMOTE_DEFAULT_PORT; REMOTE_RUNNING=0; REMOTE_NO_ACCOUNT=0
    dm_active=0
    while [ $# -gt 0 ]; do
        case "$1" in
            -y|--yes) ASSUME_YES=1 ;;
            --version) [ $# -ge 2 ] || die "--version needs a value"; WANT_VERSION=$2; shift ;;
            --version=*) WANT_VERSION=${1#*=} ;;
            --deb) [ $# -ge 2 ] || die "--deb needs a file"; LOCAL_DEB=$2; shift ;;
            --deb=*) LOCAL_DEB=${1#*=} ;;
            --user) [ $# -ge 2 ] || die "--user needs a name"; TARGET_USER=$2; shift ;;
            --user=*) TARGET_USER=${1#*=} ;;
            --keep-display-manager) KEEP_DM=1 ;;
            --no-extras) NO_EXTRAS=1 ;;
            --boot-splash) BOOT_SPLASH=yes ;;
            --no-boot-splash) BOOT_SPLASH=no ;;
            --no-systemd-check) SKIP_SYSTEMD_CHECK=1 ;;
            --remote-desktop) REMOTE=1 ;;
            --remote-port) [ $# -ge 2 ] || die "--remote-port needs a number"; REMOTE_PORT=$2; shift ;;
            --remote-port=*) REMOTE_PORT=${1#*=} ;;
            --uninstall) MODE=uninstall ;;
            --purge) PURGE=1 ;;
            -h|--help) usage; exit 0 ;;
            *) usage >&2; die "unknown option: $1" ;;
        esac
        shift
    done
    if [ -n "$LOCAL_DEB" ]; then
        case "$LOCAL_DEB" in /*) ;; *) LOCAL_DEB="$(pwd)/$LOCAL_DEB" ;; esac
    fi
    if [ -n "$WANT_VERSION" ] && [ -n "$LOCAL_DEB" ]; then die "use --version or --deb, not both"; fi
    if [ -n "$PURGE" ] && [ "$MODE" != uninstall ]; then die "--purge only goes with --uninstall"; fi
    if [ -n "$REMOTE_PORT" ]; then
        [ "$REMOTE" = 1 ] || die "--remote-port only goes with --remote-desktop"
        valid_port "$REMOTE_PORT" || die "--remote-port must be a number from 1024 to 65535 (got: $REMOTE_PORT)"
    fi
    if [ "$REMOTE" = 1 ] && [ "$MODE" = uninstall ]; then die "--remote-desktop does not go with --uninstall"; fi

    say "${C_B}LordOS installer for Debian 13${C_0} (installer v$INSTALLER_VERSION)"
    if [ "$MODE" = uninstall ]; then do_uninstall; else do_install; fi
}

main "$@"
