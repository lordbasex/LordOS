#!/bin/sh
# A real KVM boot of a scratch machine for the app's last parts (docs/VIRTUALIZATION.md 5.3, 5.5): the compatible
# processor (Nehalem-v2) under lord-vm's sandbox, a data disk added with --add-disk, a new MAC, and the network cable
# pulled and plugged in while it runs (VM_NETCABLE), seen from inside the guest (carrier) and from QEMU (info network).
#   sh tests/vm_configure_boot.sh BUILD_DIR DEBIAN_NOCLOUD_QCOW2      (a KVM host, as a normal user; not part of make test)
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${TMPDIR:-/var/tmp}/lcfgboot.$$
D=$W/m.lordvm
VM="$BIN/lord-vm"
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
cleanup() { "$VM" --poweroff "$D" >/dev/null 2>&1; [ -n "${DEV_PID:-}" ] && kill "$DEV_PID" 2>/dev/null; sleep 1; rm -rf "$W"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
trap '' PIPE

"$VM" --create "$D" --name app2-test --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1
t "create" '[ -f "$D/machine.json" ]' "$(cat "$W/create.json")"
MAC0=$(sed -n 's/.*"mac": "\([^"]*\)".*/\1/p' "$D/machine.json")
"$VM" --set "$D" --cpu-model compatible --add-disk 1 --mac new >"$W/set.json" 2>&1
t "set: compatible processor, a data disk, a new MAC" 'grep -q "\"cpu_model\":\"compatible\"" "$W/set.json" && grep -q "\"data_disks\":1" "$W/set.json" && [ -f "$D/disks/data-1.qcow2" ]' "$(cat "$W/set.json")"
MAC1=$(sed -n 's/.*"mac": "\([^"]*\)".*/\1/p' "$D/machine.json")
t "the MAC changed, still QEMU's range" '[ "$MAC0" != "$MAC1" ] && echo "$MAC1" | grep -q "^52:54:00:"' "$MAC0 -> $MAC1"

UNIT=$(cat <<'EOF'
[Unit]
After=network-online.target
[Service]
Type=simple
StandardOutput=journal
ExecStart=/bin/sh -c 'm=$$(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2); v2=$$(grep -m1 -o -w sse4_2 /proc/cpuinfo); avx=$$(grep -m1 -o -w avx2 /proc/cpuinfo); d=$$(ls /sys/block | grep -c vd); mac=$$(cat /sys/class/net/e*/address 2>/dev/null | head -n1); echo "<3>APP2-CPU model=$$m sse42=$$v2 avx2=$${avx:-none} vdisks=$$d mac=$$mac" >/dev/kmsg; while true; do c=$$(cat /sys/class/net/e*/carrier 2>/dev/null | head -n1); echo "<3>APP2-CARRIER $$c" >/dev/kmsg; sleep 1; done'
EOF
)
DROP=$(printf '[Unit]\nWants=app2.service\n')
PW="a2-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
umask 077
{ echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"
  echo "passwd.plaintext-password.root=$PW"
  echo "binary:systemd.extra-unit.app2.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
umask 022
mkfifo "$W/dev.in"
"$BIN/lordvm-dev" run "$D" --dev-qmp --keep-running --credentials-file "$W/creds" <"$W/dev.in" >"$W/dev.out" 2>"$W/dev.err" &
DEV_PID=$!
exec 7>"$W/dev.in"
if wait_state running 60; then ok "start: running"; else bad "start" "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null)"; exit 1; fi
grep -o -- "-cpu [^ ]*" /proc/$(status_field qemu_pid)/cmdline 2>/dev/null
tr '\0' ' ' </proc/$(status_field qemu_pid)/cmdline | grep -o -- "-cpu [^ ]*"
if wait_serial "APP2-CPU" 300; then
    line=$(grep -a "APP2-CPU" "$D/log/serial.log" | tail -n1); echo "      $line"
    t "guest sees the compatible processor (Nehalem class, SSE4.2, no AVX2)" 'echo "$line" | grep -q "sse42=sse4_2" && echo "$line" | grep -q "avx2=none"'
    t "guest sees two disks (system and the added one)" 'echo "$line" | grep -q "vdisks=2"'
    t "guest sees the new MAC" 'echo "$line" | grep -qi "mac=$MAC1"'
else bad "guest report" "$(tail -5 "$D/log/serial.log")"; fi
t "carrier up at first" 'wait_serial "APP2-CARRIER 1" 30'
echo "cable 0" >&7; sleep 4
tail_c() { grep -a "APP2-CARRIER" "$D/log/serial.log" | tail -n1; }
t "cable pulled: the guest's card has no carrier" '[ "$(tail_c | sed "s/.*CARRIER //" | tr -d "\r ")" = 0 ]' "$(tail_c)"
echo "cable 1" >&7; sleep 4
t "cable plugged in: carrier back" '[ "$(tail_c | sed "s/.*CARRIER //" | tr -d "\r ")" = 1 ]' "$(tail_c)"
grep -a "network: the cable" "$D/log/lord-vm.log" | tail -n2
t "lord-vm logged both" '[ "$(grep -ac "network: the cable" "$D/log/lord-vm.log")" -ge 2 ]'
"$VM" --status "$D" --json >"$W/status.json"
t "sandbox kept (Landlock, our seccomp allowlist, namespaces)" 'grep -q "\"landlock_fs\":1" "$W/status.json" && grep -q "\"seccomp_allowlist\":1" "$W/status.json" && grep -q "\"namespaces\":1" "$W/status.json"'
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
