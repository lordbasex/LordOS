#!/bin/sh
# build.sh — builds the LordOS live ISO (Debian 13 + LordOS), hybrid for USB and CD,
# with BIOS and UEFI boot. See docs/ISO.md.
#
# Usage (as root, on Debian 13, from the root of the already built repository):
#   make -C server -j"$(nproc)"          # lord-server and lord-sessiond (needs libssl-dev, libsrtp2-dev and libusrsctp-dev: the remote desktop is WebRTC only; clang for lord-netperm's BPF)
#   (the themes: npm run build on the development machine; the .bundle.js files travel with the tree)
#   sudo tools/iso/build.sh [work folder]   # default /var/tmp/lordos-iso
#
# Result: <work>/out/lordos-live-<version>-amd64.iso, lordos_<version>_amd64.deb and
# lordos-hwdata_<snapshot date>_all.deb (the signed tested-hardware snapshot, which lordos depends on)
set -eu

[ "$(id -u)" = 0 ] || { echo "build.sh: run as root (sudo)" >&2; exit 1; }
ROOT=$(pwd)
[ -f "$ROOT/package.json" ] && [ -d "$ROOT/server" ] || { echo "build.sh: run from the repository root" >&2; exit 1; }
VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' package.json | head -1)
WORK=${1:-/var/tmp/lordos-iso}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
SUITE=trixie
CH="$WORK/chroot"
IMG="$WORK/image"
OUT="$WORK/out"
ISO="$OUT/lordos-live-$VERSION-amd64.iso"
DEB="$OUT/lordos_${VERSION}_amd64.deb"
T0=$(date +%s)

for t in debootstrap mksquashfs xorriso grub-mkrescue mformat dpkg-deb; do
    command -v "$t" >/dev/null || { echo "build.sh: missing $t (see docs/ISO.md, build packages)" >&2; exit 1; }
done

# Live system packages. LordOS does not use GNOME or KDE: only what is below (xwayland only for X11 apps).
PKGS_BASE="linux-image-amd64 initramfs-tools live-boot live-boot-initramfs-tools systemd-sysv udev kmod dbus dbus-user-session sudo locales tzdata
 iproute2 iputils-ping less nano ca-certificates apt dpkg passwd procps"
# gsettings-desktop-schemas: without it GTK 3 apps can't read gtk-theme from the keyfile backend
# lord-server writes, and they draw light Adwaita instead of LordOS / LordOS-dark (docs/WAYLAND.md)
PKGS_LORDOS="libwayland-server0 libwayland-client0 libxkbcommon0 xkb-data pipewire pipewire-pulse pipewire-alsa wireplumber rtkit
 alsa-ucm-conf wpasupplicant openvpn fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji libthai-data
 skkdic rime-data-pinyin-simp fonts-dejavu-core
 libpoppler-glib8t64 libarchive13t64 poppler-data cups avahi-daemon exfatprogs ntfs-3g
 libavcodec61 libavformat61 libavutil59 libswscale8 libswresample5 libva2 libva-drm2 mesa-va-drivers
 bluez libspa-0.2-bluetooth power-profiles-daemon systemd-timesyncd shared-mime-info
 gsettings-desktop-schemas xwayland"
PKGS_FIRMWARE="firmware-amd-graphics firmware-misc-nonfree firmware-iwlwifi firmware-realtek firmware-atheros
 firmware-sof-signed"
PKGS_GPU="libgl1-mesa-dri libegl1 libgles2 libgbm1 mesa-vulkan-drivers mesa-utils vulkan-tools glmark2-es2-wayland"
# preinstalled browsers (Linux apps; they draw their own bar with LordOS's window buttons, see docs/WAYLAND.md)
PKGS_APPS="foot featherpad qt6-wayland chromium firefox-esr"
# the disk installer (server/src/install): partition, format, copy and grub for UEFI and BIOS
PKGS_INSTALLER="gdisk dosfstools e2fsprogs squashfs-tools util-linux grub-common grub2-common grub-efi-amd64-bin grub-pc-bin"
# the boot splash (the LordOS Plymouth theme comes in the lordos package; plymouth-label draws the
# password prompt's text for encrypted disks)
PKGS_SPLASH="plymouth plymouth-label"
# guest tools for VMware virtual machines (power operations from the hypervisor, time sync, guest IP
# report). The base package only: open-vm-tools-desktop targets X11 (no clipboard sharing or automatic
# resize yet). Its units carry ConditionVirtualization=vmware, so they don't run anywhere else.
PKGS_VM="open-vm-tools"

