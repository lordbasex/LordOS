#!/bin/bash
# vm_picture_2d.sh — a 2D machine's picture in a headless lord-server, end to end (docs/MACHINES-GPU.md, Part 1 steps 4
# and 5): a real Debian 13 guest (nocloud image; root's password given as a systemd credential) on QEMU/KVM, its window in
# the session. Over its serial console a loop writes two counters to tty1, one in the top-left corner and one in the
# bottom-right, 20 times a second. Checks: frames arrive and are acknowledged (at least 50 in 10 s), every frame comes as
# one or more rectangles (VD_FRAME f counting down), and what the headless display shows (test hooks' "frame output")
# has the guest's console text inside the window; reports lord-server's CPU time over those 10 s and its paint average.
# A headless lord-server composes on the CPU (gpu.c needs a KMS card), so this runs the CPU path; picTex (frames shown by
# the 2D picture's texture alone) is reported and stays 0 here. Linux's DRM console merges its damage into one rectangle
# itself (drm_fb_helper), so a Linux guest sends one rectangle a frame; several come from guests that update apart
# (vmdisplay-test checks them against a mock QEMU).
# Needs build/lord-server-test, build/lord-vm, build/machines, the theme, KVM, QEMU, unshare, python3 and IMAGE (the
# nocloud qcow2, used read-only; not in the repository). Exit 77 without them.
#   tests/vm_picture_2d.sh IMAGE [OUT_DIR]          (KEEP=1 keeps the scratch folder)
# lord-server runs as this account in its own user and mount namespaces with a private home, an empty folder over the
# real home and a tmpfs over the real runtime folder (as tests/vm_group_link.sh): it never sees the person's session.
cd "$(dirname "$0")/.." || exit 1
IMG=${1:-}
OUT=${2:-}
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server-test ] && [ -x build/machines ] && [ -x build/lord-vm ] && [ -f "$THEME" ] || { echo "skipped: needs build/lord-server-test, machines, lord-vm and $THEME"; exit 77; }
[ -r /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v unshare >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 || { echo "skipped: needs KVM, QEMU, unshare and python3"; exit 77; }
[ -f "$IMG" ] || { echo "skipped: needs IMAGE (Debian 13's nocloud qcow2)"; exit 77; }
IMG=$(realpath "$IMG")
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lpic2d.XXXXXX") || exit 1
chmod 700 "$D"
[ -n "$OUT" ] && mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
NS="unshare -Urm sh -c 'mount --bind $D/fake $REALHOME && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"'"
ns() { eval "$NS" '"$@"'; }
P=
M=
E=
stop_all() {
    [ -n "$M" ] && ns $E ./build/lord-vm --poweroff "$M" >/dev/null 2>&1
    # the background job is a shell around the namespaces: lord-server itself is found by its theme's path
    local q
    q=$(pgrep -f -- "--theme $D/theme/theme.bundle.js")
    [ -n "$P" ] && kill "$P" 2>/dev/null
    [ -n "$q" ] && { kill $q 2>/dev/null; sleep 1; kill -9 $q 2>/dev/null; }
    P=
}
trap 'stop_all; [ -n "$KEEP" ] || rm -rf "$D"' EXIT

# the theme with a probe before it: every 2 s, each machine the app polls, as one line in shell.log
mkdir -p "$D/theme"
cat >"$D/theme/pre.js" <<'EOF'
(function () {
  const v = globalThis.lord && lord.vm;
  if (!v) return;
  const real = v.poll, seen = new Set();
  v.poll = (sid) => { seen.add(sid); return real(sid); };
  setInterval(() => {
    for (const sid of seen) {
      const q = real(sid);
      if (q && q.state) console.log('PICTEST ' + JSON.stringify({ state: q.state, display: q.display, w: q.w, h: q.h, frames: q.frames, rects: q.picRects, tex: q.picTex }));
    }
  }, 2000);
})();
EOF

run() {     # run <name> <env...>
    local name=$1; shift
    rm -rf "$D/home" "$D/run" "$D/fake"
    mkdir -p "$D/home/Machines" "$D/home/.config/lordos" "$D/run" "$D/fake"
    chmod 700 "$D/run" "$D/home"
    E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D/run HOME=$D/home LANG=C.UTF-8"
    M="$D/home/Machines/Pic.lordvm"
    printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n' >"$D/home/.config/lordos/settings.json"
    ns $E ./build/lord-vm --create "$M" --name Pic --memory 1024 --vcpus 2 --network isolated --firmware bios --import "$IMG" >"$D/create.log" 2>&1 || { bad "$name: lord-vm --create: $(tail -3 "$D/create.log")"; return; }
    ns $E ./build/lord-vm --set "$M" --transfer off --agent off >/dev/null 2>&1
    cat "$D/theme/pre.js" "$THEME" >"$D/theme/theme.bundle.js"
    ns $E "$@" LORD_HEADLESS_DRAW=1 ./build/lord-server-test --headless 1600x900 --theme "$D/theme/theme.bundle.js" --timeout 600 --user "$(id -un)" --test-hooks >"$D/server.log" 2>&1 &
    P=$!
    for i in $(seq 1 100); do [ -S "$D/run/lordos-machines.sock" ] && break; sleep 0.1; done
    LS=$(pgrep -o -f -- "--theme $D/theme/theme.bundle.js")
    sleep 2
    # root's password as a systemd credential (the nocloud image's firstboot asks for one otherwise), so the machine is
    # started by lord-vm here and its window opened by the session
    printf 'firstboot.locale=C.UTF-8\nfirstboot.keymap=us\nfirstboot.timezone=UTC\npasswd.plaintext-password.root=lordos-pic\n' >"$D/creds"; chmod 600 "$D/creds"
    ns $E ./build/lord-vm --start "$M" --detach --credentials-file "$D/creds" >"$D/start.log" 2>&1 || { bad "$name: lord-vm --start: $(tail -3 "$D/start.log")"; stop_all; return; }
    sleep 1
    out=$(ns $E ./build/machines open Pic 2>&1) || { bad "$name: machines open: $out"; stop_all; return; }
    # the serial console: root, then the loop on tty1
    for i in $(seq 1 90); do grep -aq "login:" "$M/log/serial.log" 2>/dev/null && break; sleep 1; done
    grep -aq "login:" "$M/log/serial.log" 2>/dev/null || { bad "$name: no login prompt on the serial console"; stop_all; return; }
    ID=$(sed -n 's/.*"id": "\(.*\)".*/\1/p' "$M/machine.json" | head -1)
    ns python3 -I -c '
import array, select, socket, struct, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); s.connect(sys.argv[1])
s.send(struct.pack("<HHIiiii200s", 13, 1, 77, 0, 0, 0, 0, b""))
fd = -1
while fd < 0:
    msg, anc, fl, ad = s.recvmsg(256, socket.CMSG_SPACE(4))
    for l, t, d in anc:
        if l == socket.SOL_SOCKET and t == socket.SCM_RIGHTS: fd = array.array("i", d[:4])[0]
    if struct.unpack("<H", msg[:2])[0] == 64: sys.exit(2)
c = socket.socket(fileno=fd); c.setblocking(False)
def until(mark, t):
    b = b""; end = time.time() + t
    while time.time() < end:
        if select.select([c], [], [], 0.5)[0]:
            try: k = c.recv(65536)
            except BlockingIOError: continue
            if not k: break
            b += k
            if mark in b: return b
    return b
c.send(b"\r"); until(b"login:", 10)
c.send(b"root\r"); until(b"assword:", 15)
c.send(b"lordos-pic\r"); until(b"#", 15)
import base64
script = b"""setterm --term linux --blank 0 --cursor off </dev/tty1 >/dev/tty1 2>&1
clear >/dev/tty1
i=0
while [ $i -lt 1200 ]; do
  i=$((i+1))
  printf "\\033[2;3HLEFT %06d\\033[30;70HRIGHT %06d" $i $i >/dev/tty1
  sleep 0.05
done
"""
c.send(b"echo " + base64.b64encode(script) + b" | base64 -d >/tmp/l.sh; nohup sh /tmp/l.sh >/dev/null 2>&1 &\r")
until(b"#", 5)
' "$D/run/lordos-vm/$ID.sock" || { bad "$name: the serial console"; stop_all; return; }
    sleep 4
    local l0 t0 l1 t1 cpu
    l0=$(grep -a "PICTEST" "$D/home/.cache/lordos/shell.log" | tail -1 | sed 's/.*PICTEST //')
    t0=$(awk '{print $14 + $15}' /proc/$LS/stat 2>/dev/null)
    sleep 10
    l1=$(grep -a "PICTEST" "$D/home/.cache/lordos/shell.log" | tail -1 | sed 's/.*PICTEST //')
    t1=$(awk '{print $14 + $15}' /proc/$LS/stat 2>/dev/null)
    cpu=$(( (t1 - t0) * 1000 / $(getconf CLK_TCK) ))
    python3 -I - "$name" "$l0" "$l1" "$cpu" <<'EOF' || R=1
import json, sys
name, a, b, cpu = sys.argv[1], json.loads(sys.argv[2] or "{}"), json.loads(sys.argv[3] or "{}"), int(sys.argv[4])
f = b.get("frames", 0) - a.get("frames", 0); r = b.get("rects", 0) - a.get("rects", 0); t = b.get("tex", 0) - a.get("tex", 0)
print(f"{name}: picture {b.get('w')}x{b.get('h')}, in 10 s: {f} frames, {r} rectangles, {t} shown by the texture alone; lord-server CPU {cpu} ms ({cpu / 100:.1f} % of a core)")
bad = f < 50 or r < f or t != 0
print(("ok: " if not bad else "FAIL: ") + name + ": frames read and acknowledged, each as its rectangles")
sys.exit(1 if bad else 0)
EOF
    # what the display shows: the guest's console text inside the window
    ns python3 -I -c '
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); s.connect(sys.argv[1]); s.send(b"frame output"); print(s.recv(256).decode())
' "$D/run/lordos-test-hooks.sock" >"$D/hook.txt" 2>&1
    if [ -f "$D/run/lordos-test-frame-output.ppm" ]; then
        [ -n "$OUT" ] && cp "$D/run/lordos-test-frame-output.ppm" "$OUT/$name.ppm"
        python3 -I - "$D/run/lordos-test-frame-output.ppm" "$name" <<'EOF' || R=1
import sys
d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
# the guest's console: light grey text on black inside the window; count light pixels in the middle 80 % of the screen
light = 0
for y in range(h // 10, h * 9 // 10, 2):
    row = px[y * w * 3:(y + 1) * w * 3]
    for x in range(w // 10, w * 9 // 10, 2):
        r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
        if r > 150 and g > 150 and b > 150 and abs(r - g) < 20 and abs(g - b) < 20: light += 1
print(("ok: " if light > 200 else "FAIL: ") + f"{sys.argv[2]}: the display shows the guest's console ({light} light pixels sampled)")
sys.exit(0 if light > 200 else 1)
EOF
    else bad "$name: no picture from the test hooks: $(cat "$D/hook.txt")"; fi
    grep -a "UI renders" "$D/home/.cache/lordos/compositor.log" 2>/dev/null | tail -1 | sed 's/.*INFO  /  /'
    stop_all
}

run cpu env LORD_GPU=0
[ $R = 0 ] && echo "vm_picture_2d: all passed" || echo "vm_picture_2d: FAILED"
exit $R
