#!/bin/sh
# Bridged (docs/VIRTUALIZATION.md 6.5) end to end on a scratch dummy interface, never the real wired card: a real KVM boot
# of a Debian 13 guest whose card is a macvtap made by vm-net-bridge's own code (build/vm-net-cli-dummy: the admin
# helper's vmnet_admin.c, with only the "is it a wired card" check told a dummy named lordtest0 is one), on lordtest0.
# A macvlan peer on the same dummy, in a scratch network namespace, answers the guest. Checks: the real check refuses the
# dummy; the macvtap has the machine's MAC, the alias, no IPv6 address, /dev/tapN the person's 0600; lord-vm opens it and
# the guest's card has the machine's MAC and reaches the peer; the cable pulled and plugged in (VM_NETCABLE) is seen by
# the guest; vm-net-unbridge refuses while the machine runs and removes the macvtap once it's off. Root only for the
# interfaces (sudo -n); lord-vm and QEMU run as the person. No firewall rule is read or changed; everything is deleted.
#   sh tests/vm_bridge_dummy.sh BUILD_DIR DEBIAN_NOCLOUD_QCOW2       (from server/, a KVM host; not part of make test)
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${TMPDIR:-/var/tmp}/lordos-m100-bridge.$$
D=$W/m.lordvm
VM="$BIN/lord-vm"
DUMMY=lordtest0
PEER=lordtestp
NS=lordtest
PEER_IP=192.168.231.1
GUEST_IP=192.168.231.2
mkdir -p "$W/home" || exit 2
export HOME="$W/home"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
t()   { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now() { date +%s; }
status_field() { "$VM" --status "$D" --json | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
wait_state() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do [ "$(status_field state)" = "$1" ] && return 0; sleep 0.5; done; return 1; }
wait_serial() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do grep -aq "$1" "$D/log/serial.log" 2>/dev/null && return 0; sleep 1; done; return 1; }
cleanup() {
    "$VM" --poweroff "$D" >/dev/null 2>&1
    [ -n "${DEV_PID:-}" ] && kill "$DEV_PID" 2>/dev/null
    sleep 1
    [ -n "${ID:-}" ] && sudo -n "$BIN/vm-net-cli-dummy" vm-net-unbridge "$ID" >/dev/null 2>&1
    sudo -n ip netns del "$NS" 2>/dev/null
    sudo -n ip link del "$DUMMY" 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
trap '' PIPE
[ -c /dev/kvm ] && [ -w /dev/kvm ] || { echo "skipped: needs KVM"; exit 0; }
sudo -n true 2>/dev/null || { echo "skipped: needs sudo -n (root only for the scratch interfaces)"; exit 0; }
[ -x "$BIN/vm-net-cli" ] && [ -x "$BIN/vm-net-cli-dummy" ] || { echo "skipped: needs build/vm-net-cli and build/vm-net-cli-dummy"; exit 0; }
if ip link show "$DUMMY" >/dev/null 2>&1; then echo "$DUMMY is already there: not touching it"; exit 2; fi

"$VM" --create "$D" --name bridge-test --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1
t "create" '[ -f "$D/machine.json" ]' "$(cat "$W/create.json")"
"$VM" --set "$D" --network bridged --bridge "$DUMMY" >"$W/set.json" 2>&1
t "set: Bridged on $DUMMY" 'grep -q "\"network\":\"bridged\"" "$W/set.json" && grep -q "\"bridge\": *\"$DUMMY\"" "$D/machine.json"' "$(cat "$W/set.json")"
ID=$(sed -n 's/.*"id": "\([^"]*\)".*/\1/p' "$D/machine.json" | head -n1)
MAC=$(sed -n 's/.*"mac": "\([^"]*\)".*/\1/p' "$D/machine.json" | head -n1)

# the scratch link: a dummy, and a macvlan peer on it (bridge mode: it and the guest's macvtap reach each other) in a
# network namespace of its own, so nothing of this computer's own network sees either
sudo -n ip link add "$DUMMY" type dummy && sudo -n ip link set "$DUMMY" up
t "the scratch dummy $DUMMY is up" 'ip link show "$DUMMY" | grep -q "state UNKNOWN\|state UP"'
sudo -n ip netns add "$NS"
sudo -n ip link add "$PEER" link "$DUMMY" type macvlan mode bridge
sudo -n ip link set "$PEER" netns "$NS"
sudo -n ip -n "$NS" addr add "$PEER_IP/24" dev "$PEER"
sudo -n ip -n "$NS" link set "$PEER" up

OUT=$(sudo -n "$BIN/vm-net-cli" vm-net-bridge "$ID $DUMMY $MAC" 2>&1)
t "the real check refuses a dummy (no device behind it)" 'echo "$OUT" | grep -q "vm.bridgeNotWired"' "$OUT"
t "and made nothing" '! ip -o link | grep -q "lordos-vm $(id -u) $ID"'
OUT=$(sudo -n "$BIN/vm-net-cli-dummy" vm-net-bridge "$ID $DUMMY $MAC" 2>&1)
t "vm-net-bridge: ready" 'echo "$OUT" | grep -q "vm.bridgeReady"' "$OUT"
NAME=$(echo "$OUT" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p')
[ -n "$NAME" ] || NAME=$(ip -o link | awk -F': ' '/lordos-vm/ {print $2}' | cut -d@ -f1 | head -n1)
IDX=$(cat "/sys/class/net/$NAME/ifindex" 2>/dev/null)
echo "      $(ip -d link show "$NAME" | tr '\n' ' ' | tr -s ' ' | cut -c1-260)"
t "the macvtap is on $DUMMY, bridge mode, with the machine's MAC" 'ip -d link show "$NAME" | grep -q "macvtap mode bridge" && [ -e "/sys/class/net/$NAME/lower_$DUMMY" ] && [ "$(cat /sys/class/net/$NAME/address)" = "$MAC" ]'
t "its alias names this person and the machine" '[ "$(cat /sys/class/net/$NAME/ifalias)" = "lordos-vm $(id -u) $ID" ]'
t "no address of this computer's on it (no IPv6 link-local either)" '[ -z "$(ip -o addr show dev "$NAME")" ]'
t "/dev/tap$IDX is the person's, 0600" '[ "$(stat -c "%u %a" "/dev/tap$IDX")" = "$(id -u) 600" ]'

GUEST=$(cat <<EOF
sleep 5
f=\$(grep -l -i "$MAC" /sys/class/net/e*/address | head -n1); f=\${f%/address}; i=\${f##*/}
ip link set "\$i" up; ip addr add $GUEST_IP/24 dev "\$i"
echo "<3>BR-CARD \$i mac=\$(cat /sys/class/net/\$i/address)" >/dev/kmsg
sleep 2
if ping -c 3 -W 2 $PEER_IP >/dev/null 2>&1; then echo "<3>BR-PING1 ok" >/dev/kmsg; else echo "<3>BR-PING1 fail" >/dev/kmsg; fi
last=\$(cat /sys/class/net/\$i/carrier); n=0
while [ \$n -lt 240 ]; do
  c=\$(cat /sys/class/net/\$i/carrier 2>/dev/null)
  if [ "\$c" != "\$last" ]; then echo "<3>BR-CARRIER \$c" >/dev/kmsg; last=\$c
    if [ "\$c" = 1 ]; then sleep 2; if ping -c 3 -W 2 $PEER_IP >/dev/null 2>&1; then echo "<3>BR-PING2 ok" >/dev/kmsg; else echo "<3>BR-PING2 fail" >/dev/kmsg; fi; fi
  fi
  sleep 0.5; n=\$((n + 1))
done
EOF
)
G64=$(printf '%s\n' "$GUEST" | base64 -w0)
UNIT=$(printf '[Unit]\nAfter=multi-user.target\n[Service]\nType=simple\nExecStart=/bin/sh -c "echo %s | base64 -d >/run/brtest.sh; exec /bin/bash /run/brtest.sh"\n' "$G64")
DROP=$(printf '[Unit]\nWants=brtest.service\n')
PW="b-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
umask 077
{ echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"
  echo "passwd.plaintext-password.root=$PW"
  echo "binary:systemd.extra-unit.brtest.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
umask 022
mkfifo "$W/dev.in"
"$BIN/lordvm-dev" run "$D" --keep-running --credentials-file "$W/creds" <"$W/dev.in" >"$W/dev.out" 2>"$W/dev.err" &
DEV_PID=$!
exec 7>"$W/dev.in"
if wait_state running 60; then ok "start: running"; else bad "start" "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null)"; exit 1; fi
t "lord-vm opened the macvtap" 'grep -aq "network: bridged on $DUMMY through $NAME (/dev/tap$IDX)" "$D/log/lord-vm.log"' "$(grep -a network: "$D/log/lord-vm.log" | tail -n3)"
QPID=$(status_field qemu_pid)
t "QEMU has the tap as a descriptor, no tap of its own" 'tr "\0" " " <"/proc/$QPID/cmdline" | grep -q "tap,id=.*fd=" && ! tr "\0" " " <"/proc/$QPID/cmdline" | grep -q "ifname="'
if wait_serial "BR-PING1" 240; then
    grep -a "BR-" "$D/log/serial.log" | sed 's/^.*BR-/      BR-/' | tr -d '\r'
    t "the guest's card has the machine's MAC" 'grep -a "BR-CARD" "$D/log/serial.log" | grep -qi "mac=$MAC"'
    t "the guest reaches the peer on $DUMMY" 'grep -aq "BR-PING1 ok" "$D/log/serial.log"'
    t "the peer saw the guest's MAC" 'sudo -n ip -n "$NS" neigh show "$GUEST_IP" | grep -qi "$MAC"' "$(sudo -n ip -n "$NS" neigh)"
    OUT=$(sudo -n "$BIN/vm-net-cli-dummy" vm-net-unbridge "$ID" 2>&1)
    t "vm-net-unbridge refuses while the machine runs" 'echo "$OUT" | grep -q "vm.bridgeInUse" && [ -e "/sys/class/net/$NAME" ]' "$OUT"
    echo "cable 0" >&7
    t "the cable pulled: the guest sees no carrier" 'wait_serial "BR-CARRIER 0" 30'
    echo "cable 1" >&7
    t "the cable plugged in: carrier back" 'wait_serial "BR-CARRIER 1" 30'
    t "and the guest reaches the peer again" 'wait_serial "BR-PING2 ok" 30' "$(grep -a "BR-" "$D/log/serial.log" | tail -n3)"
    t "lord-vm logged both" 'grep -aq "network: the cable is pulled" "$D/log/lord-vm.log" && grep -aq "network: the cable is plugged in" "$D/log/lord-vm.log"'
else bad "guest report" "$(tail -5 "$D/log/serial.log")"; fi
"$VM" --status "$D" --json >"$W/status.json"
t "sandbox kept (Landlock, our seccomp allowlist, namespaces)" 'grep -q "\"landlock_fs\":1" "$W/status.json" && grep -q "\"seccomp_allowlist\":1" "$W/status.json" && grep -q "\"namespaces\":1" "$W/status.json"'
"$VM" --poweroff "$D" >/dev/null 2>&1
wait_state off 60 >/dev/null
exec 7>&-
kill "$DEV_PID" 2>/dev/null; wait "$DEV_PID" 2>/dev/null; DEV_PID=
OUT=$(sudo -n "$BIN/vm-net-cli-dummy" vm-net-unbridge "$ID" 2>&1)
t "vm-net-unbridge once it's off: gone" 'echo "$OUT" | grep -q "vm.bridgeGone" && [ ! -e "/sys/class/net/$NAME" ]' "$OUT"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
