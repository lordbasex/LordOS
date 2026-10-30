#!/bin/sh
# build-img.sh — LordOS image for Raspberry Pi 5 (arm64; Pi 4 where possible), to write to
# an SD card or an NVMe with Raspberry Pi Imager, dd or balenaEtcher. See docs/RASPBERRY.md.
#
# Usage (as root, on Debian 13, from the repository root with the themes already built):
#   sudo tools/rpi/build-img.sh [work folder]          # default /var/tmp/lordos-rpi
#
# Result: <work>/out/lordos-<version>-rpi5-arm64.img.xz (+ .sha256) and lordos_<version>_arm64.deb
#
# - On an arm64 host (the Pi 5 itself, an ARM server) everything is native. If server/build/lord-server
#   is already aarch64 (make -C server on the Pi) it is used; otherwise it is built in the build chroot.
# - On an x86-64 host qemu-user-static is needed (binfmt for aarch64): debootstrap and apt run
#   emulated, and lord-server is built in a separate arm64 chroot (slow: ~15–30 min the first time).
#
# Variables: MIRROR (Debian), RPI_MIRROR (Raspberry Pi archive), IMG_EXTRA_MB (free space in the
# image before it grows on boot, 512), RPI_SSH=1 (SSH server on from the start, for development;
# otherwise it ships off and the `ssh` switch file turns it on), RPI_KEY_NOCHECK=1 (do not verify
# the Raspberry Pi archive key fingerprint; not recommended), KERNEL_DEBS (folder with the LordOS
# kernel packages from tools/rpi/build-kernel.sh, default /var/tmp/lordos-kernel/out), CODE_CHECK_SERVER
# (an x86-64 host with an aarch64 server/build: a lord-server built for the host from the same tree,
# used only to check the signed system code; docs/SIGNING.md).
#
# Kernel: the LordOS build of the Raspberry Pi kernel (with Landlock, the BPF LSM and BTF) when its
# .debs are in $KERNEL_DEBS; otherwise Raspberry Pi's archive kernel, which has none of them (a loud
# warning).
set -eu

[ "$(id -u)" = 0 ] || { echo "build-img.sh: run as root (sudo)" >&2; exit 1; }
ROOT=$(pwd)
[ -f "$ROOT/package.json" ] && [ -d "$ROOT/server" ] && [ -f "$ROOT/tools/iso/mkdeb.sh" ] || {
    echo "build-img.sh: run from the repository root" >&2; exit 1; }
VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' package.json | head -1)
WORK=${1:-/var/tmp/lordos-rpi}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
RPI_MIRROR=${RPI_MIRROR:-http://archive.raspberrypi.com/debian}
# Raspberry Pi archive key. It is taken from the raspberrypi-archive-keyring package of the archive
# (found through the Packages index; trust comes only from the fingerprint check below): the loose
# raspberrypi.gpg.key at the archive root still carries its 2012 SHA-1 binding signatures, which apt's
# sqv rejects since 2026-02-01 ("SHA1 is not considered secure"), while the package's copy is re-signed
# with SHA-512. RPI_KEY_URL=<url> (an armored or binary key) skips the package. See docs/RASPBERRY.md.
RPI_KEY_URL=${RPI_KEY_URL:-}
RPI_KEY_FPR=${RPI_KEY_FPR:-CF8A1AF502A2AA2D763BAE7E82B129927FA3303E}
IMG_EXTRA_MB=${IMG_EXTRA_MB:-512}
SUITE=trixie
ARCH=arm64
CH="$WORK/rootfs"           # the image's system
BR="$WORK/buildroot"        # arm64 chroot to build lord-server (if needed)
OUT="$WORK/out"
NAME="lordos-$VERSION-rpi5-arm64"
IMG="$OUT/$NAME.img"
DEB="$OUT/lordos_${VERSION}_arm64.deb"
RPI="$ROOT/tools/rpi/rootfs"
KERNEL_DEBS=${KERNEL_DEBS:-/var/tmp/lordos-kernel/out}
T0=$(date +%s)

for t in debootstrap dpkg-deb sfdisk losetup mkfs.vfat mkfs.ext4 xz curl gpg partx python3; do
    command -v "$t" >/dev/null || { echo "build-img.sh: missing $t (see docs/RASPBERRY.md, build packages)" >&2; exit 1; }
done
case "$(uname -m)" in
    aarch64|arm64) NATIVE=1 ;;
    *) NATIVE=0
       [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || {
           echo "build-img.sh: host $(uname -m) without aarch64 binfmt: install qemu-user-static (and binfmt-support)" >&2; exit 1; } ;;
esac

# Packages. LordOS's and the sample apps are the same as the ISO's (tools/iso/build.sh); the
# kernel and firmware change (Raspberry Pi) and there is no live-boot, grub or disk installer.
PKGS_BASE="initramfs-tools systemd-sysv udev kmod dbus dbus-user-session sudo locales tzdata
 iproute2 iputils-ping less nano ca-certificates apt dpkg passwd procps e2fsprogs fdisk dosfstools util-linux
 systemd-timesyncd"
PKGS_LORDOS="libwayland-server0 libxkbcommon0 xkb-data pipewire pipewire-pulse pipewire-alsa wireplumber rtkit
 alsa-ucm-conf wpasupplicant openvpn fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji libthai-data
 skkdic rime-data-pinyin-simp fonts-dejavu-core xwayland
 cups avahi-daemon exfatprogs ntfs-3g power-profiles-daemon shared-mime-info gsettings-desktop-schemas"
# The board has no clock that survives without a battery: systemd-timesyncd sets the time from the
# network at boot (Settings → Date & Time, "Automatic time"); without it the clock starts months
# behind and every signed package index is refused as "not valid yet". The rest is the same as the
# ISO (printing, exFAT/NTFS drives, power profiles, the MIME database, and the GSettings schemas GTK
# apps need to read LordOS's GTK theme).
# Raspberry Pi: boot firmware (start*.elf are not used on the Pi 5: its firmware lives in the
# EEPROM) and Wi-Fi (CYW43455, firmware-brcm80211); kernels below
PKGS_RPI="raspi-firmware firmware-brcm80211 rsync"
# Bluetooth (the CYW43455 on a UART, brought up by the kernel; see docs/RASPBERRY.md, "Bluetooth"),
# all from Debian: BlueZ (bluetoothd, plus hciconfig/hcitool for rpi/bt-uart), the chip's patch
# (bluez-firmware: brcm/BCM4345C0.hcd), headset audio in PipeWire (libspa-0.2-bluetooth, with the
# lordos package's WirePlumber drop-in) and rfkill. The same BlueZ packages as the ISO.
PKGS_BT="bluez bluez-firmware libspa-0.2-bluetooth rfkill"
PKGS_RPI_EXTRA="rpi-eeprom raspi-utils"
# Kernels: Pi 5 (2712: 16K pages) and Pi 4/400/CM4 (v8: 4K pages). Ours (Landlock) if built,
# per flavour; otherwise Raspberry Pi's.
OWN_KDEBS=""; ARCHIVE_K=""; OWN_FLAVOURS=""
for fl in 2712 v8; do
    d=$(ls "$KERNEL_DEBS"/linux-image-*-lordos*-rpi-${fl}_*_arm64.deb 2>/dev/null | sort -V | tail -1 || true)
    if [ -n "$d" ]; then OWN_KDEBS="$OWN_KDEBS $d"; OWN_FLAVOURS="$OWN_FLAVOURS $fl"
    else ARCHIVE_K="$ARCHIVE_K linux-image-rpi-$fl"; fi
done
# Debian 13 Mesa: v3d (OpenGL ES 3.1 on the Pi 5), v3dv (Vulkan 1.2/1.3) and kmsro (vc4 + v3d)
PKGS_GPU="libgl1-mesa-dri libegl1 libgles2 libgbm1 mesa-vulkan-drivers mesa-utils vulkan-tools glmark2-es2-wayland"
# the same apps as the ISO, browsers included (Debian's arm64 builds; they draw their own bar with
# LordOS's window buttons: tools/iso/browser-defaults.sh, docs/WAYLAND.md)
PKGS_APPS="foot featherpad qt6-wayland chromium firefox-esr"
# the SSH server ships off; the `ssh` switch file on the boot partition (rpi/firstboot) or RPI_SSH=1 turns it on
PKGS_SSH="openssh-server"
# the boot splash (the LordOS Plymouth theme comes in the lordos package; plymouth-label draws the
# password prompt's text for encrypted disks). See docs/RASPBERRY.md, "Boot splash".
PKGS_SPLASH="plymouth plymouth-label"
# to build lord-server (only in the build chroot, not in the image)
# (the same as the ISO's build packages, docs/ISO.md: lord-doc needs Poppler and libarchive, lord-media
# FFmpeg, lord-netperm's BPF programs clang; without them make silently leaves those parts out)
PKGS_BUILD="build-essential pkg-config libxkbcommon-dev libwayland-dev wayland-protocols libgbm-dev libegl-dev libgles-dev
 libssl-dev libcrypt-dev libsrtp2-dev libusrsctp-dev dpkg-dev file clang
 libpoppler-glib-dev libarchive-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libva-dev"

echo "== LordOS $VERSION: Raspberry Pi image in $WORK (host $(uname -m))"
kernel_warning() {
    echo "##########################################################################################" >&2
    echo "## WARNING: no LordOS kernel for rpi-$1 in $KERNEL_DEBS: using Raspberry Pi's archive kernel." >&2
    echo "## It has NO Landlock: app and helper isolation will be seccomp-only on that board and" >&2
    echo "## doc_evil.sh / sandbox-test fail; and no BPF LSM or BTF: \"No Bluetooth\" for Linux apps" >&2
    echo "## isn't enforced. Build it first: tools/rpi/build-kernel.sh (docs/RASPBERRY.md)." >&2
    echo "##########################################################################################" >&2
}
for fl in 2712 v8; do
    case " $OWN_FLAVOURS " in *" $fl "*) echo "   kernel rpi-$fl: LordOS build ($(basename "$(ls "$KERNEL_DEBS"/linux-image-*-lordos*-rpi-${fl}_*_arm64.deb | sort -V | tail -1)"))" ;;
        *) kernel_warning "$fl" ;; esac
