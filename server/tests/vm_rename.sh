#!/bin/sh
# vm_rename.sh — renaming a machine (docs/VIRTUALIZATION.md 6.26) with lord-vm and the machines command, on throwaway
# machines in a private home (never ~/Machines): a machine with a snapshot, a transfer folder with a file and its owner
# record, and an apps-mode app.json is renamed while off (its folder, its transfer folder and every file follow; it
# still checks, lists its snapshot and is found by its new name and by its id); names that are refused (another
# machine's in another case, a folder of that name, a transfer folder of that name, a leading dot); with KVM and QEMU it
# is started from its new folder, renamed while running (only the name: the folders stay), stopped, renamed again with the
# same name (now the folders follow) and started once more. Run from server/ (tests/run_suites.sh does). Exit 77 when
# something it needs is missing (qemu-img, the binaries), 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-vm ] && [ -x build/machines ] || { echo "needs build/lord-vm and build/machines"; exit 77; }
command -v qemu-img >/dev/null 2>&1 || { echo "needs qemu-img"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "needs python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lrn.XXXXXX") || exit 1
L="$D/home/Machines"
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D HOME=$D/home LANG=C.UTF-8"
cleanup() {
    for m in "$L"/*.lordvm; do [ -d "$m" ] && $E ./build/lord-vm --poweroff "$m" >/dev/null 2>&1; done
    rm -rf "$D"
}
trap cleanup EXIT
mkdir -p "$L"
chmod 700 "$D"
R=0
ok() { echo "ok: $1"; }
no() { echo "FAIL: $1"; R=1; }
expect() {                                      # expect <exit status> <what> <command...>
    want=$1; what=$2; shift 2
    out=$($E "$@" 2>&1); got=$?
    if [ "$got" = "$want" ]; then echo "ok: $what ($got): $out"; else echo "FAIL: $what: want $want, got $got: $out"; R=1; fi
}
json() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d[sys.argv[2]])' "$1" "$2"; }
OLD="$L/Win Test (prueba).lordvm"
$E ./build/lord-vm --create "$OLD" --name "Win Test (prueba)" --memory 256 --disk 1 --vcpus 1 --network isolated --display none --firmware bios >/dev/null || { echo "lord-vm --create failed"; exit 77; }
$E ./build/lord-vm --create "$L/Other.lordvm" --name "Other" --memory 256 --disk 1 --vcpus 1 --network isolated --display none --firmware bios >/dev/null || exit 1
ID=$(json "$OLD/machine.json" id)
# what the machine has: a snapshot, its transfer folder (owned by its id) with a file, the app's apps mode
expect 0 "a snapshot while off" ./build/lord-vm --snapshot "$OLD" take "Before" snap1
mkdir -p "$L/Transfer/Win Test (prueba)" && echo moved >"$L/Transfer/Win Test (prueba)/note.txt" && printf '%s\n' "$ID" >"$L/Transfer/.Win Test (prueba).machine"
echo '{"appsMode":true,"osName":"Test OS"}' >"$OLD/app.json"
# refused: nothing changes
expect 1 "another machine's name, another case" ./build/machines rename "win test" "OTHER"
mkdir "$L/taken box.lordvm"
expect 1 "a folder of that name" ./build/machines rename "win test" "Taken Box"
mkdir "$L/Transfer/busy"
expect 1 "a transfer folder of that name" ./build/machines rename "win test" "Busy"
expect 2 "a leading dot" ./build/machines rename "win test" ".hidden"
expect 5 "lord-vm says why, exit 5" ./build/lord-vm --rename "$OLD" "Other"
[ -d "$OLD" ] && [ "$(json "$OLD/machine.json" name)" = "Win Test (prueba)" ] && ok "refusals left it as it was" || no "a refusal changed something"
# off: the name, the folder, the transfer folder
expect 0 "rename while off" ./build/machines rename "win test" "Win Test"
NEW="$L/Win Test.lordvm"
[ -d "$NEW" ] && [ ! -e "$OLD" ] && ok "the folder is Win Test.lordvm" || no "the folder didn't move"
[ "$(json "$NEW/machine.json" name)" = "Win Test" ] && [ "$(json "$NEW/machine.json" id)" = "$ID" ] && ok "machine.json: the new name, the same id" || no "machine.json"
[ -f "$L/Transfer/Win Test/note.txt" ] && [ ! -e "$L/Transfer/Win Test (prueba)" ] && ok "the transfer folder followed with its file" || no "the transfer folder"
[ "$(cat "$L/Transfer/.Win Test.machine")" = "$ID" ] && [ ! -e "$L/Transfer/.Win Test (prueba).machine" ] && ok "its owner record followed" || no "the owner record"
[ "$(json "$NEW/app.json" appsMode)" = "True" ] && ok "app.json (apps mode) came along" || no "app.json"
expect 0 "it still checks" ./build/lord-vm --check "$NEW"
expect 0 "its snapshot is there" sh -c "./build/lord-vm --snapshot '$NEW' list | grep -q '\"tag\":\"snap1\"'"
expect 0 "the command finds it by its id" sh -c "./build/machines status $ID --json | grep -q 'Win Test.lordvm'"
expect 0 "the list shows it" sh -c "./build/machines list --names | grep -qx 'Win Test'"
expect 0 "the same name again: nothing to do" ./build/machines rename "Win Test" "Win Test"
# the app's way: lord-vm --set with --name
expect 0 "set --name renames the folders too" ./build/lord-vm --set "$NEW" --name "Win Set"
[ -d "$L/Win Set.lordvm" ] && [ -f "$L/Transfer/Win Set/note.txt" ] && ok "set --name: folder and transfer folder" || no "set --name"
expect 0 "and back with the id" ./build/machines rename "$ID" "Win Test"
# running (KVM and QEMU): the name only; the folders follow at the next rename with it off
if [ -r /dev/kvm ] && [ -w /dev/kvm ] && { command -v qemu-system-x86_64 >/dev/null 2>&1 || [ "$(uname -m)" = aarch64 ]; }; then
    [ -x /usr/libexec/virtiofsd ] || $E ./build/lord-vm --set "$NEW" --transfer off >/dev/null
    expect 0 "it starts from its new folder" ./build/lord-vm --start "$NEW" --detach --keep-running
    expect 0 "it runs" sh -c "./build/machines status 'Win Test' --json | grep -q '\"state\":\"running\"'"
    expect 0 "rename while running" ./build/machines rename "Win Test" "Win Live"
    [ -d "$NEW" ] && [ "$(json "$NEW/machine.json" name)" = "Win Live" ] && ok "running: the name changed, the folder stayed" || no "running rename"
    expect 0 "it still runs" sh -c "./build/machines status 'Win Live' --json | grep -q '\"state\":\"running\"'"
    expect 0 "poweroff" ./build/machines poweroff "Win Live"
    expect 0 "the same name with it off: the folders follow" ./build/machines rename "Win Live" "Win Live"
    [ -d "$L/Win Live.lordvm" ] && [ ! -e "$NEW" ] && ok "the folder is Win Live.lordvm" || no "the folder after the stop"
    [ ! -x /usr/libexec/virtiofsd ] || { [ -f "$L/Transfer/Win Live/note.txt" ] && ok "the transfer folder too" || no "the transfer folder after the stop"; }
    expect 0 "it starts again from there" ./build/lord-vm --start "$L/Win Live.lordvm" --detach --keep-running
    expect 0 "poweroff" ./build/machines poweroff "Win Live"
else
    echo "skipped: the running part needs KVM and QEMU"
fi
exit $R
