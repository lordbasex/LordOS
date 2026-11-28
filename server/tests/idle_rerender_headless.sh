#!/bin/sh
# idle_rerender_headless.sh — a quiet desktop stays quiet, in every desktop and after switching between them (BUG-9 of
# the 0.8.9.6.2 QA: lord-server at ~69 % CPU with nothing happening, `__lordTick [shell; react made 0, changed ~4300]`
# every second: a terminal's spinning title, Claude Code's "◐ ◓ ◑ ◒", drew the whole desktop again each time: every
# window, every desktop widget, every bar item and Dock icon). A headless lord-server on the CPU path runs the desktops
# built by tests/idle_rerender_scene.mjs (a probe in the React host and a driver): five apps and five desktop widgets,
# a quiet phase, a phase with one window's title changing four times a second, then the next desktop, round
# Flow → Classic → Celeste → Luna → Workspaces → Flow. For each desktop:
#   - one title change draws at most SPIN_MAX nodes again beyond what the quiet phase drew (the widgets' own clocks and
#     graphs): ~700 before the fix on this one display, now the window itself, its title and the Dock's list;
# and after the five switches, Flow's quiet phase changes no more nodes than at the start (nothing left running by the
# desktops before it). Run from server/ (tests/run_suites.sh does). Exit 77 without build/lord-server or node and
# esbuild (npm install), 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] || { echo "needs build/lord-server"; exit 77; }
command -v node >/dev/null 2>&1 || { echo "needs node"; exit 77; }
SPIN_MAX=${SPIN_MAX:-120}
D=$(mktemp -d "${TMPDIR:-/tmp}/lidle.XXXXXX") || exit 1
P=
trap 'kill $P 2>/dev/null; rm -rf "$D"' EXIT
chmod 700 "$D"
node tests/idle_rerender_scene.mjs "$D/themes" >"$D/scene.log" 2>&1 || { cat "$D/scene.log"; echo "needs esbuild (npm install)"; exit 77; }
mkdir -p "$D/home/.config/lordos"
W='"size": "m", "desk": 1, "opts": {"pin": null}'
cat >"$D/home/.config/lordos/settings.json" <<EOF
{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en", "theme": "default",
 "deskWidgets": [
  {"id": "clock-t1", "type": "clock", "col": 6, "row": 0, "size": "m", "desk": 1, "opts": {"pin": null, "style": "analog"}},
  {"id": "calendar-t2", "type": "calendar", "col": 6, "row": 1, $W},
  {"id": "network-t3", "type": "network", "col": 6, "row": 2, $W},
  {"id": "weather-t4", "type": "weather", "col": 8, "row": 0, "size": "s", "desk": 1, "opts": {"pin": null, "city": null}},
  {"id": "devices-t5", "type": "devices", "col": 8, "row": 1, "size": "s", "desk": 1, "opts": {"pin": null}}]}
EOF
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless 1920x1080 --theme "$D/themes/default/theme.bundle.js" --timeout 240 >"$D/server.log" 2>&1 &
P=$!
LOG="$D/home/.cache/lordos/shell.log"
i=0
while [ $i -lt 480 ] && kill -0 $P 2>/dev/null && ! grep -q "PROBE done" "$LOG" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
kill $P 2>/dev/null
wait $P 2>/dev/null
P=
R=0
grep -q "PROBE done" "$LOG" 2>/dev/null || { echo "FAIL the round did not finish"; tail -30 "$LOG" "$D/server.log" 2>/dev/null; exit 1; }
grep -h "PROBE" "$LOG" | sed 's/.*PROBE /  /'
# one line per desktop: step theme idle spun titles
grep -h "PROBE idle" "$LOG" | sed 's/.*step=\([0-9]*\) theme=\([a-z]*\) idle=\([0-9]*\) spun=\([0-9]*\) titles=\([0-9]*\) windows=\([0-9-]*\).*/\1 \2 \3 \4 \5 \6/' >"$D/steps"
n=$(grep -c . "$D/steps")
[ "$n" -eq 6 ] && echo "ok   six desktops, one per JS context" || { echo "FAIL $n desktops instead of 6"; R=1; }
while read -r step th idle spun titles wins; do
    [ "$titles" -gt 0 ] || { echo "FAIL $th: the title never changed"; R=1; continue; }
    per=$(( (spun - idle) / titles ))
    if [ "$per" -le "$SPIN_MAX" ]; then echo "ok   $th (step $step): $per nodes drawn again per title change (at most $SPIN_MAX; $wins windows)"
    else echo "FAIL $th (step $step): $per nodes drawn again per title change (more than $SPIN_MAX)"; R=1; fi
done <"$D/steps"
first=$(sed -n 1p "$D/steps" | cut -d' ' -f3)
last=$(sed -n '$p' "$D/steps" | cut -d' ' -f3)
if [ "$last" -le $(( first * 3 / 2 + 60 )) ]; then echo "ok   Flow after five switches is as quiet as at the start ($first then $last nodes in the quiet phase)"
else echo "FAIL Flow after five switches changes $last nodes in the quiet phase, $first at the start"; R=1; fi
if grep -q -e "crashed:" -e "recovery screen" "$LOG" "$D/home/.cache/lordos/compositor.log" "$D/server.log" 2>/dev/null; then echo "FAIL a crash or the recovery screen"; R=1; fi
exit $R
