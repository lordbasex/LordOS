#!/bin/sh
# vm_recovery.sh — Machines' release gate "failure recovery" (docs/VIRTUALIZATION.md 3.6, 12): each multi-step operation
# (suspend, resume, snapshot take, revert and delete, compaction, linked clone) is interrupted by killing lord-vm, and
# the next open (lord-vm, as the app starts it: no terminal) finishes it or rolls it back, says which in the log, and
# leaves nothing half-made: no operation.json, no temporary file, snapshots.json and the disks' tags agreeing.
# Then the network (6.5): passt is killed while the guest fetches over HTTP twice a second, and the guest's network
# must come back by itself (measured), again after more kills, until lord-vm gives up after 5 in a minute and says so;
# and SIGTERM to the whole machine at once (a host shutdown) saves it to disk. Needs the network (deb.debian.org).
#
#   sh tests/vm_recovery.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${3:-${TMPDIR:-/var/tmp}/lvg-rec.$$}   # /var/tmp: /tmp is often a small tmpfs, and full clones need room
. "$(dirname "$0")/vm_gate_lib.sh"
gate_begin
gate_creds
D=$W/r.lordvm
"$VM" --create "$D" --name recovery --import "$IMG" --vcpus 2 --memory 1024 >/dev/null 2>&1
t "create" '[ -f "$D/disks/system.qcow2" ]'
tags() { qemu-img snapshot -l -U "$D/disks/system.qcow2" 2>/dev/null | awk 'NR > 2 { print $2 }' | sort | tr '\n' ' '; }
jtags() { "$VM" --snapshot "$D" list | sed 's/"tag":"/\n/g' | sed -n 's/^\(s[0-9-]*\)".*/\1/p' | sort | tr '\n' ' '; }
said() { grep -q "recovery: .*$1" "$D/log/lord-vm.log"; }
interrupt() {    # LABEL COMMAND...: the machine runs; COMMAND in the background; lord-vm killed when operation.json appears
    label=$1; shift
    lv=$(runpid "$D")
    "$@" >/dev/null 2>&1 &
    cp=$!
    end=$(( $(ms) + 20000 ))
    while [ "$(ms)" -lt "$end" ] && [ ! -f "$D/operation.json" ]; do sleep 0.01; done
    hit=0; [ -f "$D/operation.json" ] && hit=1
    sleep "${DELAY:-0.05}"; kill -9 "$lv" 2>/dev/null; wait_gone "$lv" 20; wait $cp 2>/dev/null
    HIT=0
    if [ $hit = 1 ] && [ -f "$D/operation.json" ]; then HIT=1; ok "$label: interrupted with operation.json in place"; else skp "$label: interrupted" "the operation finished before the kill"; fi
}

echo "-- suspend"
start_busy "$D"
DELAY=0.1 interrupt "suspend" "$VM" --suspend "$D"
"$VM" --start "$D" --detach --keep-running >/dev/null 2>&1; wait_state "$D" running 60
[ $HIT = 0 ] || t "suspend: rolled back or finished, and said so" 'said "suspend\|the suspend"'
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
[ $HIT = 0 ] || t "snapshot take: rolled back or finished, and said so" 'said "snapshot"'
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
[ $HIT = 0 ] || t "snapshot revert: finished on the disks, and said so" 'said "snapshot\|going back"'
"$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
check_disks "$D" "snapshot revert: disks clean"

echo "-- compaction"
for s in $(jtags); do "$VM" --snapshot "$D" delete "$s" >/dev/null 2>&1; done
"$VM" --compact "$D" > "$W/compact.out" 2>&1 &
cp=$!
end=$(( $(ms) + 20000 ))
while [ "$(ms)" -lt "$end" ] && [ ! -f "$D/operation.json" ]; do sleep 0.01; done
t "compaction: started (operation.json in place)" '[ -f "$D/operation.json" ]' "$(tail -2 "$W/compact.out")"
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