echo "== LordOS $VERSION: live ISO in $WORK"
umount_all() {
    for m in dev/pts dev proc sys run; do mountpoint -q "$CH/$m" 2>/dev/null && umount -l "$CH/$m" || true; done
}
trap umount_all EXIT
umount_all
rm -rf "$CH" "$IMG"
mkdir -p "$CH" "$IMG/live" "$IMG/boot/grub" "$OUT"

# 1. the LordOS packages: lordos and the tested-hardware snapshot it depends on (docs/BENCHMARK.md)
sh tools/iso/mkdeb.sh "$DEB"
HWDEB=$(sh tools/iso/mkdeb-hwdata.sh "$OUT")
echo "   $(basename "$HWDEB")"

# 2. sistema base
echo "== debootstrap $SUITE (minbase)"
debootstrap --variant=minbase --components=main,contrib,non-free,non-free-firmware \
    "$SUITE" "$CH" "$MIRROR" > "$WORK/debootstrap.log" 2>&1 || { tail -20 "$WORK/debootstrap.log"; exit 1; }

cat > "$CH/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main contrib non-free non-free-firmware
deb $MIRROR $SUITE-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $SUITE-security main contrib non-free non-free-firmware
EOF
echo lordos-live > "$CH/etc/hostname"
# networking is handled by LordOS (its own DHCP): no ifupdown or system interfaces (live-boot writes
# its own on boot; lordos-live-prep resets it to just lo)
mkdir -p "$CH/etc/network"; printf 'auto lo\niface lo inet loopback\n' > "$CH/etc/network/interfaces"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tlordos-live\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$CH/etc/hosts"
# no systemd-resolved: LordOS networking writes /etc/resolv.conf (a regular file, not a symlink)
rm -f "$CH/etc/resolv.conf"; cp /etc/resolv.conf "$CH/etc/resolv.conf" 2>/dev/null || : > "$CH/etc/resolv.conf"
# keep dpkg from starting services inside the chroot
printf '#!/bin/sh\nexit 101\n' > "$CH/usr/sbin/policy-rc.d"; chmod 755 "$CH/usr/sbin/policy-rc.d"

mount --bind /dev "$CH/dev"; mount -t devpts devpts "$CH/dev/pts"
mount -t proc proc "$CH/proc"; mount -t sysfs sysfs "$CH/sys"; mount -t tmpfs tmpfs "$CH/run"
in_ch() { chroot "$CH" env DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 PATH=/usr/sbin:/usr/bin:/sbin:/bin "$@"; }

echo "== paquetes"
in_ch apt-get update -q > "$WORK/apt.log" 2>&1
# shellcheck disable=SC2086
in_ch apt-get install -y -q --no-install-recommends $PKGS_BASE $PKGS_LORDOS $PKGS_FIRMWARE $PKGS_GPU $PKGS_APPS $PKGS_INSTALLER $PKGS_SPLASH $PKGS_VM \
    >> "$WORK/apt.log" 2>&1 || { tail -30 "$WORK/apt.log"; exit 1; }
cp "$DEB" "$CH/tmp/lordos.deb"
cp "$HWDEB" "$CH/tmp/lordos-hwdata.deb"
in_ch apt-get install -y -q --no-install-recommends /tmp/lordos-hwdata.deb /tmp/lordos.deb >> "$WORK/apt.log" 2>&1 || { tail -30 "$WORK/apt.log"; exit 1; }
# the browsers' defaults (their own bar with LordOS's window buttons), shared with the Pi image
sh tools/iso/browser-defaults.sh "$CH"
install -D -m 644 tools/iso/rootfs/etc/wireplumber/wireplumber.conf.d/lordos-bluez.conf "$CH/etc/wireplumber/wireplumber.conf.d/lordos-bluez.conf"
install -D -m 644 tools/iso/rootfs/etc/pipewire/pipewire-pulse.conf.d/lordos-no-modules.conf "$CH/etc/pipewire/pipewire-pulse.conf.d/lordos-no-modules.conf"
rm -f "$CH/tmp/lordos.deb" "$CH/tmp/lordos-hwdata.deb"