done
umount_tree() {
    for d in "$CH" "$BR"; do
        for m in dev/pts dev proc sys run; do mountpoint -q "$d/$m" 2>/dev/null && umount -l "$d/$m" || true; done
    done
    for m in "$WORK/mnt/boot/firmware" "$WORK/mnt"; do mountpoint -q "$m" 2>/dev/null && umount "$m" || true; done
    [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null || true
}
trap umount_tree EXIT
umount_tree
mkdir -p "$OUT" "$WORK"

mount_ch() {
    mount --bind /dev "$1/dev"; mount -t devpts devpts "$1/dev/pts"
    mount -t proc proc "$1/proc"; mount -t sysfs sysfs "$1/sys"; mount -t tmpfs tmpfs "$1/run"
}
umount_ch() { for m in dev/pts dev proc sys run; do mountpoint -q "$1/$m" && umount -l "$1/$m" || true; done; }
in_root() { r=$1; shift; chroot "$r" env DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 PATH=/usr/sbin:/usr/bin:/sbin:/bin "$@"; }

# debootstrap arm64 (native, or in two stages with qemu on x86)
bootstrap() {
    dir=$1; log=$2
    rm -rf "$dir"; mkdir -p "$dir"
    if [ "$NATIVE" = 1 ]; then
        debootstrap --arch=$ARCH --variant=minbase --components=main,contrib,non-free,non-free-firmware \
            "$SUITE" "$dir" "$MIRROR" > "$log" 2>&1 || { tail -20 "$log"; exit 1; }
    else
        debootstrap --arch=$ARCH --foreign --variant=minbase --components=main,contrib,non-free,non-free-firmware \
            "$SUITE" "$dir" "$MIRROR" > "$log" 2>&1 || { tail -20 "$log"; exit 1; }
        # with binfmt "F" (fix-binary, Debian's) there is no need to copy qemu; if present, just in case
        Q=$(command -v qemu-aarch64-static || true)
        [ -n "$Q" ] && cp "$Q" "$dir/usr/bin/"
        chroot "$dir" /debootstrap/debootstrap --second-stage >> "$log" 2>&1 || { tail -20 "$log"; exit 1; }
    fi
    cat > "$dir/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main contrib non-free non-free-firmware
deb $MIRROR $SUITE-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $SUITE-security main contrib non-free non-free-firmware
EOF
    printf '#!/bin/sh\nexit 101\n' > "$dir/usr/sbin/policy-rc.d"; chmod 755 "$dir/usr/sbin/policy-rc.d"
    cp /etc/resolv.conf "$dir/etc/resolv.conf" 2>/dev/null || true
}

is_aarch64() {      # aarch64 ELF (e_machine = 0xb7)
    [ -f "$1" ] && [ "$(od -An -tx1 -j18 -N2 "$1" | tr -d ' \n')" = "b700" ]
}

# ---- 1. lord-server and lord-sessiond for arm64, and the package ----
[ -f themes/greeter/theme.bundle.js ] || { echo "build-img.sh: themes missing (npm run build on the development machine)" >&2; exit 1; }
if is_aarch64 server/build/lord-server && is_aarch64 server/build/lord-sessiond; then
    echo "== lord-server: using server/build (already aarch64)"
    # mkdeb.sh checks the signed system code with lord-server, which can't run here on x86-64
    if [ "$NATIVE" = 0 ] && [ ! -x "${CODE_CHECK_SERVER:-}" ]; then
        echo "build-img.sh: server/build is aarch64 on an $(uname -m) host: set CODE_CHECK_SERVER to a lord-server built for this host from the same tree" >&2; exit 1
    fi
    DEB_ARCH=$ARCH sh tools/iso/mkdeb.sh "$DEB"
else
    echo "== lord-server: building in an arm64 chroot ($BR)"
    # (re)install the build packages when the chroot is new or lacks some (an older chroot without clang)
    if [ ! -x "$BR/usr/bin/gcc" ] || [ ! -x "$BR/usr/bin/clang" ] || [ ! -d "$BR/usr/include/aarch64-linux-gnu/libavcodec" ]; then
        [ -x "$BR/usr/bin/gcc" ] || bootstrap "$BR" "$WORK/buildroot-debootstrap.log"
        mount_ch "$BR"
        in_root "$BR" apt-get update -q > "$WORK/buildroot-apt.log" 2>&1
        # shellcheck disable=SC2086
        in_root "$BR" apt-get install -y -q --no-install-recommends $PKGS_BUILD >> "$WORK/buildroot-apt.log" 2>&1 || {
            tail -30 "$WORK/buildroot-apt.log"; exit 1; }
    else
        mount_ch "$BR"
    fi
    rm -rf "$BR/src/LordOS"; mkdir -p "$BR/src/LordOS"
    # code-manifest.json (+ .sig for a release): the verified system code (docs/SIGNING.md); the
    # ls is split on purpose (zero, one or two file names)
    # shellcheck disable=SC2046
    tar -C "$ROOT" --exclude=server/build --exclude=node_modules -cf - \
        package.json README.md server third_party assets themes tools packaging apt \
        $(cd "$ROOT" && ls code-manifest.json code-manifest.sig 2>/dev/null) | tar -C "$BR/src/LordOS" -xf -
    J=$(nproc)
    in_root "$BR" sh -c "cd /src/LordOS && make -C server -j$J GPU=1 RELEASE=${RELEASE:-0} && make -C server build/canvas-test" > "$WORK/build.log" 2>&1 || {
        tail -40 "$WORK/build.log"; exit 1; }
    is_aarch64 "$BR/src/LordOS/server/build/lord-server" || { echo "build-img.sh: the binary is not aarch64" >&2; exit 1; }
    # canvas-test: NEON against C, bit for bit (under qemu it measures nothing, but it checks)
    in_root "$BR" /src/LordOS/server/build/canvas-test > "$WORK/canvas-test.log" 2>&1 || {
        cat "$WORK/canvas-test.log"; echo "build-img.sh: canvas-test failed (NEON ≠ C); build with NEON=0" >&2; exit 1; }
    in_root "$BR" sh -c "cd /src/LordOS && DEB_ARCH=$ARCH REQUIRE_SIGNED_CODE=${REQUIRE_SIGNED_CODE:-0} sh tools/iso/mkdeb.sh /tmp/lordos.deb" >> "$WORK/build.log" 2>&1 || {
        tail -20 "$WORK/build.log"; exit 1; }
    cp "$BR/tmp/lordos.deb" "$DEB"
    # the package must be complete: the documents and media helpers and the firewall's BPF programs
    for f in lord-doc lord-media; do
        dpkg-deb -c "$DEB" | grep -q "/usr/lib/lordos/bin/$f\$" || { echo "build-img.sh: $f missing from the package (build packages, see $WORK/build.log)" >&2; exit 1; }
    done
    [ -s "$BR/src/LordOS/server/build/gen/np_bpf.h" ] || { echo "build-img.sh: lord-netperm built without its BPF programs (clang)" >&2; exit 1; }
    umount_ch "$BR"
fi
# mkdeb.sh (the same as the ISO's) takes the architecture from DEB_ARCH
echo "   $(basename "$DEB") ($(du -h "$DEB" | cut -f1))"

# ---- 2. the image's system ----
echo "== debootstrap $SUITE $ARCH (minbase)"
bootstrap "$CH" "$WORK/debootstrap.log"

# Raspberry Pi archive: only kernel, firmware and utilities (preferences.d/raspberrypi)
echo "== Raspberry Pi archive key"
KEY="$WORK/raspberrypi.pgp"
rm -f "$KEY"
if [ -n "$RPI_KEY_URL" ]; then
    KEY_SRC=$RPI_KEY_URL
    curl -fsSL "$RPI_KEY_URL" -o "$WORK/raspberrypi.key"
else
    KR=$(curl -fsSL "$RPI_MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz" | gzip -dc |
        awk '/^Package: raspberrypi-archive-keyring$/ { f = 1 } f && /^Filename:/ { print $2; exit }')
    [ -n "$KR" ] || { echo "build-img.sh: raspberrypi-archive-keyring not found in $RPI_MIRROR ($SUITE)" >&2; exit 1; }
    KEY_SRC="$RPI_MIRROR/$KR"
    rm -rf "$WORK/rpi-keyring"; mkdir -p "$WORK/rpi-keyring"
    curl -fsSL "$KEY_SRC" -o "$WORK/rpi-keyring.deb"
    dpkg-deb -x "$WORK/rpi-keyring.deb" "$WORK/rpi-keyring"
    cp "$WORK/rpi-keyring/usr/share/keyrings/raspberrypi-archive-keyring.pgp" "$WORK/raspberrypi.key"
fi
# always stored binary (dearmored): apt's sqv decides the format by the extension
gpg --dearmor < "$WORK/raspberrypi.key" > "$KEY" 2>/dev/null && [ -s "$KEY" ] || cp "$WORK/raspberrypi.key" "$KEY"
FPR=$(gpg --show-keys --with-colons "$KEY" 2>/dev/null | awk -F: '$1 == "fpr" { print $10; exit }')
if [ "$FPR" != "$RPI_KEY_FPR" ] && [ "${RPI_KEY_NOCHECK:-0}" != 1 ]; then
    echo "build-img.sh: the key from $KEY_SRC has fingerprint $FPR (expected $RPI_KEY_FPR)" >&2
    echo "  verify it and pass RPI_KEY_FPR=<fingerprint> (see docs/RASPBERRY.md)" >&2
    exit 1
fi
install -d "$CH/etc/apt/keyrings"
install -m 644 "$KEY" "$CH/etc/apt/keyrings/raspberrypi.pgp"
echo "deb [signed-by=/etc/apt/keyrings/raspberrypi.pgp] $RPI_MIRROR $SUITE main" > "$CH/etc/apt/sources.list.d/raspberrypi.list"
install -m 644 "$RPI/etc/apt/preferences.d/raspberrypi" "$CH/etc/apt/preferences.d/raspberrypi"

echo lordos-pi > "$CH/etc/hostname"
mkdir -p "$CH/etc/network"; printf 'auto lo\niface lo inet loopback\n' > "$CH/etc/network/interfaces"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tlordos-pi\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$CH/etc/hosts"
# the Raspberry Pi kernel hooks write to /boot/firmware (here, a regular folder that is later
# copied to the FAT partition)
mkdir -p "$CH/boot/firmware"

mount_ch "$CH"
echo "== paquetes"
in_root "$CH" apt-get update -q > "$WORK/apt.log" 2>&1 || { tail -30 "$WORK/apt.log"; exit 1; }
# shellcheck disable=SC2086
in_root "$CH" apt-get install -y -q --no-install-recommends $PKGS_BASE $PKGS_LORDOS $PKGS_RPI $PKGS_BT $ARCHIVE_K $PKGS_GPU $PKGS_APPS $PKGS_SSH $PKGS_SPLASH \
    >> "$WORK/apt.log" 2>&1 || { tail -30 "$WORK/apt.log"; exit 1; }
# the LordOS kernels (same hooks as Raspberry Pi's: initramfs, raspi-firmware copies kernel_2712.img,
# device trees and overlays), and an apt pin so the archive kernel of that flavour never replaces them
if [ -n "$OWN_KDEBS" ]; then
    mkdir -p "$CH/tmp/kernel"
    # shellcheck disable=SC2086
    cp $OWN_KDEBS "$CH/tmp/kernel/"
    in_root "$CH" sh -c 'apt-get install -y -q --no-install-recommends /tmp/kernel/*.deb' >> "$WORK/apt.log" 2>&1 || {
        tail -30 "$WORK/apt.log"; exit 1; }
    rm -rf "$CH/tmp/kernel"
    {
        echo "# LordOS on Raspberry Pi: the kernel is LordOS's build of the Raspberry Pi kernel (Landlock, BPF LSM,"
        echo "# tools/rpi/build-kernel.sh, docs/RASPBERRY.md). Raspberry Pi's kernel packages for the same"
        echo "# board are never installed over it; LordOS updates ship as new linux-image-*-lordos*-rpi-* packages."
        for fl in $OWN_FLAVOURS; do
            echo
            echo "Package: linux-image-rpi-$fl linux-image-*+rpt-rpi-$fl linux-headers-rpi-$fl linux-headers-*+rpt-rpi-$fl"
            echo "Pin: version *"
            echo "Pin-Priority: -1"
        done
    } > "$CH/etc/apt/preferences.d/lordos-kernel"
fi
# shellcheck disable=SC2086
in_root "$CH" apt-get install -y -q --no-install-recommends $PKGS_RPI_EXTRA >> "$WORK/apt.log" 2>&1 ||
    echo "   warning: $PKGS_RPI_EXTRA were not installed (see $WORK/apt.log)"
cp "$DEB" "$CH/tmp/lordos.deb"
in_root "$CH" apt-get install -y -q --no-install-recommends /tmp/lordos.deb >> "$WORK/apt.log" 2>&1 || { tail -30 "$WORK/apt.log"; exit 1; }
rm -f "$CH/tmp/lordos.deb"
# the browsers' defaults, the same as the ISO's
sh "$ROOT/tools/iso/browser-defaults.sh" "$CH"

# ---- 3. configuration (same as the ISO, minus the live parts) ----
echo "== configuration"
cat > "$CH/etc/locale.gen" <<'EOF'
es_ES.UTF-8 UTF-8
es_AR.UTF-8 UTF-8
es_MX.UTF-8 UTF-8
en_US.UTF-8 UTF-8
pt_BR.UTF-8 UTF-8
EOF
in_root "$CH" locale-gen > /dev/null
echo 'LANG=en_US.UTF-8' > "$CH/etc/default/locale"     # English by default, like the ISO
ln -sf /usr/share/zoneinfo/UTC "$CH/etc/localtime"; echo UTC > "$CH/etc/timezone"
echo 'net.ipv4.ping_group_range = 0 2147483647' > "$CH/etc/sysctl.d/50-lordos-ping.conf"

# setup user (first-boot setup mode, see rpi/firstboot): same as the
# live user. If lordos-user.txt/userconf.txt is present on first boot, it is deleted right away.
in_root "$CH" useradd -m -s /bin/bash -c "LordOS" -G sudo,audio,video,input,render lordos
in_root "$CH" passwd -d lordos > /dev/null
printf '# LordOS on Raspberry Pi: only during setup mode (removed by rpi/firstboot)\nlordos ALL=(ALL) NOPASSWD: ALL\n' > "$CH/etc/sudoers.d/lordos-setup"
chmod 440 "$CH/etc/sudoers.d/lordos-setup"
mkdir -p "$CH/var/lib/systemd/linger"; : > "$CH/var/lib/systemd/linger/lordos"
mkdir -p "$CH/etc/lordos"
# until first boot nobody logs in automatically; rpi/firstboot decides (login screen or lordos)
printf '# Raspberry Pi: set by /usr/lib/lordos/rpi/firstboot on first boot.\nAUTOLOGIN=\n' > "$CH/etc/lordos/session.conf"
in_root "$CH" useradd --system --home-dir /var/lib/lordos-greeter --create-home --shell /usr/sbin/nologin --comment LordOS lordos-greeter
in_root "$CH" systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service > /dev/null 2>&1 || true

# first boot and wait for vc4
install -d "$CH/usr/lib/lordos/rpi" "$CH/etc/systemd/system/lordos.service.d"
install -m 755 "$RPI/usr/lib/lordos/rpi/firstboot" "$RPI/usr/lib/lordos/rpi/wait-kms" "$CH/usr/lib/lordos/rpi/"
# the post-flash check (docs/RASPBERRY.md, "After flashing"): sudo /usr/lib/lordos/rpi/verify-board
install -m 755 "$ROOT/tools/rpi/verify-board.sh" "$CH/usr/lib/lordos/rpi/verify-board"
install -m 644 "$RPI/etc/systemd/system/lordos-rpi-firstboot.service" "$CH/etc/systemd/system/"
install -m 644 "$RPI/etc/systemd/system/lordos.service.d/rpi.conf" "$CH/etc/systemd/system/lordos.service.d/"
# the on-board Bluetooth: SCO routing helper, started by udev for the UART controller (docs/RASPBERRY.md)
install -m 755 "$RPI/usr/lib/lordos/rpi/bt-uart" "$CH/usr/lib/lordos/rpi/"
install -m 644 "$RPI/etc/systemd/system/lordos-rpi-bt@.service" "$CH/etc/systemd/system/"
install -D -m 644 "$RPI/etc/udev/rules.d/90-lordos-rpi-bt.rules" "$CH/etc/udev/rules.d/90-lordos-rpi-bt.rules"
mkdir -p "$CH/var/lib/lordos/rpi"
: > "$CH/var/lib/lordos/rpi/resize-pending"
: > "$CH/var/lib/lordos/rpi/firstboot-pending"
in_root "$CH" systemctl enable lordos-rpi-firstboot.service lordos.service > /dev/null
in_root "$CH" systemctl enable lord-fand.service > /dev/null    # fans by temperature from the first boot (docs/FANS.md)
# per-app network permissions and the device restrictions in the kernel (docs/FIREWALL.md), as on the ISO
in_root "$CH" systemctl enable lord-netperm.service > /dev/null
in_root "$CH" systemctl enable bluetooth.service > /dev/null       # bluez's postinst does it too; made sure
in_root "$CH" systemctl mask getty@tty1.service > /dev/null
in_root "$CH" systemctl mask systemd-networkd-wait-online.service > /dev/null 2>&1 || true
# no root password: the emergency console stays locked (like the installed system)
in_root "$CH" usermod -p '*' root

# the Pi kernel in /boot/firmware: the raspi-firmware hooks do it; otherwise, by hand
KV2712=$(ls "$CH/boot" | sed -n 's/^vmlinuz-\(.*-rpi-2712\)$/\1/p' | sort -V | tail -1)
KV8=$(ls "$CH/boot" | sed -n 's/^vmlinuz-\(.*-rpi-v8\)$/\1/p' | sort -V | tail -1)
[ -n "$KV2712" ] || { echo "build-img.sh: the rpi-2712 kernel was not installed" >&2; exit 1; }
for kv in $KV2712 $KV8; do
    if grep -q '^CONFIG_SECURITY_LANDLOCK=y' "$CH/boot/config-$kv" 2>/dev/null; then echo "   kernel $kv: Landlock built in"
    else echo "   WARNING: kernel $kv has NO Landlock (seccomp-only isolation on that board)" >&2; fi
    if grep -q '^CONFIG_BPF_LSM=y' "$CH/boot/config-$kv" 2>/dev/null && grep -q '^CONFIG_DEBUG_INFO_BTF=y' "$CH/boot/config-$kv"; then
        echo "   kernel $kv: BPF LSM and BTF built in"
    else echo "   WARNING: kernel $kv has no BPF LSM/BTF (\"No Bluetooth\" for Linux apps not enforced on that board)" >&2; fi
    # sock_diag SOCK_DESTROY (6.18.57-3+lordos, 0.8.4): the firewall closes the connections a new rule denies
    if grep -q '^CONFIG_INET_DIAG_DESTROY=y' "$CH/boot/config-$kv" 2>/dev/null &&
       [ "$(grep -Ec '^CONFIG_INET_(TCP|UDP)_DIAG=[ym]' "$CH/boot/config-$kv")" = 2 ]; then
        echo "   kernel $kv: sock_diag SOCK_DESTROY built in"
    else echo "   WARNING: kernel $kv has no INET_DIAG_DESTROY/INET_UDP_DIAG (blocked connections stall instead of closing)" >&2; fi
done
# boot splash: the LordOS theme, and vc4 (HDMI) with DRM in the initramfs so Plymouth draws from the
# first seconds instead of waiting for the root file system. Plymouth's own hook adds the rest of
# kernel/drivers/gpu/drm (MODULES=most), its plugins, the theme and a font for the password prompt.
in_root "$CH" plymouth-set-default-theme lordos
install -d "$CH/etc/initramfs-tools/conf.d"
grep -qx vc4 "$CH/etc/initramfs-tools/modules" 2>/dev/null || {
    echo "# LordOS: the display driver early, for the boot splash (docs/RASPBERRY.md, \"Boot splash\")"
    echo vc4
} >> "$CH/etc/initramfs-tools/modules"
printf '# LordOS: the boot splash needs the DRM drivers in the initramfs (docs/RASPBERRY.md)\nMODULES=most\n' \
    > "$CH/etc/initramfs-tools/conf.d/lordos-splash.conf"
in_root "$CH" update-initramfs -u -k all > "$WORK/initramfs.log" 2>&1 || { tail -20 "$WORK/initramfs.log"; exit 1; }
for kv in $KV2712 $KV8; do
    in_root "$CH" lsinitramfs "/boot/initrd.img-$kv" 2>/dev/null | grep -q 'plymouth/themes/lordos/lordos.script' ||
        { echo "build-img.sh: the initramfs of $kv has no LordOS splash (see $WORK/initramfs.log)" >&2; exit 1; }
done
place_kernel() {     # version, kernel name on the FAT, initramfs name
    [ -n "$1" ] || return 0
    [ -f "$CH/boot/firmware/$2" ] || cp "$CH/boot/vmlinuz-$1" "$CH/boot/firmware/$2"
    [ -f "$CH/boot/initrd.img-$1" ] && cp "$CH/boot/initrd.img-$1" "$CH/boot/firmware/$3"
    if ! ls "$CH/boot/firmware/"bcm2712*.dtb > /dev/null 2>&1; then
        D=$(ls -d "$CH/usr/lib/linux-image-$1" 2>/dev/null || true)
        [ -n "$D" ] && cp "$D"/broadcom/bcm27*.dtb "$CH/boot/firmware/" && cp -r "$D/overlays" "$CH/boot/firmware/"
    fi
}
place_kernel "$KV2712" kernel_2712.img initramfs_2712
place_kernel "$KV8" kernel8.img initramfs8
install -m 644 "$RPI/boot/firmware/config.txt" "$CH/boot/firmware/config.txt"
# case accessories (Settings → Cooling, admin action rpi-case); config.txt includes it
printf '# lordos-case.txt: written by LordOS (Settings, Cooling). No case accessories.\n' > "$CH/boot/firmware/lordos-case.txt"
install -m 644 "$RPI/boot/firmware/lordos-user.txt.example" "$CH/boot/firmware/lordos-user.txt.example"

rm -f "$CH/usr/sbin/policy-rc.d" "$CH/usr/bin/qemu-aarch64-static"
in_root "$CH" apt-get clean
rm -rf "$CH/var/lib/apt/lists/"* "$CH/var/cache/apt/"*.bin "$CH/tmp/"* "$CH/var/log/"*.log
rm -f "$CH/etc/ssh/ssh_host_"*      # each board creates its own on first boot (rpi/firstboot)
# SSH off unless asked for: the `ssh` switch file turns it on at boot (rpi/firstboot)
in_root "$CH" systemctl disable ssh.service ssh.socket > /dev/null 2>&1 || true
if [ "${RPI_SSH:-0}" = 1 ]; then in_root "$CH" systemctl enable ssh.service > /dev/null 2>&1 || true; fi
: > "$CH/etc/resolv.conf"
: > "$CH/etc/machine-id"
umount_ch "$CH"

# ---- 4. the image: MBR, 512 MiB FAT32 (bootfs) + ext4 (rootfs) ----
echo "== image"
ROOT_MB=$(( $(du -sm "$CH" | cut -f1) - $(du -sm "$CH/boot/firmware" | cut -f1) ))
BOOT_MB=512
SIZE_MB=$((4 + BOOT_MB + ROOT_MB * 115 / 100 + IMG_EXTRA_MB))
rm -f "$IMG" "$IMG.xz"
truncate -s "${SIZE_MB}M" "$IMG"
DISKID=$(od -An -tx4 -N4 /dev/urandom | tr -d ' \n')
sfdisk --quiet "$IMG" <<EOF
label: dos
label-id: 0x$DISKID
unit: sectors
start=8192, size=$((BOOT_MB * 2048)), type=c, bootable
start=$((8192 + BOOT_MB * 2048)), type=83
EOF
LOOP=$(losetup --show -f -P "$IMG")
mkfs.vfat -F 32 -n bootfs "${LOOP}p1" > /dev/null
mkfs.ext4 -q -L rootfs -O ^orphan_file "${LOOP}p2"     # without orphan_file: so older e2fsprogs can read it too
mkdir -p "$WORK/mnt"
mount "${LOOP}p2" "$WORK/mnt"
tar -C "$CH" --exclude=./boot/firmware -cf - . | tar -C "$WORK/mnt" -xpf -
mkdir -p "$WORK/mnt/boot/firmware"
mount "${LOOP}p1" "$WORK/mnt/boot/firmware"
cp -r "$CH/boot/firmware/." "$WORK/mnt/boot/firmware/"
PARTUUID_BOOT="$DISKID-01"
PARTUUID_ROOT="$DISKID-02"
cat > "$WORK/mnt/etc/fstab" <<EOF
# LordOS on Raspberry Pi (tools/rpi/build-img.sh)
PARTUUID=$PARTUUID_ROOT  /              ext4  defaults,noatime  0 1
PARTUUID=$PARTUUID_BOOT  /boot/firmware vfat  defaults,umask=0077  0 2
EOF
# lordos.gpu=0 (safe mode, everything on the CPU) and lordos.lang=en|pt can be added here, as on the ISO.
# The boot splash: quiet splash (Plymouth), no serial console for it, no kernel logo or blinking cursor;
# the console is tty1, where Plymouth draws and where Esc shows the boot messages. Removing "splash"
# boots in text as before (docs/RASPBERRY.md, "Boot splash").
# lsm=: the same list as the LordOS kernel's CONFIG_LSM (Landlock for app isolation, server/src/sec;
# Yama for ptrace_scope; bpf for lord-netperm's "No Bluetooth" program); those the kernel lacks are
# ignored, so the active list is "capability,landlock,yama,bpf".
printf 'console=serial0,115200 console=tty1 root=PARTUUID=%s rootfstype=ext4 fsck.repair=yes rootwait quiet splash plymouth.ignore-serial-consoles logo.nologo vt.global_cursor_default=0 loglevel=3 cgroup_enable=memory lsm=landlock,lockdown,yama,integrity,bpf lordos.lang=en\n' \
    "$PARTUUID_ROOT" > "$WORK/mnt/boot/firmware/cmdline.txt"
sync
umount "$WORK/mnt/boot/firmware"
umount "$WORK/mnt"
losetup -d "$LOOP"; LOOP=""

echo "== xz"
xz -T0 -6 -f "$IMG"
(cd "$OUT" && sha256sum "$(basename "$IMG").xz" > "$(basename "$IMG").xz.sha256")

T1=$(date +%s)
echo "== done in $(( (T1 - T0) / 60 )) min $(( (T1 - T0) % 60 )) s"
ls -l "$IMG.xz" "$DEB"
echo "image: ${SIZE_MB} MB uncompressed (grows by itself on boot) · system: ${ROOT_MB} MB"
