#!/bin/sh
# studio_headless.sh — LordOS Studio's popover in a headless lord-server (docs/CAMERA-STUDIO-PLAN.md 9.1), as
# screenshots: the real theme bundle with tests/studio_scene/pre.js before it (made-up cameras, the preview a still of
# build/studio-test's output: no real camera is ever opened) and driver.js after it (an app "uses" the camera so the
# capture item by the clock shows, then the item, "Video effects" and Background are clicked, a screenshot after each).
# STUDIO_SCENE=mic: an app uses only the microphone, then the item and Mic Mode; STUDIO_SCENE=app: the LordOS Studio
# app's window, then its Portrait page (tests/studio_scene/driver.js).
# A test tool: not part of `make test`, never installed. From server/, after `npm run build`, `make` and
# `make build/studio-test`:
#   [STUDIO_SCENE=effects|mic|app] tests/studio_headless.sh OUT_DIR [theme] [language] [WxH]
#                                                   → OUT_DIR/<theme>-<language>[-<scene>]-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/studio_headless.sh OUT_DIR [theme] [language] [WxH]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -x build/studio-test ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server, build/studio-test and themes/$THEME/theme.bundle.js"; exit 77; }
command -v ffmpeg >/dev/null || { echo "needs ffmpeg (the still)"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zstudio/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lstudio.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zstudio"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zstudio"
mkdir -p "$ROOT/themes/zstudio" "$D/home/.config/lordos" "$D/out"
cp -r "$ROOT/themes/$THEME" "$T"
STUDIO_TEST_OUT="$D/out" ./build/studio-test >/dev/null 2>&1
ffmpeg -v error -y -i "$D/out/f04-blur.ppm" "$D/home/studio-still.png" || exit 1
# the labels the driver clicks, in the scene's language
SCENE=${STUDIO_SCENE:-effects}
case "$SCENE" in effects) TAG= ;; mic|app) TAG="-$SCENE" ;; *) echo "STUDIO_SCENE: effects, mic or app"; exit 2 ;; esac
LABELS=$(node -e "const s=require('$ROOT/assets/locales/$LANGID/studio.json');process.stdout.write(JSON.stringify([s.open,s.background,s.mic,s.portrait]))")
{ printf "globalThis.STUDIO_STILL = '%s';\nglobalThis.STUDIO_SCENE = '%s';\n" "$D/home/studio-still.png" "$SCENE"
  printf "globalThis.STUDIO_OPEN = %s[0];\nglobalThis.STUDIO_BG = %s[1];\nglobalThis.STUDIO_MIC = %s[2];\nglobalThis.STUDIO_PORTRAIT = %s[3];\n" "$LABELS" "$LABELS" "$LABELS" "$LABELS"
  cat tests/studio_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/studio_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zstudio/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "studio": {"mode": "blur", "blur": 60}}\n' "$LANGID" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 120 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 120); do grep -aq 'STUDIO-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'STUDIO-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*STUDIO-SCENE/ /'
for k in 1 2 3; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID$TAG-$k.png"; done
ls "$OUT"/"$THEME-$LANGID$TAG"-*.png 2>/dev/null
