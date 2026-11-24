#!/bin/sh
# vm_slice.sh — the machines' slice (docs/VIRTUALIZATION.md 3.7) with a real machine: this build's lord-vm boots a guest
# and asks for its scope over cgroup.sock exactly as in a session; the answer comes from lord-sessiond's own placement
# code (cgroup_place_serve, run by `cgslice-test serve` as root) on a throwaway cgroup tree standing in for lordos.service.
# The machine runs as UID with a private HOME, in a private mount namespace whose /run/lordos holds only that socket:
# never the running lord-sessiond, lordos.service or the person's own machines.
#
#   sudo sh tests/vm_slice.sh BUILD_DIR GUEST_IMAGE UID
#
# BUILD_DIR holds lord-vm and cgslice-test; GUEST_IMAGE a qcow2 of this computer's architecture (copied, never changed).
# Scratch data in /var/tmp/lordos-slice/run-<pid>, removed at the end. PASS/FAIL lines; exit 1 on any FAIL.
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2 U=$3
[ "$(id -u)" = 0 ] || { echo "SKIP  vm_slice.sh: needs root"; exit 0; }
W=/var/tmp/lordos-slice/run-$$
mkdir -p "$W/home" && chown -R "$U:$U" "$W"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
"$BIN/cgslice-test" serve "$BIN" "$U" >"$W/serve.log" 2>&1 &
SP=$!
for i in $(seq 1 50); do grep -q '^READY' "$W/serve.log" && break; sleep 0.1; done
set -- $(sed -n 's/^READY //p' "$W/serve.log")
SESS=${1:-} SOCK=${2:-}
[ -n "$SESS" ] || { cat "$W/serve.log"; kill "$SP"; exit 1; }
cat >"$W/inner.sh" <<EOF
mount -t tmpfs -o mode=755 tmpfs /run/lordos || exit 1
touch /run/lordos/cgroup.sock && mount --bind "$SOCK" /run/lordos/cgroup.sock || exit 1
echo \$\$ > "$SESS/server/cgroup.procs" || exit 1
exec setpriv --reuid=$U --regid=$U --init-groups env -i HOME=$W/home PATH=/usr/bin:/bin XDG_RUNTIME_DIR=$W/home LORDOS_SESSIOND=1 sh "$W/user.sh"
EOF
cat >"$W/user.sh" <<EOF
D=$W/home/m.lordvm
"$BIN/lord-vm" --create "\$D" --name slice-test --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1 || { cat "$W/create.json"; exit 1; }
"$BIN/lord-vm" --start "\$D" --detach >"$W/start.json" 2>&1 || { cat "$W/start.json"; exit 1; }
for i in \$(seq 1 60); do grep -q '"state":"running"' "\$D/run.json" 2>/dev/null && break; sleep 1; done
sed -n 's/.*"qemu_pid":\([0-9]*\).*/\1/p' "\$D/run.json" >"$W/qemu.pid"
"$BIN/lord-vm" --status "\$D" --json >"$W/status.json"
sleep 20                                    # the guest boots meanwhile; the checks run from outside
"$BIN/lord-vm" --poweroff "\$D" >/dev/null 2>&1
for i in \$(seq 1 90); do grep -q '"state":"running"' "\$D/run.json" 2>/dev/null || break; sleep 1; done
EOF
unshare -m --propagation private sh "$W/inner.sh" >"$W/inner.log" 2>&1 &
IP=$!
for i in $(seq 1 90); do [ -s "$W/qemu.pid" ] && break; sleep 1; done
sleep 3
Q=$(cat "$W/qemu.pid" 2>/dev/null)
SL=$SESS/lordos-machines.slice
CG=$(sed -n 's/^0:://p' "/proc/$Q/cgroup" 2>/dev/null)
echo "QEMU $Q in $CG"
case "$CG" in
  "${SL#/sys/fs/cgroup}"/app-lordos-lordos-vm-*.scope|"${SL#/sys/fs/cgroup}"/app-lordos-lordos-vm-*.scope/*) ok "QEMU's scope is in the machines' slice" ;;
  *) bad "QEMU's scope is in the machines' slice" "$CG" ;;
esac
SC=$(ls -d "$SL"/app-lordos-lordos-vm-*.scope 2>/dev/null | head -n1)
[ "$(cat "$SL/cpu.weight" 2>/dev/null)" = 50 ] && ok "slice cpu.weight 50" || bad "slice cpu.weight" "$(cat "$SL/cpu.weight" 2>&1)"
if grep -qw io "$SESS/cgroup.subtree_control"; then
  [ "$(cat "$SL/io.weight" 2>/dev/null)" = "default 50" ] && ok "slice io.weight default 50" || bad "slice io.weight" "$(cat "$SL/io.weight" 2>&1)"
else bad "io controller on in the session" "$(cat "$SESS/cgroup.subtree_control")"; fi
[ "$(cat "$SL/memory.max" 2>/dev/null)" = max ] && ok "the slice itself has no memory limit" || bad "slice memory.max" "$(cat "$SL/memory.max" 2>&1)"
M=$(cat "$SC/memory.max" 2>/dev/null)
[ -n "$M" ] && [ "$M" != max ] && ok "the machine's own memory.max ($M)" || bad "the machine's memory.max" "$M"
C=$(cat "$SC/cpu.max" 2>/dev/null)
case "$C" in max*|"") bad "the machine's own cpu.max" "$C" ;; *) ok "the machine's own cpu.max ($C)" ;; esac
[ "$(stat -c %u "$SL")" = 0 ] && [ "$(stat -c %u "$SC/cgroup.procs")" = 0 ] && ok "slice and scope are root's" || bad "slice and scope are root's"
grep -q '"cgroup":"scope"' "$W/status.json" && ok "lord-vm --status: in its scope" || bad "lord-vm --status" "$(cat "$W/status.json")"
grep -q '"state":"running"' "$W/status.json" && ok "the machine runs" || bad "the machine runs" "$(cat "$W/status.json" "$W/inner.log")"
wait "$IP"
[ -z "$(cat "$SL"/*/cgroup.procs 2>/dev/null)" ] && ok "nothing left in the slice after power off" || bad "processes left in the slice"
kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null
grep '^placed' "$W/serve.log" | sort | uniq -c
rm -rf "$W"
echo "vm_slice.sh: $pass passed, $fail failed"
[ "$fail" = 0 ]
