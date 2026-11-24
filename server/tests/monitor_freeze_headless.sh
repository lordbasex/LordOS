#!/bin/sh
# monitor_freeze_headless.sh — (0.8.9.6) opening a Terminal group of 240 programs in Monitor → Processes (Grouped) in a
# headless lord-server: the real theme bundle with tests/monitor_scene/pre.js (MONITOR_BIG: the made-up list, no real
# process is ever signalled) before it and tests/monitor_scene/freeze.js after it, which prints the main thread's longest
# run (lord.stats(1)) when the group opens, while it stays open and when it closes. A private HOME and runtime folder;
# never the person's session. A test tool: not part of `make test`. From server/, after `npm run build` and `make`:
#   tests/monitor_freeze_headless.sh [theme]
cd "$(dirname "$0")/.." || exit 1
THEME=${1:-default}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
T="$ROOT/themes/zfreeze/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lfreeze.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zfreeze"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zfreeze"
mkdir -p "$ROOT/themes/zfreeze" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
LABELS=$(node -e "const m=require('$ROOT/assets/locales/en/monitor.json');process.stdout.write(JSON.stringify({grouped:m['group.toggle'],terminal:m['comp.terminal']}))")
{ printf 'globalThis.MONITOR_L = %s;\nglobalThis.MONITOR_BIG = true;\n' "$LABELS"
  cat tests/monitor_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/monitor_scene/freeze.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zfreeze/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "en", "monitor": {"procGrouped": true}}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless 1600x900 --theme "$T/theme.bundle.js" --timeout 60 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 100); do grep -aq 'MONITOR-FREEZE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
grep -ah 'MONITOR-FREEZE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*MONITOR-FREEZE/ /'
