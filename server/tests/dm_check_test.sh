#!/bin/sh
# dm_check_test.sh — packaging/bin/lordos-dm-check over fake system trees (--root): another display manager enabled next
# to lordos.service is found the ways Debian's display managers enable themselves (the display-manager.service alias,
# a target's wants), a masked one or LordOS itself is not, and --warn speaks only when lordos.service is enabled too
# (GitHub #187). Run from server/ by tests/run_suites.sh.
set -u
C=$(cd "$(dirname "$0")/../.." && pwd)/packaging/bin/lordos-dm-check
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-dmcheck.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0 n=0
S=etc/systemd/system

# tree NAME: an empty system tree; link PATH TARGET: a symlink in it
tree() { R="$T/$1"; mkdir -p "$R/$S"; }
link() { mkdir -p "$(dirname "$R/$1")"; ln -s "$2" "$R/$1"; }
# expect NAME WANT_STATUS WANT_OUTPUT ARGS...: runs the check on the current tree
expect() {
    name=$1 want_rc=$2 want=$3; shift 3
    n=$((n + 1))
    out=$(sh "$C" --root "$R" "$@" 2>&1); rc=$?
    if [ "$rc" != "$want_rc" ]; then echo "FAIL $name: exit $rc, want $want_rc ($out)"; fails=$((fails + 1)); return; fi
    case "$want" in
        "") [ -z "$out" ] || { echo "FAIL $name: printed: $out"; fails=$((fails + 1)); } ;;
        *) case "$out" in *"$want"*) ;; *) echo "FAIL $name: want \"$want\" in: $out"; fails=$((fails + 1)) ;; esac ;;
    esac
}

tree lordos-only
link $S/multi-user.target.wants/lordos.service /usr/lib/systemd/system/lordos.service
expect "LordOS alone: nothing" 0 ""
expect "LordOS alone: --warn says nothing" 0 "" --warn

tree issue-187                       # the ISO's lordos.service, and LightDM installed with another desktop
link $S/multi-user.target.wants/lordos.service /usr/lib/systemd/system/lordos.service
link $S/display-manager.service /lib/systemd/system/lightdm.service
expect "LightDM through display-manager.service" 1 "lightdm.service"
expect "--warn names it and both ways out" 1 "sudo systemctl disable lightdm.service" --warn
expect "--warn: keeping the other one instead" 1 "sudo systemctl disable lordos.service" --warn

tree two-others                      # GDM through the alias, SDDM wanted by graphical.target
link $S/multi-user.target.wants/lordos.service /usr/lib/systemd/system/lordos.service
link $S/display-manager.service /lib/systemd/system/gdm3.service
link $S/graphical.target.wants/sddm.service /usr/lib/systemd/system/sddm.service
expect "both are listed" 1 "gdm3.service
sddm.service"
expect "--warn names both on one line" 1 "next to LordOS's: gdm3.service sddm.service." --warn

tree kept-dm                         # install.sh --keep-display-manager: GDM stays, lordos.service is not enabled
link $S/display-manager.service /lib/systemd/system/gdm3.service
expect "the other one is found" 1 "gdm3.service"
expect "--warn: LordOS not enabled, the person's choice, nothing said" 0 "" --warn

tree masked                          # the other one masked: it can't start
link $S/multi-user.target.wants/lordos.service /usr/lib/systemd/system/lordos.service
link $S/display-manager.service /lib/systemd/system/lightdm.service
link $S/lightdm.service /dev/null
expect "a masked display manager doesn't count" 0 "" --warn

tree alias-lordos                    # display-manager.service pointing at LordOS itself
link $S/display-manager.service /usr/lib/systemd/system/lordos.service
expect "LordOS as display-manager.service is not another one" 0 ""

tree empty                           # an image being built: nothing enabled yet
expect "an empty tree: nothing" 0 "" --warn

n=$((n + 1))
if sh "$C" --bogus >/dev/null 2>&1; then echo "FAIL an unknown option is refused"; fails=$((fails + 1)); fi

echo "dm-check: $n checks, $fails failed"
[ "$fails" = 0 ]
