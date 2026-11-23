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
OBSNR=$(sed -n 's/^#define STUDIO_OBS_DEV "\/dev\/video\([0-9]*\)".*/\1/p' "$HDR")
OBSCARD=$(sed -n 's/^#define STUDIO_OBS_CARD "\(.*\)".*/\1/p' "$HDR")

# a copy of a postinst run against a fake /sys and /etc, with a modprobe that only records its arguments:
#   run_postinst <postinst> <case folder> <args...>; the calls end up in <case folder>/calls
run_postinst() {
    P=$1 C=$2
    shift 2
    mkdir -p "$C/bin"
    printf '#!/bin/sh\necho "$*" >> "%s/calls"\n' "$C" > "$C/bin/modprobe"
    chmod 755 "$C/bin/modprobe"
    : > "$C/calls"
    sed -e "s#/sys/#$C/sys/#g" -e "s#/etc/modprobe.d/#$C/etc/modprobe.d/#g" "$P" > "$C/postinst"
    PATH="$C/bin:$PATH" sh "$C/postinst" "$@" 2>/dev/null
}
# fake state: <case folder> <loaded 0|1> <our device there 0|1> <OBS's device there 0|1> <refcnt>, and the conffile
fake_sys() {
    C=$1
    rm -rf "$C/sys" "$C/etc"
    mkdir -p "$C/sys/class/video4linux" "$C/etc/modprobe.d"
    cp "$X/etc/modprobe.d/lordos-studio.conf" "$C/etc/modprobe.d/"
    if [ "$2" = 1 ]; then mkdir -p "$C/sys/module/v4l2loopback"; echo "$5" > "$C/sys/module/v4l2loopback/refcnt"; fi
    if [ "$3" = 1 ]; then mkdir -p "$C/sys/class/video4linux/video$NR"; echo "$CARD" > "$C/sys/class/video4linux/video$NR/name"; fi
    if [ "$4" = 1 ]; then mkdir -p "$C/sys/class/video4linux/video$OBSNR"; echo "$OBSCARD" > "$C/sys/class/video4linux/video$OBSNR/name"; fi
}
for A in amd64 arm64; do
    echo "$A:"
    D=$(sh tools/iso/mkdeb-studiocamera.sh "$T" "$A")
    check "built $(basename "$D")" '[ -f "$D" ]'
    check "its name and architecture" '[ "$(dpkg-deb -f "$D" Package)" = lordos-studio-camera ] && [ "$(dpkg-deb -f "$D" Architecture)" = "$A" ]'
    DEP=$(dpkg-deb -f "$D" Depends)
    if [ "$A" = amd64 ]; then
        check "amd64: Debian's DKMS module, the headers and mokutil for the Secure Boot step ($DEP)" 'echo "$DEP" | grep -q "v4l2loopback-dkms (>= 0.13.0)" && echo "$DEP" | grep -q linux-headers-amd64 && echo "$DEP" | grep -q mokutil'
    else
        check "arm64: the module is in the LordOS kernel, no DKMS ($DEP)" '[ "$DEP" = lordos ]'
    fi
    X="$T/x-$A"
    dpkg-deb -R "$D" "$X"
    FILES=$(cd "$X" && find . -type f ! -path './DEBIAN/*' | sort | tr '\n' ' ')
    check "only the two configuration files and the copyright ($FILES)" \
        '[ "$FILES" = "./etc/modprobe.d/lordos-studio.conf ./etc/modules-load.d/lordos-studio.conf ./usr/share/doc/lordos-studio-camera/copyright " ]'
    OPT=$(grep '^options' "$X/etc/modprobe.d/lordos-studio.conf")
    check "two devices, video_nr=$NR,$OBSNR: ours as lord-server expects, then OBS's" 'echo "$OPT" | grep -q "devices=2 video_nr=$NR,$OBSNR "'
    check "card_label \"$CARD\",\"$OBSCARD\" (ours as lord-server checks)" 'echo "$OPT" | grep -q "card_label=\"$CARD\",\"$OBSCARD\" "'
    check "exclusive_caps=1,1 (browsers list each only while something writes it)" 'echo "$OPT" | grep -q " exclusive_caps=1,1 "'
    check "OBS's device sorts before ours (OBS takes the first idle loopback in version order)" '[ "$OBSNR" -lt "$NR" ]'
    check "nothing else in modprobe.d (no install or remove commands)" '[ "$(grep -v "^#" "$X/etc/modprobe.d/lordos-studio.conf" | grep -c .)" = 1 ]'
    check "modules-load.d loads v4l2loopback only" '[ "$(grep -v "^#" "$X/etc/modules-load.d/lordos-studio.conf")" = v4l2loopback ]'
    check "both are conffiles" '[ "$(cat "$X/DEBIAN/conffiles")" = "/etc/modprobe.d/lordos-studio.conf
