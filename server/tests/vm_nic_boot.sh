#!/bin/sh
# A real KVM boot for the network card's model and an imported machine's other cards (docs/VIRTUALIZATION.md 3.9, 6.5,
# 6.22): the first card's model chosen with --set --nic-model, a second card carried in extra_nics (another model, its
# own MAC), both Host-only. The guest reports each card's driver and MAC, and reaches this computer's loopback through
# each card's own passt (10.0.2.2 for the first, 10.0.3.2 for the second); a third run with Isolated leaves the extra
# card with no cable.
#   sh tests/vm_nic_boot.sh BUILD_DIR DEBIAN_NOCLOUD_QCOW2      (a KVM host, as a normal user; not part of make test)
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${TMPDIR:-/var/tmp}/lnicboot.$$
D=$W/m.lordvm
VM="$BIN/lord-vm"
mkdir -p "$W/home" "$W/www" || exit 2
export HOME="$W/home"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
t()   { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now() { date +%s; }
status_field() { "$VM" --status "$D" --json | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
wait_state() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do [ "$(status_field state)" = "$1" ] && return 0; sleep 0.5; done; return 1; }
wait_serial() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do grep -aq "$1" "$D/log/serial.log" 2>/dev/null && return 0; sleep 1; done; return 1; }
cleanup() { "$VM" --poweroff "$D" >/dev/null 2>&1; [ -n "${DEV_PID:-}" ] && kill "$DEV_PID" 2>/dev/null; [ -n "${WWW_PID:-}" ] && kill "$WWW_PID" 2>/dev/null; sleep 1; rm -rf "$W"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
trap '' PIPE

# a listener on this computer's loopback only: what each card's passt reaches as its gateway in Host-only
TOKEN="nic-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
echo "$TOKEN" >"$W/www/t.txt"
PORT=$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -I -m http.server "$PORT" --bind 127.0.0.1 --directory "$W/www" >/dev/null 2>&1 &
WWW_PID=$!

"$VM" --create "$D" --name nic-test --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1
t "create" '[ -f "$D/machine.json" ]' "$(cat "$W/create.json")"
"$VM" --set "$D" --nic-model e1000e --network hostonly >"$W/set.json" 2>&1
t "set: e1000e, Host-only" 'grep -q "\"nic_model\":\"e1000e\"" "$W/set.json" && grep -q "\"network\":\"hostonly\"" "$W/set.json"' "$(cat "$W/set.json")"
XMAC="52:54:00:7e:57:02"
# what an import writes for a second card (lordvm.c imp_*): its MAC and model
python3 -I - "$D/machine.json" "$XMAC" <<'EOF'
import json, sys
p, mac = sys.argv[1], sys.argv[2]
m = json.load(open(p))
m["extra_nics"] = [{"mac": mac, "model": "rtl8139"}]
json.dump(m, open(p, "w"), indent=2)
EOF
"$VM" --status "$D" --json >/dev/null 2>"$W/check.err"
t "machine.json with extra_nics is accepted" '[ ! -s "$W/check.err" ]' "$(cat "$W/check.err")"
MAC1=$(sed -n 's/.*"mac": "\([^"]*\)".*/\1/p' "$D/machine.json" | head -n1)

# the guest's side, as a script the unit decodes (no quoting through systemd)
GUEST=$(cat <<EOF
sleep 8
for n in /sys/class/net/e*; do i=\${n##*/}; drv=\$(basename "\$(readlink \$n/device/driver)"); ip link set "\$i" up
  echo "<3>NIC-CARD \$i driver=\$drv mac=\$(cat \$n/address)" >/dev/kmsg; done
dev_of() { f=\$(grep -l -i "\$1" /sys/class/net/e*/address | head -n1); f=\${f%/address}; echo "\${f##*/}"; }
f=\$(dev_of $MAC1); x=\$(dev_of $XMAC)
sleep 5
ip -4 -o addr show dev "\$f" | grep -q 10.0.2.15 || ip addr add 10.0.2.15/24 dev "\$f"
ip -4 -o addr show dev "\$x" | grep -q 10.0.3.15 || ip addr add 10.0.3.15/24 dev "\$x"
for g in 10.0.2.2 10.0.3.2; do
  r=none
  if exec 3<>/dev/tcp/\$g/$PORT; then printf 'GET /t.txt HTTP/1.0\r\n\r\n' >&3; r=\$(timeout 10 cat <&3 | tail -n1); exec 3<&-; fi
  echo "<3>NIC-REACH \$g \$r" >/dev/kmsg
done
echo "<3>NIC-DONE" >/dev/kmsg
EOF
)
G64=$(printf '%s\n' "$GUEST" | base64 -w0)
UNIT=$(printf '[Unit]\nAfter=network-online.target\n[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/bin/sh -c "echo %s | base64 -d >/run/nictest.sh; exec /bin/bash /run/nictest.sh"\n' "$G64")
DROP=$(printf '[Unit]\nWants=nictest.service\n')
PW="n-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
umask 077
{ echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"
  echo "passwd.plaintext-password.root=$PW"
  echo "binary:systemd.extra-unit.nictest.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
umask 022
mkfifo "$W/dev.in"
"$BIN/lordvm-dev" run "$D" --keep-running --credentials-file "$W/creds" <"$W/dev.in" >"$W/dev.out" 2>"$W/dev.err" &
DEV_PID=$!
exec 7>"$W/dev.in"
if wait_state running 60; then ok "start: running"; else bad "start" "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null)"; exit 1; fi
QPID=$(status_field qemu_pid)
tr '\0' ' ' <"/proc/$QPID/cmdline" | grep -o -- "-device e1000e[^ ]*\|-device rtl8139[^ ]*\|-netdev hubport,id=xnic[^ ]*"
t "lord-vm connected the second card" 'grep -aq "network: card 2 ($XMAC, rtl8139) connected" "$D/log/lord-vm.log"' "$(grep -a network: "$D/log/lord-vm.log" | tail -n3)"
if wait_serial "NIC-DONE" 300; then
    grep -a "NIC-" "$D/log/serial.log" | sed 's/^.*NIC-/      NIC-/' | tr -d '\r'
    t "the first card is an e1000e (driver e1000e) with its MAC" 'grep -a "NIC-CARD" "$D/log/serial.log" | grep -qi "driver=e1000e mac=$MAC1"'
    t "the second card is an RTL8139 (driver 8139cp or 8139too) with its MAC" 'grep -a "NIC-CARD" "$D/log/serial.log" | grep -i "mac=$XMAC" | grep -q "driver=8139"'
    t "this computer reached through the first card (10.0.2.2)" 'grep -a "NIC-REACH 10.0.2.2" "$D/log/serial.log" | grep -q "$TOKEN"'
    t "this computer reached through the second card's own passt (10.0.3.2)" 'grep -a "NIC-REACH 10.0.3.2" "$D/log/serial.log" | grep -q "$TOKEN"'
else bad "guest report" "$(tail -5 "$D/log/serial.log")"; fi
NP=$(pgrep -c -f "^/usr/bin/passt .*--fd 3" -P "$(pgrep -f "lord-vm.*$D" | head -n1)" 2>/dev/null || true)
"$VM" --status "$D" --json >"$W/status.json"
t "sandbox kept (Landlock, our seccomp allowlist, namespaces)" 'grep -q "\"landlock_fs\":1" "$W/status.json" && grep -q "\"seccomp_allowlist\":1" "$W/status.json" && grep -q "\"namespaces\":1" "$W/status.json"'
"$VM" --poweroff "$D" >/dev/null 2>&1
wait_state off 60 >/dev/null
kill "$DEV_PID" 2>/dev/null; wait "$DEV_PID" 2>/dev/null; DEV_PID=
t "no passt left once it's off" '! pgrep -f "passt .*-a 10.0.3.15" >/dev/null'

# Isolated: the second card is there with its MAC, with no backend (no passt, no hub)
"$VM" --set "$D" --network isolated >/dev/null 2>&1
: >"$D/log/serial.log" 2>/dev/null
"$BIN/lordvm-dev" run "$D" --keep-running --credentials-file "$W/creds" </dev/null >"$W/dev2.out" 2>"$W/dev2.err" &
DEV_PID=$!
if wait_state running 60; then
    QPID=$(status_field qemu_pid)
    t "Isolated: the second card is there without a backend" 'tr "\0" " " <"/proc/$QPID/cmdline" | grep -q "rtl8139,id=nic1,mac=$XMAC" && ! tr "\0" " " <"/proc/$QPID/cmdline" | grep -q xnic'
    t "Isolated: no passt for it" '! grep -aq "card 2 ($XMAC, rtl8139) connected" "$D/log/lord-vm.log" || [ "$(grep -ac "card 2 ($XMAC, rtl8139) connected" "$D/log/lord-vm.log")" = 1 ]'
else bad "Isolated start" "$(tail -5 "$D/log/lord-vm.log")"; fi
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
