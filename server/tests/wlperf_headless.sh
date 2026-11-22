#!/bin/sh
# wlperf_headless.sh — build/wlperf-client (tests/wlperf_client.c) against a headless lord-server on the CPU path, in a
# private runtime folder and home (docs/WAYLAND.md, "Frames, titles and damage"). The default theme runs with a few
# lines after it that log the Linux windows' events the theme gets ("WLPERF <type> <data>"), so the checks count them.
#   titles     a title set 1000 times again gives no event; a burst of 1000 gives a few, the last one last
#   frames     a client drawing on its frame callbacks gets about the display's rate (50–75 a second here)
#   protocols  the versions and events of the xdg-shell polish and the new globals; a single-pixel buffer at 50 %
#   transform  a 200×100 buffer turned 90° maps as a 100×200 window
#   damage     a client damaging a 10×10 square: lord-server stays up and the frames go on
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build), 1 on a failure or if lord-server didn't survive. WLPERF_ONLY=titles,frames runs some of them.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/wlperf-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/wlperf-client and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lwp.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
# outside the repository: a development build runs a theme there unchecked
{ cat "$THEME"; cat <<'EOF'
;(() => {
  const orig = globalThis.__lordWayland;
  globalThis.__lordWayland = (id, type, d) => {
    if (type === 'title' || type === 'map') console.log('WLPERF ' + type + ' ' + JSON.stringify(type === 'map' ? { title: d.title, width: d.width, height: d.height } : d.title));
    return orig(id, type, d);
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
    [ "$b" -ge 1 ] && [ "$b" -le 3 ] || fail "titles: a burst of 1000 gave $b events"
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
if want damage; then
    client damage 30 || fail "damage: the client"
fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; R=1; }
exit $R
