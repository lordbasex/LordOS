#!/bin/sh
# machines_group_headless.sh — a machine group in Machines (docs/VIRTUALIZATION.md 19.8) in a headless lord-server, as
# screenshots: the real theme bundle with tests/machines_group_scene/pre.js before it (a made-up library with one Whonix
# group and the admin helper's answers made up; nothing is downloaded, started or changed) and driver.js after it (the
# library's group card, its menu, Destroy and Start Fresh's dialog with and without the Gateway, Configure's group view,
# the assistant's Whonix (Tor) card and the About Whonix step).
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/machines_group_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]   → OUT_DIR/<theme>-<language>-<mode>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/machines_group_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
MODE=${5:-dark}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zgroup/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lgroupui.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zgroup"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zgroup"
mkdir -p "$ROOT/themes/zgroup" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
# the labels the driver clicks, in the scene's language
LABELS=$(node -e "const m=require('$ROOT/assets/locales/$LANGID/machines.json');process.stdout.write(JSON.stringify({fresh:m['group.menu.fresh'],alsoGateway:m['group.fresh.gateway'],cancel:m['cancel'],configure:m['group.menu.configure'],newMachine:m['newMachine'],card:m['whonix.card'].replace('{name}','Whonix'),continue:m['as.continue'],rename:m['group.menu.rename'],delete:m['group.menu.delete'],repair:m['group.menu.repair']}))")
{ printf 'globalThis.GROUP_L = %s;\n' "$LABELS"
  cat tests/machines_group_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/machines_group_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zgroup/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "mode": "%s"}\n' "$LANGID" "$MODE" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
# lord-server in its own user and mount namespaces (as tests/vm_group_link.sh): an empty folder over the real home and a
# tmpfs over the real runtime folder, so it never sees the person's files or their session; without unshare it stops
command -v unshare >/dev/null 2>&1 && unshare -Urm true 2>/dev/null || { echo "needs unshare (user and mount namespaces)"; exit 77; }
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
mkdir -p "$D/fake"
unshare -Urm sh -c "mount --bind '$D/fake' '$REALHOME' && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"" \
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 150 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 200); do grep -aq 'GROUP-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'GROUP-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*GROUP-SCENE/ /'
grep -ai 'TypeError\|ReferenceError\|exception\|error' "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null | tail -8
for k in 1 2 3 4 5 6 7 8 9 10 11 12 13; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID-$MODE-$k.png"; done
ls "$OUT"/"$THEME-$LANGID-$MODE"-*.png 2>/dev/null
