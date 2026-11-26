#!/bin/sh
# popup_headless.sh [client] — build/popup-client (tests/popup_client.c, GitHub #159), or another Wayland test client
# of build/ (cycle-client: tests/cycle_client.c), against a headless lord-server on the CPU path, in a private runtime
# folder and home. Run from server/ (tests/run_suites.sh does). Exit 77 when something it
# needs is missing (the theme bundle: npm run build), 1 on a failure or if lord-server didn't survive.
cd "$(dirname "$0")/.." || exit 1
CLIENT=${1:-popup-client}
case $CLIENT in *[!a-z-]*|'') echo "bad client name"; exit 1 ;; esac
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x "build/$CLIENT" ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/$CLIENT and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lpc.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
# a desktop already chosen and the welcome seen: nothing of the first start takes the keyboard from the client
echo '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en"}' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 \
    ./build/lord-server --headless 640x480 --theme "$THEME" --timeout 40 >"$D/server.log" 2>&1 &
P=$!
i=0 S=''
while [ $i -lt 120 ] && [ -z "$S" ]; do
    sleep 0.25
    S=$(ls "$D" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    i=$((i + 1))
done
[ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; exit 1; }
XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" "./build/$CLIENT"
R=$?
sleep 0.5
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; R=1; }
exit $R
