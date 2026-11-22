#!/bin/sh
# theme_switch_headless.sh — a theme switch where both the new theme and the one running fail to load (the running
# one's file changed under the session, as a package update does), against a headless lord-server on the CPU path.
# lord-server used to be left with no JavaScript context at all, and its next js_tick crashed in
# JS_GetGlobalObject(NULL) (2026-11-22 13:41). It must show the recovery screen and go on. Run from server/
# (tests/run_suites.sh does). Exit 77 when build/lord-server is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] || { echo "needs build/lord-server"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lts.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/home" "$D/themes/a" "$D/themes/b"
chmod 700 "$D"
# theme a: a root node, and three seconds in it asks for theme b
cat > "$D/themes/a/theme.bundle.js" <<'EOF'
var t0 = 0, asked = false;
globalThis.__lordTick = function (now) {
  if (!t0) t0 = now;
  if (!asked && now - t0 > 3000) { asked = true; lord.themes.use('b'); }
  return 50;
};
globalThis.__lordWantsFrame = function () { return false; };
globalThis.__lordFrame = function () {};
lord.setRoot(lord.createNode('view'));
EOF
echo "throw new Error('theme b does not load');" > "$D/themes/b/theme.bundle.js"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 \
    ./build/lord-server --headless 640x480 --theme "$D/themes/a/theme.bundle.js" --timeout 12 >"$D/server.log" 2>&1 &
P=$!
sleep 1.5
# theme a is gone from under the running session (a package update replaced it; here it can't be read at all, as a
# refused bundle, which also leaves no JavaScript context): going back to it fails too
rm -f "$D/themes/a/theme.bundle.js"
sleep 4
R=0
LOG="$D/home/.cache/lordos/compositor.log"
[ -f "$LOG" ] || LOG="$D/server.log"
if kill -0 $P 2>/dev/null; then echo "ok   lord-server is still running after the failed switch"
else echo "FAIL lord-server is gone"; R=1; fi
if grep -q "did not load again either: the recovery screen" "$LOG" "$D/server.log" 2>/dev/null; then echo "ok   it went to the recovery screen"
else echo "FAIL no recovery screen in the log"; R=1; fi
[ $R = 0 ] || tail -30 "$LOG" "$D/server.log" 2>/dev/null
exit $R
