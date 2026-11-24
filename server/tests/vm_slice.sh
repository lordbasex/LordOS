#!/bin/sh
# vm_slice.sh — the machines' slice (docs/VIRTUALIZATION.md 3.7) with a real machine: this build's lord-vm boots a guest
# and asks for its scope over cgroup.sock exactly as in a session; the answer comes from lord-sessiond's own placement
# code (cgroup_place_serve, run by `cgslice-test serve` as root) on a throwaway cgroup tree standing in for lordos.service.
# The machine runs as UID with a private HOME, in a private mount namespace whose /run/lordos holds only that socket:
# never the running lord-sessiond, lordos.service or the person's own machines.
#
# Two boots: (1) with the stand-in lord-netperm current (cgslice-test is built with CG_NETPERM_PROCS pointing at
# /var/tmp/lordos-slice/netperm.procs; absent = not running = the installed one starts next) the scope must land in
# lordos-machines.slice with the slice's weights and its own limits; (2) with that file naming a running lord-netperm
# whose file was replaced (an update without the restart) it must land beside the server leaf, where an older
# lord-netperm still finds it.
#
#   sudo sh tests/vm_slice.sh BUILD_DIR GUEST_IMAGE UID
#
# BUILD_DIR holds lord-vm and cgslice-test; GUEST_IMAGE a qcow2 of this computer's architecture (imported as a copy, never
# changed). Scratch data in /var/tmp/lordos-slice/run-<pid>, removed at the end. PASS/FAIL lines; exit 1 on any FAIL.
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2 U=$3
[ "$(id -u)" = 0 ] || { echo "SKIP  vm_slice.sh: needs root"; exit 0; }
B=/var/tmp/lordos-slice
W=$B/run-$$
NP=$B/netperm.procs
mkdir -p "$W/home" && chown -R "$U:$U" "$W"
rm -f "$NP"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
"$BIN/cgslice-test" serve "$BIN" "$U" >"$W/serve.log" 2>&1 &
SP=$!
for i in $(seq 1 50); do grep -q '^READY' "$W/serve.log" && break; sleep 0.1; done
set -- $(sed -n 's/^READY //p' "$W/serve.log")
SESS=${1:-} SOCK=${2:-}
[ -n "$SESS" ] || { cat "$W/serve.log"; kill "$SP"; exit 1; }
SL=$SESS/lordos-machines.slice

cat >"$W/inner.sh" <<EOF
mount -t tmpfs -o mode=755 tmpfs /run/lordos || exit 1
touch /run/lordos/cgroup.sock && mount --bind "$SOCK" /run/lordos/cgroup.sock || exit 1
echo \$\$ > "$SESS/server/cgroup.procs" || exit 1
exec setpriv --reuid=$U --regid=$U --init-groups env -i HOME=$W/home PATH=/usr/bin:/bin XDG_RUNTIME_DIR=$W/home LORDOS_SESSIOND=1 sh "$W/user.sh" "\$1"
EOF
cat >"$W/user.sh" <<EOF
D=$W/home/m\$1.lordvm
"$BIN/lord-vm" --create "\$D" --name slice-test-\$1 --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1 || { cat "$W/create.json"; exit 1; }
"$BIN/lord-vm" --start "\$D" --detach >"$W/start.json" 2>&1 || { cat "$W/start.json"; exit 1; }
for i in \$(seq 1 60); do grep -q '"state":"running"' "\$D/run.json" 2>/dev/null && break; sleep 1; done
"$BIN/lord-vm" --status "\$D" --json >"$W/status.json"
sed -n 's/.*"qemu_pid":\([0-9]*\).*/\1/p' "\$D/run.json" >"$W/qemu.pid"
while [ ! -e "$W/done" ]; do sleep 1; done   # the checks run from outside meanwhile
"$BIN/lord-vm" --poweroff "\$D" >/dev/null 2>&1
for i in \$(seq 1 90); do grep -q '"state":"running"' "\$D/run.json" 2>/dev/null || break; sleep 1; done
rm -rf "\$D"
EOF

boot() {   # N: start machine N, leave its QEMU's pid in $W/qemu.pid
    rm -f "$W/qemu.pid" "$W/done" "$W/status.json"
    unshare -m --propagation private sh "$W/inner.sh" "$1" >"$W/inner.log" 2>&1 &
    IP=$!
    for i in $(seq 1 120); do [ -s "$W/qemu.pid" ] && break; sleep 1; done
    sleep 3
    Q=$(cat "$W/qemu.pid" 2>/dev/null)
    CG=$(sed -n 's/^0:://p' "/proc/$Q/cgroup" 2>/dev/null)
    echo "boot $1: QEMU $Q in $CG"
    grep -q '"state":"running"' "$W/status.json" 2>/dev/null && ok "boot $1: the machine runs" || bad "boot $1: the machine runs" "$(cat "$W/status.json" "$W/inner.log" 2>&1)"
}
stop_boot() { touch "$W/done"; wait "$IP"; }

# (1) the slice
boot 1
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
stop_boot
[ -z "$(cat "$SL"/*/cgroup.procs 2>/dev/null)" ] && ok "nothing left in the slice after power off" || bad "processes left in the slice"

# (2) an older lord-netperm still runs: beside the server leaf
cp /bin/sleep "$B/lord-netperm"
"$B/lord-netperm" 600 &
FP=$!
rm -f "$B/lord-netperm"                       # its file replaced: /proc/<pid>/exe ends in "(deleted)"
echo "$FP" >"$NP"
boot 2
case "$CG" in
  "${SESS#/sys/fs/cgroup}"/app-lordos-lordos-vm-*.scope|"${SESS#/sys/fs/cgroup}"/app-lordos-lordos-vm-*.scope/*) ok "with an older lord-netperm: beside the server leaf" ;;
  *) bad "with an older lord-netperm: beside the server leaf" "$CG" ;;
esac
stop_boot
kill "$FP" 2>/dev/null; wait "$FP" 2>/dev/null
rm -f "$NP"

kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null
grep '^placed' "$W/serve.log" | sort | uniq -c
rm -rf "$W"
rmdir "$B" 2>/dev/null
echo "vm_slice.sh: $pass passed, $fail failed"
[ "$fail" = 0 ]
