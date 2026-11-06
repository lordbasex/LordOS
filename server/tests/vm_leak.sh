#!/bin/sh
# vm_leak.sh — Machines' release gate "leak test" (docs/VIRTUALIZATION.md 12): a machine started and stopped N times
# (100 by default; every fourth stop an ACPI shutdown, the others Force Off). Afterwards nothing of it is left: no
# lord-vm, QEMU or passt process, no zombie, no control socket, no cgroup scope, no lock held, no
# operation, at most three logs, and its disks pass qemu-img check. Times and the slowest cycle are printed.
#
#   sh tests/vm_leak.sh BUILD_DIR [CYCLES] [WORK_DIR]
set -u
BIN=$(cd "$1" && pwd) || exit 2
N=${2:-100}
W=${3:-${TMPDIR:-/var/tmp}/lvg-leak.$$}   # /var/tmp: /tmp is often a small tmpfs, and full clones need room
IMG=
. "$(dirname "$0")/vm_gate_lib.sh"
gate_begin
D=$W/k.lordvm
"$VM" --create "$D" --name leak --vcpus 1 --memory 512 --disk 2 >/dev/null 2>&1
t "create" '[ -f "$D/disks/system.qcow2" ]'
ID=$(sed -n 's/.*"id": "\(.*\)",*/\1/p' "$D/machine.json")
RT=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/lordos-vm
fdcount() { ls /proc/self/fd 2>/dev/null | wc -l; }
slow=0 bad_cycles=0 t0=$(ms)
for i in $(seq 1 "$N"); do
    c0=$(ms)
    "$VM" --start "$D" --detach --keep-running >/dev/null 2>&1 && wait_state "$D" running 30 || { bad_cycles=$((bad_cycles + 1)); echo "      cycle $i: didn't start: $(tail -2 "$D/log/lord-vm.log" | tr '\n' ' ')"; }
    lv=$(runpid "$D"); qp=$(qemupid "$D")
    if [ $((i % 4)) = 0 ]; then "$VM" --stop "$D" --timeout 5 >/dev/null 2>&1; else "$VM" --poweroff "$D" >/dev/null 2>&1; fi
    wait_gone "$lv" 20 && wait_gone "$qp" 20 || { bad_cycles=$((bad_cycles + 1)); echo "      cycle $i: lord-vm or QEMU still there"; }
    d=$(( $(ms) - c0 )); [ $d -gt $slow ] && slow=$d
done
total=$(( $(ms) - t0 ))
echo "      $N cycles in $((total / 1000)) s, $((total / N)) ms each on average, the slowest $slow ms"
t "every cycle started and stopped" '[ $bad_cycles = 0 ]' "$bad_cycles bad"
t "no lord-vm, QEMU or passt left for the machine" '! pgrep -f "$ID" >/dev/null && ! pgrep -f "lord-vm --start $D" >/dev/null'
t "no zombie processes" '[ -z "$(ps -eo stat= | grep "^Z")" ]'
t "no control socket left" '[ ! -e "$RT/$ID.sock" ]'
t "no cgroup scope left" '! systemctl --user list-units --all --no-legend "app-lordos-lordos-vm-$ID.scope" 2>/dev/null | grep -q scope && ! ls -d /sys/fs/cgroup/*/*/*lordos-vm-$ID* >/dev/null 2>&1'
t "the lock is free, no operation pending" 'flock -n "$D/lock" true && [ ! -f "$D/operation.json" ]'
t "logs rotated: at most three runs kept" '[ "$(ls "$D"/log/lord-vm*.log | wc -l)" -le 3 ]'
t "run.json says stopped" 'grep -q "\"state\":\"stopped\"" "$D/run.json"'
check_disks "$D" "disks clean after $N cycles"
gate_end "vm-leak"
