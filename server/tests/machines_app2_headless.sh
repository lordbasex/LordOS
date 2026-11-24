#!/bin/sh
# machines_app2_headless.sh — the Machines app's last parts in a headless lord-server, as screenshots
# (docs/VIRTUALIZATION.md 5.1, 5.2, 5.5): the real theme bundle with tests/machines_app2_scene/pre.js before it (a made-up
# library whose folders and app.json files are real, in the scene's home) and driver.js after it; the New Machine
# assistant reads a tiny arm64 installer disc (mkiso.py) with the real lord-vm --iso-detect. Each step prints "done" or
# "missing"; the exit status is 1 when one is missing.
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/machines_app2_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]   → OUT_DIR/<theme>-<language>-<mode>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/machines_app2_headless.sh OUT_DIR [theme] [language] [WxH] [dark|light]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
MODE=${5:-dark}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -x build/lord-vm ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "needs build/lord-server, build/lord-vm and themes/$THEME/theme.bundle.js"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zmapp2/$THEME"
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lmapp2.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zmapp2"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zmapp2"
mkdir -p "$ROOT/themes/zmapp2" "$D/home/.config/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
# the library's folders: app.json is the app's own (meta.js); machine.json isn't read (pre.js gives lord.vm.list)
for m in "Ubuntu 24.04" "Base Debian" "Fedora"; do mkdir -p "$D/home/Machines/$m.lordvm"; done
printf '{"os":"ubuntu","osName":"Ubuntu 24.04","tags":["work","web"],"color":"violet"}\n' >"$D/home/Machines/Ubuntu 24.04.lordvm/app.json"
printf '{"os":"debian","osName":"Debian 13","template":true}\n' >"$D/home/Machines/Base Debian.lordvm/app.json"
printf '{"os":"fedora","osName":"Fedora 43"}\n' >"$D/home/Machines/Fedora.lordvm/app.json"
python3 -I tests/machines_app2_scene/mkiso.py "$D/home/ubuntu-24.04.1-live-server-arm64.iso" "Ubuntu-Server 24.04.1 LTS arm64" BOOTAA64.EFI || exit 1
{ printf 'globalThis.APP_M = '; cat "$ROOT/assets/locales/$LANGID/machines.json"; printf ';\nglobalThis.APP_S = '; cat "$ROOT/assets/locales/$LANGID/shell.json"; printf ';\n'
  cat tests/machines_app2_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/machines_app2_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zmapp2/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "mode": "%s"}\n' "$LANGID" "$MODE" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 150 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 260); do grep -aq 'APP-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'APP-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*APP-SCENE/ /'
grep -ai 'TypeError\|ReferenceError\|exception' "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null | tail -8
for k in 1 2 3 4 5 6 7 8 9 10; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID-$MODE-$k.png"; done
ls "$OUT"/"$THEME-$LANGID-$MODE"-*.png 2>/dev/null
! grep -aq 'APP-SCENE .* missing' "$D/home/.cache/lordos/shell.log" 2>/dev/null && grep -aq 'APP-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null
