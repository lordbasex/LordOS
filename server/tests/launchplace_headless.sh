#!/bin/sh
# launchplace_headless.sh — Workspaces: a window launched from a display's bar lands on that display, in that
# activity and its layout slot, even when the app shows a picker first and the person goes to another activity meanwhile
# (Google Chrome with profiles; themes/workspaces/model/launches.js, docs/WORKSPACES.md "Where launched windows go"), a
# dialog told its parent late goes over it (sdk/dialogmodel.js lateParentFrame), and the bar shows every window of the
# activity, a line under a minimized one. Against a headless build/lord-server-test (CPU path, --test-hooks for the
# screenshots) in a private runtime folder and home in a fresh private folder (or $LAUNCHPLACE_DIR, an existing folder
# of this user), with the real
# launcher: a .desktop entry runs build/launchplace-client (tests/launchplace_client.c). LAUNCHPLACE_LANG=ar runs it
# right to left. The screenshots stay in $LAUNCHPLACE_DIR/shots-<lang>/ (PNG). Never against a live session.
# Run from server/. Exit 77 when something it needs is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server-test ] && [ -x build/launchplace-client ] || { echo "skipped: needs build/lord-server-test and build/launchplace-client"; exit 77; }
command -v node >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 || { echo "skipped: needs node and python3"; exit 77; }
LANG_UI=${LAUNCHPLACE_LANG:-en}
# the folder for the run and its pictures: a fresh private one (mktemp -d), or the one given if it is a real folder of
# this user (not a link someone else could have planted)
if [ -n "$LAUNCHPLACE_DIR" ]; then
    [ -d "$LAUNCHPLACE_DIR" ] && [ -O "$LAUNCHPLACE_DIR" ] && [ ! -L "$LAUNCHPLACE_DIR" ] || { echo "FAIL: $LAUNCHPLACE_DIR must be an existing folder of this user, not a link"; exit 1; }
    BASE=$LAUNCHPLACE_DIR
else
    BASE=$(mktemp -d "${TMPDIR:-/tmp}/lordos-ws-bar.XXXXXX") || exit 1
fi
chmod 700 "$BASE"
D=$(mktemp -d "$BASE/run.XXXXXX") || exit 1
SHOTS="$BASE/shots-$LANG_UI"
[ -L "$SHOTS" ] && { echo "FAIL: $SHOTS is a link"; exit 1; }
rm -rf "$SHOTS"; mkdir -m 700 "$SHOTS" || exit 1
P=''
stop() { [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; pkill -f "$D/launchplace-client" 2>/dev/null; [ -z "$LAUNCHPLACE_KEEP" ] && rm -rf "$D"; }
trap stop EXIT
node tests/launchplace_scene.mjs "$D/theme" >"$D/scene.log" 2>&1 || { echo "skipped: the scene needs node with esbuild (npm install)"; cat "$D/scene.log"; exit 77; }
cp build/launchplace-client "$D/launchplace-client"
mkdir -p "$D/home/.config/lordos" "$D/home/.local/share/applications"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "welcomeShown": true, "theme": "workspaces"}\n' "$LANG_UI" >"$D/home/.config/lordos/settings.json"
cat >"$D/home/.local/share/applications/fakechrome.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Fake Chrome
Exec=$D/launchplace-client fakechrome 2600 after dialog 40000
StartupWMClass=fakechrome
Icon=web-browser
Categories=Network;WebBrowser;
DESK
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LANG=en_US.UTF-8 \
    ./build/lord-server-test --headless 1600x900 --theme "$D/theme/theme.bundle.js" --timeout 120 --user "$(id -un)" --test-hooks >"$D/server.log" 2>&1 &
P=$!
logs() { cat "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null; }
python3 - "$D" "$SHOTS" <<'PY'
import os, socket, sys, time, re, subprocess
d, shots = sys.argv[1], sys.argv[2]
def logs():
    out = ''
    for f in (os.path.join(d, 'home/.cache/lordos/shell.log'), os.path.join(d, 'server.log')):
        try: out += open(f, errors='replace').read()
        except OSError: pass
    return out
sock = None
for _ in range(200):
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); sock.connect(os.path.join(d, 'lordos-test-hooks.sock')); break
    except OSError:
        sock = None; time.sleep(0.2)