/etc/modules-load.d/lordos-studio.conf" ]'
    check "files owned by root, not writable by others" '[ -z "$(dpkg-deb -c "$D" | awk "{print \$1, \$2}" | grep -v "root/root" )" ] && ! dpkg-deb -c "$D" | grep -q "^-.......w"'
    check "maintainer scripts parse" 'sh -n "$X/DEBIAN/postinst" && sh -n "$X/DEBIAN/postrm"'
    check "postinst runs nothing but modprobe" '! grep -Eq "mokutil|dkms |apt|curl|wget|rmmod|insmod|v4l2loopback-ctl|/dev/v4l2loopback" "$X/DEBIAN/postinst"'
    C="$T/case-$A"
    fake_sys "$C" 0 0 0 0; run_postinst "$X/DEBIAN/postinst" "$C" configure
    check "install, module not loaded: loads it" '[ "$(cat "$C/calls")" = v4l2loopback ]'
    fake_sys "$C" 1 1 0 0; run_postinst "$X/DEBIAN/postinst" "$C" configure 0.8.9.4
    check "update from one device, unused: reloads it" '[ "$(tr "\n" "|" < "$C/calls")" = "-r v4l2loopback|v4l2loopback|" ]'
    fake_sys "$C" 1 1 0 1; run_postinst "$X/DEBIAN/postinst" "$C" configure 0.8.9.4
    check "update from one device, in use: leaves it (next restart)" '[ ! -s "$C/calls" ]'
    fake_sys "$C" 1 1 1 0; run_postinst "$X/DEBIAN/postinst" "$C" configure 0.8.9.4
    check "update with both devices already there: nothing" '[ ! -s "$C/calls" ]'
    fake_sys "$C" 1 0 0 0; run_postinst "$X/DEBIAN/postinst" "$C" configure 0.8.9.4
    check "a v4l2loopback someone else loaded (no LordOS Studio device): left alone" '[ ! -s "$C/calls" ]'
    fake_sys "$C" 1 1 0 0; run_postinst "$X/DEBIAN/postinst" "$C" configure
    check "loaded, first install (no old version): left alone" '[ ! -s "$C/calls" ]'
    fake_sys "$C" 1 1 0 0; sed -i "s/devices=2/devices=1/" "$C/etc/modprobe.d/lordos-studio.conf"; run_postinst "$X/DEBIAN/postinst" "$C" configure 0.8.9.4
    check "update, the person kept an edited one-device file: left alone" '[ ! -s "$C/calls" ]'
    fake_sys "$C" 1 1 0 0; run_postinst "$X/DEBIAN/postinst" "$C" abort-upgrade 0.8.9.4
    check "anything but configure: nothing" '[ ! -s "$C/calls" ]'
done

# nothing in LordOS writes OBS's device: its number and label are named only in mstudiohub.h (constants nothing opens),
# the hub's writer opens STUDIO_LOOPBACK_DEV alone, and the helper refuses any loopback whose card isn't ours
echo "sources:"
USES=$(grep -rlE "video$OBSNR|STUDIO_OBS_DEV|STUDIO_OBS_CARD|OBS Virtual Camera" server/src sdk apps themes runtime 2>/dev/null | tr '\n' ' ')
check "OBS's device is named only in mstudiohub.h ($USES)" '[ "$USES" = "server/src/media/mstudiohub.h " ]'
check "lord-server opens the loopback only by STUDIO_LOOPBACK_DEV" \
    '[ "$(grep -c "open(STUDIO_LOOPBACK_DEV" server/src/media/mstudio.c)" = 1 ] && [ "$(grep -rl "STUDIO_LOOPBACK_DEV" server/src | sort | tr "\n" " ")" = "server/src/media/mstudio.c server/src/media/mstudiohub.h " ]'
check "lord-studio checks the card too (stcam_hub)" 'grep -q "strcmp((const char \*)cap.card, STUDIO_LOOPBACK_CARD)" server/src/media/studio/stcam.c'
[ "$fails" = 0 ] || { echo "studio-package-test: $fails failures"; exit 1; }
echo "studio-package-test: ok"
