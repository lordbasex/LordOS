#!/bin/sh
# monitor_headless.sh — Monitor → Processes in a headless lord-server, as screenshots: the real theme bundle with
# tests/monitor_scene/pre.js before it (a made-up process list with fixed owners; no real process is ever signalled) and
# driver.js after it (the flat list, Grouped, groups open with who started each program, a group's hover summary,
# Quit All's sheet, a program's Get Info, the Flatpak filter, the rolling statistics' columns and a summary's sparklines).
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/monitor_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]   → OUT_DIR/<theme>-<language>-<mode>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/monitor_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
MODE=${5:-dark}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zmonitor/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lmonitor.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zmonitor"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zmonitor"
mkdir -p "$ROOT/themes/zmonitor" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
# the labels the driver clicks, in the scene's language
LABELS=$(node -e "const m=require('$ROOT/assets/locales/$LANGID/monitor.json');process.stdout.write(JSON.stringify({grouped:m['group.toggle'],terminal:m['comp.terminal'],processes:m['processes'],stats:m['proc.tab.stats'],flatpak:m['filter.flatpak'],quitAll:m['group.quitAll']}))")
{ printf 'globalThis.MONITOR_L = %s;\n' "$LABELS"
  cat tests/monitor_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/monitor_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zmonitor/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "mode": "%s"}\n' "$LANGID" "$MODE" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 150 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 200); do grep -aq 'MONITOR-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'MONITOR-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*MONITOR-SCENE/ /'
for k in 1 2 3 4 5 6 7; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID-$MODE-$k.png"; done
ls "$OUT"/"$THEME-$LANGID-$MODE"-*.png 2>/dev/null
