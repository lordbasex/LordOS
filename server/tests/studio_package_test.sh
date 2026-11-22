#!/bin/sh
# studio_package_test.sh — the Store's LordOS Studio Camera item (lordos-studio-camera, tools/iso/mkdeb-studiocamera.sh;
# docs/CAMERA-STUDIO-PLAN.md 2.2, E-S2): builds it for amd64 and arm64 into a temporary folder and checks what it holds,
# that its options match what lord-server checks (src/media/mstudiohub.h) and its scripts. It never installs anything
# and never loads a module: the real device is checked on a VM (tests/fwlinux/devices.sh).
#   sh tests/studio_package_test.sh          (from server/; needs dpkg-deb)
set -eu
cd "$(dirname "$0")/../.."
command -v dpkg-deb >/dev/null || { echo "skipped: needs dpkg-deb"; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

HDR=server/src/media/mstudiohub.h
NR=$(sed -n 's/^#define STUDIO_LOOPBACK_DEV "\/dev\/video\([0-9]*\)".*/\1/p' "$HDR")
CARD=$(sed -n 's/^#define STUDIO_LOOPBACK_CARD "\(.*\)".*/\1/p' "$HDR")
for A in amd64 arm64; do
    echo "$A:"
    D=$(sh tools/iso/mkdeb-studiocamera.sh "$T" "$A")
    check "built $(basename "$D")" '[ -f "$D" ]'
    check "its name and architecture" '[ "$(dpkg-deb -f "$D" Package)" = lordos-studio-camera ] && [ "$(dpkg-deb -f "$D" Architecture)" = "$A" ]'
    DEP=$(dpkg-deb -f "$D" Depends)
    if [ "$A" = amd64 ]; then
        check "amd64: Debian's DKMS module and the headers ($DEP)" 'echo "$DEP" | grep -q "v4l2loopback-dkms (>= 0.13.0)" && echo "$DEP" | grep -q linux-headers-amd64'
    else
        check "arm64: the module is in the LordOS kernel, no DKMS ($DEP)" '[ "$DEP" = lordos ]'
    fi
    X="$T/x-$A"
    dpkg-deb -R "$D" "$X"
    FILES=$(cd "$X" && find . -type f ! -path './DEBIAN/*' | sort | tr '\n' ' ')
    check "only the two configuration files and the copyright ($FILES)" \
        '[ "$FILES" = "./etc/modprobe.d/lordos-studio.conf ./etc/modules-load.d/lordos-studio.conf ./usr/share/doc/lordos-studio-camera/copyright " ]'
    OPT=$(grep '^options' "$X/etc/modprobe.d/lordos-studio.conf")
    check "one device, video_nr=$NR as lord-server expects" 'echo "$OPT" | grep -q "devices=1 video_nr=$NR "'
    check "card_label \"$CARD\" as lord-server checks" 'echo "$OPT" | grep -q "card_label=\"$CARD\""'
    check "exclusive_caps=1 (browsers list it only while lord-studio writes)" 'echo "$OPT" | grep -q "exclusive_caps=1"'
    check "nothing else in modprobe.d (no install or remove commands)" '[ "$(grep -v "^#" "$X/etc/modprobe.d/lordos-studio.conf" | grep -c .)" = 1 ]'
    check "modules-load.d loads v4l2loopback only" '[ "$(grep -v "^#" "$X/etc/modules-load.d/lordos-studio.conf")" = v4l2loopback ]'
    check "both are conffiles" '[ "$(cat "$X/DEBIAN/conffiles")" = "/etc/modprobe.d/lordos-studio.conf
/etc/modules-load.d/lordos-studio.conf" ]'
    check "files owned by root, not writable by others" '[ -z "$(dpkg-deb -c "$D" | awk "{print \$1, \$2}" | grep -v "root/root" )" ] && ! dpkg-deb -c "$D" | grep -q "^-.......w"'
    check "maintainer scripts parse" 'sh -n "$X/DEBIAN/postinst" && sh -n "$X/DEBIAN/postrm"'
    check "postinst only loads the module (and only when nothing loaded it)" \
        '[ "$(grep -c "modprobe" "$X/DEBIAN/postinst")" = 2 ] && grep -q "! -d /sys/module/v4l2loopback" "$X/DEBIAN/postinst" && ! grep -Eq "mokutil|dkms |apt|curl|wget" "$X/DEBIAN/postinst"'
done
[ "$fails" = 0 ] || { echo "studio-package-test: $fails failures"; exit 1; }
echo "studio-package-test: ok"
