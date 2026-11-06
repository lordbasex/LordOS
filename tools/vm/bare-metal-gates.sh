#!/bin/sh
# bare-metal-gates.sh — Machines' three release gates (disk integrity, failure recovery, leaks; docs/VIRTUALIZATION.md
# 12 and 3.9, "Bare-metal gates") on a physical amd64 PC with VT-x or AMD-V, from the LordOS live USB or an installed
# LordOS, and one results file to attach to the release notes or an issue. Runs as the session user, never as root.
#
#   sh tools/vm/bare-metal-gates.sh [--lord-vm DIR] [--image FILE] [--work DIR] [--cycles N] [--out FILE]
#
#   --lord-vm DIR  folder with lord-vm (default: the installed one, /usr/lib/lordos/bin)
#   --image FILE   a Debian 13 "nocloud" amd64 qcow2 (default: downloaded from cloud.debian.org into the work
#                  folder and checked against Debian's SHA512SUMS)
#   --work DIR     where the machines are made; needs about 15 GB free on a real disk. On the live USB /var/tmp is
#                  in memory: give a folder on an internal disk or a USB SSD (default: /var/tmp)
#   --cycles N     start/stop cycles of the leak gate (default 100)
#   --out FILE     the results file (default: ./machines-gates-<host>-<date>.txt)
#
# Needs: /dev/kvm usable by this user (Machines' "Turn On" or the kvm group), qemu-system-x86, qemu-utils, passt and
# ovmf (Machines' Requirements page installs them), python3, curl, and the network (the recovery gate fetches from
# deb.debian.org in the guest). The gate scripts are next to this one in the source tree (server/tests/vm_*.sh).
# Exit status: 0 when the three gates pass, 1 when one fails, 2 when the PC can't run them.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tests=$(cd "$here/../../server/tests" 2>/dev/null && pwd) || { echo "run it from the LordOS source tree (server/tests is missing)"; exit 2; }
BIN=/usr/lib/lordos/bin IMG= WORK=/var/tmp N=100 OUT=
while [ $# -gt 0 ]; do
    case $1 in
        --lord-vm) BIN=$2; shift 2 ;;
        --image) IMG=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --cycles) N=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done
stamp=$(date -u +%Y%m%dT%H%M%SZ)
OUT=${OUT:-$PWD/machines-gates-$(hostname)-$stamp.txt}
R=$WORK/lordos-gates-$stamp
die() { echo "can't run the gates: $*"; rm -rf "$R"; exit 2; }   # $R is only this run's folder

# ---- is this PC fit to run them ----
[ "$(id -u)" = 0 ] && die "run as your normal user, not root"
[ "$(uname -m)" = x86_64 ] || die "this is $(uname -m): the bare-metal gates are for amd64 (the Pi has its own run, 3.9)"
virt=$(systemd-detect-virt 2>/dev/null || echo none)
# GATES_ALLOW_VM=1: a dry run of this script on a virtual machine (its results are nested numbers, not bare metal)
[ "$virt" = none ] || [ "${GATES_ALLOW_VM:-}" = 1 ] || die "this is a virtual machine ($virt): the nested numbers are already in 3.9; run it on the PC itself"
grep -qwE 'vmx|svm' /proc/cpuinfo || die "the CPU offers no VT-x or AMD-V (or it is off in the firmware settings)"
[ -r /dev/kvm ] && [ -w /dev/kvm ] || die "/dev/kvm isn't usable by $(id -un): open Machines and press Turn On, or log in again after it"
[ -x "$BIN/lord-vm" ] || die "no lord-vm in $BIN (give --lord-vm DIR)"
for c in qemu-img qemu-system-x86_64 passt python3 curl sha512sum; do command -v "$c" >/dev/null || die "$c is missing: Machines → What's Missing installs the engine (or: sudo apt install qemu-system-x86 qemu-utils passt ovmf python3 curl)"; done
[ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] || echo "note: no OVMF (ovmf package): the gates' machines use SeaBIOS"
mkdir -p "$R" || die "can't make $R"
free_gb=$(df -Pk "$R" | awk 'NR == 2 { print int($4 / 1048576) }')
[ "$free_gb" -ge 15 ] || die "$free_gb GB free in $WORK: the full clones need about 15 GB (give --work DIR on a bigger disk)"
mem_gb=$(awk '/MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
[ "$mem_gb" -ge 2 ] || die "$mem_gb GB of memory available: the gates' machines need 2 GB"
case $(df -PT "$R" | awk 'NR == 2 { print $2 }') in tmpfs|overlay|squashfs) die "$WORK is in memory on this system: give --work DIR on a real disk" ;; esac

