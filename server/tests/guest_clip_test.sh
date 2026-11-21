#!/bin/sh
# guest_clip_test.sh — which clipboard tool lordos-guest (tools/guest/lordos-guest.c) uses, and its hello without one.
# Stub xclip / wl-paste / wl-copy scripts in a private PATH, a Python listener as the agent's port (a Unix socket:
# lordos-guest --port SOCK --once says hello, looks at the clipboard once and ends). Run from server/
# (tests/run_suites.sh does). Exit 77 when build/lordos-guest or python3 is missing, 1 on a failure.
# Not tested here: a real desktop's clipboard (GNOME's Xwayland bridge), the 30 s look again.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lordos-guest ] && command -v python3 >/dev/null 2>&1 || { echo "needs build/lordos-guest and python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lgc.XXXXXX") || exit 1
L=''
trap 'kill $L 2>/dev/null; rm -rf "$D"' EXIT
G=$(pwd)/build/lordos-guest
mkdir -p "$D/none" "$D/x" "$D/wl" "$D/both" "$D/py"
# the listener: one connection, every frame as a line (HELLO <text> / CLIP <text> / KIND <n>), until the helper ends
cat >"$D/py/listen.py" <<'EOF'
import os, socket, struct, sys
path, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
s.settimeout(10)
c, _ = s.accept()
c.settimeout(10)
buf = b''
while True:
    d = c.recv(65536)
    if not d:
        break
    buf += d
with open(out, 'w') as f:
    while len(buf) >= 8:
        n, kind, _ = struct.unpack('<IHH', buf[:8])
        if len(buf) < 8 + n:
            break
        p = buf[8:8 + n].decode('utf-8', 'replace')
        buf = buf[8 + n:]
        f.write({1: 'HELLO ', 2: 'CLIP '}.get(kind, 'KIND %d ' % kind) + p + '\n')
EOF
# stubs: xclip says "UTF8_STRING" for TARGETS and "from xclip" for the text; wl-paste the same with "from wl"
cat >"$D/x/xclip" <<'EOF'
#!/bin/sh
case "$*" in *TARGETS*) printf 'TARGETS\nUTF8_STRING\n' ;; *-o*) printf 'from xclip' ;; esac
EOF
cat >"$D/wl/wl-paste" <<'EOF'
#!/bin/sh
case "$*" in *-l*) printf 'text/plain;charset=utf-8\n' ;; *) printf 'from wl' ;; esac
EOF
printf '#!/bin/sh\ncat >/dev/null\n' >"$D/wl/wl-copy"
cp "$D/x/xclip" "$D/wl/wl-paste" "$D/wl/wl-copy" "$D/both/"
chmod 755 "$D/x/xclip" "$D/wl/wl-paste" "$D/wl/wl-copy" "$D/both/"*
R=0
# case NAME PATH-DIR ENV... : runs the helper once against a fresh listener; $D/out has the frames, $D/err its stderr
case_run() {
    name=$1 dir=$2; shift 2
    rm -f "$D/sock" "$D/out"
    python3 -I "$D/py/listen.py" "$D/sock" "$D/out" &
    L=$!
    i=0; while [ ! -S "$D/sock" ] && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
    env -i PATH="$D/$dir" "$@" "$G" --port "$D/sock" --once >/dev/null 2>"$D/err"
    wait $L; L=''
    echo "== $name"; sed 's/^/   /' "$D/out" "$D/err"
}
check() { if grep -q -- "$2" "$3"; then echo "ok: $1"; else echo "FAIL: $1 (no \"$2\" in $3)"; R=1; fi; }
nocheck() { if grep -q -- "$2" "$3"; then echo "FAIL: $1 (\"$2\" in $3)"; R=1; else echo "ok: $1"; fi; }

case_run "no tool" none DISPLAY=:9 WAYLAND_DISPLAY=wayland-9
check "no tool: the hello says noclip" '^HELLO lordos-guest [0-9]* noclip$' "$D/out"
nocheck "no tool: no clipboard capability" 'clipboard' "$D/out"
nocheck "no tool: nothing sent" '^CLIP' "$D/out"
check "no tool: the journal says what to install" 'no clipboard tool: install xclip or wl-clipboard' "$D/err"

case_run "xclip only (X11)" x DISPLAY=:9
check "xclip: the hello says clipboard" '^HELLO lordos-guest [0-9]* clipboard clipx$' "$D/out"
check "xclip: the text is sent" '^CLIP from xclip$' "$D/out"
check "xclip: the journal names it" 'clipboard through xclip (X11)' "$D/err"

case_run "wl-clipboard only (Wayland)" wl WAYLAND_DISPLAY=wayland-9
check "wl: the text is sent" '^CLIP from wl$' "$D/out"

case_run "both, GNOME with Xwayland" both DISPLAY=:9 WAYLAND_DISPLAY=wayland-9 XDG_CURRENT_DESKTOP=ubuntu:GNOME
check "GNOME: xclip, not wl-paste" '^CLIP from xclip$' "$D/out"
check "GNOME: the journal says Xwayland" 'clipboard through xclip (Xwayland, GNOME)' "$D/err"
nocheck "GNOME: no wl-clipboard fallback" 'then wl-clipboard' "$D/err"

case_run "both, another Wayland desktop" both DISPLAY=:9 WAYLAND_DISPLAY=wayland-9 XDG_CURRENT_DESKTOP=KDE
check "KDE: wl-clipboard first" '^CLIP from wl$' "$D/out"
check "KDE: then xclip" 'clipboard through wl-clipboard (Wayland), then xclip' "$D/err"
exit $R
