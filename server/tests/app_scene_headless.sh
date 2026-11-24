#!/bin/sh
# app_scene_headless.sh — runs one app scene in a headless lord-server and keeps its screenshots: the real theme bundle
# with tests/<scene>/pre.js before it and tests/<scene>/driver.js after it, in a private home made here (the scene's
# setup.sh, when there is one, fills it first: `setup.sh HOME`). Scenes: notes_scene (Notes: the window, its attachments
# and the sticky notes on the desktop, GitHub #238), files_marquee_scene (Files' rubber-band selection, GitHub #233),
# screenshots_menu_scene (Screenshots' Options menu above the bar on its first open, #237) and settings_region_scene
# (Settings → Region names each region's language, #239).
# The driver prints lines starting with "APP-SCENE" in the shell log; each check says "done" or "missing", and the exit
# status is 1 when one is missing. Nothing outside the temporary home is read or written (never the person's own notes).
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/app_scene_headless.sh SCENE OUT_DIR [theme] [language] [dark|light] [restart]
#     → OUT_DIR/<scene>-<theme>-<language>-<mode>-1.png …
#   restart: the server is started a second time on the same home (the scene's driver is told RESTART=1), to check what
#   persists across a new session.
cd "$(dirname "$0")/.." || exit 1
SCENE=${1:?usage: tests/app_scene_headless.sh SCENE OUT_DIR [theme] [language] [dark|light] [restart]}
OUT=${2:?usage: tests/app_scene_headless.sh SCENE OUT_DIR [theme] [language] [dark|light] [restart]}
THEME=${3:-default}
LANGID=${4:-en}
MODE=${5:-dark}
AGAIN=${6:-}
ROOT=$(cd .. && pwd)
[ -f "tests/$SCENE/driver.js" ] || { echo "no scene tests/$SCENE"; exit 2; }
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "skipped: needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zscene/$THEME"
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lscene.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zscene"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zscene"
mkdir -p "$ROOT/themes/zscene" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
[ -x "tests/$SCENE/setup.sh" ] && { "tests/$SCENE/setup.sh" "$D/home" || exit 1; }
bundle() {
    { printf 'globalThis.SCENE_RESTART = %s;\n' "$1"
      for ns in notes files shell; do printf 'globalThis.L_%s = ' "$ns"; cat "$ROOT/assets/locales/$LANGID/$ns.json"; printf ';\n'; done
      cat "tests/$SCENE/pre.js" "$ROOT/themes/$THEME/theme.bundle.js" "tests/$SCENE/driver.js"; } >"$T/theme.bundle.js"
    node tests/perf/manifest.mjs "$ROOT" "themes/zscene/$THEME/theme.bundle.js" || exit 1
}
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "%s", "mode": "%s"}\n' "$LANGID" "$MODE" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
run() {
    rm -f "$D/home/.cache/lordos/shell.log"
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
        XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
        ./build/lord-server --headless 1280x800 --theme "$T/theme.bundle.js" --timeout 170 >"$D/server.log" 2>&1 &
    P=$!
    for i in $(seq 1 320); do grep -aq 'APP-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
    sleep 2
    grep -ah 'APP-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*APP-SCENE/ /'
    grep -ai 'TypeError\|ReferenceError\|exception' "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null | tail -8
    cat "$D/home/.cache/lordos/shell.log" >>"$D/all.log" 2>/dev/null
    kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; P=
    for k in $(seq 1 40); do [ -f "$D/home/shot-$k.png" ] && mv "$D/home/shot-$k.png" "$OUT/$SCENE-$THEME-$LANGID-$MODE-$2$k.png"; done
}
bundle 0
run 0 ''
if [ "$AGAIN" = restart ]; then bundle 1; run 1 r; fi
ls "$OUT"/"$SCENE-$THEME-$LANGID-$MODE"-*.png 2>/dev/null
! grep -aq 'APP-SCENE .* missing' "$D/all.log" 2>/dev/null && grep -aq 'APP-SCENE done' "$D/all.log" 2>/dev/null
