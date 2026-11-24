#!/bin/sh
# guest_tools_install_test.sh — tools/guest/install.sh, the LordOS Guest Tools disc's installer (docs/VIRTUALIZATION.md
# 6.17): what it chooses on each kind of Linux, with `install.sh --plan` (it changes nothing then) over stand-in
# programs in PATH and fake system trees (--sysroot): the package manager (apt-get, dnf, yum, zypper, pacman, apk, in
# that order; none), the init system (systemd, OpenRC, none), the processor, LordOS (no clipboard tools: lord-server is
# the clipboard), and how it asks for the password (already root; pkexec only with a desktop and a polkit agent running;
# sudo; doas; su). Also: a disc mounted under a folder with an unusual name is refused, and --sysroot without --plan.
# The real installs ran in a Debian 13 chroot and in guests (docs/VIRTUALIZATION.md, "Checked"). Run from server/ by
# tests/run_suites.sh.
set -u
S=$(cd "$(dirname "$0")/../.." && pwd)/tools/guest/install.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-gti.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0 n=0
REAL="cat tr dirname grep sed cut sh"

# bin DIR PROGS...: a PATH of the real basics plus stand-ins (exit 0, print nothing) for PROGS; uname=ARCH, id=0, ps=NAME
# (the stand-ins' "$1" is written as it is)
# shellcheck disable=SC2016
bin() {
    d=$1; shift
    rm -rf "$d"; mkdir -p "$d"
    for p in $REAL; do ln -s "$(command -v "$p")" "$d/$p"; done
    printf '#!/bin/sh\necho x86_64\n' > "$d/uname"
    printf '#!/bin/sh\n[ "$1" = -u ] && echo 1000 || echo user\n' > "$d/id"
    printf '#!/bin/sh\necho bash\n' > "$d/ps"
    for p in "$@"; do
        case $p in
            uname=*) printf '#!/bin/sh\necho %s\n' "${p#uname=}" > "$d/uname" ;;
            id=0) printf '#!/bin/sh\n[ "$1" = -u ] && echo 0 || echo root\n' > "$d/id" ;;
            ps=*) printf '#!/bin/sh\necho bash\necho %s\n' "${p#ps=}" > "$d/ps" ;;
            *) printf '#!/bin/sh\nexit 0\n' > "$d/$p" ;;
        esac
    done
    for f in "$d"/*; do [ -L "$f" ] || chmod 755 "$f"; done
}
# case NAME "PROGS" SYSROOT-KIND "KEY=VALUE..." [env...]: the plan has every KEY=VALUE line
case_() {
    n=$((n + 1))
    name=$1 progs=$2 kind=$3 want=$4; shift 4
    # shellcheck disable=SC2086
    bin "$T/bin-$name" $progs
    R="$T/root-$name"; mkdir -p "$R"
    case $kind in
        systemd) mkdir -p "$R/run/systemd/system" ;;
        lordos) mkdir -p "$R/run/systemd/system" "$R/usr/bin"; : > "$R/usr/bin/lord-server"; chmod 755 "$R/usr/bin/lord-server" ;;
    esac
    out=$(env -i PATH="$T/bin-$name" "$@" "$T/bin-$name/sh" "$S" --plan --sysroot "$R" 2>&1) || { echo "FAIL $name: exited with $? ($out)"; fails=$((fails + 1)); return; }
    for kv in $want; do
        printf '%s\n' "$out" | grep -qx "$kv" || { echo "FAIL $name: no '$kv' in:"; printf '%s\n' "$out" | sed 's/^/    /'; fails=$((fails + 1)); return; }
    done
}
case_ debian "apt-get sudo" systemd "arch=amd64 pm=apt-get packages=qemu-guest-agent init=systemd lordos=0 escalate=sudo"
case_ apt-first "dnf apt-get zypper sudo" systemd "pm=apt-get"
case_ fedora-agent "dnf pkexec sudo ps=gnome-shell" systemd "pm=dnf escalate=pkexec" WAYLAND_DISPLAY=wayland-0
case_ fedora-kde "dnf pkexec sudo ps=polkit-kde-auth" systemd "escalate=pkexec" DISPLAY=:0
case_ no-agent "dnf pkexec sudo" systemd "escalate=sudo" WAYLAND_DISPLAY=wayland-0
case_ no-desktop "dnf pkexec sudo ps=gnome-shell" systemd "escalate=sudo"
case_ centos7 "yum sudo" systemd "pm=yum"
case_ opensuse "zypper sudo" systemd "pm=zypper"
case_ arch "pacman sudo" systemd "pm=pacman"
case_ alpine "apk doas rc-update rc-service" none "pm=apk init=openrc escalate=doas"
case_ bare "" none "pm=none init=none escalate=su"
case_ lordos "apt-get sudo" lordos "lordos=1 clip=none pm=apt-get"
case_ arm64 "apt-get sudo uname=aarch64" systemd "arch=arm64"
case_ riscv "apt-get sudo uname=riscv64" systemd "arch=unknown"
case_ root "apt-get sudo pkexec id=0" systemd "escalate=root"
# clip's value has a space: checked whole
n=$((n + 1))
bin "$T/bin-c" apt-get sudo
mkdir -p "$T/root-c/run/systemd/system"
out=$(env -i PATH="$T/bin-c" "$T/bin-c/sh" "$S" --plan --sysroot "$T/root-c")
printf '%s\n' "$out" | grep -qx 'clip=xclip wl-clipboard' || { echo "FAIL clip: $out"; fails=$((fails + 1)); }
# a disc mounted under an unusual folder name: refused (the name goes into su -c)
n=$((n + 1))
mkdir -p "$T/odd dir'x"
cp "$S" "$T/odd dir'x/install.sh"
out=$(env -i PATH="$T/bin-c" "$T/bin-c/sh" "$T/odd dir'x/install.sh" --plan 2>&1) && { echo "FAIL odd folder: accepted"; fails=$((fails + 1)); }
printf '%s\n' "$out" | grep -q "unusual name" || { echo "FAIL odd folder: $out"; fails=$((fails + 1)); }
# --sysroot without --plan, and an unknown argument
n=$((n + 1))
env -i PATH="$T/bin-c" "$T/bin-c/sh" "$S" --sysroot /tmp >/dev/null 2>&1 && { echo "FAIL --sysroot without --plan accepted"; fails=$((fails + 1)); }
env -i PATH="$T/bin-c" "$T/bin-c/sh" "$S" --rm-rf / >/dev/null 2>&1 && { echo "FAIL an unknown argument accepted"; fails=$((fails + 1)); }
echo "guest-tools install.sh: $n cases, $fails failed"
[ "$fails" = 0 ]
