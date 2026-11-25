#!/bin/sh
# fsub_headless.sh — a fullscreen window drawn in a desync subsurface, as Firefox draws a page (GitHub #232):
# build/fsub-client (tests/fsub_client.c) against a headless build/lord-server-test with --test-hooks on the CPU path,
# in a private runtime folder and home, with LORD_PAINT_VERIFY=1 (every frame painted with only that damage must equal
# a full repaint). Never against a live session. Run from server/ (tests/run_suites.sh does). Exit 77 when something it
# needs is missing (the theme bundle: npm run build; build/lord-server-test), 1 on a failure or if lord-server died.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server-test ] && [ -x build/fsub-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server-test, build/fsub-client and $THEME"; exit 77; }
P='' D=''
stop() { [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; [ -n "$D" ] && rm -rf "$D"; P='' D=''; }
trap stop EXIT
D=$(mktemp -d "${TMPDIR:-/tmp}/lfs.XXXXXX") || exit 1
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LORD_PAINT_VERIFY=1 LANG=en_US.UTF-8 \
    ./build/lord-server-test --headless 1280x800 --theme "$THEME" --timeout 90 --test-hooks >"$D/server.log" 2>&1 &
P=$!
i=0 S=''
while [ $i -lt 120 ] && [ -z "$S" ]; do
    sleep 0.25
    S=$(ls "$D" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    i=$((i + 1))
done
[ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/server.log"; exit 1; }
sleep 2.5                                   # the theme is up
XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ${FSUB_DEBUG:+env FSUB_DEBUG=1} ./build/fsub-client
R=$?
[ $R -eq 77 ] && exit 77
sleep 0.5
v=$(grep -ah 'paint-verify' "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null | wc -l)
bad=$(grep -ah 'paint-verify' "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null | grep -vc ': 0 pixels differ')
if [ "$v" -gt 0 ] && [ "$bad" -eq 0 ]; then echo "ok   every painted frame equals a full repaint ($v checked)"
else echo "FAIL paint check: $bad of $v frames differ from a full repaint"; grep -ah 'paint-verify' "$D/home/.cache/lordos/compositor.log" "$D/server.log" | grep -v ': 0 pixels differ' | head -5; R=1; fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -20 "$D/server.log"; R=1; }
exit $R
