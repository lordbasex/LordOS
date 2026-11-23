#!/bin/sh
# mkdeb-studiocamera.sh — builds lordos-studio-camera_<version>_<arch>.deb, the Store's "LordOS Studio Camera" item
# (docs/CAMERA-STUDIO-PLAN.md 2.2, exception E-S2 in AGENTS.md): the "LordOS Studio" V4L2 camera that browsers, OBS and
# every other Linux app see, written by lord-studio. It holds two static, root-owned files and nothing else:
#   /etc/modprobe.d/lordos-studio.conf      the v4l2loopback options: two devices, /dev/video50 "LordOS Studio" (lord-studio
#                                           writes it) and /dev/video49 "OBS Virtual Camera" (OBS Studio's own virtual
#                                           camera; nothing in LordOS writes it), both with exclusive_caps (browsers list
#                                           a loopback only while it announces capture)
#   /etc/modules-load.d/lordos-studio.conf  loads v4l2loopback at boot
# On amd64 it depends on Debian's v4l2loopback-dkms (main, GPL-2.0) and the kernel headers: DKMS builds the module for
# the running kernel (with Secure Boot its key must be enrolled at the next restart, as for any DKMS module). On arm64
# the module ships prebuilt in the LordOS Raspberry Pi kernel (tools/rpi/build-kernel.sh, E-S3), so nothing else.
# There is no runtime root action: lord-server never creates or removes loopback devices (/dev/v4l2loopback stays unused).
# Why OBS's device is number 49: OBS's virtual camera (plugins/linux-v4l2/v4l2-output.c) doesn't look for a card label;
# it takes the first /dev/video* in version order that announces video output. With exclusive_caps an idle loopback
# announces output, so OBS's device must come before /dev/video50, or OBS would take LordOS Studio's device whenever
# LordOS Studio's camera is off. When v4l2loopback is already loaded, OBS loads nothing (it only runs modprobe itself
# when the module is missing).
#   tools/iso/mkdeb-studiocamera.sh <output folder> [arch]     (needs dpkg-deb; arch defaults to this machine's)
set -eu
OUTDIR=${1:?usage: mkdeb-studiocamera.sh <output folder> [arch]}
V=$(sed -n 's/.*"version": *"\([0-9.]*\).*/\1/p' package.json | head -1)
ARCH=${2:-$(dpkg --print-architecture)}
case "$ARCH" in
    amd64) DEPS="lordos, v4l2loopback-dkms (>= 0.13.0), linux-headers-amd64, mokutil" ;;
    arm64) DEPS="lordos" ;;
    *) echo "mkdeb-studiocamera.sh: unknown architecture $ARCH (amd64 or arm64)" >&2; exit 1 ;;
