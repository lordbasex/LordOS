#!/bin/sh
# color_headless.sh — build/color-client (tests/color_client.c) against a headless lord-server on the CPU path, in a
# private runtime folder and home (docs/WAYLAND.md, "Colour management"):
#   protocols  what color-management-v1 and color-representation-v1 advertise, the output's and the preferred image
#              descriptions, parametric descriptions, and every protocol error
#   pixels     the same 200×100 pixels (0x80, 0x40, 0x20) shown as linear light and then with no description: the
#              theme reads the screen after each one maps and counts both colours. Linear light must be on screen as
#              (0xba, 0x88, 0x63), 255·(v/255)^(1/2.2), and the undescribed window as it was sent
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build), 1 on a failure or if lord-server didn't survive. COLOR_ONLY=protocols runs one of them.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/color-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/color-client and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lcm.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
# the theme's screenshots go through the Screenshots app's "screen" permission
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
# outside the repository: a development build runs a theme there unchecked
{ cat "$THEME"; cat <<'EOF'
;(() => {
  const orig = globalThis.__lordWayland;
  // the whole screen in 64×64 reads: how many pixels are exactly each colour
  const count = (want) => {
    const id = lord.media.screenshot('screenshots', {});
    const n = want.map(() => 0);
    for (let y = 0; y < 800; y += 64)
      for (let x = 0; x < 1280; x += 64) {
        const px = new Uint8Array(lord.media.pixels(id, x, y, 64, 64));
        for (let i = 0; i < px.length; i += 4)
          want.forEach((w, k) => { if (px[i] === w[0] && px[i + 1] === w[1] && px[i + 2] === w[2] && px[i + 3] === 255) n[k]++; });
      }
    return n;
  };
  globalThis.__lordWayland = (id, type, d) => {
    const r = orig(id, type, d);
    if (type === 'map' && /^color:(linear|plain)$/.test(d.title || '')) {
      setTimeout(() => {
        let n = [-1, -1];
        try { n = count([[0xba, 0x88, 0x63], [0x80, 0x40, 0x20]]); } catch (e) { console.log('WLPERF colorpx error ' + e); }
        console.log('WLPERF colorpx ' + JSON.stringify({ title: d.title, converted: n[0], raw: n[1] }));
      }, 1500);
    }
    return r;
  };
})();
EOF
} >"$D/theme.js"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless 1280x800 --theme "$D/theme.js" --timeout 120 >"$D/server.log" 2>&1 &
P=$!
i=0 S=''
while [ $i -lt 120 ] && [ -z "$S" ]; do
    sleep 0.25
    S=$(ls "$D" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    i=$((i + 1))
done
[ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; exit 1; }
sleep 2                                     # the theme is up
LOG="$D/home/.cache/lordos/shell.log"
R=0
fail() { echo "FAIL $*"; R=1; }
want() { [ -z "$COLOR_ONLY" ] || printf ',%s,' "$COLOR_ONLY" | grep -q ",$1,"; }
client() { XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/color-client "$@"; }
events() { grep -a "WLPERF $1" "$LOG" "$D/server.log" 2>/dev/null | sed 's/.*WLPERF //'; }

if want protocols; then
    client protocols || fail "protocols"
fi
if want pixels; then
    client show || fail "pixels: the client"
    sleep 0.5
    lin=$(events colorpx | grep '"color:linear"' | tail -1)
    pla=$(events colorpx | grep '"color:plain"' | tail -1)
    echo "pixels: linear light $lin"
    echo "pixels: no description $pla"
    # a 200×100 buffer: at least 90 % of it in the expected colour, none in the other
    c=$(echo "$lin" | sed -n 's/.*"converted":\([0-9-]*\).*/\1/p'); r=$(echo "$lin" | sed -n 's/.*"raw":\([0-9-]*\).*/\1/p')
    [ -n "$c" ] && [ "$c" -ge 18000 ] && [ "$r" -eq 0 ] || fail "pixels: linear light shows $c converted and $r unconverted pixels"
    c=$(echo "$pla" | sed -n 's/.*"converted":\([0-9-]*\).*/\1/p'); r=$(echo "$pla" | sed -n 's/.*"raw":\([0-9-]*\).*/\1/p')
    [ -n "$r" ] && [ "$r" -ge 18000 ] && [ "$c" -eq 0 ] || fail "pixels: no description shows $r pixels as sent and $c converted"
fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; R=1; }
exit $R
