#!/bin/sh
# vm_slice.sh — the machines' slice (docs/VIRTUALIZATION.md 3.7) with a real machine: this build's lord-vm boots a guest
# and asks for its scope over cgroup.sock exactly as in a session; the answer comes from lord-sessiond's own placement
# code (cgroup_place_serve, run by `cgslice-test serve` as root) on a throwaway cgroup tree standing in for lordos.service.
# The machine runs as UID with a private HOME, in a private mount namespace whose /run/lordos holds only that socket:
# never the running lord-sessiond, lordos.service or the person's own machines.
#
# Two boots: (1) with the stand-in lord-netperm current (cgslice-test is built with CG_NETPERM_PROCS pointing at
# /run/lordos-slicetest/netperm.procs; absent = not running = the installed one starts next) the scope must land in
# lordos-machines.slice with the slice's weights and its own limits; (2) with that file naming a running lord-netperm
# whose file was replaced (an update without the restart) it must land beside the server leaf, where an older
# lord-netperm still finds it.
#
#   sudo sh tests/vm_slice.sh BUILD_DIR GUEST_IMAGE UID
#
# BUILD_DIR holds lord-vm and cgslice-test; GUEST_IMAGE a qcow2 of this computer's architecture (imported as a copy, never
# changed). Scratch data in a fresh root-owned folder (mktemp -d /var/tmp/lordos-slice.XXXXXX, 0711), removed at the end:
# the scripts root runs and the stand-in lord-netperm stay root's there, and only its home/ and out/ belong to UID. The
# stand-in's procs file lives in /run/lordos-slicetest, a folder only root can make (checked: a real folder of root's, 0711).
# PASS/FAIL lines; exit 1 on any FAIL.
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2 U=$3
case "$U" in ''|*[!0-9]*) echo "usage: vm_slice.sh BUILD_DIR GUEST_IMAGE UID"; exit 2 ;; esac
[ "$(id -u)" = 0 ] || { echo "SKIP  vm_slice.sh: needs root"; exit 0; }
B=/run/lordos-slicetest
NP=$B/netperm.procs
[ -e "$B" ] || [ -L "$B" ] || mkdir -m 0711 "$B" || exit 1
[ -d "$B" ] && [ ! -L "$B" ] && [ "$(stat -c %u "$B")" = 0 ] && [ -z "$(find "$B" -maxdepth 0 -perm /022)" ] && chmod 0711 "$B" \
    || { echo "FAIL  $B must be a folder of root's that no one else can write to"; exit 1; }
W=$(mktemp -d /var/tmp/lordos-slice.XXXXXX) || exit 1
chmod 0711 "$W"
mkdir "$W/home" "$W/out" && chown "$U:$U" "$W/home" "$W/out" || exit 1
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
"$BIN/lord-vm" --create "\$D" --name slice-test-\$1 --import "$IMG" --vcpus 2 --memory 1024 >"$W/out/create.json" 2>&1 || { cat "$W/out/create.json"; exit 1; }
"$BIN/lord-vm" --start "\$D" --detach >"$W/out/start.json" 2>&1 || { cat "$W/out/start.json"; exit 1; }
for i in \$(seq 1 60); do grep -q '"state":"running"' "\$D/run.json" 2>/dev/null && break; sleep 1; done
"$BIN/lord-vm" --status "\$D" --json >"$W/out/status.json"
sed -n 's/.*"qemu_pid":\([0-9]*\).*/\1/p' "\$D/run.json" >"$W/out/qemu.pid"
while [ ! -e "$W/done" ]; do sleep 1; done   # the checks run from outside meanwhile
"$BIN/lord-vm" --poweroff "\$D" >/dev/null 2>&1
for i in \$(seq 1 90); do grep -q '"state":"running"' "\$D/run.json" 2>/dev/null || break; sleep 1; done
rm -rf "\$D"
EOF

boot() {   # N: start machine N, leave its QEMU's pid in $W/out/qemu.pid
    rm -f "$W/out/qemu.pid" "$W/done" "$W/out/status.json"
    unshare -m --propagation private sh "$W/inner.sh" "$1" >"$W/inner.log" 2>&1 &
    IP=$!
    for i in $(seq 1 120); do [ -s "$W/out/qemu.pid" ] && break; sleep 1; done
    sleep 3
    Q=$(cat "$W/out/qemu.pid" 2>/dev/null)
    case "$Q" in *[!0-9]*) Q= ;; esac             # written by UID: a pid or nothing
    CG=$(sed -n 's/^0:://p' "/proc/$Q/cgroup" 2>/dev/null)
    echo "boot $1: QEMU $Q in $CG"
    grep -q '"state":"running"' "$W/out/status.json" 2>/dev/null && ok "boot $1: the machine runs" || bad "boot $1: the machine runs" "$(cat "$W/out/status.json" "$W/inner.log" 2>&1)"
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
grep -q '"cgroup":"scope"' "$W/out/status.json" && ok "lord-vm --status: in its scope" || bad "lord-vm --status" "$(cat "$W/out/status.json")"
stop_boot
[ -z "$(cat "$SL"/*/cgroup.procs 2>/dev/null)" ] && ok "nothing left in the slice after power off" || bad "processes left in the slice"

# (2) an older lord-netperm still runs: beside the server leaf
cp /bin/sleep "$W/lord-netperm"
"$W/lord-netperm" 600 &
FP=$!
rm -f "$W/lord-netperm"                       # its file replaced: /proc/<pid>/exe ends in "(deleted)"
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
