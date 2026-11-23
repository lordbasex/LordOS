#!/bin/sh
# moddrag_headless.sh — Settings → Appearance → Window options → Move and resize with (GitHub #219, docs/WINDOWS.md
# "Modifier drag") against a headless build/lord-server-test with --test-hooks (src/testhooks.h: the pointer, the buttons
# and the modifier keys) on the CPU path, in a private runtime folder and home, twice:
#   1. Off, the default: build/moddrag-client off — ❖ + drag, ❖ + right click, Alt + click are the app's, nothing moves
#      (lord-server takes no press at all);
#   2. ❖ chosen: build/moddrag-client (a Wayland app) and, where Xwayland and libxcb are installed, build/moddrag-x11 (an
#      X11 app) — ❖ + drag moves, ❖ + right drag resizes, the app gets neither press; a locked pointer or held shortcuts
#      keep ❖ + click the app's.
# The theme is Lord Flow's bundle with one hook that allows the keyboard shortcuts prompt. Never against a live session.
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build; build/lord-server-test), 1 on a failure or if lord-server died. MODDRAG_DEBUG=1 shows the logs.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server-test ] && [ -x build/moddrag-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server-test, build/moddrag-client and $THEME"; exit 77; }
P='' D=''
stop() { [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; [ -n "$D" ] && rm -rf "$D"; P='' D=''; }
trap stop EXIT

# start MODIFIER: a fresh lord-server whose person chose MODIFIER ('' for nothing saved: the default); sets P, D and S
start() {
    D=$(mktemp -d "${TMPDIR:-/tmp}/lmd.XXXXXX") || exit 1
    mkdir -p "$D/home/.config/lordos"
    chmod 700 "$D"
    mod=''
    [ -n "$1" ] && mod=", \"windowDragModifier\": \"$1\""
    printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true%s}\n' "$mod" >"$D/home/.config/lordos/settings.json"
    cp "$THEME" "$D/theme.js"
    cat >>"$D/theme.js" <<'EOF'
;(() => {
  const orig = globalThis.__lordWayland;
  globalThis.__lordWayland = (id, type, d) => {
    if (type === 'shortcuts') { setTimeout(() => { try { lord.wayland.shortcutsAnswer(d.app, true); } catch (e) {} }, 100); return undefined; }
    return orig(id, type, d);
  };
})();
EOF
    env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
        XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LANG=en_US.UTF-8 \
        ./build/lord-server-test --headless 1280x800 --theme "$D/theme.js" --timeout 120 --user "$(id -un)" --test-hooks >"$D/server.log" 2>&1 &
    P=$!
    i=0 S=''
    while [ $i -lt 120 ] && [ -z "$S" ]; do
        sleep 0.25
        S=$(ls "$D" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
        i=$((i + 1))
    done
    [ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/server.log"; exit 1; }
    sleep 2.5                               # the theme is up
}
# alive: lord-server survived (0), else its log and 1
alive() {
    sleep 0.5
    [ -n "$MODDRAG_DEBUG" ] && { tail -60 "$D/server.log"; tail -40 "$D/home/.cache/lordos/shell.log" 2>/dev/null; }
    kill -0 $P 2>/dev/null && return 0
    echo "lord-server is gone"; tail -20 "$D/server.log"; return 1
}

R=0
echo "== Off (the default: nothing saved)"
start ''
XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/moddrag-client off
r=$?
[ $r -eq 77 ] && exit 77
[ $r -eq 0 ] || R=1
alive || R=1
stop

echo "== ❖ chosen"
start super
XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ${MODDRAG_DEBUG:+env WAYLAND_DEBUG=client} ./build/moddrag-client || R=1
if [ -x build/moddrag-x11 ] && command -v Xwayland >/dev/null 2>&1; then
    X=$(cat "$D/server.log" "$D/home/.cache/lordos/compositor.log" 2>/dev/null | sed -n 's/.*xwayland: DISPLAY=\(:[0-9]*\).*/\1/p' | head -1)
    if [ -n "$X" ]; then
        env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$D" DISPLAY="$X" XAUTHORITY="$D/lord-xauth.${X#:}" ./build/moddrag-x11 || R=1
    else echo "X11 part skipped: lord-server opened no X socket"; fi
else echo "X11 part skipped: needs build/moddrag-x11 (libxcb1-dev) and Xwayland"; fi
alive || R=1
exit $R
