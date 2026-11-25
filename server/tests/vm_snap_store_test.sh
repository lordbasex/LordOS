#!/bin/bash
# Snapshots' settings and Reclaim space's compression, end to end with the real lord-vm, qemu-img and QEMU/KVM
# (docs/VIRTUALIZATION.md 6.2 "What a snapshot holds", 6.4, 6.16): each snapshot keeps the machine's settings
# (snapshots/<tag>.json), going back puts its hardware back and keeps every other choice, a disk added after it refuses
# going back before anything changes, deleting it removes its files; a running machine's snapshot keeps the hardware it
# runs with; --compact --compress compresses the disk with its data intact, and is refused for an encrypted machine.
# Headless, isolated network, a private HOME under /var/tmp. Run from server/: bash tests/vm_snap_store_test.sh
set -u
cd "$(dirname "$0")/.." || exit 1
LV=$PWD/build/lord-vm
[ -x "$LV" ] || { echo "skipped: needs build/lord-vm"; exit 77; }
[ -r /dev/kvm ] && [ -w /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null && command -v qemu-img >/dev/null && command -v qemu-io >/dev/null &&
    command -v python3 >/dev/null && [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] ||
    { echo "skipped: needs KVM, QEMU (qemu-img, qemu-io), OVMF and python3"; exit 77; }
T=/var/tmp/lordos-vmsnapstore-test.$$
export HOME=$T/home XDG_RUNTIME_DIR=$T/run
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$T/lib"; chmod 700 "$XDG_RUNTIME_DIR"
cleanup() { for d in "$T"/lib/*.lordvm; do [ -d "$d" ] && "$LV" --poweroff "$d" >/dev/null 2>&1; done; sleep 1; rm -rf "$T"; }
trap cleanup EXIT
FAILS=0 PASSES=0
ok() { echo "ok: $*"; PASSES=$((PASSES + 1)); }
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
t() { if eval "$2"; then ok "$1"; else bad "$1${3:+: $3}"; fi; }
jget() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get(sys.argv[2], ""))' "$1" "$2" 2>/dev/null; }
M=$T/lib/s.lordvm
running() { "$LV" --status "$M" 2>/dev/null | grep -q running; }

# ---- the settings kept with each snapshot, and put back
"$LV" --create "$M" --name Snaps --vcpus 1 --memory 512 --disk 1 --display none --network isolated >/dev/null || { bad create; exit 1; }
"$LV" --snapshot "$M" take "One CPU" s1 >/dev/null 2>"$T/e" || bad "take s1: $(cat "$T/e")"
t "s1 keeps machine.json as it was" '[ -f "$M/snapshots/s1.json" ] && [ "$(jget "$M/snapshots/s1.json" vcpus)" = 1 ] && [ "$(jget "$M/snapshots/s1.json" memory_mb)" = 512 ]'
t "s1.json is the person's only (0600)" '[ "$(stat -c %a "$M/snapshots/s1.json")" = 600 ]'
"$LV" --set "$M" --vcpus 2 --memory 768 --boot-menu on >/dev/null || bad "set"
"$LV" --snapshot "$M" take "Two CPUs" s2 >/dev/null || bad "take s2"
t "s2 keeps the new settings" '[ "$(jget "$M/snapshots/s2.json" vcpus)" = 2 ]'
"$LV" --snapshot "$M" revert s1 >/dev/null 2>"$T/e" || bad "revert s1: $(cat "$T/e")"
t "back to s1: its hardware is back (1 vCPU, 512 MB)" '[ "$(jget "$M/machine.json" vcpus)" = 1 ] && [ "$(jget "$M/machine.json" memory_mb)" = 512 ]'
t "back to s1: other choices kept (the boot menu)" '[ "$(jget "$M/machine.json" boot_menu)" = True ]'
t "back to s1: name and id kept" '[ "$(jget "$M/machine.json" name)" = Snaps ] && [ "$(jget "$M/machine.json" id)" = "$(jget "$M/snapshots/s1.json" id)" ]'
"$LV" --snapshot "$M" revert s2 >/dev/null || bad "revert s2"
t "back to s2: 2 vCPUs, 768 MB" '[ "$(jget "$M/machine.json" vcpus)" = 2 ] && [ "$(jget "$M/machine.json" memory_mb)" = 768 ]'
"$LV" --set "$M" --add-disk 1 >/dev/null 2>"$T/e" && bad "a disk was added to a machine with snapshots"
t "no disk is added while there are snapshots" 'grep -q "has snapshots" "$T/e"' "$(cat "$T/e")"
echo "note" >"$M/snapshots/s2.note"; : >"$M/snapshots/s2.png"
"$LV" --snapshot "$M" delete s2 >/dev/null || bad "delete s2"
t "deleting s2 removes its settings, note and picture" '[ ! -e "$M/snapshots/s2.json" ] && [ ! -e "$M/snapshots/s2.note" ] && [ ! -e "$M/snapshots/s2.png" ]'
# settings that say fewer disks than the machine has now (a hand-made machine.json): going back is refused before
# anything changes
"$LV" --snapshot "$M" delete s1 >/dev/null || bad "delete s1"
"$LV" --set "$M" --add-disk 1 >/dev/null 2>&1 || bad "add a disk"
"$LV" --snapshot "$M" take "With a disk" s4 >/dev/null || bad "take s4"
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d.pop("data_disks", None); json.dump(d, open(p, "w"))' "$M/snapshots/s4.json"
cp "$M/machine.json" "$T/before.json"
"$LV" --snapshot "$M" revert s4 >/dev/null 2>"$T/e" && bad "a revert with fewer disks in its settings went on"
t "refused with the reason" 'grep -q "a disk was added after this snapshot" "$T/e"' "$(cat "$T/e")"
t "refused before anything changed (machine.json, no operation.json)" 'cmp -s "$M/machine.json" "$T/before.json" && [ ! -e "$M/operation.json" ]'
rm -f "$M/snapshots/s4.json"
"$LV" --snapshot "$M" revert s4 >/dev/null 2>"$T/e"; t "a snapshot without settings (taken before they were kept) goes back as before" '[ $? = 0 ]' "$(cat "$T/e")"
"$LV" --snapshot "$M" delete s4 >/dev/null || bad "delete s4"

# ---- a running machine's snapshot keeps the hardware it runs with, not a change waiting for its next start
"$LV" --start "$M" --detach --dev-qmp >/dev/null 2>&1; sleep 2
if running; then
    "$LV" --set "$M" --vcpus 3 >/dev/null 2>&1   # refused while it runs, or kept for its next start: either way the snapshot says 2
    "$LV" --snapshot "$M" take "Running" s3 >/dev/null 2>"$T/e" || bad "take s3 running: $(cat "$T/e")"
    t "the running snapshot holds memory and the hardware it runs with (2 vCPUs)" '[ "$(jget "$M/snapshots/s3.json" vcpus)" = 2 ] && grep -q "\"memory\":true" "$M/snapshots.json"'
    "$LV" --snapshot "$M" revert s3 >/dev/null 2>"$T/e" || bad "revert s3 running: $(cat "$T/e")"
    t "back to s3 while running: machine.json says its hardware" '[ "$(jget "$M/machine.json" vcpus)" = 2 ]'
    "$LV" --poweroff "$M" >/dev/null 2>&1; for _ in $(seq 1 30); do running || break; sleep 0.3; done
    "$LV" --snapshot "$M" delete s3 >/dev/null || bad "delete s3"
else bad "the machine didn't start: $(tail -3 "$M/log/lord-vm.log")"; fi

# ---- Reclaim space with compression (6.4)
SYS=$M/disks/system.qcow2
qemu-io -f qcow2 "$SYS" -c "write -P 0x41 0 96M" >/dev/null 2>&1 || bad "write a pattern"
before=$(du -k "$SYS" | cut -f1)
"$LV" --compact "$M" --estimate >"$T/est.json" 2>&1 || bad "estimate: $(cat "$T/est.json")"
"$LV" --compact "$M" --compress >"$T/c.json" 2>"$T/e" || bad "compact --compress: $(cat "$T/e")"
after=$(du -k "$SYS" | cut -f1)
echo "   system disk: ${before} KB before, ${after} KB after ($(cat "$T/c.json"))"
t "compressed: it says so, and it is at most a fifth of what it was" 'grep -q "\"compressed\":true" "$T/c.json" && [ "$after" -lt $((before / 5)) ]'
t "the compressed disk passes qemu-img check" 'qemu-img check -q -f qcow2 "$SYS"'
t "its data is intact (96 MB of the pattern read back)" 'qemu-io -f qcow2 "$SYS" -c "read -P 0x41 0 96M" 2>&1 | grep -q "read 100663296"'
t "qemu-img map shows compressed clusters" 'qemu-img map --output=json -f qcow2 "$SYS" | grep -q "\"compressed\": true"'
"$LV" --start "$M" --detach >/dev/null 2>&1; sleep 2
t "the machine starts on its compressed disk" running "$(tail -3 "$M/log/lord-vm.log")"
"$LV" --poweroff "$M" >/dev/null 2>&1; for _ in $(seq 1 30); do running || break; sleep 0.3; done
"$LV" --compact "$M" --compress --bogus >/dev/null 2>&1; t "an unknown argument is refused" '[ $? = 2 ]'
# an encrypted machine: refused (qcow2 has no compressed encrypted clusters)
E=$T/lib/e.lordvm
KEY=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
"$LV" --create "$E" --name E --memory 512 --disk 1 --display none --network isolated --encrypt on --key-fd 5 5< <(printf %s "$KEY") >/dev/null || bad "create encrypted"
"$LV" --compact "$E" --compress --key-fd 5 5< <(printf %s "$KEY") >/dev/null 2>"$T/e" && bad "an encrypted machine was compressed"
t "an encrypted machine's disk isn't compressed (refused, said why)" 'grep -q "encrypted machine" "$T/e"' "$(cat "$T/e")"
echo "== $PASSES passed, $FAILS failed"
[ "$FAILS" = 0 ]