if not sock: print('FAIL: no test hooks socket'); print(logs()[-2000:]); sys.exit(1)
sock.settimeout(10)
taken, t0 = set(), time.time()
while time.time() - t0 < 90:
    L = logs()
    for name in re.findall(r'SCENE shot (\S+)', L):
        if name in taken: continue
        time.sleep(0.6)
        sock.send(b'frame output'); r = sock.recv(64).decode()
        src = os.path.join(d, 'lordos-test-frame-output.ppm')
        if r == 'ok' and os.path.exists(src):
            from PIL import Image
            Image.open(src).save(os.path.join(shots, name + '.png'))
        print(f'shot {name}: {r}')
        taken.add(name)
    if 'SCENE click menu' in L and 'menu' not in taken:
        # the browser's icon in the display's bar: the second window icon of the tray (Files minimized, then the browser),
        # found from the bar's pixels would be fragile; its place is fixed by the header's layout at 1600 px
        x = 1600 - 376 if os.environ.get('LAUNCHPLACE_LANG') == 'ar' else 376
        for c in (f'ptr {x} 86', 'btn 2 1', 'btn 2 0'):
            sock.send(c.encode()); sock.recv(64)
        time.sleep(1.2)
        sock.send(b'frame output'); r = sock.recv(64).decode()
        from PIL import Image
        Image.open(os.path.join(d, 'lordos-test-frame-output.ppm')).save(os.path.join(shots, 'menu.png'))
        print(f'shot menu: {r}')
        taken.add('menu')
    if 'SCENE done' in L: break
    time.sleep(0.15)
L = logs()
bad = 0
def fail(m):
    global bad; bad += 1; print('FAIL: ' + m)
m = re.search(r'SCENE ready dev=(\S+) chat=(\S+) dk=(\S+) halves=(\S+);(\S+)', L)
if not m: print(L[-3000:]); print('FAIL: the scene never got ready'); sys.exit(1)
dev, chat, dk, h0, h1 = m.groups()
def wins(tag):
    out = {}
    for w in re.finditer(r'SCENE %s win (\S+) app=(\S*) desk=(\S+) min=(\d) parent=(\S*) rect=(\S+)' % tag, L):
        out[w.group(1)] = dict(app=w.group(2), desk=w.group(3), min=w.group(4), parent=w.group(5), rect=w.group(6))
    return out
pk = [w for w in wins('picker').values() if w['app'] == 'fakechrome']
if len(pk) != 1: fail(f'one picker window, got {pk}')
elif pk[0]['desk'] != dev or pk[0]['rect'] != h1: fail(f'the picker in Desarrollo\'s free half {h1}: {pk[0]}')
else: print('ok: the picker took the free half of the display it was launched from')
after = wins('after')
br = [ (k, w) for k, w in after.items() if w['app'] == 'fakechrome' and not w['parent'] ]
dl = [ (k, w) for k, w in after.items() if w['app'] == 'fakechrome' and w['parent'] ]
if len(br) != 1: fail(f'one browser window after the picker, got {br}')
elif br[0][1]['desk'] != dev: fail(f'the browser window opened while Chat was in view must go to Desarrollo: {br[0]}')
else: print('ok: the browser window went to the launch\'s activity (Desarrollo), not the one in view (Chat)')
if len(dl) != 1: fail(f'one dialog, got {dl}')
elif dl[0][1]['desk'] != dev or dl[0][1]['parent'] != br[0][0]: fail(f'the dialog with its parent, in Desarrollo: {dl}')
else: print('ok: the dialog told its parent late is with it, in Desarrollo')
back = wins('back')
b = back.get(br[0][0]) if br else None
files = [w for k, w in back.items() if k.startswith('files')]
if not b or b['rect'] != h1: fail(f'the browser window in the picker\'s half {h1}: {b}')
else: print('ok: the browser window took the picker\'s slot')
if not files or files[0]['rect'] != h0: fail(f'Files kept the first half {h0}: {files}')
if dl:
    bx, by, bw, bh = map(int, b['rect'].split(',')) if b else (0, 0, 0, 0)
    dx, dy, dw, dh = map(int, back[dl[0][0]]['rect'].split(','))
    if abs((dx + dw / 2) - (bx + bw / 2)) > 2 or abs((dy + dh / 2) - (by + bh / 2)) > 2 or (dw, dh) != (420, 260):
        fail(f'the dialog centred over its parent with its own size: {back[dl[0][0]]} vs {b}')
    else: print('ok: the dialog sits centred over its parent window, at its own size')
mn = wins('min')
if not any(k.startswith('files') and w['min'] == '1' for k, w in mn.items()): fail('Files minimized')
for n in ('start', 'picker', 'chat-after', 'placed', 'bar-minimized'):
    if not os.path.exists(os.path.join(shots, n + '.png')): fail('no screenshot ' + n)
sys.exit(1 if bad else 0)
PY
R=$?
kill -0 $P 2>/dev/null || { echo "FAIL: lord-server died"; logs | tail -20; R=1; }
[ -n "$LAUNCHPLACE_DEBUG" ] && logs | grep -E "SCENE|launchplace|error|Error" | tail -60
[ $R = 0 ] && echo "launch placement headless ($LANG_UI): ok, screenshots in $SHOTS" || logs | grep -E "SCENE|launchplace|rror" | tail -40
exit $R