echo "-- network: passt killed while the guest fetches"
PROBE=$(printf '%s\n' '[Unit]' 'After=network-online.target' '[Service]' "ExecStart=/bin/bash -c 'while :; do if timeout 2 bash -c \"exec 3<>/dev/tcp/deb.debian.org/80; printf \\\\\"HEAD / HTTP/1.0\\\\\\\\r\\\\\\\\nHost: deb.debian.org\\\\\\\\r\\\\\\\\n\\\\\\\\r\\\\\\\\n\\\\\" >&3; head -1 <&3\" >/dev/null 2>&1; then echo \"<3>NETOK\" >/dev/kmsg; else echo \"<3>NETFAIL\" >/dev/kmsg; fi; sleep 0.5; done'")
umask 077
{ grep -v 'systemd\.' "$W/creds"; echo "binary:systemd.extra-unit.lordvm-net.service=$(printf '%s\n' "$PROBE" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '[Unit]\nWants=lordvm-net.service\n' | base64 -w0)"; } >"$W/netcreds"
umask 022
N=$W/n.lordvm
"$VM" --create "$N" --name network --import "$IMG" --vcpus 1 --memory 768 --display none >/dev/null 2>&1
"$VM" --start "$N" --detach --keep-running --credentials-file "$W/netcreds" >/dev/null 2>&1
wait_state "$N" running 60; wait_serial "$N" NETOK 180
oks() { grep -ac NETOK "$N/log/serial.log"; }
t "network: the guest fetches before" '[ "$(oks)" -gt 0 ]'
back_after_kill() {    # kills passt, prints the milliseconds until the guest's next successful fetch (host clock), or -1
    pp=$(sed -n 's/.*"passt_pid":\([0-9]*\).*/\1/p' "$N/run.json")
    n0=$(oks); t0=$(ms); kill -9 "$pp" 2>/dev/null
    while [ $(( $(ms) - t0 )) -lt 30000 ]; do [ "$(oks)" -gt "$n0" ] && { echo $(( $(ms) - t0 )); return; }; sleep 0.05; done
    echo -1
}
b1=$(back_after_kill)
t "network: back by itself after passt was killed ($b1 ms to the guest's next fetch)" '[ "$b1" -ge 0 ] && [ "$b1" -lt 10000 ]'
t "network: lord-vm said it reconnected" 'grep -q "network: the network is back" "$N/log/lord-vm.log"'
sleep 2; b2=$(back_after_kill); sleep 2; b3=$(back_after_kill)
t "network: back again after two more kills ($b2 ms, $b3 ms)" '[ "$b2" -ge 0 ] && [ "$b3" -ge 0 ]'
t "network: the new passt runs" 'kill -0 "$(sed -n "s/.*\"passt_pid\":\([0-9]*\).*/\1/p" "$N/run.json")"'
for k in 1 2 3; do pp=$(sed -n 's/.*"passt_pid":\([0-9]*\).*/\1/p' "$N/run.json"); kill -9 "$pp" 2>/dev/null; sleep 9; done
t "network: after 5 restarts in a minute it gives up, and says so" 'grep -q "again and again: the machine has no network" "$N/log/lord-vm.log"'
t "network: the machine itself keeps running" '[ "$(field "$N" state)" = running ]'

echo "-- the computer stops: SIGTERM to lord-vm, QEMU and passt at once"
lv=$(runpid "$N"); qp=$(qemupid "$N")
kill -TERM "$lv" "$qp" 2>/dev/null; wait_gone "$lv" 60
t "host stop: the machine was saved to disk, not shut down" '[ -f "$N/suspend.state" ] && grep -q "state stopped (suspended)" "$N/log/lord-vm.log"'
"$VM" --start "$N" --detach --keep-running >/dev/null 2>&1; wait_state "$N" running 60
t "host stop: the next start resumes it" 'grep -q "resume: loaded" "$N/log/lord-vm.log" && [ ! -f "$N/suspend.state" ]'
pl=$(sed -n 's/.*"passt_pid":\([0-9]*\).*/\1/p' "$N/run.json")
"$VM" --poweroff "$N" >/dev/null 2>&1; wait_state "$N" stopped 30
t "network: its passt is gone with it" 'sleep 1; ! kill -0 "$pl" 2>/dev/null'
gate_end "vm-recovery"