# 3. configuration
echo "== configuration"
cat > "$CH/etc/locale.gen" <<'EOF'
es_ES.UTF-8 UTF-8
es_AR.UTF-8 UTF-8
es_MX.UTF-8 UTF-8
en_US.UTF-8 UTF-8
pt_BR.UTF-8 UTF-8
EOF
in_ch locale-gen > /dev/null
echo 'LANG=en_US.UTF-8' > "$CH/etc/default/locale"     # English by default (lordos-live-prep switches it)
ln -sf /usr/share/zoneinfo/UTC "$CH/etc/localtime"; echo UTC > "$CH/etc/timezone"

# ping without root (in the live system the executable's cap_net_raw capability does not survive)
echo 'net.ipv4.ping_group_range = 0 2147483647' > "$CH/etc/sysctl.d/50-lordos-ping.conf"

# live session user: no password, passwordless sudo. Logs in automatically (lord-sessiond
# autologin); locking the screen unlocks again with the empty password (only on the monitor and
# only for the autologin user; see docs/SESSIONS.md)
in_ch useradd -m -s /bin/bash -c "LordOS" -G sudo,audio,video,input,render,lpadmin,pipewire,plugdev lordos
in_ch passwd -d lordos > /dev/null
mkdir -p "$CH/etc/lordos"
printf '# Live ISO: logs in automatically as the lordos user. The installed system uses the login screen.\nAUTOLOGIN=lordos\n' > "$CH/etc/lordos/session.conf"
# the login screen's system user (lord-sessiond also creates it if missing)
in_ch useradd --system --home-dir /var/lib/lordos-greeter --create-home --shell /usr/sbin/nologin --comment LordOS lordos-greeter
install -m 440 tools/iso/rootfs/etc/sudoers.d/lordos-live "$CH/etc/sudoers.d/lordos-live"
mkdir -p "$CH/var/lib/systemd/linger"; : > "$CH/var/lib/systemd/linger/lordos"
# user PipeWire (the packages already enable the global units; just in case)
in_ch systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service > /dev/null 2>&1 || true
# LordOS on tty1 instead of the text login; the other consoles remain (Ctrl+Alt+F2)
in_ch systemctl enable lordos.service > /dev/null
in_ch systemctl enable lord-netperm.service > /dev/null
in_ch systemctl enable lord-fand.service > /dev/null     # fans by temperature (docs/FANS.md)
in_ch systemctl mask getty@tty1.service > /dev/null
# the boot splash: the LordOS theme (the initramfs below is built with it); docs/ISO.md, "Boot splash"
in_ch plymouth-set-default-theme lordos
# quiet and faster boot
in_ch systemctl mask systemd-networkd-wait-online.service > /dev/null 2>&1 || true
printf 'root:lordos\n' | in_ch chpasswd     # emergency console: root / lordos (documented)

rm -f "$CH/usr/sbin/policy-rc.d"
in_ch apt-get clean
rm -rf "$CH/var/lib/apt/lists/"* "$CH/var/cache/apt/"*.bin "$CH/tmp/"* "$CH/var/log/"*.log
: > "$CH/etc/resolv.conf"
: > "$CH/etc/machine-id"     # generated on every boot
in_ch update-initramfs -u > /dev/null 2>&1
# /etc/.updated and /var/.updated: without them every live boot ran ldconfig.service and
# systemd-update-done (ConditionNeedsUpdate= compares them with /usr's time), about 7 s on VM 117.
# The ld.so cache is already current (dpkg's triggers); written last, after every change to /usr.
in_ch ldconfig
in_ch /usr/lib/systemd/systemd-update-done
umount_all