# ---- the guest ----
if [ -z "$IMG" ]; then
    base=https://cloud.debian.org/images/cloud/trixie/latest
    IMG=$R/debian-13-nocloud-amd64.qcow2
    echo "-- downloading Debian 13's nocloud image (about 400 MB)"
    curl -fsSL --proto =https -o "$R/SHA512SUMS" "$base/SHA512SUMS" && curl -fL --proto =https -o "$IMG" "$base/debian-13-nocloud-amd64.qcow2" ||
        die "the guest image couldn't be downloaded (give --image FILE)"
    want=$(awk '$2 == "debian-13-nocloud-amd64.qcow2" { print $1 }' "$R/SHA512SUMS")
    [ -n "$want" ] && [ "$(sha512sum "$IMG" | cut -d' ' -f1)" = "$want" ] || die "the guest image's SHA-512 isn't Debian's"
fi
[ -f "$IMG" ] || die "no image at $IMG"

# ---- the results file: this PC, then each gate ----
{
    echo "LordOS Machines release gates, $([ "$virt" = none ] && echo 'bare metal' || echo "NOT bare metal: a $virt virtual machine (GATES_ALLOW_VM)")"
    echo "date:      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "system:    $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-?}"), LordOS $(dpkg-query -W -f '${Version}' lordos 2>/dev/null || echo 'not installed')$( [ -d /run/live ] && echo ', live USB')"
    echo "kernel:    $(uname -r)"
    echo "cpu:       $(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1), $(nproc) threads, $(grep -qw vmx /proc/cpuinfo && echo VT-x || echo AMD-V)"
    echo "board:     $(cat /sys/class/dmi/id/sys_vendor /sys/class/dmi/id/product_name 2>/dev/null | tr '\n' ' ')"
    echo "memory:    $(awk '/MemTotal/ { printf "%.1f GB", $2 / 1048576 }' /proc/meminfo)"
    echo "disk:      $(df -PT "$R" | awk 'NR == 2 { print $2 }') on $(df -P "$R" | awk 'NR == 2 { print $1 }')"
    echo "qemu:      $(qemu-system-x86_64 --version | head -1)"
    echo "passt:     $(passt --version 2>&1 | head -1)"
    echo "lord-vm:   $BIN/lord-vm, sha256 $(sha256sum "$BIN/lord-vm" | cut -d' ' -f1)"
    echo "guest:     $(basename "$IMG"), sha256 $(sha256sum "$IMG" | cut -d' ' -f1)"
} >"$OUT"
summary=
rc_all=0
gate() {         # NAME SCRIPT ARGS…
    name=$1; shift
    echo "-- $name ($(date +%H:%M:%S))"
    t0=$(date +%s)
    log=$R/$name.log
    sh "$@" >"$log" 2>&1; rc=$?
    secs=$(( $(date +%s) - t0 ))
    line=$(grep '^== vm-' "$log" | tail -1)
    [ $rc = 0 ] || rc_all=1
    res=$([ $rc = 0 ] && echo PASS || echo FAIL)
    summary="$summary$res  $name: ${line#== } ($secs s)
"
    echo "$res  $name: ${line#== } ($secs s)"
    { echo; echo "===== $name: $res in $secs s"; cat "$log"; } >>"$OUT"
}
gate integrity "$tests/vm_integrity.sh" "$BIN" "$IMG" "$R/int"
gate recovery "$tests/vm_recovery.sh" "$BIN" "$IMG" "$R/rec"
gate leak "$tests/vm_leak.sh" "$BIN" "$N" "$R/leak"
{ echo; echo "===== summary"; printf '%s' "$summary"; } >>"$OUT"
rm -rf "$R"
echo
printf '%s' "$summary"
echo "results: $OUT (attach it)"
exit $rc_all
