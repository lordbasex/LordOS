#!/bin/sh
# vm_guest_tools_e2e.sh — the LordOS Guest Tools disc end to end on a real guest (docs/VIRTUALIZATION.md 6.17): a Linux
# machine whose system has no qemu-guest-agent waits for the agent; VAG_DISC puts lord-vm's own copy of the disc in its
# CD drive; "the person" (this script, typing on the guest's own console as a person would) runs install.sh from it;
# qemu-guest-agent connects and lord-vm takes the disc out by itself. Then Install (VAG_INSTALL, through
# qemu-guest-agent) runs on the same guest: the existing automatic path. Runs as a normal user with KVM, never as root.
#
#   sh tests/vm_guest_tools_e2e.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
#
# BUILD_DIR holds lord-vm, lord-vmdisplay, lordvm-dev, lordos-guest and lordos-guest-tools.iso (make -C server; lord-vm
# --dev-qmp takes those two from LORDVM_HELPER and LORDVM_GUESTTOOLS). GUEST_IMAGE: a Debian 13 "nocloud" qcow2 of this
# computer's architecture (no qemu-guest-agent; root's password comes as a systemd credential), copied, never changed.
# The screenshots of each step are kept in WORK_DIR/shots (WORK_DIR default /var/tmp/lgt.$$; removed at the end unless
# KEEP=1). One line per check: PASS or FAIL; exit 1 on any FAIL. (The installed LordOS 0.8.9 guest of 6.17 was driven by
# hand with lordvm-dev the same way: its desktop's greeter, then a text console and the guest's own sudo prompt.)
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${3:-/var/tmp/lgt.$$}
D=$W/m.lordvm
VM="$BIN/lord-vm"
pass=0 fail=0
ok()   { pass=$((pass + 1)); echo "PASS  $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
t()    { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now()  { date +%s; }
status_field() { "$VM" --status "$D" --json | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
wait_log() {     # TEXT SECONDS: lord-vm's log says TEXT
    end=$(( $(now) + $2 ))
    while [ "$(now)" -lt "$end" ]; do grep -aq "$1" "$D/log/lord-vm.log" 2>/dev/null && return 0; sleep 1; done
    return 1
}
wait_out() {     # TEXT SECONDS: lordvm-dev printed TEXT
    end=$(( $(now) + $2 ))
    while [ "$(now)" -lt "$end" ]; do grep -aq "$1" "$W/dev.out" 2>/dev/null && return 0; sleep 1; done
    return 1
}
cmd() { printf '%s\n' "$*" >&7; sleep 0.3; }
shot() { cmd "png $W/shots/$1.png"; sleep 1; }
type_line() { cmd "type $1\\n"; }
cleanup() {
    "$VM" --poweroff "$D" >/dev/null 2>&1
    [ -n "${DEV_PID:-}" ] && kill "$DEV_PID" 2>/dev/null
    [ "${KEEP:-}" = 1 ] || rm -rf "$W"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
trap '' PIPE
[ "$(id -u)" = 0 ] && { echo "run as a normal user"; exit 2; }
[ -f "$BIN/lordos-guest-tools.iso" ] || { echo "no $BIN/lordos-guest-tools.iso"; exit 2; }
[ -e "$W" ] && { echo "$W exists"; exit 2; }
mkdir -p "$W/shots" "$W/home" || exit 2
export HOME="$W/home"
unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS

# ---- the machine ----
"$VM" --create "$D" --name guest-tools-test --import "$IMG" --vcpus 2 --memory 2048 --agent on --clipboard both >"$W/create.json" 2>&1
t "create: a machine with the guest agent on" 'grep -q "\"agent\": true" "$D/machine.json"' "$(cat "$W/create.json")"
PW="lgt-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
umask 077
: >"$W/creds"
printf 'firstboot.locale=C.UTF-8\nfirstboot.keymap=us\nfirstboot.timezone=UTC\npasswd.plaintext-password.root=%s\n' "$PW" >"$W/creds"
umask 022

mkfifo "$W/dev.in"
LORDVM_HELPER="$BIN/lordos-guest" LORDVM_GUESTTOOLS="$BIN/lordos-guest-tools.iso" "$BIN/lordvm-dev" run "$D" --dev-qmp --keep-running --credentials-file "$W/creds" \
    <"$W/dev.in" >"$W/dev.out" 2>"$W/dev.err" &
DEV_PID=$!
exec 7>"$W/dev.in"
end=$(( $(now) + 60 )); while [ "$(now)" -lt "$end" ] && [ "$(status_field state)" != running ]; do sleep 0.5; done
t "start: running" '[ "$(status_field state)" = running ]' "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null)"

# ---- no agent: Machines waits ----
sleep 40
shot 01-booted
t "the guest has no qemu-guest-agent: the agent is waiting" '! grep -q "agent: connected" "$D/log/lord-vm.log" && grep -q "\"agent\":\"waiting\"" "$D/run.json"' "$(cat "$D/run.json")"

# ---- the disc ----
cmd "agent disc"
t "the LordOS Guest Tools disc goes in, checked" 'wait_log "Guest Tools disc goes into the machine.s drive ([a-z0-9]*, checked)" 10 && wait_log "Guest Tools disc is in the machine.s drive" 10' "$(tail -3 "$D/log/lord-vm.log")"
t "lord-vm reports it (VAC_TOOLS_DISC in the agent's caps)" 'sleep 2; [ -n "$(grep "\"ev\":\"agent\"" "$W/dev.out" | tail -1 | sed -n "s/.*caps\":\([0-9]*\).*/\1/p" | awk "{ if (int(\$1 / 2097152) % 2) print }")" ]' "$(grep '"ev":"agent"' "$W/dev.out" | tail -2)"
cmd "agent disc"
t "asked again: answered, nothing changes" 'wait_out "\"text\":\"inserted\"" 5' "$(tail -2 "$W/dev.out")"

# ---- the person runs install.sh in the guest, as root on a text console ----
type_line root; sleep 2
type_line "$PW"; sleep 4
shot 02-logged-in
type_line "mount -r /dev/sr0 /mnt && ls /mnt && sh /mnt/install.sh 2>&1 | tail -n 6"
t "qemu-guest-agent connects once install.sh ran" 'wait_log "agent: connected" 420' "$(tail -5 "$D/log/lord-vm.log")"
shot 03-installed
t "the disc comes out by itself" 'wait_log "Guest Tools disc came out of the drive" 30' "$(grep -a "Guest Tools\|cd:" "$D/log/lord-vm.log" | tail -4)"
t "the caps no longer say the disc is in" '[ -z "$(grep "\"ev\":\"agent\"" "$W/dev.out" | tail -1 | sed -n "s/.*caps\":\([0-9]*\).*/\1/p" | awk "{ if (int(\$1 / 2097152) % 2) print }")" ]' "$(grep '"ev":"agent"' "$W/dev.out" | tail -1)"

# ---- Install through qemu-guest-agent (the path for guests that have it): lordos-guest again, by the fixed script ----
cmd "agent install"
t "Install through qemu-guest-agent works on this guest" 'wait_log "agent: lordos-guest installed" 600' "$(grep -a "agent:" "$D/log/lord-vm.log" | tail -4)"
cmd "agent disc"
t "the disc isn't offered while qemu-guest-agent can install" 'wait_out "qemu-guest-agent answers: Install goes through it" 5' "$(tail -2 "$W/dev.out")"
shot 05-done
cp "$D/log/lord-vm.log" "$W/lord-vm.log" 2>/dev/null
echo "== $pass passed, $fail failed (screenshots: $W/shots)"
[ "$fail" = 0 ]
