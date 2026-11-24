#!/bin/bash
# Machines' storage end to end (docs/VIRTUALIZATION.md 6.2 disposable sessions, 6.4 raw disks and Export, 3.6 the kept
# UEFI code, 8 encrypted machines): real qemu-img and QEMU/KVM, headless (no display, isolated network), a private HOME
# and runtime folder under /var/tmp, never the person's library. Run from server/: bash tests/vm_storage_test.sh
# A write made through the guest's own disk device (QEMU's qemu-io on the virtio-blk device) shows what each mode keeps.
set -u
cd "$(dirname "$0")/.." || exit 1
LV=$PWD/build/lord-vm
[ -x "$LV" ] || { echo "skipped: needs build/lord-vm"; exit 77; }
[ -r /dev/kvm ] && [ -w /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null && command -v qemu-img >/dev/null && command -v qemu-io >/dev/null &&
    command -v swtpm >/dev/null && command -v python3 >/dev/null && [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] ||
    { echo "skipped: needs KVM, QEMU (qemu-img, qemu-io), swtpm, OVMF and python3"; exit 77; }
T=/var/tmp/lordos-vmstorage-test.$$
export HOME=$T/home XDG_RUNTIME_DIR=$T/run
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$T/lib" "$T/out"; chmod 700 "$XDG_RUNTIME_DIR"
KEY=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
kf() { printf %s "$KEY"; }
cleanup() {
    for d in "$T"/lib/*.lordvm; do [ -d "$d" ] && "$LV" --poweroff "$d" >/dev/null 2>&1; done
    sleep 1; rm -rf "$T"
}
trap cleanup EXIT
FAILS=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
M=$T/lib/e.lordvm
QS="driver=qcow2,file.filename=$M/disks/system.qcow2,encrypt.key-secret=k"
pattern() { qemu-io --object secret,id=k,data="$KEY" --image-opts "$QS" -c "read -P 0x$1 0 64k" 2>&1 | grep -q "read 65536"; }
running() { "$LV" --status "$M" 2>/dev/null | grep -q running; }
stopm() { "$LV" --poweroff "$M" >/dev/null 2>&1; for _ in $(seq 1 30); do running || return 0; sleep 0.3; done; }
startm() { "$LV" --start "$M" --detach --dev-qmp --key-fd 5 5< <(kf) >/dev/null 2>&1; sleep 2; running; }
gwrite() { "$LV" --qmp "$M" "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"qemu-io -d sysdisk \\\"write -P 0x$1 0 64k\\\"\"}}" >/dev/null; }

# ---- an encrypted machine (8)
"$LV" --create "$M" --name E --memory 512 --disk 1 --tpm on --display none --network isolated 2>/dev/null --encrypt on && bad "--encrypt without a key"
"$LV" --create "$M" --name E --memory 512 --disk 1 --tpm on --display none --network isolated --encrypt on --key-fd 5 5< <(kf) >/dev/null || { bad create; exit 1; }
grep -q '"encrypted": true' "$M/machine.json" && ok "machine.json: encrypted" || bad "machine.json"
for f in system nvram; do qemu-img info "$M/disks/$f.qcow2" | grep -q "encrypted: yes" || bad "$f.qcow2 isn't LUKS"; done; ok "system and UEFI variables are qcow2 LUKS"
"$LV" --start "$M" --detach >/dev/null 2>&1 && bad "started without its key" || ok "refused without its key"
"$LV" --start "$M" --detach --key-fd 5 5< <(printf %064d 0) >/dev/null 2>&1; sleep 1; running && bad "ran with a wrong key" || ok "refused with a wrong key"
stopm
startm || bad "start with its key"
grep -q "tpm: swtpm pid [0-9]*, state in disks/tpm, encrypted" "$M/log/lord-vm.log" && ok "TPM state encrypted by swtpm" || bad "swtpm key"
grep -q "secret,id=lvk,file=/proc/self/fd/" "$M/log/lord-vm.log" && ! grep -q "$KEY" "$M/log/lord-vm.log" && ok "the key reaches QEMU on a descriptor, never in the log" || bad "key path"
grep -q "landlock-fs=1.*seccomp(ours)=1.*allowlist=1" "$M/log/lord-vm.log" && ok "QEMU's sandbox unchanged" || bad "sandbox"
[ -f "$M/firmware-code.fd" ] && ok "the UEFI code is kept with the machine" || bad "firmware-code.fd"
[ -e "$M/log/serial.log" ] && bad "serial.log of an encrypted machine" || ok "no serial log on disk"
gwrite ab; stopm
pattern ab && ok "a normal session's write is kept" || bad "persistent write"
grep -aq "$(printf '\xab\xab\xab\xab\xab\xab\xab\xab\xab\xab\xab\xab')" "$M/disks/system.qcow2" && bad "the guest's bytes in the clear" || ok "the guest's bytes aren't in the file in the clear"
"$LV" --start "$M" --detach --key-fd 5 5< <(kf) >/dev/null 2>&1; sleep 2
"$LV" --suspend "$M" 2>&1 | grep -q "never saved to a file" && ok "suspend refused" || bad "suspend of an encrypted machine"
stopm

# ---- disposable sessions (6.2), both kinds
for mode in memory disk; do
    "$LV" --set "$M" --disposable $mode >/dev/null || bad "set disposable $mode"
    startm || bad "start ($mode session)"
    gwrite cd
    "$LV" --snapshot "$M" take x >/dev/null 2>&1; "$LV" --suspend "$M" 2>&1 | grep -q "keeps nothing" && ok "$mode: suspend refused" || bad "$mode suspend"
    stopm
    grep -q "disposable session: [0-9]* MB .*, gone" "$M/log/lord-vm.log" && ok "$mode: the session's layers went with QEMU" || bad "$mode log"
    pattern ab && ok "$mode: the session's write is gone after power-off" || bad "$mode: the session's write reached the disk"
    ls -a "$M/disks" | grep -q '^\.session' && bad "$mode: a session file is left" || ok "$mode: no session file left"
done
"$LV" --set "$M" --disposable off >/dev/null

# ---- offline snapshot, clone, export, import (6.3, 6.4)
"$LV" --snapshot "$M" take s1 --key-fd 5 5< <(kf) >/dev/null 2>&1
qemu-img snapshot -l --object secret,id=k,data="$KEY" --image-opts "$QS" | grep -q s20 && ok "snapshot with the key" || bad "snapshot"
NID=$(cat /proc/sys/kernel/random/uuid)
"$LV" --clone "$M" "$T/lib/c.lordvm" --key-fd 5 5< <(kf) >/dev/null 2>&1 && bad "clone of an encrypted machine without --new-id"
"$LV" --clone "$M" "$T/lib/c.lordvm" --new-id "$NID" --key-fd 5 5< <(kf) >/dev/null && grep -q "$NID" "$T/lib/c.lordvm/machine.json" &&
    qemu-img info "$T/lib/c.lordvm/disks/system.qcow2" | grep -q "encrypted: yes" && ok "clone: the id given, still encrypted" || bad clone
"$LV" --export "$M" --to "$T/out/e.lordvm" >/dev/null 2>&1 && bad "export without the key"
"$LV" --export "$M" --to "$T/out/e.lordvm" --key-fd 5 5< <(kf) >/dev/null && ok "export .lordvm" || bad export
qemu-img info "$T/out/e.lordvm/disks/system.qcow2" | grep -q "encrypted: yes" && ok "the export stays encrypted" || bad "export decrypted"
"$LV" --export "$M" --to "$T/out/e.ova" --format ova --key-fd 5 5< <(kf) >/dev/null 2>&1 && bad "OVA of an encrypted machine"
"$LV" --import-lordvm "$T/out/e.lordvm" "$T/lib/i.lordvm" >/dev/null 2>&1; [ $? = 4 ] && ok "import asks for the key" || bad "import without key"
"$LV" --import-lordvm "$T/out/e.lordvm" "$T/lib/i.lordvm" --key-fd 5 5< <(printf %064d 7) >/dev/null 2>&1; [ $? = 5 ] && ok "a wrong key is refused" || bad "wrong key at import"
"$LV" --import-lordvm "$T/out/e.lordvm" "$T/lib/i.lordvm" --key-fd 5 5< <(kf) >/dev/null && ok "import with the key" || bad import
cp -a "$T/out/e.lordvm" "$T/out/t1.lordvm"; printf X | dd of="$T/out/t1.lordvm/disks/nvram.qcow2" bs=1 seek=70000 conv=notrunc 2>/dev/null
"$LV" --import-lordvm "$T/out/t1.lordvm" "$T/lib/t1.lordvm" --key-fd 5 5< <(kf) 2>&1 | grep -q "doesn't match" && ok "a changed byte is refused" || bad tamper
cp -a "$T/out/e.lordvm" "$T/out/t2.lordvm"; echo x > "$T/out/t2.lordvm/run.sh"
"$LV" --import-lordvm "$T/out/t2.lordvm" "$T/lib/t2.lordvm" --key-fd 5 5< <(kf) 2>&1 | grep -q "doesn't match" && ok "an extra file is refused" || bad extra
cp -a "$T/out/e.lordvm" "$T/out/t3.lordvm"; ln -sf /etc/passwd "$T/out/t3.lordvm/machine.json"
"$LV" --import-lordvm "$T/out/t3.lordvm" "$T/lib/t3.lordvm" --key-fd 5 5< <(kf) 2>&1 | grep -q "doesn't match" && ok "a symlink is refused" || bad symlink

# ---- a plain machine: OVA, and a hostile export whose machine.json asks for everything
P=$T/lib/p.lordvm
"$LV" --create "$P" --name P --memory 512 --disk 1 --display none --network isolated >/dev/null || bad "plain create"
"$LV" --export "$P" --to "$T/out/p.ova" --format ova >/dev/null && tar -tf "$T/out/p.ova" | head -1 | grep -q '\.ovf$' && ok "OVA: the descriptor first" || bad ova
mkdir -p "$T/out/ova" && tar -xf "$T/out/p.ova" -C "$T/out/ova" && qemu-img info "$T/out/ova/disk1.vmdk" | grep -q streamOptimized &&
    (cd "$T/out/ova" && sed 's/SHA256(\(.*\))= \(.*\)/\2  \1/' machine.mf | sha256sum -c - >/dev/null) && ok "OVA: streamOptimized VMDK, .mf matches" || bad "ova contents"
"$LV" --export "$P" --to "$T/out/h.lordvm" >/dev/null && python3 -I tests/vm_export_hostile.py "$T/out/h.lordvm" || bad "hostile export made"
"$LV" --import-lordvm "$T/out/h.lordvm" "$T/lib/h.lordvm" >/dev/null || bad "hostile import"
python3 -I - "$T/lib/h.lordvm/machine.json" <<'EOF' && ok "a hostile machine.json comes in with a new machine's safe defaults" || bad "hostile machine.json kept settings"
import json, sys
m = json.load(open(sys.argv[1]))
bad = [k for k, v in {'agent': False, 'transfer': False, 'microphone': False}.items() if m.get(k, False) != v]
bad += [k for k in ('ports', 'shares', 'iso', 'sound_out', 'sound_in', 'capture_fullscreen', 'pointer', 'perf_profile', 'cpu_pin', 'disposable', 'backup', 'group', 'role', 'profile_lock') if k in m]
if m.get('clipboard', 'off') != 'off' or m.get('clipboard_ask') is not True: bad.append('clipboard')
if m.get('graphics', '2d') != '2d' or m.get('network') != 'shared': bad += ['graphics/network']
sys.exit(1 if bad else print(*bad) or 0)
EOF

# ---- the UEFI code kept with the machine (3.6): a "firmware update" is the system's file no longer being the one the
# machine was made with (here machine.json is pointed at another OVMF build, which has another hash)
F=$T/lib/f.lordvm
"$LV" --create "$F" --name F --memory 512 --disk 1 --display none --network isolated >/dev/null || bad "firmware machine"
"$LV" --start "$F" --detach >/dev/null 2>&1; sleep 1; "$LV" --poweroff "$F" >/dev/null 2>&1; sleep 1
cmp -s "$F/firmware-code.fd" /usr/share/OVMF/OVMF_CODE_4M.fd && ok "the code file is kept at the first start" || bad "kept copy"
ALT=/usr/share/OVMF/OVMF_CODE_4M.snakeoil.fd
if [ -f "$ALT" ] && ! cmp -s "$ALT" /usr/share/OVMF/OVMF_CODE_4M.fd; then
    sed -i "s|/usr/share/OVMF/OVMF_CODE_4M.fd|$ALT|" "$F/machine.json"
    "$LV" --start "$F" --detach >/dev/null 2>&1; sleep 2
    grep -q "starts with the copy kept with it" "$F/log/lord-vm.log" && grep -q "fw-code-file.*/dev/fdset/3" "$F/log/lord-vm.log" &&
        "$LV" --status "$F" | grep -q running && ok "after a firmware change it starts with the kept code, by descriptor" || bad "kept firmware not used"
    "$LV" --poweroff "$F" >/dev/null 2>&1; sleep 1
    printf 'x' >> "$F/firmware-code.fd"
    "$LV" --start "$F" --detach >/dev/null 2>&1; sleep 1
    grep -q "no copy of the old one is kept" "$F/log/lord-vm.log" && ok "a kept copy that changed is never used" || bad "a changed kept copy was used"
    "$LV" --poweroff "$F" >/dev/null 2>&1; sleep 1
    "$LV" --set "$F" --firmware-update yes >/dev/null && [ ! -e "$F/firmware-code.fd" ] && grep -q "$(sha256sum "$ALT" | cut -c1-64)" "$F/machine.json" &&
        ok "--firmware-update moves to this computer's code" || bad "firmware update"
else echo "note: no second OVMF build to change to: the firmware change isn't tried"; fi

# ---- an encrypted machine group (19.1, isolation layer 5): two empty stand-in disks through the test build's
# --dev-gateway/--dev-workstation (no archive); the members' overlays are LUKS, the bases (Whonix's public image) aren't
LVT=$PWD/build/lord-vm-testpaths
if [ -x "$LVT" ]; then
    mkdir -p "$T/g"; qemu-img create -q -f qcow2 "$T/g/gw.qcow2" 1G; qemu-img create -q -f qcow2 "$T/g/ws.qcow2" 1G
    GID=$(cat /proc/sys/kernel/random/uuid)
    G=$T/lib/W.lordgroup
    LORDVM_TEST_CATALOG="$(realpath ../assets/machines/systems.json)" "$LVT" --group-create "$G" --catalog-id whonix --dev-gateway "$T/g/gw.qcow2" \
        --dev-workstation "$T/g/ws.qcow2" --encrypt on --id "$GID" --key-fd 5 5< <(kf) >/dev/null 2>&1 || bad "encrypted group"
    n=0; for mm in "$G"/*.lordvm; do grep -q '"encrypted": true' "$mm/machine.json" && qemu-img info "$mm/disks/system.qcow2" | grep -q "encrypted: yes" && n=$((n + 1)); done
    [ $n = 2 ] && ok "both members' disks are LUKS under the group's key" || bad "group members not encrypted"
    for b in "$T"/lib/.shared/whonix-*.qcow2; do qemu-img info "$b" | grep -q "encrypted: yes" && bad "a group's base encrypted"; done
    WS=$(grep -l '"role": "workstation"' "$G"/*.lordvm/machine.json | xargs dirname)
    "$LVT" --group-set "$G" --disposable on >/dev/null
    "$LVT" --start "$WS" --detach --key-fd 5 5< <(kf) >/dev/null 2>&1; sleep 2
    "$LVT" --status "$WS" | grep -q running && grep -q "disposable session: the workstation's changes stay in memory" "$WS/log/lord-vm.log" &&
        ok "the encrypted workstation runs its disposable session" || bad "encrypted workstation"
    "$LVT" --poweroff "$WS" >/dev/null 2>&1; sleep 1
    "$LVT" --group-fresh "$G" >/dev/null 2>&1 && bad "Start Fresh of an encrypted group without its key"
    "$LVT" --group-fresh "$G" --key-fd 5 5< <(kf) >/dev/null 2>&1 && qemu-img info "$WS/disks/system.qcow2" | grep -q "encrypted: yes" &&
        ok "Start Fresh keeps the workstation encrypted" || bad "Start Fresh of an encrypted group"
else echo "note: build/lord-vm-testpaths isn't built: the encrypted group isn't tried"; fi

# ---- raw disks (6.4)
R=$T/lib/r.lordvm
"$LV" --create "$R" --name R --memory 512 --disk 1 --display none --network isolated --firmware bios --disk-format raw >/dev/null && [ -f "$R/disks/system.raw" ] && ok "a raw disk" || bad "raw create"
"$LV" --create "$T/lib/x.lordvm" --name X --disk-format raw --encrypt on --key-fd 5 5< <(kf) >/dev/null 2>&1 && bad "raw + encrypted"
"$LV" --snapshot "$R" take s >/dev/null 2>&1 && bad "raw snapshot" || ok "raw: no snapshots"
"$LV" --compact "$R" >/dev/null 2>&1 && bad "raw compaction" || ok "raw: no compaction"
"$LV" --start "$R" --detach >/dev/null && sleep 1 && "$LV" --status "$R" | grep -q running && ok "raw machine runs" || bad "raw start"
"$LV" --poweroff "$R" >/dev/null 2>&1
[ $FAILS = 0 ] && echo "vm-storage: all passed" || echo "vm-storage: $FAILS failed"
exit $((FAILS > 0))
