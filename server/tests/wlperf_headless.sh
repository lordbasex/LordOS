#!/bin/sh
# wlperf_headless.sh — build/wlperf-client (tests/wlperf_client.c) against a headless lord-server on the CPU path, in a
# private runtime folder and home (docs/WAYLAND.md, "Frames, titles and damage"). The default theme runs with a few
# lines after it that log the Linux windows' events the theme gets ("WLPERF <type> <data>"), so the checks count them.
#   titles     a title set 1000 times again gives no event; a burst of 1000 gives a few (at most 10), the last one last
#   frames     a client drawing on its frame callbacks gets about the display's rate (50–75 a second here)
#   protocols  the versions and events of the xdg-shell polish and the new globals; a single-pixel buffer at 50 %
#   transform  a 200×100 buffer turned 90° maps as a 100×200 window
#   idle       ext-idle-notify: the first notification asks ("idle" permission; the theme here answers WLPERF_IDLE,
#              allow by default), an allowed one is told idled
#   icon       xdg-toplevel-icon: the kept buffer, re-encoded to straight RGBA, reaches the theme; a bad one is an error
#   fixes      wl_fixes v1: destroy_registry destroys a registry (its id comes back), not-a-registry is an error
#              (tests/wlfixes_client.c, build/wlfixes-client)
#   subsurface desync subsurfaces (GTK 3's menus in Firefox) shown, redrawn, destroyed, made elsewhere and moved: every
#              frame painted then equals a full repaint (a closed menu used to stay on screen, a new one not to show)
#   damage     a client damaging a 10×10 square: every frame painted with only that damage equals a full repaint
#              (LORD_PAINT_VERIFY=1, the default here; WLPERF_VERIFY=0 turns it off)
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build), 1 on a failure or if lord-server didn't survive. WLPERF_ONLY=titles,frames runs some of them.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/wlperf-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/wlperf-client and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lwp.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
# outside the repository: a development build runs a theme there unchecked
{ cat "$THEME"; printf ';globalThis.WLPERF_IDLE = %s;\n' "'${WLPERF_IDLE:-allow}'"; cat <<'EOF'
;(() => {
  const orig = globalThis.__lordWayland;
  globalThis.__lordWayland = (id, type, d) => {
    // the "idle" prompt (ext-idle-notify): logged, and answered as WLPERF_IDLE says (allow, deny, or left open)
    if (type === 'idlenotify') {
      console.log('WLPERF idlenotify ' + JSON.stringify(d.app));
      if (globalThis.WLPERF_IDLE === 'allow' || globalThis.WLPERF_IDLE === 'deny') setTimeout(() => lord.wayland.idleAnswer(d.app, globalThis.WLPERF_IDLE === 'allow'), 3500);   // after the first client's 3 s
      return undefined;
    }
    if (type === 'icon') { const px = lord.wayland.icon(id); console.log('WLPERF icon ' + JSON.stringify({ size: d.size, width: px ? px.width : 0, bytes: px ? px.data.byteLength : 0, rgba: px ? [...new Uint8Array(px.data, 0, 4)] : [] })); }
    if (type === 'title' || type === 'map') console.log('WLPERF ' + type + ' ' + JSON.stringify(type === 'map' ? { title: d.title, width: d.width, height: d.height } : d.title));
    return orig(id, type, d);
  };
})();
EOF
} >"$D/theme.js"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 LORD_PAINT_VERIFY=${WLPERF_VERIFY:-1} \
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
want() { [ -z "$WLPERF_ONLY" ] || printf ',%s,' "$WLPERF_ONLY" | grep -q ",$1,"; }
client() { XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/wlperf-client "$@"; }
events() { grep -a "WLPERF $1" "$LOG" "$D/server.log" 2>/dev/null | sed 's/.*WLPERF //'; }

