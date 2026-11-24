#!/bin/sh
# LordOS Guest Tools: installs Machines' guest agent in a Linux virtual machine (docs/VIRTUALIZATION.md 6.17, "The
# LordOS Guest Tools disc"). It is on the read-only disc Machines puts in the machine's CD drive when the machine has
# no qemu-guest-agent yet; the person runs it in the machine:
#
#   sh /media/$USER/LORDOS_TOOLS/install.sh      (or /run/media/$USER/..., or wherever the disc is mounted)
#
# It asks for the administrator's password with the machine's own prompt (pkexec when a polkit agent runs in the
# desktop, else sudo or su in the terminal). The host never sees or types that password. Then, as root:
#   1. lordos-guest from this disc as /usr/local/bin/lordos-guest (its SHA-256 checked against sha256.txt), the udev
#      rule that lets the person at the machine's seat open its port, its user unit (or an XDG autostart entry
#      without systemd) and the shutdown hook, as Machines' Install Agent writes them; first, because Machines takes
#      the disc out as soon as qemu-guest-agent answers;
#   2. qemu-guest-agent from the system's own package manager (apt-get, dnf, yum, zypper, pacman or apk) and its
#      repositories, started now and at every boot;
#   3. xclip and wl-clipboard the same way, best effort (the clipboard's tools; not on LordOS, whose lord-server is the
#      clipboard);
#   4. lordos-guest started again for the person who ran this.
# It takes no argument from anyone but these two, runs only the fixed commands below, and changes nothing else.
#
#   install.sh            install (as above)
#   install.sh --plan [--sysroot DIR]   print what it would do here, change nothing (tests; DIR is read instead of /
#                         for /etc/os-release, /run/systemd/system and LordOS's own files)
set -u

say() { printf '%s\n' "LordOS Guest Tools: $*"; }
die() { printf '%s\n' "LordOS Guest Tools: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

PLAN=0
SYSROOT=
while [ $# -gt 0 ]; do
    case $1 in
        --plan) PLAN=1 ;;
        --sysroot) [ $# -ge 2 ] || die "--sysroot needs a folder"; SYSROOT=$2; shift ;;
        *) die "usage: install.sh [--plan [--sysroot DIR]]" ;;
    esac
    shift
done
[ "$PLAN" = 1 ] || [ -z "$SYSROOT" ] || die "--sysroot is only for --plan"
# the system's own programs first (--plan keeps the PATH it was given: the tests' stand-ins)
[ "$PLAN" = 1 ] || { PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}; export PATH; }

# the disc's folder (this script's), as an absolute path made only of plain characters
here=$(cd "$(dirname "$0")" 2>/dev/null && pwd -P) || die "can't find the disc's folder"
case $here in *[!A-Za-z0-9/_.@+-]*) die "the disc's folder has an unusual name ($here): mount it somewhere plain, e.g. /mnt" ;; esac

# ---- what this machine is ----
case $(uname -m) in
    x86_64|amd64) arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) arch=unknown ;;
esac
disc_arch=$( { tr -cd 'a-z0-9' < "$here/arch.txt"; } 2>/dev/null)

lordos=0
[ -x "$SYSROOT/usr/bin/lord-server" ] || [ -d "$SYSROOT/usr/lib/lordos" ] && lordos=1

pm=none
for p in apt-get dnf yum zypper pacman apk; do
    if have "$p"; then pm=$p; break; fi
done

init=none
if [ -d "$SYSROOT/run/systemd/system" ]; then init=systemd
elif have rc-update && have rc-service; then init=openrc
fi

clip="xclip wl-clipboard"
[ "$lordos" = 1 ] && clip=none