# 4. kernel, initramfs and squashfs
echo "== squashfs"
KVER=$(ls "$CH/boot" | sed -n 's/^vmlinuz-//p' | sort -V | tail -1)
cp "$CH/boot/vmlinuz-$KVER" "$IMG/live/vmlinuz"
cp "$CH/boot/initrd.img-$KVER" "$IMG/live/initrd.img"
rm -f "$IMG/live/filesystem.squashfs"
mksquashfs "$CH" "$IMG/live/filesystem.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -quiet > /dev/null

# 5. grub (BIOS + UEFI in the same hybrid ISO). GRUB menus are English; the entries with a
# language name pass lordos.lang to lordos-live-prep (English is the default).
# The LordOS boot menu theme (dark, small crown, rounded selection); "safe mode" boots
# without the splash (text), the other entries with it.
# Legacy BIOS (0.8.0 bug): the theme's terminal font was Inter, a proportional font, and GRUB's
# gfxterm corrupts its own memory with it at 640x480, the mode "auto" falls back to on BIOS (VBE)
# when the display has no EDID (the VMware-compatible display): VMware triple-faulted when the
# timeout fired, QEMU hung at the menu. The theme now uses GRUB's fixed-width Unifont for the
# terminal, and BIOS asks for 1024x768 or 800x600 first (the theme's layout fits; 640x480 still
# works). UEFI is unchanged (GOP, native mode). docs/ISO.md, "Firmware and displays".
install -d "$IMG/boot/grub/themes/lordos"
install -m 644 tools/iso/grub/lordos/theme.txt tools/iso/grub/lordos/*.png tools/iso/grub/lordos/*.pf2 "$IMG/boot/grub/themes/lordos/"
cat > "$IMG/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=5
insmod all_video
insmod gfxterm
insmod png
loadfont unicode
if [ "\$grub_platform" = pc ]; then
    set gfxmode=1024x768x32,800x600x32,auto
else
    set gfxmode=auto
fi
set gfxpayload=keep
terminal_output gfxterm
loadfont /boot/grub/themes/lordos/inter-regular-14.pf2
loadfont /boot/grub/themes/lordos/inter-regular-16.pf2
loadfont /boot/grub/themes/lordos/inter-semibold-16.pf2
set theme=/boot/grub/themes/lordos/theme.txt
menuentry "LordOS $VERSION (live)" {
    linux /live/vmlinuz boot=live components quiet loglevel=3 splash vt.global_cursor_default=0 lordos.lang=en
    initrd /live/initrd.img
}
menuentry "LordOS $VERSION (safe mode: no GPU)" {
    linux /live/vmlinuz boot=live components quiet loglevel=3 nosplash lordos.gpu=0 lordos.lang=en
    initrd /live/initrd.img
}
menuentry "LordOS $VERSION (Español)" {
    linux /live/vmlinuz boot=live components quiet loglevel=3 splash vt.global_cursor_default=0 lordos.lang=es
    initrd /live/initrd.img
}
menuentry "LordOS $VERSION (Português)" {
    linux /live/vmlinuz boot=live components quiet loglevel=3 splash vt.global_cursor_default=0 lordos.lang=pt
    initrd /live/initrd.img
}
menuentry "LordOS $VERSION (copy to RAM: the USB drive can be removed)" {
    linux /live/vmlinuz boot=live components quiet loglevel=3 splash vt.global_cursor_default=0 toram lordos.lang=en
    initrd /live/initrd.img
}
EOF
echo "== ISO"
rm -f "$ISO"
grub-mkrescue -o "$ISO" "$IMG" -- -volid LORDOS_LIVE > "$WORK/grub-mkrescue.log" 2>&1 || { tail -20 "$WORK/grub-mkrescue.log"; exit 1; }
(cd "$OUT" && sha256sum "$(basename "$ISO")" > "$(basename "$ISO").sha256")

T1=$(date +%s)
echo "== done in $(( (T1 - T0) / 60 )) min $(( (T1 - T0) % 60 )) s"
ls -l "$ISO" "$DEB"
echo "squashfs: $(du -h "$IMG/live/filesystem.squashfs" | cut -f1) · uncompressed system: $(du -sh --exclude=proc --exclude=sys "$CH" | cut -f1)"
