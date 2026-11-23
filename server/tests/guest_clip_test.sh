#!/bin/sh
# guest_clip_test.sh — which clipboard tool lordos-guest (tools/guest/lordos-guest.c) uses, and its hello without one.
# Stub xclip / wl-paste / wl-copy scripts in a private PATH, a Python listener as the agent's port (a Unix socket:
# lordos-guest --port SOCK --once says hello, looks at the clipboard once and ends). Run from server/
# (tests/run_suites.sh does). Exit 77 when build/lordos-guest or python3 is missing, 1 on a failure.
# Then, with Xvfb and xclip (skipped without them), the host's clipboard on demand: lordos-guest owns the X CLIPBOARD
# for an offer, asks the "host" (a Python script) only when xclip pastes, answers TARGETS itself, sends a large picture
# as INCR, renders nothing for a refused form, asks once per form, and goes back to reading the guest's clipboard when
# another program copies.
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
check "no tool: the hello says noclip" '^HELLO lordos-guest [0-9]* noclip\( stopping\)\{0,1\}\( perf\)\{0,1\}$' "$D/out"
nocheck "no tool: no clipboard capability" 'clipboard' "$D/out"
nocheck "no tool: nothing sent" '^CLIP' "$D/out"
check "no tool: the journal says what to install" 'no clipboard tool: install xclip or wl-clipboard' "$D/err"

case_run "xclip only (X11)" x DISPLAY=:9
check "xclip: the hello says clipboard" '^HELLO lordos-guest [0-9]* clipboard clipx\( stopping\)\{0,1\}\( perf\)\{0,1\}$' "$D/out"
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

# ---- the host's clipboard on demand (X11) ----
if ! command -v Xvfb >/dev/null 2>&1 || ! command -v xclip >/dev/null 2>&1; then echo "skipped: on demand needs Xvfb and xclip"; exit $R; fi
cat >"$D/py/host.py" <<'EOF2'
import os, socket, struct, sys, time, zlib
path, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
s.settimeout(15)
c, _ = s.accept()
c.settimeout(0.2)
log = open(out, 'w', buffering=1)
def send(kind, p):
    c.sendall(struct.pack('<IHH', len(p), kind, 0) + p)
def png(size):
    ihdr = struct.pack('>IIBBBBB', 64, 48, 8, 6, 0, 0, 0)
    head = b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + b'IHDR' + ihdr + struct.pack('>I', zlib.crc32(b'IHDR' + ihdr))
    return head + b'\0' * (size - len(head))
PIC = png(300000)
buf = b''
offered = False
end = time.time() + 30
while time.time() < end:
    try:
        d = c.recv(65536)
        if not d:
            break
        buf += d
    except socket.timeout:
        pass
    while len(buf) >= 8:
        n, kind, _ = struct.unpack('<IHH', buf[:8])
        if len(buf) < 8 + n:
            break
        p, buf = buf[8:8 + n], buf[8 + n:]
        if kind == 1:
            log.write('HELLO ' + p.decode() + '\n')
            send(1, b'lord-vm 2 clipboard clipx clipreq')
            if b' clipreq' in p and not offered:
                offered = True
                send(11, struct.pack('<IBB', 7, 1 | 2 | 4, 0))
        elif kind == 12:
            gen, form = struct.unpack('<IB', p)
            log.write('REQ %d %d\n' % (gen, form))
            if form == 1:
                send(13, struct.pack('<IBB', gen, form, 0) + 'from the host ü'.encode())
            elif form == 3:
                send(13, struct.pack('<IBB', gen, form, 0) + PIC)
            else:
                send(13, struct.pack('<IBB', gen, form, 1))
        elif kind == 2:
            log.write('CLIP ' + p.decode('utf-8', 'replace') + '\n')
        else:
            log.write('KIND %d\n' % kind)
EOF2
N=93
while [ -e /tmp/.X11-unix/X$N ] && [ $N -lt 120 ]; do N=$((N + 1)); done
Xvfb :$N -nolisten tcp >/dev/null 2>&1 &
X=$!
trap 'kill $L $X $H 2>/dev/null; rm -rf "$D"' EXIT
i=0; while [ ! -S /tmp/.X11-unix/X$N ] && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
rm -f "$D/sock" "$D/out"
python3 -I "$D/py/host.py" "$D/sock" "$D/out" &
L=$!
i=0; while [ ! -S "$D/sock" ] && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
env -i PATH=/usr/bin:/bin DISPLAY=:$N HOME="$D" "$G" --port "$D/sock" 2>"$D/err" &
H=$!
xc() { env -i PATH=/usr/bin:/bin DISPLAY=:$N HOME="$D" timeout 10 xclip -o -selection clipboard "$@"; }
i=0; while ! xc -t TARGETS 2>/dev/null | grep -q UTF8_STRING && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
echo "== on demand (X11)"
check "on demand: the hello says clipreq" '^HELLO lordos-guest [0-9]* clipboard clipx clipreq\( stopping\)\{0,1\}\( perf\)\{0,1\}$' "$D/out"
nocheck "on demand: TARGETS asks the host nothing" '^REQ' "$D/out"
xc -t TARGETS >"$D/targets" 2>/dev/null
check "on demand: TARGETS lists the text" '^UTF8_STRING$' "$D/targets"
check "on demand: TARGETS lists the picture" '^image/png$' "$D/targets"
check "on demand: TARGETS lists the rich text" '^text/html$' "$D/targets"
xc -t UTF8_STRING >"$D/text" 2>/dev/null
check "on demand: a paste gets the host's text" '^from the host ü$' "$D/text"
check "on demand: the text was asked for" '^REQ 7 1$' "$D/out"
xc -t UTF8_STRING >/dev/null 2>&1
[ "$(grep -c '^REQ 7 1$' "$D/out")" = 1 ] && echo "ok: on demand: asked once per form" || { echo "FAIL: on demand: the text was asked for twice"; R=1; }
xc -t image/png >"$D/pic" 2>/dev/null
[ "$(wc -c <"$D/pic")" = 300000 ] && echo "ok: on demand: a large picture comes whole (INCR)" || { echo "FAIL: on demand: the picture has $(wc -c <"$D/pic") bytes"; R=1; }
xc -t text/html >"$D/html" 2>/dev/null
[ ! -s "$D/html" ] && echo "ok: on demand: a refused form pastes nothing" || { echo "FAIL: on demand: a refused form pasted something"; R=1; }
check "on demand: the rich text was asked for" '^REQ 7 2$' "$D/out"
printf 'guest copy' | env -i PATH=/usr/bin:/bin DISPLAY=:$N HOME="$D" xclip -i -selection clipboard 2>/dev/null
i=0; while ! grep -q '^CLIP guest copy$' "$D/out" && [ $i -lt 40 ]; do i=$((i + 1)); sleep 0.1; done
check "on demand: another program's copy goes to the host again" '^CLIP guest copy$' "$D/out"
nocheck "on demand: the host's own text never came back" '^CLIP from the host' "$D/out"
kill $H 2>/dev/null; wait $H 2>/dev/null; H=''
wait $L 2>/dev/null; L=''
sed 's/^/   /' "$D/err"
exit $R
