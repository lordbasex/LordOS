#!/bin/sh
# vm_list_headless.sh — lord.vm.list() and lord-server's events for the shell over a throwaway library in a headless
# lord-server (docs/VIRTUALIZATION.md, "The list's cache" and "The shell and the machines"): N machine folders under a
# temporary HOME (UEFI ones on this computer's OVMF code, two pairs whose ids start with letters that once shared a
# firmware cache slot, and BIOS ones), nothing running. tests/vm_list_scene.js times a cold list and 200 cached ones (each
# under 1 ms); then this script renames one machine and adds another (the next list must show both, after
# __lordVm('library')), runs a stand-in "lord-vm" (a copy of sleep) and writes a run.json naming it (__lordVm('state')
# must say that machine is active), and kills it (another state event, none active: lord-server's pidfd saw it). No
# "stall: … call vm.list" line may reach the log. Exit 77 when something is missing.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] || { echo "skipped: needs build/lord-server"; exit 77; }
FW=/usr/share/OVMF/OVMF_CODE_4M.fd
[ -f "$FW" ] || { echo "skipped: needs $FW"; exit 77; }
N=${VMLIST_N:-12}
D=$(mktemp -d "${TMPDIR:-/tmp}/lvml.XXXXXX") || exit 1
P=
V=
cleanup() { [ -n "$V" ] && kill "$V" 2>/dev/null; [ -n "$P" ] && { kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; }; [ -n "$KEEP" ] || rm -rf "$D"; }
trap cleanup EXIT
chmod 700 "$D"
L="$D/home/Machines"
mkdir -p "$L" "$D/home/.config/lordos"
# one machine.json (schema 1: lord-server reads it forward); $1 folder, $2 id, $3 name, $4 uefi|bios
machine() {
    mkdir -p "$L/$1.lordvm"
    if [ "$4" = uefi ]; then fw="\"firmware\":\"uefi\",\"firmware_code\":\"$FW\",\"firmware_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    else fw='"firmware":"bios",'; fi
    printf '{"schema":1,"id":"%s","name":"%s","arch":"amd64","accel":"kvm","vcpus":2,"memory_mb":2048,"disk_gb":20,%s"machine_type":"pc-q35-10.0","display":"window","network":"shared","mac":"52:54:00:12:ab:%02x","created":"2026-11-04T20:00:00Z","updated":"2026-11-04T20:00:00Z"}\n' \
        "$2" "$3" "$fw" "$5" >"$L/$1.lordvm/machine.json"
}
i=0
while [ $i -lt "$N" ]; do
    # ids 1…, a…, 2…, b…: '1' and 'a' (and '2' and 'b') once landed in the same firmware cache slot (id[0] % 16)
    case $((i % 4)) in 0) c=1;; 1) c=a;; 2) c=2;; *) c=b;; esac
    kind=uefi; [ $i -ge 8 ] && kind=bios
    machine "M$i" "$(printf '%s%07d-3b4d-4e5f-8a6b-7c8d9e0f1a2b' $c $i)" "Machine $i" $kind $i
    i=$((i + 1))
done
{ printf 'globalThis.VMLIST_N = %d;\n' "$N"; cat tests/vm_list_scene.js; } >"$D/theme.js"
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=C.UTF-8 \
    ./build/lord-server --headless 640x480 --theme "$D/theme.js" --timeout 60 >"$D/out.log" 2>&1 &
P=$!
LOG="$D/home/.cache/lordos/shell.log"
seen() { grep -aqs "VMLIST $1" "$LOG" "$D/out.log"; }
for k in $(seq 1 600); do seen READY && break; seen done && break; sleep 0.1; done
seen READY || { echo "FAIL: the scene did not start"; tail -20 "$D/out.log" "$D/home/.cache/lordos/compositor.log" 2>/dev/null; exit 1; }
sed -i 's/"name":"Machine 3"/"name":"Renamed"/' "$L/M3.lordvm/machine.json"
mkdir -p "$L/.new.tmp" && machine New "c0000000-3b4d-4e5f-8a6b-7c8d9e0f1a2b" "New one" bios 99
for k in $(seq 1 300); do seen START && break; seen done && break; sleep 0.1; done
if seen START && ! seen done; then
    mkdir -p "$D/bin" && cp "$(command -v sleep)" "$D/bin/lord-vm"
    "$D/bin/lord-vm" 600 &
    V=$!
    printf '{"state":"running","reason":"none","code":0,"pid":%d,"qemu_pid":0,"passt_pid":0,"since":"2026-11-26T20:00:00Z","detail":"","agent":"off"}\n' "$V" >"$L/M5.lordvm/.run.json.tmp"
    mv "$L/M5.lordvm/.run.json.tmp" "$L/M5.lordvm/run.json"
    for k in $(seq 1 300); do seen KILL && break; seen done && break; sleep 0.1; done
    kill "$V" 2>/dev/null; wait "$V" 2>/dev/null; V=
fi
for k in $(seq 1 300); do seen done && break; sleep 0.1; done
grep -ah 'VMLIST' "$LOG" "$D/out.log" 2>/dev/null | sed 's/.*VMLIST/ /' | sort -u
R=0
seen done || { echo "FAIL: the scene did not finish"; R=1; }
grep -aqs 'VMLIST FAIL' "$LOG" "$D/out.log" && R=1
if grep -aqs 'call vm.list' "$D/home/.cache/lordos/compositor.log" "$D/out.log"; then
    echo "FAIL: stall lines:"; grep -ah 'call vm.list' "$D/home/.cache/lordos/compositor.log" "$D/out.log" | head -5; R=1
fi
[ $R -eq 0 ] && echo "ok"
exit $R
