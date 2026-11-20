#!/bin/sh
# guestwin_headless.sh — apps mode's guest side (server/src/ui/js_guestwin.c, sdk/guestwin.js, tools/guest/lordos-guest.c)
# against a headless lord-server on the CPU path, in a private runtime folder and home, with LORD_GUESTWIN=1 (outside a
# Machines guest the endpoint isn't made). build/guestwin-client talks to the endpoint directly, then plays the host's
# end of org.lordos.agent for lordos-guest (--port a Unix socket). Run from server/ (tests/run_suites.sh does). Exit 77
# when something it needs is missing, 1 on a failure. Not tested here: a peer of another uid being refused (needs root).
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/guestwin-client ] && [ -x build/lordos-guest ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/guestwin-client, build/lordos-guest and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lgw.XXXXXX") || exit 1
G=''
trap 'kill $P $G 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/home"
chmod 700 "$D"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_GUESTWIN=1 \
    ./build/lord-server --headless 1024x768 --theme "$THEME" --timeout 90 >"$D/server.log" 2>&1 &
P=$!
R=0
echo "== directly"
./build/guestwin-client --direct "$D/lordos-guestwin.sock" || R=1
[ -S "$D/lordos-guestwin.sock" ] && [ "$(stat -c %a "$D/lordos-guestwin.sock")" = 600 ] && echo "ok: the socket is 0600" || { echo "FAIL: the socket isn't 0600"; R=1; }
echo "== through lordos-guest"
( sleep 0.5; exec env XDG_RUNTIME_DIR="$D" ./build/lordos-guest --port "$D/agent.sock" ) >"$D/guest.log" 2>&1 &
G=$!
./build/guestwin-client --host "$D/agent.sock" || R=1
if grep -qi "TypeError\|ReferenceError\|exception" "$D/server.log"; then echo "FAIL: a JS error in the session"; grep -i -A3 "TypeError\|ReferenceError\|exception" "$D/server.log" | head -20; R=1; else echo "ok: no JS error in the session (apps mode on and off)"; fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -30 "$D/server.log"; R=1; }
[ $R = 0 ] || { echo "-- server log"; grep -i "guestwin\|error\|exception" "$D/server.log" | tail -20; }
exit $R
