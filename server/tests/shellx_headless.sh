#!/bin/sh
# shellx_headless.sh — build/shellx-client (tests/shellx_client.c) against a headless lord-server on the CPU path, in a
# private runtime folder and home: the shell extras (src/wl/wlshell.h, docs/WAYLAND.md "Shell extras"). The default
# theme runs with a few lines after it that log the Linux windows' events it gets ("SHELLX <type> <data>").
#   globals   the five globals at version 1, blur offered (CPU path, effects on)
#   tag       the tag set before the first frame comes with the window; a later one as a "tag" event, cleaned
#   bell      a burst of ten rings is one, a ring with no window goes to window 0
#   blur      the region as rects in the window's coordinates; a second object is background_effect_exists
#   session   a new session is saved with its window; a saved one restores its window at its place (the shell places
#             it there and the next save keeps it); another program's session id finds nothing; in_use; already_mapped
#   drag      (SHELLX_DRAG=1, needs root to drive the pointer through the session's remote.sock, as the e2e bench)
#             a tab torn off follows the pointer, is never the drag's target, and stays where it was let go
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing (the theme bundle: npm run
# build), 1 on a failure or if lord-server didn't survive. SHELLX_ONLY=tag,bell runs some of them.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -x build/lord-server ] && [ -x build/shellx-client ] && [ -f "$THEME" ] || { echo "needs build/lord-server, build/shellx-client and $THEME"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lsx.XXXXXX") || exit 1
trap 'kill $P 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home/.config/lordos/wl-sessions"
chmod 700 "$D" "$D/home/.config/lordos/wl-sessions"
printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
# sessions saved before: one of this client (by the program lord-server checks it runs) and one of another program
ME="program:$(realpath build/shellx-client)"
A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa B=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
{
    echo "lordos-wl-sessions 1"
    echo "S $A 1 $ME"
    echo "T 333 222 400 300 0 main"
    echo "T 100 100 400 300 0 drag-main"
    echo "S $B 1 program:/usr/bin/another-app"
    echo "T 50 50 300 200 0 main"
} >"$D/home/.config/lordos/wl-sessions/sessions"
chmod 600 "$D/home/.config/lordos/wl-sessions/sessions"
{ cat "$THEME"; cat <<'EOF'
;(() => {
  const orig = globalThis.__lordWayland;
  globalThis.__lordWayland = (id, type, d) => {
    if (type === 'map') console.log('SHELLX map ' + JSON.stringify({ id, title: d.title, tag: lord.wayland.tag(id) }));
    else if (['tag', 'bell', 'blur', 'tldrag'].includes(type)) console.log('SHELLX ' + type + ' ' + JSON.stringify({ id, ...d }));
    return orig(id, type, d);
  };
})();
EOF
} >"$D/theme.js"
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
[ -n "$S" ] || { echo "lord-server made no Wayland socket"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; exit 1; }
sleep 2                                     # the theme is up
LOG="$D/home/.cache/lordos/shell.log"
FILE="$D/home/.config/lordos/wl-sessions/sessions"
R=0
fail() { echo "FAIL $*"; R=1; }
want() { [ -z "$SHELLX_ONLY" ] || printf ',%s,' "$SHELLX_ONLY" | grep -q ",$1,"; }
client() { XDG_RUNTIME_DIR="$D" WAYLAND_DISPLAY="$S" ./build/shellx-client "$@"; }
events() { grep -a "SHELLX $1 " "$LOG" "$D/server.log" 2>/dev/null | sed 's/.*SHELLX [a-z]* //'; }

if want globals; then
    out=$(client globals) || fail "globals: $out"
    echo "$out" | sed 's/^/globals: /'
    echo "$out" | grep -q 'caps 1$' || fail "globals: blur not offered on the CPU path"
fi
if want tag; then
    client tag || fail "tag: the client"
    sleep 0.3
    m=$(events map | grep '"title":"tagged"' | tail -1)
    t=$(events tag | tail -1)
    echo "tag: map $m"
    echo "tag: later $t"
    echo "$m" | grep -q '"tag":{"tag":"main window","description":"Main window"}' || fail "tag: the window came without its tag: $m"
    echo "$t" | grep -q '"tag":"preferences","description":"Main window"' || fail "tag: the later tag: $t"
fi
if want bell; then
    before=$(events bell | wc -l)
    client bell || fail "bell: the client"
    sleep 0.3
    win=$(events bell | tail -n +$((before + 1)) | grep -vc '"id":0')
    none=$(events bell | tail -n +$((before + 1)) | grep -c '"id":0')
    echo "bell: $win on the window for a burst of 10, $none with no window"
    [ "$win" -eq 1 ] || fail "bell: a burst of 10 rang $win times"
    [ "$none" -eq 1 ] || fail "bell: a ring with no window rang $none times"
fi
if want blur; then
    client blur || fail "blur: the client"
    sleep 0.3
    b=$(events blur | grep -v '"rects":\[\]' | tail -1)
    echo "blur: $b"
    echo "$b" | grep -q '"rects":\[\[200,0,20,20\],\[10,10,100,50\]\]' || fail "blur: $b"
fi
if want session; then
    out=$(client session-new) || fail "session: a new one: $out"
    id=$(echo "$out" | sed -n 's/^id //p')
    echo "session: new $id"
    sleep 0.5
    grep -q "^S $id [0-9]* $ME\$" "$FILE" || fail "session: the new session isn't saved"
    awk -v id="$id" '$1=="S"{on=$2==id} on&&$1=="T"&&$7=="main"' "$FILE" | grep -q . || fail "session: its window isn't saved"
    out=$(client session $A main inuse) || fail "session: restore: $out"
    echo "$out" | sed 's/^/session: restore: /'
    echo "$out" | grep -q '^created 0 restored 1' || fail "session: the saved session wasn't restored"
    echo "$out" | grep -q '^toplevel restored 1 (before its first configure: 1), size 400x300' || fail "session: the window wasn't restored (or not before its first configure)"
    sleep 0.5
    t=$(awk -v id="$A" '$1=="S"{on=$2==id} on&&$1=="T"&&$7=="main"{print $2,$3,$4,$5}' "$FILE")
    echo "session: its place after: $t"
    [ "$t" = "333 222 400 300" ] || fail "session: restored at $t, not 333 222 400 300"
    out=$(client session $B main) || fail "session: another app's: $out"
    echo "$out" | sed 's/^/session: other: /'
    echo "$out" | grep -q '^created 1 restored 0' || fail "session: another program's session id was found"
    echo "$out" | grep -q '^toplevel restored 0' || fail "session: another program's window was restored"
    grep -q "^S $B 1 program:/usr/bin/another-app\$" "$FILE" || fail "session: the other program's session changed"
    out=$(client session-late $A) || fail "session: already_mapped: $out"
    echo "$out" | sed 's/^/session: late: /'
    ls -l "$FILE" | grep -q '^-rw-------' || fail "session: the file isn't 0600"
fi
if [ -n "$SHELLX_DRAG" ] && want drag; then
    out=$(client drag-errors) || fail "drag: errors: $out"
    echo "$out" | sed 's/^/drag: errors: /'
    if sudo -n true 2>/dev/null; then
        client drag $A >"$D/drag.out" 2>&1 &
        C=$!
        i=0
        while [ $i -lt 40 ] && ! grep -q ready "$D/drag.out"; do sleep 0.25; i=$((i + 1)); done
        sleep 1
        # the main window is at 100,100 (400 x 300): pressed in it, dragged out, back over it, and let go far away
        sudo -n python3 -I "$PWD/tests/shellx_inject.py" "$D/lordos/remote.sock" move:200,150 down:200,150 move:260,180 move:500,400 \
            move:700,500 move:300,200 move:250,180 move:800,500 move:900,600 up:900,600 >"$D/inject.out" 2>&1 || fail "drag: the injector: $(cat "$D/inject.out")"
        wait $C || fail "drag: the client"
        sed 's/^/drag: /' "$D/drag.out"
        t=$(awk -v id="$A" '$1=="S"{on=$2==id} on&&$1=="T"&&$7=="torn"{print $2,$3,$4,$5}' "$FILE")
        echo "drag: the torn window was let go at $t"
        [ "${t% * *}" = "880 590" ] || fail "drag: the torn window is at $t, not under the pointer at (900, 600) less (20, 10)"
        events tldrag | sed 's/^/drag: event /'
    else
        echo "drag: skipped the pointer part: needs root (sudo -n) for the session's remote.sock"
    fi
fi
kill -0 $P 2>/dev/null || { echo "lord-server is gone"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; R=1; }
exit $R
