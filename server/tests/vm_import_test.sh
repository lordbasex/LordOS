#!/bin/sh
# vm_import_test.sh — Machines: what `lord-vm --create --import` refuses (docs/VIRTUALIZATION.md, "Phase 1
# implementation", Import). A crafted qcow2 can name a backing file or an external data file; if the import
# followed it, the new disk would hold a copy of a file the guest must never read. Also: a raw file named .qcow2,
# a qcow2 with a backing file hidden under a .img name, and a system disk swapped for one with a backing file
# after the machine was made (lord-vm refuses to start it).
#
#   sh tests/vm_import_test.sh BUILD_DIR
#
# Needs qemu-img and qemu-system-<this arch> (no KVM: nothing boots). One line per check; exit 1 on any FAIL.
set -u
BIN=$(cd "$1" && pwd) || exit 2
VM="$BIN/lord-vm"
W=$(mktemp -d /tmp/lvimp.XXXXXX) || exit 2
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
command -v qemu-img >/dev/null || { echo "SKIP  qemu-img isn't installed"; exit 0; }

# the "secret": a file the person can read, with a marker the guest must never see
SECRET="$W/secret.raw"
head -c 1048576 /dev/zero >"$SECRET"
printf 'LORDOS-IMPORT-SECRET-MARKER' | dd of="$SECRET" conv=notrunc 2>/dev/null
has_secret() { qemu-img convert -q -O raw "$1" "$W/probe.raw" 2>/dev/null && grep -aq LORDOS-IMPORT-SECRET-MARKER "$W/probe.raw"; r=$?; rm -f "$W/probe.raw"; return $r; }

refused() {   # NAME IMAGE
    D="$W/m-$1.lordvm"
    if "$VM" --create "$D" --name "$1" --import "$2" --network isolated --vcpus 1 --memory 512 >"$W/$1.out" 2>&1; then
        bad "$1: refused" "$(cat "$W/$1.out")"
        [ -f "$D/disks/system.qcow2" ] && has_secret "$D/disks/system.qcow2" && bad "$1: the secret reached the new disk"
    else
        [ -f "$D/machine.json" ] || [ -f "$D/disks/system.qcow2" ] && bad "$1: nothing left behind" || ok "$1: refused ($(tail -1 "$W/$1.out" | cut -c1-90))"
    fi
}

qemu-img create -q -f qcow2 -F raw -b "$SECRET" "$W/backing.qcow2" 1M
refused backing-file "$W/backing.qcow2"
cp "$W/backing.qcow2" "$W/backing-as.img"
refused backing-file-named-img "$W/backing-as.img"
cp "$SECRET" "$W/data-copy.raw"
if qemu-img create -q -f qcow2 -o data_file="$W/data-copy.raw",data_file_raw=on "$W/datafile.qcow2" 1M 2>/dev/null; then
    refused external-data-file "$W/datafile.qcow2"
else echo "SKIP  external-data-file: this qemu-img can't make one"; fi
cp "$SECRET" "$W/raw-pretends.qcow2"
refused raw-named-qcow2 "$W/raw-pretends.qcow2"
cp "$SECRET" "$W/unknown.vmdk"
refused unknown-extension "$W/unknown.vmdk"
printf 'QFI\373\000\000\000\003' >"$W/short.qcow2"
refused short-header "$W/short.qcow2"
# a qcow2 header with the backing offset patched in by hand (qemu-img can't make it point anywhere odd)
qemu-img create -q -f qcow2 "$W/patched.qcow2" 1M
printf '\000\000\000\000\000\000\002\000\000\000\000\010' | dd of="$W/patched.qcow2" bs=1 seek=8 conv=notrunc 2>/dev/null
refused patched-backing-offset "$W/patched.qcow2"

# a good qcow2 and a good raw image: accepted, and the new disk has no backing file
qemu-img create -q -f qcow2 "$W/good.qcow2" 64M
D="$W/m-good.lordvm"
if "$VM" --create "$D" --name good --import "$W/good.qcow2" --network isolated --vcpus 1 --memory 512 >"$W/good.out" 2>&1; then
    if qemu-img info "$D/disks/system.qcow2" | grep -q "backing file"; then bad "good qcow2: new disk without backing"; else ok "good qcow2: imported, no backing file"; fi
else bad "good qcow2: imported" "$(cat "$W/good.out")"; fi
head -c 2097152 /dev/zero >"$W/good.img"
D2="$W/m-raw.lordvm"
if "$VM" --create "$D2" --name raw --import "$W/good.img" --network isolated --vcpus 1 --memory 512 >"$W/raw.out" 2>&1; then ok "good raw .img: imported"
else bad "good raw .img: imported" "$(cat "$W/raw.out")"; fi

# --set (Configure) on the raw machine, which is off: CPU, memory and network change, the disk grows and never
# shrinks, an ISO is attached and taken out, a held lock (a running machine) refuses every change
if [ -f "$D2/machine.json" ]; then
    if "$VM" --set "$D2" --vcpus 1 --memory 768 --network shared >"$W/set.out" 2>&1 && grep -q '"memory_mb": 768' "$D2/machine.json" && grep -q '"network": "shared"' "$D2/machine.json"
    then ok "set: memory and network changed"; else bad "set: memory and network changed" "$(cat "$W/set.out")"; fi
    if "$VM" --set "$D2" --disk 21 >"$W/grow.out" 2>&1 && qemu-img info "$D2/disks/system.qcow2" | grep -q "virtual size: 21 GiB"
    then ok "set: disk grown to 21 GB"; else bad "set: disk grown to 21 GB" "$(cat "$W/grow.out") $(qemu-img info "$D2/disks/system.qcow2" | grep virtual)"; fi
    if "$VM" --set "$D2" --disk 19 >"$W/shrink.out" 2>&1; then bad "set: disk never shrinks"; else ok "set: disk never shrinks ($(tail -1 "$W/shrink.out" | cut -c1-60))"; fi
    head -c 65536 /dev/zero >"$W/cd.iso"
    if "$VM" --set "$D2" --iso "$W/cd.iso" >/dev/null 2>&1 && grep -q '"iso"' "$D2/machine.json" && "$VM" --set "$D2" --iso none >/dev/null 2>&1 && ! grep -q '"iso"' "$D2/machine.json"
    then ok "set: ISO attached and taken out"; else bad "set: ISO attached and taken out"; fi
    if "$VM" --set "$D2" --memory 999999 >/dev/null 2>&1; then bad "set: memory out of range refused"; else ok "set: memory out of range refused"; fi
    ( flock -x 9; "$VM" --set "$D2" --vcpus 1 >"$W/locked.out" 2>&1; echo $? >"$W/locked.rc" ) 9>"$D2/lock"
    if [ "$(cat "$W/locked.rc")" = 3 ]; then ok "set: refused while the machine runs (lock held)"; else bad "set: refused while running" "$(cat "$W/locked.out")"; fi
fi

# the system disk swapped afterwards for one with a backing file: lord-vm refuses to start it
if [ -f "$D/disks/system.qcow2" ]; then
    cp "$W/backing.qcow2" "$D/disks/system.qcow2"
    timeout 30 "$VM" --start "$D" >"$W/start.out" 2>&1
    if "$VM" --status "$D" --json | grep -q '"reason":"start-failed"' && grep -q "system disk can't be used" "$D/log/lord-vm.log"; then ok "swapped system disk: start refused"
    else bad "swapped system disk: start refused" "$("$VM" --status "$D" --json) $(tail -3 "$D/log/lord-vm.log")"; fi
fi

echo "vm-import: $pass passed, $fail failed"
[ "$fail" = 0 ]
