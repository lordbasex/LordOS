#!/bin/sh
# perf_headless.sh — how long the main thread is busy for everyday actions (docs/MAIN-THREAD.md):
# a headless lord-server on the CPU path runs a theme with tests/perf/driver.js after it, which opens apps,
# clicks their sidebars and changes settings, one step every 1.5 s, and logs per step the main thread's JavaScript,
# layout and paint time and its longest single run, and the JS heap per scenario. Not part of `make test` (minutes).
#   tests/perf_headless.sh [theme] [plan,...]      e.g. tests/perf_headless.sh default store,settings
# Run from server/ after `npm run build` and `make`. The theme copy (themes/zperf/<theme>) is removed at the end.
cd "$(dirname "$0")/.." || exit 1
THEME=${1:-default}
PLAN=${2:-idle,settingsSet,launcher,store,settings,files,monitor}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
# one level deeper than a theme: lord-server's chosen_theme (the real user's settings.json "theme") looks for its
# choice next to the folder asked for, and themes/zperf has none, so this theme runs whatever the user chose
T="$ROOT/themes/zperf/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lperf.XXXXXX") || exit 1
cleanup() { kill $P 2>/dev/null; rm -rf "$D" "$ROOT/themes/zperf"; node "$ROOT/tools/code-manifest.mjs" >/dev/null; }
trap cleanup EXIT
rm -rf "$ROOT/themes/zperf"
mkdir -p "$ROOT/themes/zperf"
cp -r "$ROOT/themes/$THEME" "$T"
PLANJS=$(printf '%s' "$PLAN" | sed "s/[^a-zA-Z,]//g; s/,/','/g")
# PERF_BUNDLE: another build of the theme (an older checkout's, to compare)
# PERF_MARK=1: a line before each step, to tell which step a stall line (PERF_OUT) belongs to
{ printf "globalThis.PERF_PLAN = ['%s'];\nglobalThis.PERF_MARK = %s;\n" "$PLANJS" "$([ -n "$PERF_MARK" ] && echo 1 || echo 0)"; cat tests/perf/pre.js "${PERF_BUNDLE:-$ROOT/themes/$THEME/theme.bundle.js}" tests/perf/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zperf/$THEME/theme.bundle.js" || exit 1
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
# PERF_FLATPAK_FILES=N (the flatpak plan: 200000 by default): that many small files in the test home's
# ~/.local/share/flatpak and ~/.var/app, about what a few Flathub apps and their runtimes leave there
case ",$PLAN," in *,flatpak,*) : "${PERF_FLATPAK_FILES:=200000}" ;; esac
if [ "${PERF_FLATPAK_FILES:-0}" -gt 0 ]; then
    node "$ROOT/server/tests/perf/seed.mjs" "$D/home" "$PERF_FLATPAK_FILES" || exit 1
fi
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless 1920x1080 --theme "$T/theme.bundle.js" --timeout 900 >"$D/server.log" 2>&1 &
P=$!
while kill -0 $P 2>/dev/null; do
    grep -q 'PERF {"done"' "$D/server.log" 2>/dev/null && break
    sleep 1
done
LOG=$(ls "$D/home/.cache/lordos/shell.log" "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null)
[ -n "$PERF_OUT" ] && cat $LOG | grep -a "stall:" >"$PERF_OUT"
cat $LOG | grep -a "PERF " | tee ${PERF_RAW:-/dev/null} | sed 's/.*PERF //' | node "$ROOT/server/tests/perf/table.mjs"
