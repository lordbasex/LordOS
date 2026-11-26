#!/bin/sh
# protected_headless.sh — protected windows (docs/PRIVACY.md "Protected windows") against a headless build/lord-server-test
# with --test-hooks, on the CPU path, in a private runtime folder and home. The real Flow with a small driver
# (tests/protected_scene.mjs) opens Calculator protected (wm.open's `protected`, locked) and Notes beside it; then the
# test hooks write, as PPMs (testhooks.h "frame"): what the display shows (output), a screenshot (media_screenshot),
# what the remote desktop, the screen-sharing API and a recording get (media_composed), the Screenshot portal's still
# (cast_still) and each window shared alone (cast_still of a window). tests/protected_check.py then wants the protected
# window drawn on the output and the protected fill in every capture, and Notes captured as shown.
# Never against a live session. Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing
# (node with esbuild: npm install; python3; build/lord-server-test), 1 on a failure. PROTECTED_KEEP=DIR keeps the
# pictures there; PROTECTED_DEBUG=1 shows the logs.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server-test ] || { echo "needs build/lord-server-test"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: needs python3"; exit 77; }
command -v node >/dev/null 2>&1 || { echo "skipped: needs node"; exit 77; }
P='' D=''
stop() { [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; [ -n "$D" ] && rm -rf "$D"; P='' D=''; }
trap stop EXIT
D=$(mktemp -d "${TMPDIR:-/tmp}/lpw.XXXXXX") || exit 1
chmod 700 "$D"
if ! node tests/protected_scene.mjs "$D/theme" >"$D/scene.log" 2>&1; then
    echo "skipped: the scene needs node with esbuild (npm install)"; cat "$D/scene.log"; exit 77
fi
mkdir -p "$D/home/.config/lordos"
printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LANG=en_US.UTF-8 \
    ./build/lord-server-test --headless 1280x800 --theme "$D/theme/theme.bundle.js" --timeout 90 --user "$(id -un)" --test-hooks >"$D/server.log" 2>&1 &
P=$!
logs() { cat "$D/home/.cache/lordos/shell.log" "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null; }
i=0
while [ $i -lt 120 ] && ! logs | grep -q 'SCENE ready'; do sleep 0.25; i=$((i + 1)); done
[ -n "$PROTECTED_DEBUG" ] && logs | tail -60
logs | grep -q 'SCENE ready' || { echo "FAIL: the scene never got ready"; logs | tail -30; exit 1; }
rect() { logs | grep "SCENE win $1 " | tail -1 | sed 's/.*rect=\([0-9,-]*\).*/\1/'; }
node_of() { logs | grep "SCENE win $1 " | tail -1 | sed 's/.*node=\([0-9]*\).*/\1/'; }
R=0
logs | grep -q 'SCENE win calculator .*protected=1' || { echo "FAIL: Calculator did not open protected"; R=1; }
logs | grep -q 'SCENE win notes .*protected=0' || { echo "FAIL: Notes opened protected"; R=1; }
logs | grep -q 'SCENE ready count=[1-9].* locked=1' || { echo "FAIL: no protected node, or the lock let setProtected(false) through"; logs | grep 'SCENE'; R=1; }
PN=$(node_of calculator) ON=$(node_of notes)
python3 - "$D" "$PN" "$ON" <<'EOF' || R=1
import os, socket, sys
d, pn, on = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect(os.path.join(d, 'lordos-test-hooks.sock'))
s.settimeout(10)
bad = 0
for cmd, name in (('frame output', 'output'), ('frame screenshot', 'screenshot'), ('frame mirror', 'mirror'), ('frame portal', 'portal'),
                  (f'frame window {pn}', 'window'), (f'frame window {on}', 'window')):
    s.send(cmd.encode())
    r = s.recv(256).decode()
    print(f'hook {cmd}: {r}')
    bad += r != 'ok'
    if cmd.startswith('frame window'):
        tag = 'window-p' if cmd.endswith(' ' + pn) else 'window-o'
        os.replace(os.path.join(d, 'lordos-test-frame-window.ppm'), os.path.join(d, f'lordos-test-frame-{tag}.ppm'))
sys.exit(1 if bad else 0)
EOF
python3 tests/protected_check.py "$D" "$(rect calculator)" "$(rect notes)" || R=1
kill -0 $P 2>/dev/null || { echo "FAIL: lord-server died"; logs | tail -20; R=1; }
if [ -n "$PROTECTED_KEEP" ]; then mkdir -p "$PROTECTED_KEEP" && cp "$D"/lordos-test-frame-*.ppm "$PROTECTED_KEEP"/; fi
[ $R = 0 ] && echo "protected windows headless: ok" || logs | grep -E 'SCENE|protect' | tail -20
exit $R
