#!/bin/sh
# gpu_headless.sh — Monitor → GPU in a headless lord-server, as screenshots: the real theme bundle with
# tests/gpu_scene/pre.js before it (made-up cards, processes and AI engines; nothing of the machine is shown) and
# driver.js after it (one GPU, then Side by side).
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/gpu_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]   → OUT_DIR/<theme>-<language>-<mode>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/gpu_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1440x900}
MODE=${5:-dark}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zgpu/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lgpu.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zgpu"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zgpu"
mkdir -p "$ROOT/themes/zgpu" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
# the labels the driver clicks, in the scene's language
LABELS=$(node -e "const m=require('$ROOT/assets/locales/$LANGID/monitor.json');process.stdout.write(JSON.stringify({side:m['gpu.v.side']}))")
{ printf 'globalThis.GPU_L = %s;\n' "$LABELS"
  cat tests/gpu_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/gpu_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zgpu/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "mode": "%s"}\n' "$LANGID" "$MODE" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 150 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 200); do grep -aq 'GPU-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'GPU-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*GPU-SCENE/ /'
for k in 1 2 3 4 5 6 7; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID-$MODE-$k.png"; done
ls "$OUT"/"$THEME-$LANGID-$MODE"-*.png 2>/dev/null
