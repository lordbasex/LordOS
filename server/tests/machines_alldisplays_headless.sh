#!/bin/sh
# machines_alldisplays_headless.sh — All Displays (docs/VIRTUALIZATION.md 5.4) in a headless lord-server, as screenshots:
# the real theme bundle with tests/machines_alldisplays_scene/pre.js before it (the headless frame as two displays, a
# made-up running machine with two outputs; nothing is started) and driver.js after it. Each step prints "done" or
# "missing"; the exit status is 1 when one is missing. The engine side (two outputs of a real guest) is tests/vm_multihead.sh.
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/machines_alldisplays_headless.sh OUT_DIR [theme] [language] [WxH]   → OUT_DIR/alld-<theme>-<language>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/machines_alldisplays_headless.sh OUT_DIR [theme] [language] [WxH]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zmalld/$THEME"
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lmalld.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zmalld"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zmalld"
mkdir -p "$ROOT/themes/zmalld" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
W=${SIZE%x*}; H=${SIZE#*x}
LABELS=$(node -e "const m=require('$ROOT/assets/locales/$LANGID/machines.json');process.stdout.write(JSON.stringify({item:m['alld.item'],back:m['alld.back'],half:Math.floor($W/2)&~1,h:$H&~1}))")
{ printf 'globalThis.APP_L = %s;\n' "$LABELS"
  cat tests/machines_alldisplays_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/machines_alldisplays_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zmalld/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "mode": "dark"}\n' "$LANGID" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 90 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 160); do grep -aq 'AD-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'AD-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*AD-SCENE/ /'
grep -ai 'TypeError\|ReferenceError\|exception' "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null | tail -8
for k in 1 2 3 4; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/alld-$THEME-$LANGID-$k.png"; done
ls "$OUT"/alld-"$THEME-$LANGID"-*.png 2>/dev/null
! grep -aq 'AD-SCENE .* missing' "$D/home/.cache/lordos/shell.log" 2>/dev/null && grep -aq 'AD-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null