if want titles; then
    client titles || fail "titles: the client"
    sleep 0.5
    a=$(events title | grep -c '^title "A"$')
    b=$(events title | grep -c '^title "b')
    last=$(events title | grep '^title "b' | tail -1)
    echo "titles: $a events for 1000 repeats of the title it had, $b for a burst of 1000 ($last last)"
    [ "$a" -eq 0 ] || fail "titles: a title set again gave $a events"
    [ "$b" -ge 1 ] && [ "$b" -le 10 ] || fail "titles: a burst of 1000 gave $b events"
    [ "$last" = 'title "b999"' ] || fail "titles: the burst's last title is $last"
    events title | grep -q '^title "end"$' || fail "titles: the last title never came"
fi
if want frames; then
    out=$(client frames 3) || fail "frames: the client"
    echo "$out" | tail -1
    fps=$(echo "$out" | tail -1 | sed -n 's/.*= \([0-9]*\)\.[0-9] per second.*/\1/p')
    [ -n "$fps" ] && [ "$fps" -ge 45 ] && [ "$fps" -le 75 ] || fail "frames: $fps frame callbacks a second"
fi
if want protocols; then
    client protocols || fail "protocols"
fi
if want transform; then
    client transform || fail "transform: the client"
    sleep 0.3
    m=$(events map | grep '"title":"transform"' | tail -1)
    echo "transform: $m"
    echo "$m" | grep -q '"width":100,"height":200' || fail "transform: a 200x100 buffer at 90 degrees maps as $m"
fi
if want idle; then
    # the first notification asks (no "idled" before the answer); the theme here allows it, so a second run is told
    out=$(client idle) || fail "idle: the client"
    echo "idle (first, asked): $out"
    echo "$out" | grep -q '^idle: 0 idled' || fail "idle: told before the person answered"
    sleep 1
    events idlenotify | grep -q 'program:' || fail "idle: no prompt with the program lord-server checked"
    out=$(client idle) || fail "idle: the client"
    echo "idle (allowed): $out"
    echo "$out" | grep -q '^idle: [1-9]' || fail "idle: allowed, but never idled"
    p=$(events idlenotify | wc -l)
    [ "$p" -eq 1 ] || fail "idle: $p prompts for one app"
fi
if want icon; then
    client icon || fail "icon: the client"
    sleep 0.3
    i=$(events icon | tail -1)
    echo "icon: $i"
    # the 64 px buffer (closest to 128 of 64 and 48; 300 is past the limit), as RGBA: XRGB8888 0x80ff0000 is opaque red
    echo "$i" | grep -q '"size":64,"width":64,"bytes":16384,"rgba":\[255,0,0,255\]' || fail "icon: $i"
fi
if want fixes; then
    out=$(XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/wlfixes-client) || fail "fixes: the client"
    echo "$out"
fi
pv() { grep -ah 'paint-verify' "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null; }
if want subsurface; then
    v0=$(pv | wc -l)
    client subsurface || fail "subsurface: the client"
    sleep 0.3
    v=$(($(pv | wc -l) - v0))
    bad=$(pv | tail -n "$v" | grep -vc ': 0 pixels differ')
    echo "subsurface: $v frames checked against a full repaint, $bad differ"
    [ "$v" -ge 8 ] && [ "$bad" -eq 0 ] || fail "subsurface: $bad of $v frames differ from a full repaint"
fi
if want damage; then
    client damage 30 || fail "damage: the client"
    sleep 0.3
    # LORD_PAINT_VERIFY=1: every frame painted with only the damage is compared with a full repaint
    v=$(grep -ah 'paint-verify' "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null | wc -l)
    bad=$(grep -ah 'paint-verify' "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null | grep -vc ': 0 pixels differ')
    echo "damage: $v frames checked against a full repaint, $bad differ"
    [ "$v" -gt 0 ] && [ "$bad" -eq 0 ] || fail "damage: $bad of $v frames differ from a full repaint"
fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; R=1; }
exit $R