# (ps rather than pgrep: busybox systems may not have pgrep)
# shellcheck disable=SC2009
if [ "$(id -u)" = 0 ]; then escalate=root
elif have pkexec && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && ps -e -o comm= 2>/dev/null | grep -Eq '^(polkit-gnome-au|polkit-kde-auth|polkit-mate-aut|lxpolkit|lxqt-policykit|xfce-polkit|gnome-shell|cinnamon|budgie-polkit-d|hyprpolkitagen|polkit-agent|mate-polkit|ukui-polkit|deepin-polkit-a)'; then escalate=pkexec
elif have sudo; then escalate=sudo
elif have doas; then escalate=doas
else escalate=su
fi

if [ "$PLAN" = 1 ]; then
    printf 'arch=%s\ndisc=%s\npm=%s\npackages=qemu-guest-agent\nclip=%s\ninit=%s\nlordos=%s\nescalate=%s\n' \
        "$arch" "${disc_arch:-none}" "$pm" "$clip" "$init" "$lordos" "$escalate"
    exit 0
fi

# ---- not root yet: the machine's own prompt asks for the password ----
if [ "$escalate" != root ]; then
    say "the administrator's password is asked for by this machine (Machines never sees it)"
    case $escalate in
        pkexec) exec pkexec /bin/sh "$here/install.sh" ;;
        sudo) exec sudo /bin/sh "$here/install.sh" ;;
        doas) exec doas /bin/sh "$here/install.sh" ;;
        su) exec su root -c "/bin/sh $here/install.sh" ;;
    esac
fi

# ---- as root ----
[ "$arch" != unknown ] || die "this machine's processor ($(uname -m)) isn't one Machines has tools for"
[ "$arch" = "$disc_arch" ] || die "this disc has the tools for ${disc_arch:-another processor}; this machine is $arch"
[ -f "$here/lguest" ] || die "the disc has no lordos-guest (lguest)"
if have sha256sum && [ -f "$here/sha256.txt" ]; then
    want=$(sed -n 's/^\([0-9a-f]\{64\}\)  lguest$/\1/p' "$here/sha256.txt")
    got=$(sha256sum "$here/lguest" | cut -d' ' -f1)
    if [ -z "$want" ] || [ "$want" != "$got" ]; then die "lordos-guest on the disc doesn't match its checksum: the disc can't be read correctly"; fi
fi

# lordos-guest first, as Machines' Install Agent puts it in (lordvm.c INSTALL_SH): everything is read off the disc
# before qemu-guest-agent starts, because Machines takes the disc out as soon as that agent answers
install -d -m 755 /usr/local/bin
if ! { cp "$here/lguest" /usr/local/bin/.lordos-guest.new && chmod 755 /usr/local/bin/.lordos-guest.new &&
        mv -f /usr/local/bin/.lordos-guest.new /usr/local/bin/lordos-guest; }; then
    die "lordos-guest couldn't be copied to /usr/local/bin"
fi
# the seat's person may open the port (logind's uaccess). LordOS's own sessions aren't logind seats: there the desktop's
# users are in the video group (as for the screen), so the port is that group's too
rule='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess"'
[ "$lordos" = 1 ] && rule="$rule"', GROUP="video", MODE="0660"'
install -d -m 755 /etc/udev/rules.d
printf '%s\n' "$rule" > /etc/udev/rules.d/71-lordos-guest.rules
udevadm control --reload >/dev/null 2>&1 || true
udevadm trigger --subsystem-match=virtio-ports >/dev/null 2>&1 || true
want=graphical-session.target
[ "$lordos" = 1 ] && want=default.target
if [ "$init" = systemd ]; then
    install -d -m 755 /etc/systemd/user
    printf '%s\n' '[Unit]' 'Description=LordOS Machines clipboard helper' 'ConditionPathExists=/dev/virtio-ports/org.lordos.agent' \
        '[Service]' 'ExecStart=/usr/local/bin/lordos-guest' 'Restart=on-failure' 'RestartSec=3' '[Install]' "WantedBy=$want" \
        > /etc/systemd/user/lordos-guest.service
    systemctl --global enable lordos-guest.service >/dev/null 2>&1 || true
    # (the hook's "$1" is written as it is)
    # shellcheck disable=SC2016
    { install -d -m 755 /usr/lib/systemd/system-shutdown &&
      printf '%s\n' '#!/bin/sh' 'exec /usr/local/bin/lordos-guest --stopping "$1"' > /usr/lib/systemd/system-shutdown/lordos-guest-local &&
      chmod 755 /usr/lib/systemd/system-shutdown/lordos-guest-local; } 2>/dev/null || true
