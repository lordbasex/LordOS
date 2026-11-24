#!/bin/sh
# machines_app_headless.sh — Machines' app and its system ties in a headless lord-server, as screenshots
# (docs/VIRTUALIZATION.md 3.3, 3.7, 5.1, 5.7, 6.14): the real theme bundle with tests/machines_app_scene/pre.js before it
# (a made-up library, a kernel without Landlock, on battery, short of memory; nothing is started or changed) and driver.js
# after it. Each step prints "done" or "missing"; the exit status is 1 when one is missing.
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/machines_app_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]   → OUT_DIR/<theme>-<language>-<mode>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/machines_app_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
MODE=${5:-dark}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zmapp/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lmapp.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zmapp"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zmapp"
mkdir -p "$ROOT/themes/zmapp" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
# the texts the driver looks for and clicks, in the scene's language
LABELS=$(node -e "const m=require('$ROOT/assets/locales/$LANGID/machines.json');const f=(k,v)=>Object.entries(v||{}).reduce((s,[a,b])=>s.split('{'+a+'}').join(b),m[k]);process.stdout.write(JSON.stringify({toolsOn:m['light.agent.connected'],diskBusy:m['light.disk.busy'],weak:m['weak.title'],budget:m['sum.budget'],pressure:m['sum.pressure'],canRun:m['pi.menu'],nested:m['run.note.nested'],sensors:m['run.sensors'],back:m['back'],budgetTitle:f('budget.title',{name:'Windows 11'}),suspendDebian:f('budget.suspend',{name:'Debian 13'}),less:f('budget.less',{mb:4864}),cancel:m['cancel'],atLogin:m['cfg.atLogin'],autoPause:m['cfg.autoPause']}))")
{ printf 'globalThis.APP_L = %s;\n' "$LABELS"
  cat tests/machines_app_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/machines_app_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zmapp/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "mode": "%s"}\n' "$LANGID" "$MODE" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 120 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 200); do grep -aq 'APP-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'APP-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*APP-SCENE/ /'
grep -ai 'TypeError\|ReferenceError\|exception' "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null | tail -8
for k in 1 2 3 4 5 6; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID-$MODE-$k.png"; done
ls "$OUT"/"$THEME-$LANGID-$MODE"-*.png 2>/dev/null
! grep -aq 'APP-SCENE .* missing' "$D/home/.cache/lordos/shell.log" 2>/dev/null && grep -aq 'APP-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null
