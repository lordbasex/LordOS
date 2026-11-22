#!/bin/sh
# wltiming_headless.sh — build/wltiming-client (tests/wltiming_client.c) against a headless lord-server on the CPU path,
# in a private runtime folder and home (docs/WAYLAND.md, "Frame timing"):
#   globals   wp_fifo_manager_v1, wp_commit_timing_manager_v1, wp_tearing_control_manager_v1, wp_content_type_manager_v1
#   fifo      Mesa's FIFO present for 3 s: about the display's rate (45–75 a second here), no update shown in the same
#             refresh as the one before, none left waiting; the same without fifo must share refreshes (the check works)
#   timing    targets 17–400 ms ahead: none presented before its target, none more than two refreshes after it
#   errors    each protocol error the four protocols define
#   destroy   a surface destroyed with updates held; nomap: a surface never on screen never stalls; hints: no error
#   vkcube    if vkcube and Mesa's lavapipe are installed: vkcube in FIFO mode binds fifo and commit timing, runs at about
#             the display's rate (WAYLAND_DEBUG kept in the log); skipped otherwise
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build; THEME=… names another), 1 on a failure or if lord-server didn't survive.
cd "$(dirname "$0")/.." || exit 1
THEME=${THEME:-../themes/default/theme.bundle.js}
[ -x build/lord-server ] && [ -x build/wltiming-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/wltiming-client and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lwt.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
cp "$THEME" "$D/theme.js"                   # outside the repository: a development build runs a theme there unchecked
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless 1280x800 --theme "$D/theme.js" --timeout 180 >"$D/server.log" 2>&1 &
P=$!
i=0 S=''
while [ $i -lt 120 ] && [ -z "$S" ]; do
    sleep 0.25
    S=$(ls "$D" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    i=$((i + 1))
done
[ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/server.log"; exit 1; }
sleep 2                                     # the theme is up
R=0
fail() { echo "FAIL $*"; R=1; }
want() { [ -z "$WLT_ONLY" ] || printf ',%s,' "$WLT_ONLY" | grep -q ",$1,"; }
client() { XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/wltiming-client "$@"; }

if want globals; then client globals || fail "globals"; fi
if want fifo; then
    out=$(client fifo 3) || fail "fifo: the client"
    echo "$out"
    rate=$(echo "$out" | sed -n 's/.*= \([0-9]*\)\.[0-9] per second.*/\1/p')
    [ -n "$rate" ] && [ "$rate" -ge 45 ] && [ "$rate" -le 75 ] || fail "fifo: $rate commits a second"
    off=$(client fifo 2 off) || fail "fifo off: the client"
    echo "$off"
    shared=$(echo "$off" | sed -n 's/.*; \([0-9]*\) presented in the same refresh.*/\1/p')
    [ -n "$shared" ] && [ "$shared" -gt 0 ] || fail "fifo off: no update shared a refresh (the check can't fail)"
fi
if want timing; then client timing || fail "timing"; fi
if want errors; then
    for k in fifo-twice timer-twice tearing-twice ctype-twice timestamp-twice bad-nsec fifo-destroyed timer-destroyed; do
        client errors $k || fail "errors $k"
    done
fi
if want destroy; then client destroy || fail "destroy"; fi
if want nomap; then client nomap || fail "nomap"; fi
if want hints; then client hints || fail "hints"; fi
if want vkcube; then
    if command -v vkcube >/dev/null 2>&1 && ls /usr/share/vulkan/icd.d/lvp_icd*.json >/dev/null 2>&1; then
        icd=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | grep -v i686 | head -1)
        XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" VK_ICD_FILENAMES="$icd" WAYLAND_DEBUG=1 \
            timeout 4 vkcube --wsi wayland --present_mode 2 >"$D/vkcube.out" 2>"$D/vkcube.log"
        binds=$(grep -c 'wl_registry#[0-9]*\.bind([0-9]*, "wp_\(fifo_manager\|commit_timing_manager\)_v1"' "$D/vkcube.log")
        barriers=$(grep -c 'wp_fifo_v1#[0-9]*\.wait_barrier()' "$D/vkcube.log")
        stamps=$(grep -c 'wp_commit_timer_v1#[0-9]*\.set_timestamp(' "$D/vkcube.log")
        # the frames shown: the presentation feedback's times (s, ns), their rate and the gaps over 3 refreshes
        stat=$(grep 'wp_presentation_feedback#[0-9]*\.presented(' "$D/vkcube.log" |
            sed 's/.*presented([0-9]*, \([0-9]*\), \([0-9]*\),.*/\1 \2/' |
            LC_ALL=C awk '{ t = $1 + $2 / 1e9; if (n == 0) a = t; if (n && t - p > 0.05) g++; p = t; n++ }
                END { d = p - a; r = 0; if (d > 0) r = (n - 1) / d; printf "%d %d %d", n, r, g }')
        set -- $stat
        echo "vkcube (FIFO, lavapipe): binds $binds of fifo/commit-timing, $barriers wait_barrier, $stamps set_timestamp; $1 frames presented at $2 a second, $3 gaps over 3 refreshes"
        [ "$binds" -ge 2 ] || fail "vkcube: did not bind fifo and commit timing"
        [ "$barriers" -ge 10 ] || fail "vkcube: no FIFO barriers"
        [ "$2" -ge 45 ] && [ "$2" -le 75 ] || fail "vkcube: $2 frames a second"
        [ "$3" -le 2 ] || fail "vkcube: $3 stalls"
        [ -n "$WLT_VKLOG" ] && cp "$D/vkcube.log" "$WLT_VKLOG"
    else echo "vkcube: skipped: needs vkcube and Mesa's lavapipe (mesa-vulkan-drivers)"; fi
fi
kill -0 $P 2>/dev/null || { fail "lord-server died"; tail -30 "$D/server.log"; }
[ $R = 0 ] && echo "wltiming: ok"
exit $R