else
    install -d -m 755 /etc/xdg/autostart
    printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=LordOS Machines clipboard helper' 'Exec=/usr/local/bin/lordos-guest' \
        'NoDisplay=true' > /etc/xdg/autostart/lordos-guest.desktop
fi

pm_install() {     # $@: package names (fixed below)
    case $pm in
        apt-get) DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@" ||
                 { DEBIAN_FRONTEND=noninteractive apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@"; } ;;
        dnf) dnf install -y "$@" ;;
        yum) yum install -y "$@" ;;
        zypper) zypper --non-interactive install --no-recommends "$@" ;;
        pacman) pacman -S --needed --noconfirm "$@" ;;
        apk) apk add "$@" ;;
        *) return 1 ;;
    esac
}

qga_ok=0
if have qemu-ga || [ -x /usr/bin/qemu-ga ] || [ -x /usr/sbin/qemu-ga ]; then
    say "qemu-guest-agent is already installed"
    qga_ok=1
elif [ "$pm" = none ]; then
    say "no package manager found (apt-get, dnf, yum, zypper, pacman, apk): install qemu-guest-agent yourself"
else
    say "installing qemu-guest-agent with $pm"
    if pm_install qemu-guest-agent; then qga_ok=1
    else
        [ "$pm" = pacman ] && say "pacman couldn't install it: update the system first (pacman -Syu), then run this again"
        say "qemu-guest-agent couldn't be installed; the clipboard and the display still work, a clean shutdown and the logs need it"
    fi
fi
if [ "$qga_ok" = 1 ]; then
    case $init in
        systemd) udevadm trigger --subsystem-match=virtio-ports >/dev/null 2>&1 || true
                 systemctl enable qemu-guest-agent.service >/dev/null 2>&1 || true
                 systemctl restart qemu-guest-agent.service >/dev/null 2>&1 || say "note: qemu-guest-agent starts at the next boot" ;;
        openrc) rc-update add qemu-guest-agent default >/dev/null 2>&1 || true
                rc-service qemu-guest-agent restart >/dev/null 2>&1 || say "note: qemu-guest-agent starts at the next boot" ;;
    esac
fi

if [ "$clip" != none ] && ! have xclip && [ "$pm" != none ]; then
    say "installing the clipboard's tools (xclip, wl-clipboard)"
    pm_install xclip wl-clipboard >/dev/null 2>&1 || say "note: xclip and wl-clipboard couldn't be installed; the clipboard needs one of them"
fi

# started again now for the person who ran this (sudo, pkexec and doas say who), else for every signed-in person
started=0
restart_for() {     # $1: a user name from the system's own database
    [ "$init" = systemd ] || return 0
    systemctl --user -M "$1@" daemon-reload >/dev/null 2>&1 || return 0
    systemctl --user -M "$1@" restart lordos-guest.service >/dev/null 2>&1 && started=1
}
uid=${PKEXEC_UID:-${SUDO_UID:-${DOAS_UID:-}}}
case $uid in ''|*[!0-9]*) uid= ;; esac
if [ -n "$uid" ]; then
    name=$(id -nu "$uid" 2>/dev/null) && restart_for "$name"
else
    for d in /run/user/*; do
        u=${d##*/}
        case $u in ''|*[!0-9]*) continue ;; esac
        [ "$u" -ge 1000 ] || continue
        name=$(id -nu "$u" 2>/dev/null) && restart_for "$name"
    done
fi

if [ "$started" = 1 ]; then say "done: Machines connects to this machine in a few seconds; you can take the disc out"
else say "done: the helper starts at the next sign-in (sign out and in again); Machines takes the disc out when it connects"
fi
exit 0
