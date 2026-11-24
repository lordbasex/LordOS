#!/bin/sh
# chrome_portal_headless.sh — Google Chrome's "Save as" goes through LordOS's own FileChooser portal (version 3,
# src/ui/portal.c; Chromium's select_file_dialog_linux_portal.cc wants 3), shows LordOS's file dialog over the Chrome
# window (its xdg-foreign handle) and saves where the person picks. A real Chrome (/opt/google/chrome/chrome) against a
# headless build/lord-server-test on a private session bus (dbus-run-session) with a private home and Chrome profile under
# ${CHROME_PORTAL_DIR:-/var/tmp/lordos-ws-bar}; the screenshot stays there as chrome-saveas.png. Opt-in (not in
# run_suites.sh: it needs Google Chrome, and the clicks are at the places this 1600×900 scene puts Chrome's idle
# permission prompt and the dialog's Save button). Never against a live session.
#   dbus-run-session -- sh tests/chrome_portal_headless.sh        (from server/)
# Exit 77 when something is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
CHROME=/opt/google/chrome/chrome
[ -x build/lord-server-test ] && [ -x "$CHROME" ] && [ -f ../themes/workspaces/theme.bundle.js ] || { echo "skipped: needs build/lord-server-test, Google Chrome and the Workspaces bundle"; exit 77; }
[ -n "$DBUS_SESSION_BUS_ADDRESS" ] || { echo "skipped: run it under dbus-run-session"; exit 77; }
BASE=${CHROME_PORTAL_DIR:-/var/tmp/lordos-ws-bar}
mkdir -p "$BASE" && chmod 700 "$BASE"
D=$(mktemp -d "$BASE/chrome.XXXXXX") || exit 1
P='' C=''
stop() { [ -n "$C" ] && kill $C 2>/dev/null; [ -n "$P" ] && kill $P 2>/dev/null; sleep 0.5; [ -n "$C" ] && kill -9 $C 2>/dev/null; [ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"; }
trap stop EXIT
mkdir -p "$D/home/.config/lordos" "$D/home/Downloads"
printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LANG=en_US.UTF-8 \
    ./build/lord-server-test --headless 1600x900 --theme ../themes/workspaces/theme.bundle.js --timeout 120 --user "$(id -un)" --test-hooks >"$D/server.log" 2>&1 &
P=$!
sleep 6
S=$(ls "$D" | grep '^wayland-[0-9]*$' | head -1)
[ -n "$S" ] || { echo "FAIL: no Wayland socket"; exit 1; }
env -u DISPLAY XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" HOME="$D/home" "$CHROME" --user-data-dir="$D/chrome" --no-first-run \
    --no-default-browser-check --ozone-platform=wayland --disable-gpu --password-store=basic \
    "data:text/html,<title>Report</title><h1>Save me</h1>" >"$D/chrome.log" 2>&1 &
C=$!
sleep 9
python3 - "$D" "$BASE" <<'PY'
import socket, os, sys, time
d, base = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); s.connect(os.path.join(d, 'lordos-test-hooks.sock')); s.settimeout(10)
def cmd(c): s.send(c.encode()); return s.recv(64).decode()
def click(x, y): cmd(f'ptr {x} {y}'); cmd('btn 0 1'); cmd('btn 0 0')
click(855, 316); time.sleep(1)                 # Chrome asks for the idle permission: Allow
click(800, 500); time.sleep(0.8)               # the page
for k, v in ((29, 1), (31, 1), (31, 0), (29, 0)): cmd(f'key {k} {v}')   # Ctrl+S
time.sleep(5)
cmd('frame output')
from PIL import Image
Image.open(os.path.join(d, 'lordos-test-frame-output.ppm')).save(os.path.join(base, 'chrome-saveas.png'))
click(1234, 740); time.sleep(3)                # LordOS's dialog: Save
got = os.listdir(os.path.join(d, 'home/Downloads'))
print('saved:', got)
sys.exit(0 if 'Report.html' in got else 1)
PY
R=$?
[ $R = 0 ] && echo "chrome portal: ok (Chrome's Save as through LordOS's FileChooser; screenshot $BASE/chrome-saveas.png)" || echo "FAIL: Chrome did not save through the portal"
exit $R
