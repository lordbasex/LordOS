#!/bin/sh
# vm_recovery.sh — Machines' release gate "failure recovery" (docs/VIRTUALIZATION.md 3.6, 12): each multi-step operation
# (suspend, resume, snapshot take, revert and delete, compaction, linked clone) is interrupted by killing lord-vm, and
# the next open (lord-vm, as the app starts it: no terminal) finishes it or rolls it back, says which in the log, and
# leaves nothing half-made: no operation.json, no temporary file, snapshots.json and the disks' tags agreeing.
#
#   sh tests/vm_recovery.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${3:-/tmp/lvg-rec.$$}
. "$(dirname "$0")/vm_gate_lib.sh"
gate_begin
gate_creds
D=$W/r.lordvm
"$VM" --create "$D" --name recovery --import "$IMG" --vcpus 2 --memory 1024 >/dev/null 2>&1
t "create" '[ -f "$D/disks/system.qcow2" ]'
tags() { qemu-img snapshot -l -U "$D/disks/system.qcow2" 2>/dev/null | awk 'NR > 2 { print $2 }' | sort | tr '\n' ' '; }
jtags() { "$VM" --snapshot "$D" list | sed 's/"tag":"/\n/g' | sed -n 's/^\(s[0-9-]*\)".*/\1/p' | sort | tr '\n' ' '; }
said() { tail -40 "$D/log/lord-vm.log" | grep -q "recovery: .*$1"; }
interrupt() {    # LABEL COMMAND...: the machine runs; COMMAND in the background; lord-vm killed when operation.json appears
    label=$1; shift
    lv=$(runpid "$D")
    "$@" >/dev/null 2>&1 &
    cp=$!
    end=$(( $(ms) + 20000 ))
    while [ "$(ms)" -lt "$end" ] && [ ! -f "$D/operation.json" ]; do sleep 0.01; done
    hit=0; [ -f "$D/operation.json" ] && hit=1
    sleep "${DELAY:-0.05}"; kill -9 "$lv" 2>/dev/null; wait_gone "$lv" 20; wait $cp 2>/dev/null
    if [ $hit = 1 ] && [ -f "$D/operation.json" ]; then ok "$label: interrupted with operation.json in place"; else skp "$label: interrupted" "the operation finished before the kill"; fi
}

echo "-- suspend"
start_busy "$D"
DELAY=0.1 interrupt "suspend" "$VM" --suspend "$D"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60
t "suspend: rolled back or finished, and said so" 'said "suspend\|the suspend"'
t "suspend: no temporary state left" '! ls "$D"/.suspend.* >/dev/null 2>&1 && [ ! -f "$D/operation.json" ]'
"$VM" --suspend "$D" >/dev/null 2>&1; wait_state "$D" stopped 60

echo "-- resume"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1 &
end=$(( $(ms) + 20000 ))
while [ "$(ms)" -lt "$end" ] && ! grep -q '"op":"resume"' "$D/operation.json" 2>/dev/null; do sleep 0.005; done
lv=$(runpid "$D"); kill -9 "$lv" 2>/dev/null; wait_gone "$lv" 20; wait 2>/dev/null
if [ -f "$D/suspend.state" ]; then ok "resume: interrupted before it finished: the saved state is kept"
else skp "resume: interrupted" "it finished before the kill"; fi
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60
t "resume: the next start goes on from the saved state or from the disks" '[ "$(field "$D" state)" = running ] && [ ! -f "$D/operation.json" ]'

echo "-- snapshot take"
interrupt "snapshot take" "$VM" --snapshot "$D" take "interrupted"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60
t "snapshot take: rolled back or finished, and said so" 'said "snapshot"'
"$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
t "snapshot take: snapshots.json and the disks agree ($(jtags)/ $(tags))" '[ "$(jtags)" = "$(tags)" ]'

echo "-- snapshot delete"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60
"$VM" --snapshot "$D" take "to delete" >/dev/null 2>&1
DT=$(jtags | awk '{ print $NF }')
interrupt "snapshot delete" "$VM" --snapshot "$D" delete "$DT"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60; "$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
t "snapshot delete: finished: the tag is gone everywhere" '! echo "$(jtags) $(tags)" | grep -q "$DT"'

echo "-- snapshot revert"
start_busy "$D"
"$VM" --snapshot "$D" take "to go back to" >/dev/null 2>&1
RT=$(jtags | awk '{ print $NF }')
interrupt "snapshot revert" "$VM" --snapshot "$D" revert "$RT"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60
t "snapshot revert: finished on the disks, and said so" 'said "snapshot\|going back"'
"$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
check_disks "$D" "snapshot revert: disks clean"

echo "-- compaction"
for s in $(jtags); do "$VM" --snapshot "$D" delete "$s" >/dev/null 2>&1; done
"$VM" --compact "$D" >/dev/null 2>&1 &
cp=$!
end=$(( $(ms) + 20000 ))
while [ "$(ms)" -lt "$end" ] && [ ! -f "$D/operation.json" ]; do sleep 0.01; done
sleep 0.3; kill -9 $cp 2>/dev/null; pkill -9 -f "qemu-img convert.*$D" 2>/dev/null; wait $cp 2>/dev/null
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60; "$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
t "compaction: rolled back, and said so" 'said "compaction"'
t "compaction: no half-made file, no operation" '! ls "$D"/disks/.system.qcow2.compact >/dev/null 2>&1 && [ ! -f "$D/operation.json" ]'
check_disks "$D" "compaction: disks clean"

echo "-- linked clone"
ok_link=0
for d in 0.005 0.02 0.04 0.08; do
    "$VM" --clone "$D" "$W/l.lordvm" --name linked --linked >/dev/null 2>&1 &
    cp=$!; sleep $d; kill -9 $cp 2>/dev/null; wait $cp 2>/dev/null
    [ -f "$D/operation.json" ] && { ok_link=1; break; }
    rm -rf "$W/l.lordvm"
    if grep -q '"base"' "$D/machine.json"; then break; fi
done
if [ $ok_link = 1 ]; then ok "linked clone: interrupted with operation.json in place"; else skp "linked clone: interrupted" "it finished before the kill"; fi
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60; "$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
t "linked clone: finished or rolled back: a disk to start from, no half-made clone" '[ -f "$D/disks/system.qcow2" ] && [ ! -f "$D/operation.json" ] && ! ls -d "$W"/.*.part-* >/dev/null 2>&1'
if grep -q '"base"' "$D/machine.json"; then t "linked clone: the source runs on its base" '[ -f "$(sed -n "s/.*\"base\": \"\(.*\)\",*/\1/p" "$D/machine.json")" ]'
else t "linked clone: the disk is back in its folder, no base left" '! ls "$W"/.shared/*.qcow2 >/dev/null 2>&1'; fi
check_disks "$D" "linked clone: disks clean"
gate_end "vm-recovery"
