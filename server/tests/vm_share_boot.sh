#!/bin/sh
# Shared folders and the transfer folder in a real KVM guest (docs/VIRTUALIZATION.md 6.7, 6.7.1), on either architecture
# (the Pi's arm64 guests too): a machine with a read-only share, a writable share and its transfer folder, each its own
# virtiofsd. The guest (Debian 13 nocloud, a script in a systemd credential) mounts the three with `mount -t virtiofs`,
# reads a 64 MB file of this computer's back with its SHA-256, is refused a write in the read-only one, writes into the
# writable one and the transfer folder (seen here at once), and copies a 200 MB file out through the transfer folder
# (its time is printed). Then: one virtiofsd per folder, QEMU holds no folder, the sandbox kept.
# A share must be inside the account's home (vm_share_allowed reads it from the password database), so the script runs only
# inside the Machines harness's namespaces, where that home is an empty scratch folder over the real one:
#   sh ../tools/vm/e2e/machines/ns-run.sh sh tests/vm_share_boot.sh BUILD_DIR DEBIAN_NOCLOUD_QCOW2
#   (from server/, a KVM host, as a normal user; not in make test)
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
[ -n "${MH_NS_DIR:-}" ] || { echo "skipped: run it through tools/vm/e2e/machines/ns-run.sh (it writes in the account's home)"; exit 0; }
H=$(getent passwd "$(id -u)" | cut -d: -f6)
[ -d "$H" ] && [ -z "$(ls -A "$H")" ] || { echo "the home folder isn't the harness's empty scratch one: not touching it"; exit 2; }
W=$MH_NS_DIR/share
LIB=$W/home/Machines
D=$LIB/share-test.lordvm
VM="$BIN/lord-vm"
mkdir -p "$LIB" "$H/RoShare" "$H/RwShare" || exit 2
export HOME="$W/home"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
t()   { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now() { date +%s; }
status_field() { "$VM" --status "$D" --json | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
wait_state() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do [ "$(status_field state)" = "$1" ] && return 0; sleep 0.5; done; return 1; }
wait_serial() { end=$(( $(now) + $2 )); while [ "$(now)" -lt "$end" ]; do grep -aq "$1" "$D/log/serial.log" 2>/dev/null && return 0; sleep 1; done; return 1; }
cleanup() { "$VM" --poweroff "$D" >/dev/null 2>&1; [ -n "${DEV_PID:-}" ] && kill "$DEV_PID" 2>/dev/null; sleep 1; chmod -R u+w "$W" 2>/dev/null; rm -rf "$W" "$H/RoShare" "$H/RwShare"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
trap '' PIPE
[ -c /dev/kvm ] && [ -w /dev/kvm ] || { echo "skipped: needs KVM"; exit 0; }
command -v virtiofsd >/dev/null 2>&1 || [ -x /usr/libexec/virtiofsd ] || { echo "skipped: needs virtiofsd"; exit 0; }

head -c 64M /dev/urandom >"$H/RoShare/data.bin"
SUM=$(sha256sum "$H/RoShare/data.bin" | cut -d' ' -f1)
"$VM" --create "$D" --name share-test --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1
t "create" '[ -f "$D/machine.json" ]' "$(cat "$W/create.json")"
"$VM" --set "$D" --share "ro:roshare:$H/RoShare" --share "rw:rwshare:$H/RwShare" --transfer on >"$W/set.json" 2>&1
t "set: a read-only share, a writable share, the transfer folder" 'grep -q "\"transfer\":true" "$W/set.json" && [ "$(grep -o "\"tag\"" "$D/machine.json" | wc -l)" = 2 ]' "$(cat "$W/set.json")"

GUEST=$(cat <<EOF
sleep 3
for tg in roshare rwshare LordOS; do mkdir -p /mnt/\$tg
  if mount -t virtiofs \$tg /mnt/\$tg; then echo "<3>SH-MOUNT \$tg ok" >/dev/kmsg; else echo "<3>SH-MOUNT \$tg fail" >/dev/kmsg; fi; done
echo "<3>SH-SUM \$(sha256sum /mnt/roshare/data.bin | cut -d' ' -f1)" >/dev/kmsg
if (echo x >/mnt/roshare/new.txt) 2>/run/ro.err; then echo "<3>SH-ROWRITE allowed" >/dev/kmsg; else echo "<3>SH-ROWRITE refused \$(cat /run/ro.err | tr -d '\n' | cut -c1-80)" >/dev/kmsg; fi
echo "from-guest-rw" >/mnt/rwshare/guest.txt && echo "<3>SH-RWWRITE ok" >/dev/kmsg
echo "from-guest-xfer" >/mnt/LordOS/guest.txt && echo "<3>SH-XFWRITE ok" >/dev/kmsg
head -c 200M /dev/urandom >/root/big.bin
s=\$(date +%s.%N); cp /root/big.bin /mnt/LordOS/big.bin; sync -f /mnt/LordOS/big.bin; e=\$(date +%s.%N)
echo "<3>SH-BIG \$(sha256sum /root/big.bin | cut -d" " -f1) \$(awk -v a=\$s -v b=\$e 'BEGIN{printf "%.2f", b - a}')" >/dev/kmsg
echo "<3>SH-DONE" >/dev/kmsg
EOF
)
# the script in pieces of its own credentials (lord-vm takes lines of up to 2 KB), joined in the guest
printf '%s\n' "$GUEST" | split -b 1200 - "$W/gpart."
UNIT=$(printf '[Unit]\nAfter=multi-user.target\n[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/bin/sh -c "cat /run/credentials/@system/shtest.* >/run/shtest.sh; exec /bin/bash /run/shtest.sh"\n')
DROP=$(printf '[Unit]\nWants=shtest.service\n')
PW="s-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
umask 077
{ echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"
  echo "passwd.plaintext-password.root=$PW"
  for f in "$W"/gpart.*; do echo "binary:shtest.${f##*.}=$(base64 -w0 "$f")"; done
  echo "binary:systemd.extra-unit.shtest.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
umask 022
"$BIN/lordvm-dev" run "$D" --keep-running --credentials-file "$W/creds" </dev/null >"$W/dev.out" 2>"$W/dev.err" &
DEV_PID=$!
if wait_state running 90; then ok "start: running"; else bad "start" "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null) $(tail -3 "$W/dev.err")"; exit 1; fi
LVPID=$(pgrep -f "lord-vm.*--run.*$D" | head -n1)
[ -n "$LVPID" ] || LVPID=$(pgrep -f "lord-vm.*$D" | head -n1)
XF=$(ls -d "$LIB/Transfer/share-test" 2>/dev/null)
if wait_serial "SH-DONE" 400; then
    grep -a "SH-" "$D/log/serial.log" | sed 's/^.*SH-/      SH-/' | tr -d '\r'
    t "the guest mounts the three folders (virtiofs)" '[ "$(grep -a "SH-MOUNT" "$D/log/serial.log" | grep -c " ok")" = 3 ]'
    t "the read-only share reads back with this computer's SHA-256" 'grep -aq "SH-SUM $SUM" "$D/log/serial.log"'
    t "a write in the read-only share is refused (read-only file system)" 'grep -a "SH-ROWRITE" "$D/log/serial.log" | grep -qi "refused.*read-only" && [ ! -e "$H/RoShare/new.txt" ]'
    t "a write in the writable share is here" '[ "$(cat "$H/RwShare/guest.txt" 2>/dev/null)" = "from-guest-rw" ]'
    t "the transfer folder is <library>/Transfer/<name>, 0700 parent" '[ -n "$XF" ] && [ "$(stat -c %a "$LIB/Transfer")" = 700 ]'
    t "what the guest saved in the transfer folder is here" '[ "$(cat "$XF/guest.txt" 2>/dev/null)" = "from-guest-xfer" ]'
    BIG=$(grep -a "SH-BIG" "$D/log/serial.log" | sed 's/.*SH-BIG //' | tr -d '\r')
    t "the 200 MB file out through the transfer folder is whole here" '[ "$(sha256sum "$XF/big.bin" 2>/dev/null | cut -d" " -f1)" = "$(echo "$BIG" | cut -d" " -f1)" ]' "$BIG"
    echo "      200 MB out through the transfer folder: $(echo "$BIG" | cut -d' ' -f2) s"
else bad "guest report" "$(tail -5 "$D/log/serial.log")"; fi
NV=$(pgrep -c -f "virtiofsd .*--fd=3" -P "$LVPID" 2>/dev/null || echo 0)
NVA=$(pgrep -f "virtiofsd" | while read -r p; do grep -q "^PPid:[[:space:]]*$LVPID$" "/proc/$p/status" 2>/dev/null && echo "$p"; done | wc -l)
t "one virtiofsd per folder (3)" '[ "$NVA" -ge 3 ]' "found $NVA (by -P: $NV)"
QPID=$(status_field qemu_pid)
t "QEMU holds no shared folder (vhost-user sockets only)" '! ls -l "/proc/$QPID/fd" 2>/dev/null | grep -q "RoShare\|RwShare\|Transfer"'
"$VM" --status "$D" --json >"$W/status.json"
t "sandbox kept (Landlock, our seccomp allowlist, namespaces)" 'grep -q "\"landlock_fs\":1" "$W/status.json" && grep -q "\"seccomp_allowlist\":1" "$W/status.json" && grep -q "\"namespaces\":1" "$W/status.json"'
"$VM" --poweroff "$D" >/dev/null 2>&1
wait_state off 60 >/dev/null
kill "$DEV_PID" 2>/dev/null; wait "$DEV_PID" 2>/dev/null; DEV_PID=
t "no virtiofsd left once it's off" '! pgrep -f "virtiofsd.*share-test" >/dev/null'
echo "== $pass passed, $fail failed ($(uname -m))"
[ "$fail" = 0 ]