esac
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/DEBIAN" "$T/etc/modprobe.d" "$T/etc/modules-load.d" "$T/usr/share/doc/lordos-studio-camera"
# the number, the label and exclusive_caps are what lord-server checks (server/src/media/mstudiohub.h): keep them in step
cat > "$T/etc/modprobe.d/lordos-studio.conf" <<'C'
# LordOS Studio's camera for every app (lordos-studio-camera; docs/CAMERA-STUDIO-PLAN.md). lord-studio is its only
# writer; apps read it as the "LordOS Studio" camera. The second device, /dev/video49 "OBS Virtual Camera", is for OBS
# Studio's own virtual camera (OBS takes the first idle loopback); LordOS never writes it. Removing the package removes
# this file. The labels are one quoted value split at the comma (the kernel strips only the outer quotes).
options v4l2loopback devices=2 video_nr=50,49 card_label="LordOS Studio,OBS Virtual Camera" exclusive_caps=1,1 max_buffers=4
C
cat > "$T/etc/modules-load.d/lordos-studio.conf" <<'C'
# LordOS Studio's camera (lordos-studio-camera): the v4l2loopback module at boot, with /etc/modprobe.d/lordos-studio.conf
v4l2loopback
C
chmod 644 "$T/etc/modprobe.d/lordos-studio.conf" "$T/etc/modules-load.d/lordos-studio.conf"
printf '/etc/modprobe.d/lordos-studio.conf\n/etc/modules-load.d/lordos-studio.conf\n' > "$T/DEBIAN/conffiles"
cat > "$T/usr/share/doc/lordos-studio-camera/copyright" <<'C'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Files: *
License: MIT
 See the LordOS repository. This package holds two configuration files and depends on
 v4l2loopback (GPL-2.0, https://github.com/umlaeute/v4l2loopback), from Debian on amd64
 and in the LordOS Raspberry Pi kernel on arm64.
C
# postinst: load the module now, with these options, when nothing loaded it yet (a v4l2loopback another program loaded
# with its own options is left alone: ours apply at the next restart). A module Secure Boot refuses just doesn't load.
# On an update, when the module loaded is ours but from an older file, it loads it again so the new options apply:
# from the one-device file (our device there, no /dev/video49: OBS's device appears) or from 0.8.9.5's file (whose
# quoting left the labels «LordOS Studio"» and «"OBS Virtual Camera»). Only when the file is this package's (a person
# who kept an edited one is left alone) and nothing uses the module (refcnt 0, and modprobe -r itself refuses a module
# in use); otherwise the new options apply at the next restart.
cat > "$T/DEBIAN/postinst" <<'C'
#!/bin/sh
set -e
[ "$1" = configure ] && command -v modprobe >/dev/null 2>&1 || exit 0
NAME=$(cat /sys/class/video4linux/video50/name 2>/dev/null || true)
if [ ! -d /sys/module/v4l2loopback ]; then
    modprobe v4l2loopback 2>/dev/null || echo "lordos-studio-camera: v4l2loopback didn't load now (Secure Boot key not enrolled yet?); it loads at the next restart" >&2
elif [ -n "${2:-}" ] && { [ "$NAME" = "LordOS Studio" ] || [ "$NAME" = 'LordOS Studio"' ]; } \
     && { [ ! -e /sys/class/video4linux/video49 ] || [ "$NAME" != "LordOS Studio" ]; } \
     && grep -q '^options v4l2loopback devices=2 video_nr=50,49 card_label="LordOS Studio,OBS Virtual Camera" ' /etc/modprobe.d/lordos-studio.conf 2>/dev/null; then
    if [ "$(cat /sys/module/v4l2loopback/refcnt 2>/dev/null)" = 0 ] && modprobe -r v4l2loopback 2>/dev/null; then
        modprobe v4l2loopback 2>/dev/null || echo "lordos-studio-camera: v4l2loopback didn't load again now; it loads at the next restart" >&2
    else
        echo "lordos-studio-camera: the camera is in use; the LordOS Studio camera's new settings apply at the next restart" >&2
    fi
fi
exit 0
C
# postrm: unload it when nobody uses it (in use, it stays until the restart)
cat > "$T/DEBIAN/postrm" <<'C'
#!/bin/sh
set -e
if [ "$1" = remove ] && [ -d /sys/module/v4l2loopback ] && command -v modprobe >/dev/null 2>&1; then
    modprobe -r v4l2loopback 2>/dev/null || true
fi
exit 0
C
chmod 755 "$T/DEBIAN/postinst" "$T/DEBIAN/postrm"
cat > "$T/DEBIAN/control" <<C
Package: lordos-studio-camera
Version: $V
Architecture: $ARCH
Maintainer: LordOS <lord.basex@gmail.com>
Section: video
Priority: optional
Depends: $DEPS
Description: LordOS Studio camera for every app
 Adds the "LordOS Studio" camera (/dev/video50, v4l2loopback) that browsers, video
 calls and every other app can choose, with LordOS Studio's background blur and
 pictures. LordOS's sandboxed lord-studio is its only writer. A second device,
 /dev/video49 "OBS Virtual Camera", is left for OBS Studio's own virtual camera.
C
dpkg-deb --root-owner-group --build "$T" "$OUTDIR/lordos-studio-camera_${V}_$ARCH.deb" >/dev/null
echo "$OUTDIR/lordos-studio-camera_${V}_$ARCH.deb"
