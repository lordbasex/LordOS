#!/bin/sh
# machinescli_headless.sh — the machines command's session channel (server/src/ui/js_vmcli.c, apps/machines/cli.js,
# server/src/cli/machines.c) against a headless lord-server on the CPU path, in a private runtime folder and home with a
# throwaway machine (isolated network, 256 MB, no system: a BIOS that finds nothing to boot keeps running). Checks: the
# socket is 0600; open refuses a machine that's off (3); start --no-window through the session (0, running); open then
# opens its window (0); a start of a running one says so (0); poweroff (0); start in its window (0, running); requests
# the channel refuses (a verb, a path outside the library: 2 and 4); no JS error in the session. Run from server/
# (tests/run_suites.sh does). Exit 77 when something it needs is missing (KVM, QEMU, the theme), 1 on a failure.
# Not tested here: a peer of another uid being refused (needs root).
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/machines ] && [ -x build/lord-vm ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/machines, build/lord-vm and $THEME"; exit 77; }
[ -r /dev/kvm ] && [ -w /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null 2>&1 || [ "$(uname -m)" = aarch64 ] || { echo "needs KVM and QEMU"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "needs python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lmc.XXXXXX") || exit 1
P=''
M="$D/home/Machines/CLI Test.lordvm"
cleanup() {
    env HOME="$D/home" XDG_RUNTIME_DIR="$D" ./build/lord-vm --poweroff "$M" >/dev/null 2>&1
    kill $P 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT
mkdir -p "$D/home/Machines"
chmod 700 "$D"
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D HOME=$D/home LANG=C.UTF-8"
$E ./build/lord-vm --create "$M" --name "CLI Test" --memory 256 --disk 1 --vcpus 1 --network isolated --display none --firmware bios >/dev/null || { echo "lord-vm --create failed"; exit 77; }
$E ./build/lord-vm --set "$M" --transfer off >/dev/null || exit 1
$E LORD_GPU=0 ./build/lord-server --headless 1024x768 --theme "$THEME" --timeout 120 >"$D/server.log" 2>&1 &
P=$!
R=0
i=0
while [ ! -S "$D/lordos-machines.sock" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$D/lordos-machines.sock" ] && [ "$(stat -c %a "$D/lordos-machines.sock")" = 600 ] && echo "ok: the socket is there, 0600" || { echo "FAIL: no 0600 socket"; R=1; }
sleep 1.5                                       # the Machines app's poller starts a moment after the desktop
expect() {                                      # expect <exit status> <what> <command...>
    want=$1; what=$2; shift 2
    out=$($E "$@" 2>&1); got=$?
    if [ "$got" = "$want" ]; then echo "ok: $what ($got): $out"; else echo "FAIL: $what: want $want, got $got: $out"; R=1; fi
}
expect 3 "open while off" ./build/machines open "cli test"
expect 0 "start --no-window through the session" ./build/machines start cli --no-window
expect 0 "it runs" sh -c "./build/machines status cli --json | grep -q '\"state\":\"running\"'"
expect 0 "open its window" ./build/machines open cli
expect 0 "start again: already running" ./build/machines start cli
expect 0 "poweroff" ./build/machines poweroff cli
expect 0 "start in its window" ./build/machines start cli
expect 0 "poweroff" ./build/machines poweroff cli
# the channel itself: one line, a closed verb, a machine of this library only
raw() { python3 -I -c '
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(sys.argv[1]); s.sendall(sys.argv[2].encode() + b"\n")
d = b""
while not d.endswith(b"\n"):
    k = s.recv(2048)
    if not k: break
    d += k
print(d.decode(errors="replace").strip())' "$D/lordos-machines.sock" "$1"; }
a=$(raw "run $M"); [ "${a%% *}" = 2 ] && echo "ok: a verb that isn't one: $a" || { echo "FAIL: a verb that isn't one: $a"; R=1; }
a=$(raw "start $M --no-window"); [ "${a%% *}" = 2 ] && echo "ok: words after the path: $a" || { echo "FAIL: words after the path: $a"; R=1; }
mkdir -p "$D/other.lordvm"
a=$(raw "start $D/other.lordvm"); [ "${a%% *}" = 4 ] && echo "ok: a folder outside the library: $a" || { echo "FAIL: a folder outside the library: $a"; R=1; }
a=$(raw "release $M"); [ "${a%% *}" = 0 ] && echo "ok: release: $a" || { echo "FAIL: release: $a"; R=1; }
if grep -qi "TypeError\|ReferenceError\|exception" "$D/server.log"; then echo "FAIL: a JS error in the session"; grep -i -A3 "TypeError\|ReferenceError\|exception" "$D/server.log" | head -20; R=1; else echo "ok: no JS error in the session"; fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -30 "$D/server.log"; R=1; }
[ $R = 0 ] || { echo "-- server log"; grep -i "vmcli\|vm\b\|error\|exception" "$D/server.log" | tail -20; }
exit $R
