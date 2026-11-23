#!/bin/sh
# store_rankings_headless.sh — the Store's Flathub home with Flathub's rankings (E10) in a headless lord-server, as
# screenshots, the way tests/studio_headless.sh does it: the real theme bundle with tests/store_scene/pre.js before it
# and driver.js after it (the Store opens on Flathub; the home's top, its next rows, Popular's "See all", a category
# with its subcategory chips, the rankings' switch turned off and the home without them), in a private HOME and
# XDG_RUNTIME_DIR. The catalog is this machine's own Flathub appstream (~/.local/share/flatpak/appstream/flathub, copied
# into the private HOME) when lord-appstream reads it, else the recorded one (tests/data/flathub/catalog-2026-11-22.json,
# without icons). The rankings are seeded into ~/.cache/lordos/flathub/<list>.json from Flathub's recorded answers
# (tests/data/flathub/api-2026-11-22), in fetch/fhsvc.c's format and dated today, so nothing is fetched; with "live"
# nothing is seeded and lord-fetch asks flathub.org (only the home is shot, and the cache files are printed).
# A test tool: not part of `make test`, never installed. From server/, after `npm run build` and `make`:
#   tests/store_rankings_headless.sh OUT_DIR [theme] [language] [WxH] [live]  → OUT_DIR/<theme>-<language>-1.png …
cd "$(dirname "$0")/.." || exit 1
OUT=${1:?usage: tests/store_rankings_headless.sh OUT_DIR [theme] [language] [WxH] [live]}
THEME=${2:-default}
LANGID=${3:-en}
SIZE=${4:-1600x900}
LIVE=${5:-}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -x build/lord-appstream ] && [ -x build/lord-fetch ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] ||
    { echo "needs build/lord-server, build/lord-appstream, build/lord-fetch and themes/$THEME/theme.bundle.js"; exit 77; }
[ -x /usr/bin/flatpak ] || { echo "skipped: needs flatpak (/usr/bin/flatpak)"; exit 77; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T="$ROOT/themes/zstore/$THEME"
D=$(mktemp -d "${TMPDIR:-/tmp}/lstore.XXXXXX") || exit 1
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    rm -rf "$D" "$ROOT/themes/zstore"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
rm -rf "$ROOT/themes/zstore"
mkdir -p "$ROOT/themes/zstore" "$D/home/.config/lordos" "$D/home/.cache/lordos"
cp -r "$ROOT/themes/$THEME" "$T"
# the catalog: this machine's appstream when lord-appstream reads it as this user, else the recorded one
AS="$HOME/.local/share/flatpak/appstream/flathub/x86_64"
FIXTURE=
if [ -f "$AS/active/appstream.xml" ] && ./build/lord-appstream en x86_64 <"$AS/active/appstream.xml" >/dev/null 2>&1; then
    mkdir -p "$D/home/.local/share/flatpak/appstream/flathub"
    cp -a "$AS" "$D/home/.local/share/flatpak/appstream/flathub/" || exit 1
    echo "catalog: this machine's Flathub appstream"
else
    FIXTURE=tests/data/flathub/catalog-2026-11-22.json
    echo "catalog: the recorded one ($FIXTURE)"
fi
# the rankings: fhsvc.c's cache, {"at":N,"tried":N,"fails":N,"ids":[…]}, from the recorded answers
if [ "$LIVE" != live ]; then
    node -e '
const fs = require("fs"), [dir, out] = process.argv.slice(1);
const api = (f) => JSON.parse(fs.readFileSync(`${dir}/${f}.json`, "utf8"));
const lists = {
  popular: api("popular").hits.map((x) => x.app_id), trending: api("trending").hits.map((x) => x.app_id),
  added: api("recently-added").hits.map((x) => x.app_id), updated: api("recently-updated").hits.map((x) => x.app_id),
  day: [api("app-of-the-day").app_id], week: api("apps-of-the-week").apps.sort((a, b) => a.position - b.position).map((x) => x.app_id),
};
const now = Math.floor(Date.now() / 1000);
fs.mkdirSync(out, { recursive: true, mode: 0o700 });
for (const [k, ids] of Object.entries(lists))
  fs.writeFileSync(`${out}/${k}.json`, JSON.stringify({ at: now, tried: now, fails: 0, ids: [...new Set(ids)].slice(0, 250) }) + "\n", { mode: 0o600 });
' tests/data/flathub/api-2026-11-22 "$D/home/.cache/lordos/flathub" || exit 1
fi
{ [ -n "$FIXTURE" ] && printf 'globalThis.STORE_FIXTURE = %s;\n' "$(cat "$FIXTURE")"
  [ "$LIVE" = live ] && printf 'globalThis.STORE_LIVE = true;\n'
  cat tests/store_scene/pre.js "$ROOT/themes/$THEME/theme.bundle.js" tests/store_scene/driver.js; } >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zstore/$THEME/theme.bundle.js" || exit 1
chmod 700 "$D"
# Flathub on, its appstream fresh (no `flatpak update --appstream` in the scene)
printf '{"schema": 9, "desktopChosen": true, "language": "%s", "flathub": {"on": true, "appstreamAt": %s000}}\n' "$LANGID" "$(date +%s)" >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless "$SIZE" --theme "$T/theme.bundle.js" --timeout 240 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 400); do grep -aq 'STORE-SCENE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
sleep 2
grep -ah 'STORE-SCENE' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*STORE-SCENE/ /'
if [ "$LIVE" = live ]; then
    for f in "$D"/home/.cache/lordos/flathub/*.json; do [ -f "$f" ] && echo "cache $(basename "$f"): $(head -c 160 "$f")"; done
fi
for k in $(seq 1 9); do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/$THEME-$LANGID${LIVE:+-live}-$k.png"; done
ls "$OUT"/"$THEME-$LANGID${LIVE:+-live}"-*.png 2>/dev/null
