#!/bin/sh
# vm_multihead.sh — All Displays (docs/VIRTUALIZATION.md 5.4) on a real KVM guest: a machine with "heads": 2 starts with
# a virtio-gpu of two outputs; lordvm-dev plays lord-server's part with LORDVM_DEV_HEADS=2: one lord-vmdisplay --heads 2
# (QEMU's peer-to-peer D-Bus display serves one connection) with a channel per output, as js_vm.c starts it for
# lord.vm.allDisplays. Checks:
#   - lord-vm --set --heads: 2 written under schema 19, 5 refused, 1 takes the key away;
#   - QEMU's command line has max_outputs=2 (and none with one display);
#   - lord-vmdisplay registers a listener on Console_0 and Console_1, each checked to be that output, the sound on
#     output 0 only;
#   - asked for a size and given its place beside output 0 (VD_LAYOUT), the guest turns its second output on (its DRM
#     connector Virtual-2 connected and enabled, read by a unit on the serial line) and draws on it: both pictures arrive,
#     at their sizes, not black;
#   - output 1's window closing (VD_CLOSE on its channel) stops output 1 only: output 0's frames go on;
#   - --heads 2 on a machine with one output fails cleanly (Console_1 isn't there), the machine runs on.
# Runs as a normal user with /dev/kvm, in its own user and mount namespaces with a private home and runtime folder (the
# person's ~/Machines and session are never seen).
#
#   sh tests/vm_multihead.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
# GUEST_IMAGE: a Debian 13 "nocloud" qcow2 (copied, never changed). Exit 77 when something it needs is missing.
# MH_SHOTS=DIR keeps the two outputs' pictures there.
set -u
BIN=$(cd "${1:-}" 2>/dev/null && pwd) || { echo "usage: vm_multihead.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]"; exit 2; }
IMG=${2:-}
W=${3:-/var/tmp/lordos-multihead/run.$$}
[ -f "$IMG" ] || { echo "skipped: needs a Debian 13 nocloud qcow2"; exit 77; }
[ -x "$BIN/lord-vm" ] && [ -x "$BIN/lord-vmdisplay" ] && [ -x "$BIN/lordvm-dev" ] || { echo "skipped: needs lord-vm, lord-vmdisplay and lordvm-dev"; exit 77; }
[ -r /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v unshare >/dev/null 2>&1 || { echo "skipped: needs KVM, QEMU and unshare"; exit 77; }
[ "$(uname -m)" = x86_64 ] || { echo "skipped: amd64 only (virtio-vga)"; exit 77; }
if [ -z "${MH_INNER:-}" ]; then
    # the namespaces: root in the first only to mount, then this account again
    [ -e "$W" ] && { echo "$W exists: give a new work folder"; exit 2; }
    mkdir -p "$W/home" "$W/run" "$W/fake" && chmod 700 "$W/home" "$W/run" || exit 2
    REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
    RT=/run/user/$(id -u)
    unshare -Urm sh -c "mount --bind '$W/fake' '$REALHOME' && { [ -d '$RT' ] && mount -t tmpfs -o mode=700 tmpfs '$RT' || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) env -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS -u WAYLAND_DISPLAY -u DISPLAY MH_INNER=1 HOME='$W/home' XDG_RUNTIME_DIR='$W/run' sh '$0' '$BIN' '$IMG' '$W'"
    rc=$?
    [ "${KEEP:-}" = 1 ] || rm -rf "$W"
    exit $rc
fi
VM="$BIN/lord-vm"
D=$W/home/Machines/twohead.lordvm
mkdir -p "$W/home/Machines"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
t()   { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now() { date +%s; }
field() { "$VM" --status "$D" --json 2>/dev/null | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
wait_state() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do [ "$(field state)" = "$1" ] && return 0; sleep 0.5; done; return 1; }
wait_in() { end=$(( $(now) + $3 )); while [ "$(now)" -lt "$end" ]; do grep -aq "$2" "$1" 2>/dev/null && return 0; sleep 0.5; done; return 1; }
png_field() { grep "\"cmd\":\"png\"$2" "$W/dev.out" | tail -1 | sed -n "s/.*\"$1\":\([0-9.]*\).*/\1/p"; }
cleanup() { "$VM" --poweroff "$D" >/dev/null 2>&1; [ -n "${P:-}" ] && kill "$P" 2>/dev/null; }
trap cleanup EXIT
trap '' PIPE
echo "== $(uname -m), kernel $(uname -r), $(qemu-system-x86_64 --version | head -1)"

"$VM" --create "$D" --name twohead --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1
t "create" '[ -f "$D/machine.json" ]' "$(cat "$W/create.json")"
"$VM" --set "$D" --heads 5 >/dev/null 2>&1; rc5=$?
t "--heads 5 refused" '[ $rc5 != 0 ] && ! grep -q "\"heads\": " "$D/machine.json"'
"$VM" --set "$D" --heads 2 >"$W/set.json" 2>&1
t "--heads 2: written under schema 19" 'grep -q "\"heads\": 2" "$D/machine.json" && grep -q "\"schema\": \(19\|[2-9][0-9]\)" "$D/machine.json"' "$(cat "$W/set.json")"

# a unit on the serial line: the guest's DRM outputs, every 2 s
UNIT=$(cat <<'EOF'
[Unit]
After=local-fs.target
[Service]
ExecStart=/bin/sh -c 'while :; do s=""; for c in /sys/class/drm/card*-Virtual-*; do s="$s $(basename $c)=$(cat $c/status)/$(cat $c/enabled)/$(head -n1 $c/modes 2>/dev/null)"; done; echo "<3>LORDVM-HEADS$s" >/dev/kmsg; sleep 2; done'
EOF
)
DROP=$(printf '[Unit]\nWants=lordvm-heads.service\n')
umask 077
{ echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"
  echo "passwd.plaintext-password.root=mh-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
  echo "binary:systemd.extra-unit.lordvm-heads.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
umask 022

mkfifo "$W/dev.in"
LORDVM_DEV_HEADS=2 "$BIN/lordvm-dev" run "$D" --keep-running --credentials-file "$W/creds" <"$W/dev.in" >"$W/dev.out" 2>"$W/dev.err" &
P=$!
exec 7>"$W/dev.in"
t "start: running" 'wait_state running 60' "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null)"
QPID=$(field qemu_pid)
t "QEMU: virtio-vga with two outputs" 'tr "\0" " " </proc/$QPID/cmdline 2>/dev/null | grep -q "virtio-vga,id=video0,addr=0x4,max_outputs=2"'
t "lord-vmdisplay: a listener on Console_0 and on Console_1" 'wait_in "$W/dev.err" "console 1 " 20 && grep -q "console 0 " "$W/dev.err"' "$(grep lord-vmdisplay "$W/dev.err" | tail -3)"
t "lord-vmdisplay: the sound once (output 0)" '[ "$(grep -c "RegisterOutListener: listener registered" "$W/dev.err")" = 1 ]'
t "output 0 and output 1 ready" 'wait_in "$W/dev.out" "display_ready\",\"head\":1" 20 && grep -q "\"ev\":\"display_ready\",\"w\"" "$W/dev.out"'
t "the guest booted (its unit reports)" 'wait_in "$D/log/serial.log" LORDVM-HEADS 180' "$(tail -3 "$D/log/serial.log" 2>/dev/null)"
# the desktop the outputs make: 1024x768 and 800x600 side by side (lord.vm.allDisplays)
echo "layout 0 0 1024 768 1824 768" >&7; echo "resize 1024 768" >&7
echo "h1 layout 1024 0 800 600 1824 768" >&7; echo "h1 resize 800 600" >&7
t "the guest turned its second output on" 'wait_in "$D/log/serial.log" "Virtual-2=connected/enabled" 60' "$(grep -a LORDVM-HEADS "$D/log/serial.log" | tail -1)"
t "output 1: first frame" 'wait_in "$W/dev.out" "first_frame\",\"head\":1" 30'
sleep 8
echo "png $W/head0.png" >&7; echo "h1 png $W/head1.png" >&7
sleep 3
lit0=$(png_field lit_pct ',"w"'); lit1=$(png_field lit_pct ',"head":1')
w1=$(png_field w ',"head":1'); h1=$(png_field h ',"head":1')
echo "      output 0 lit ${lit0:-?} %, output 1 lit ${lit1:-?} % at ${w1:-?}x${h1:-?}; guest:$(grep -a LORDVM-HEADS "$D/log/serial.log" | tail -1 | sed 's/.*LORDVM-HEADS//')"
t "output 0: the guest draws on it" '[ -n "$lit0" ] && awk "BEGIN { exit !($lit0 > 0.2) }"'
t "output 1: the guest draws on it" '[ -n "$lit1" ] && awk "BEGIN { exit !($lit1 > 0.2) }"'
t "output 1: at the size asked for" '[ "${w1:-}x${h1:-}" = 800x600 ]'
[ -n "${MH_SHOTS:-}" ] && cp "$W/head0.png" "$W/head1.png" "$MH_SHOTS/" 2>/dev/null
# output 1's window goes: output 0 goes on
before=$(grep -c '"ev":"display_stats"' "$W/dev.out")
echo "h1 close" >&7
t "output 1 closed alone (the helper says so)" 'wait_in "$W/dev.err" "output 1.s window went" 10'
t "the guest turned its second output off" 'sleep 5; grep -a LORDVM-HEADS "$D/log/serial.log" | tail -1 | grep -q "Virtual-2=disconnected\|Virtual-2=connected/disabled"' "$(grep -a LORDVM-HEADS "$D/log/serial.log" | tail -1)"
echo "type x" >&7
t "output 0 goes on (its frames keep coming)" 'sleep 7; [ "$(grep -c "\"ev\":\"display_stats\"" "$W/dev.out")" -gt "$before" ] && ! grep -q "\"ev\":\"vmdisplay-gone\"}" "$W/dev.out"'
t "the helper refused nothing" '! grep -q "lord-vmdisplay: refused" "$W/dev.err"' "$(grep "lord-vmdisplay: refused" "$W/dev.err" | head -3)"
exec 7>&-
kill $P 2>/dev/null; P=
"$VM" --poweroff "$D" >/dev/null 2>&1
wait_state stopped 30

# one display again: --heads 2 has no Console_1 to find
"$VM" --set "$D" --heads 1 >/dev/null 2>&1
t "--heads 1: the key is gone" '! grep -q "\"heads\": " "$D/machine.json"'
LORDVM_DEV_HEADS=2 "$BIN/lordvm-dev" run "$D" --keep-running <"$W/dev.in" >"$W/dev2.out" 2>"$W/dev2.err" &
P=$!
exec 7>"$W/dev.in"
wait_state running 60
t "one display: no max_outputs" '! tr "\0" " " </proc/$(field qemu_pid)/cmdline 2>/dev/null | grep -q max_outputs'
t "--heads 2 on one output: refused, the helper ends" 'wait_in "$W/dev2.err" "console 1 isn.t there" 20 && wait_in "$W/dev2.out" "vmdisplay-gone" 10' "$(grep lord-vmdisplay "$W/dev2.err" | tail -2)"
t "the machine runs on" '[ "$(field state)" = running ]'
exec 7>&-
echo "== $pass passed, $fail failed"
[ $fail = 0 ]
