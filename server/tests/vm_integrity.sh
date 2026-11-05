#!/bin/sh
# vm_integrity.sh — Machines' release gate "disk integrity" (docs/VIRTUALIZATION.md 12): lord-vm or QEMU is killed
# (SIGKILL) while the guest writes, during a snapshot, a revert, a clone (linked and full), a compaction and a suspend;
# every disk then passes qemu-img check, right away (leaked clusters allowed: allocated, never referenced) and after the
# next open (clean: recovery gives leaks back). A power cut isn't simulated here.
#
#   sh tests/vm_integrity.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${3:-/tmp/lvg-int.$$}
. "$(dirname "$0")/vm_gate_lib.sh"
gate_begin
gate_creds
D=$W/a.lordvm
"$VM" --create "$D" --name integrity --import "$IMG" --vcpus 2 --memory 1024 >/dev/null 2>&1
t "create" '[ -f "$D/disks/system.qcow2" ]'
kill_at() {      # PID MS: SIGKILL after MS milliseconds
    sleep "$(echo "$2" | awk '{ printf "%.3f", $1 / 1000 }')"; kill -9 "$1" 2>/dev/null
}
case_kill() {    # LABEL WHO(qemu|lord-vm) OP [DELAY_MS]: the machine runs and writes; OP starts in the background; WHO is killed
    label=$1 who=$2 op=$3 delay=${4:-300}
    echo "-- $label"
    if ! start_busy "$D"; then bad "$label: start" "$(tail -3 "$D/log/lord-vm.log")"; return; fi
    lv=$(runpid "$D"); qp=$(qemupid "$D")
    case $op in
        write) ;;
        snapshot) "$VM" --snapshot "$D" take "gate $(ms)" >/dev/null 2>&1 & ;;
        revert) "$VM" --snapshot "$D" revert "$REVERT_TAG" >/dev/null 2>&1 & ;;
        suspend) "$VM" --suspend "$D" >/dev/null 2>&1 & ;;
    esac
    if [ "$who" = qemu ]; then kill_at "$qp" "$delay"; else kill_at "$lv" "$delay"; fi
    wait_gone "$lv" 20 && wait_gone "$qp" 20
    t "$label: lord-vm and QEMU gone" '! kill -0 "$lv" 2>/dev/null && ! kill -0 "$qp" 2>/dev/null'
    [ -f "$D/operation.json" ] && echo "      (interrupted in the middle: operation.json $(sed -n 's/.*"op":"\([^"]*\)".*/\1/p' "$D/operation.json"))"
    check_disks "$D" "$label: qemu-img check right after" leaks-ok
    reopen "$D" "$label: qemu-img check after the next open"
    t "$label: no operation left" '[ ! -f "$D/operation.json" ]'
}
case_kill "writes, QEMU killed" qemu write 2000
case_kill "writes, lord-vm killed" lord-vm write 2000
case_kill "snapshot, lord-vm killed" lord-vm snapshot 300
case_kill "snapshot, QEMU killed" qemu snapshot 300
# a snapshot to go back to (complete), then going back is interrupted
start_busy "$D" && "$VM" --snapshot "$D" take "gate base" >/dev/null 2>&1
REVERT_TAG=$("$VM" --snapshot "$D" list | sed -n 's/.*"tag":"\([^"]*\)","name":"gate base".*/\1/p' | head -1)
"$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
if [ -n "$REVERT_TAG" ]; then case_kill "revert, lord-vm killed" lord-vm revert 250; case_kill "revert, QEMU killed" qemu revert 250
else skp "revert" "no snapshot to go back to"; fi
case_kill "suspend, lord-vm killed" lord-vm suspend 400
case_kill "suspend, QEMU killed" qemu suspend 400
# clones, killed in the middle (several times each: the timing varies)
for s in $("$VM" --snapshot "$D" list | sed 's/"tag":"/\n/g' | sed -n 's/^\(s[0-9-]*\)".*/\1/p'); do "$VM" --snapshot "$D" delete "$s" >/dev/null 2>&1; done
for i in 1 2 3; do
    echo "-- full clone $i, lord-vm --clone killed"
    "$VM" --clone "$D" "$W/full$i.lordvm" --name "full $i" >/dev/null 2>&1 &
    cp=$!; kill_at "$cp" $((400 * i)); wait $cp 2>/dev/null
    check_disks "$D" "full clone $i: the source's disks"
    [ -d "$W/full$i.lordvm" ] && check_disks "$W/full$i.lordvm" "full clone $i: the clone's disks (it finished)"
done
"$VM" --clone "$D" "$W/after.lordvm" --name after >/dev/null 2>&1
t "full clone: a killed clone's hidden folder is swept by the next one" '! ls -d "$W"/.*.part-* >/dev/null 2>&1'
for i in 1 2 3 4; do
    echo "-- linked clone $i, killed after $((i * 15)) ms"
    "$VM" --clone "$D" "$W/link$i.lordvm" --name "link $i" --linked >/dev/null 2>&1 &
    cp=$!; kill_at "$cp" $((i * 15)); wait $cp 2>/dev/null
    reopen "$D" "linked clone $i: the source after its next open"
    [ -d "$W/link$i.lordvm" ] && check_disks "$W/link$i.lordvm" "linked clone $i: the clone (it finished)"
    if grep -q '"base"' "$D/machine.json"; then echo "      (the source is now on a shared base)"; break; fi
done
# compaction (a machine without a base or snapshots: the full clone made above)
C=$W/after.lordvm
for i in 1 2; do
    echo "-- compaction $i, killed"
    "$VM" --compact "$C" >/dev/null 2>&1 &
    cp=$!; kill_at "$cp" $((300 * i)); wait $cp 2>/dev/null
    reopen "$C" "compaction $i: after the next open"
    t "compaction $i: no half-made file left" '! ls "$C"/disks/.system.qcow2.compact >/dev/null 2>&1'
done
gate_end "vm-integrity"
