#!/bin/bash
# vm_group_manage.sh — a machine group's Delete, Rename and Repair, and Start Fresh making a base again from the verified
# archive (docs/VIRTUALIZATION.md 19.7–19.9), with lord-vm and the machines command on throwaway groups in a private home
# (never ~/Machines). The archive is a fixture made here: a GNU tar, compressed with xz, of two small qcow2 disks named as
# Whonix names its members, kept as the image store keeps it (images/<sha512>.tar.xz, LORDVM_TEST_IMAGES). Checks:
#   - --group-create from the store's name with --release, and from the download's own name; a second group of the same
#     archive uses the bases the first one recorded;
#   - rename: group.json, the members' names and the folder (renameat2); a name another group has, or a running member,
#     is refused; `machines rename` on a group;
#   - repair: a member folder removed, a machine.json of another group, a base removed (refused without the archive,
#     exit 4; made again from it), a base changed (made again, its members start fresh);
#   - Start Fresh with a changed base makes it again from the archive;
#   - delete: refused while a member runs (its lock held); a link planted in the group to a folder outside, and a member
#     folder that is a link, are removed as links (what they point at stays); a group folder that is a link is refused; a
#     base another group uses stays, the last group's bases go; a group whose group.json can't be read is deleted too.
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-vm-testpaths ] || make -s build/lord-vm-testpaths >/dev/null 2>&1
[ -x build/lord-vm-testpaths ] && [ -x build/lord-vmunpack ] && [ -x build/machines ] || { echo "skipped: needs build/lord-vm-testpaths, build/lord-vmunpack and build/machines"; exit 77; }
for t in qemu-img qemu-io xz tar python3 sha512sum; do command -v $t >/dev/null 2>&1 || { echo "skipped: needs $t"; exit 77; }; done
[ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo "skipped: needs /dev/kvm (the members are KVM machines)"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lgman.XXXXXX") || exit 1
chmod 700 "$D"
L="$D/home/Machines"
mkdir -p "$L" "$D/images" "$D/empty" "$D/outside" "$D/fx"
cleanup() { [ -n "${FP:-}" ] && kill $FP 2>/dev/null; chmod -R u+w "$D" 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT
CAT=$(realpath ../assets/machines/systems.json)
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D HOME=$D/home LANG=C.UTF-8 LORDVM_TEST_CATALOG=$CAT LORDVM_TEST_IMAGES=$D/images"
VM="./build/lord-vm-testpaths"
R=0
ok() { echo "ok: $1"; }
no() { echo "FAIL: $1"; R=1; }
expect() {                                      # expect <exit status> <what> <command...>
    want=$1; what=$2; shift 2
    out=$($E "$@" 2>&1); got=$?
    if [ "$got" = "$want" ]; then echo "ok: $what ($got)"; else echo "FAIL: $what: want $want, got $got: $out"; R=1; fi
}
# a member "running": its lock held by another process (as its lord-vm holds it), until kill $FP
hold() { python3 -I -c 'import fcntl,sys,time; f=open(sys.argv[1],"a"); fcntl.flock(f,fcntl.LOCK_EX); time.sleep(60)' "$1" & FP=$!; sleep 0.5; }
jget() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): d=d[k]
print(d)' "$1" "$2"; }

# the archive: two small disks with some data, named as Whonix names them, in a GNU tar compressed with xz
REL=99.0.0.1
for r in Gateway Workstation; do
    f="$D/fx/Whonix-$r-LXQt-$REL.Intel_AMD64.qcow2"
    qemu-img create -q -f qcow2 "$f" 1G && qemu-io -c "write -P 0x5$([ $r = Gateway ] && echo 1 || echo 2) 0 2M" "$f" >/dev/null || { echo "can't make the fixture"; exit 77; }
done
(cd "$D/fx" && tar --format=gnu -cf - Whonix-Gateway-LXQt-$REL.Intel_AMD64.qcow2 Whonix-Workstation-LXQt-$REL.Intel_AMD64.qcow2) | xz -0 -T1 >"$D/archive.xz"
SHA=$(sha512sum "$D/archive.xz" | cut -d' ' -f1)
STORE="$D/images/$SHA.tar.xz"
cp "$D/archive.xz" "$STORE"
cp "$D/archive.xz" "$D/Whonix-LXQt-$REL.Intel_AMD64.qcow2.libvirt.xz"

G="$L/Whonix.lordgroup"
expect 0 "create from the image store's name with --release" $VM --group-create "$G" --catalog-id whonix --archive "$STORE" --sha512 "$SHA" --release $REL --name Whonix
expect 2 "the store's name without --release is refused" $VM --group-create "$L/Other.lordgroup" --catalog-id whonix --archive "$STORE" --sha512 "$SHA"
expect 0 "create from the download's own name" $VM --group-create "$L/Tor2.lordgroup" --catalog-id whonix --archive "$D/Whonix-LXQt-$REL.Intel_AMD64.qcow2.libvirt.xz" --sha512 "$SHA" --name Tor2
BG=$(jget "$G/group.json" image.bases.gateway); BW=$(jget "$G/group.json" image.bases.workstation)
[ -f "$BG" ] && [ -f "$BW" ] && ok "the bases are in .shared" || no "bases: $BG $BW"
[ "$(jget "$L/Tor2.lordgroup/group.json" image.bases.workstation)" = "$BW" ] && ok "the second group uses the same bases" || no "the second group's bases"
SW=$(jget "$G/group.json" image.base_sha256.workstation)

# rename
expect 5 "rename to another group's name (any case)" $VM --group-rename "$G" "tor2"
hold "$G/Gateway.lordvm/lock"
sleep 0.3
expect 3 "rename with a member running (its lock held)" $VM --group-rename "$G" "Tor Work"
kill $FP 2>/dev/null; wait $FP 2>/dev/null
expect 0 "rename" $VM --group-rename "$G" "Tor Work"
G="$L/Tor Work.lordgroup"
[ -d "$G" ] && [ ! -e "$L/Whonix.lordgroup" ] && ok "the folder followed" || no "the folder: $(ls "$L")"
[ "$(jget "$G/group.json" name)" = "Tor Work" ] && [ "$(jget "$G/Gateway.lordvm/machine.json" name)" = "Tor Work Gateway" ] && ok "group.json and the members' names" || no "names after rename"
out=$($E ./build/machines rename "Tor Work" "Whonix" 2>&1) && [ -d "$L/Whonix.lordgroup" ] && ok "machines rename on a group: $out" || no "machines rename: $out"
G="$L/Whonix.lordgroup"
# a long name of two-byte characters: the members' names are cut on a character's boundary, never in the middle of one
LONG=ñññññññññññññññññññññññññ
expect 0 "rename to a long name" $VM --group-rename "$G" "$LONG"
[ "$(jget "$L/Machine.lordgroup/Gateway.lordvm/machine.json" name)" = "ññññññññññññññññññññ Gateway" ] && ok "the member's name is cut on a character" || no "the long name: $(ls "$L")"
expect 0 "rename back" $VM --group-rename "$L/Machine.lordgroup" Whonix

# repair: a member folder gone, a machine.json of another group
rm -rf "$G/Workstation.lordvm"
expect 0 "repair a missing member" $VM --group-repair "$G"
[ "$(jget "$G/Workstation.lordvm/machine.json" group)" = "$(jget "$G/group.json" id)" ] && [ -f "$G/Workstation.lordvm/disks/system.qcow2" ] && ok "the workstation was made again from its base" || no "the remade workstation"
python3 -I -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["group"]="00000000-0000-4000-8000-000000000000"; json.dump(d,open(p,"w"))' "$G/Gateway.lordvm/machine.json"
expect 1 "Start Fresh refuses a member that isn't the group's" $VM --group-fresh "$G" --gateway
expect 0 "repair a member of another group" $VM --group-repair "$G"
[ "$(jget "$G/Gateway.lordvm/machine.json" group)" = "$(jget "$G/group.json" id)" ] && ok "the gateway is the group's again" || no "the remade gateway"
ls -a "$G" | grep -q '^\.repair-' && no "a repair folder was left: $(ls -a "$G")" || ok "no repair folder left"

# repair: a base missing; without the archive it says so (exit 4)
chmod u+w "$(dirname "$BW")"; rm -f "$BW"
out=$($E LORDVM_TEST_IMAGES="$D/empty" $VM --group-repair "$G" 2>&1); rc=$?
[ $rc = 4 ] && echo "$out" | grep -q needArchive && ok "a missing base without the archive: exit 4" || no "missing base, no archive: $rc $out"
expect 0 "repair a missing base from the archive" $VM --group-repair "$G"
[ -f "$BW" ] && [ "$(sha256sum "$BW" | cut -d' ' -f1)" = "$SW" ] && ok "the base was made again, the same bytes" || no "the remade base"
[ "$(stat -c %a "$BW")" = 444 ] && ok "the base is read-only" || no "the base's mode: $(stat -c %a "$BW")"

# Start Fresh with a changed base: made again from the archive
chmod u+w "$BW"; printf 'x' >>"$BW"; chmod 444 "$BW"
expect 0 "Start Fresh makes a changed base again" $VM --group-fresh "$G"
[ "$(sha256sum "$BW" | cut -d' ' -f1)" = "$SW" ] && ok "the changed base is the verified one again" || no "the base after fresh"
# repair with a changed base: its members start fresh
chmod u+w "$BG"; printf 'x' >>"$BG"; chmod 444 "$BG"
GID=$(jget "$G/Gateway.lordvm/machine.json" id)
expect 0 "repair a changed base" $VM --group-repair "$G"
[ "$(jget "$G/Gateway.lordvm/machine.json" id)" != "$GID" ] && ok "the gateway over the changed base started fresh" || no "the gateway kept its identity over a changed base"

# a disposable session over no base (a hand-edited machine.json) is refused: QEMU would open the system disk writable
W="$G/Workstation.lordvm"
expect 0 "disposable on" $VM --group-set "$G" --disposable on
cp "$W/machine.json" "$D/ws.json"; mv "$W/disks/system.qcow2" "$D/ws.qcow2"; qemu-img create -q -f qcow2 "$W/disks/system.qcow2" 1G
python3 -I -c 'import json,sys; p=sys.argv[1]; m=json.load(open(p)); m.pop("base",None); open(p,"w").write(json.dumps(m,indent=2)+"\n")' "$W/machine.json"
out=$($E $VM --start "$W" --detach 2>&1); sleep 2
if $E $VM --status "$W" 2>/dev/null | grep -q running; then $E $VM --poweroff "$W" >/dev/null 2>&1; no "a disposable session started without its base"
else grep -q "a disposable session needs the workstation's base" "$W/log/lord-vm.log" 2>/dev/null && ok "a disposable session without its base is refused" ||
    { echo "$out" | grep -q "base" && ok "a disposable session without its base is refused (by the check)" || no "no base: $out"; }; fi
cp "$D/ws.json" "$W/machine.json"; mv -f "$D/ws.qcow2" "$W/disks/system.qcow2"
expect 0 "disposable off" $VM --group-set "$G" --disposable off

# delete: refused while a member runs
hold "$G/Workstation.lordvm/lock"
sleep 0.3
expect 3 "delete with a member running" $VM --group-delete "$G"
kill $FP 2>/dev/null; wait $FP 2>/dev/null
[ -f "$G/group.json" ] && ok "the group is still there" || no "the group went while running"
# links in it are removed as links
echo keep >"$D/outside/canary"
ln -s "$D/outside" "$G/escape"
ln -s "$D/outside" "$G/Evil.lordvm"
ln -s "$D/outside" "$L/Link.lordgroup"
expect 1 "a group folder that is a link is refused" $VM --group-delete "$L/Link.lordgroup"
rm -f "$L/Link.lordgroup"
expect 0 "delete" $VM --group-delete "$G"
[ ! -e "$G" ] && [ -f "$D/outside/canary" ] && ok "the group went; what its links pointed at stayed" || no "after delete: $(ls -a "$L") $(ls "$D/outside")"
[ -f "$BW" ] && [ -f "$BG" ] && ok "the bases another group uses stay" || no "a used base went"
ls -a "$L" | grep -q '^\.group-deleting' && no "a deleting folder was left" || ok "nothing left in the library"
# the last group: its bases go too; its group.json unreadable doesn't stop it
echo '{ not json' >"$L/Tor2.lordgroup/group.json"
expect 0 "delete a group whose group.json can't be read" $VM --group-delete "$L/Tor2.lordgroup"
[ ! -e "$L/Tor2.lordgroup" ] && [ ! -e "$BW" ] && [ ! -e "$BG" ] && ok "the last group's bases went" || no "after the last delete: $(ls -a "$L" "$L/.shared")"

echo "vm_group_manage: $([ $R = 0 ] && echo ok || echo FAILED)"
exit $R
