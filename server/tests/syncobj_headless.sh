#!/bin/sh
# syncobj_headless.sh — build/syncobj-client (tests/syncobj_client.c: explicit sync, linux-drm-syncobj-v1) against a
# headless lord-server on the CPU path, in a private runtime folder and home (docs/SYNCOBJ-PLAN.md, docs/WAYLAND.md).
# The timelines and buffers are the kernel's (a render node with timeline syncobjs, /dev/udmabuf): no GPU is needed.
# LORD_GPU=1 is the same against a session on a monitor (the GPU path); SYNCOBJ_ONLY="wait order" runs some checks.
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build; a render node with timelines; /dev/udmabuf), 1 on a failure or if lord-server didn't survive.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/syncobj-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/syncobj-client and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lso.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos"
chmod 700 "$D"
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
    ./build/lord-server --headless 1280x800 --theme "$THEME" --timeout 90 >"$D/server.log" 2>&1 &
P=$!
i=0 S=''
while [ $i -lt 120 ] && [ -z "$S" ]; do
    sleep 0.25
    S=$(ls "$D" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    i=$((i + 1))
done
[ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; exit 1; }
sleep 2                                     # the theme is up
# shellcheck disable=SC2086
XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/syncobj-client $SYNCOBJ_ONLY
R=$?
grep -ah "explicit sync\|a frame waits" "$D/server.log" "$D/home/.cache/lordos/compositor.log" 2>/dev/null | sed 's/^/lord-server: /' | sort -u
sleep 0.5
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; R=1; }
exit $R
