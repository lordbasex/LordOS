#!/bin/sh
# community_theme_headless.sh — community themes in a headless lord-server (docs/THEMES-GUIDE.md, "Community themes";
# AGENTS.md "Community themes"). Each fixture of tests/community_themes/ is put into a private home's per-user themes
# folder the way an install does (community_theme_install.mjs, the same compiler), chosen in settings.json, and the
# session runs it from LordOS's default theme:
#   okminimal  the minimal example as a community theme: it starts in its own realm and draws (no watchdog), and
#              nothing it does through the SDK is refused; a session that ends normally is no crash, two killed under
#              it in a row (kill -9) keep it off the next one
#   quitter    breaks, then calls session.quit itself: its own logout never clears its crash count
#   probe      (installed unchecked) every way to reach past its policy: lord.term/fs/admin don't exist in its realm,
#              code from text gives nothing, LordOS's built-ins are frozen, and lord.setStyle on a node not its own is
#              refused directly, from Promise.then, queueMicrotask, setTimeout (bound and with arguments),
#              requestAnimationFrame, a store subscription, an effect and a ref (a bound restricted native handed to
#              LordOS's code each time: nodes 101…109), and from a theme store's get React reads later (110); on its own
#              node it works; forged elements (a host element, LordOS's raw component, a context's Provider holding a
#              host element; in children, another prop, a nested object, an iterable, a lazy element, a thenable or a store's
#              value) never hand it a raw node; a forged app registry is never
#              used; accessors LordOS's code reads (a getter, a Proxy's traps, a custom iterator, a toStringTag
#              accessor; theme functions and bound natives, nodes 111-117) are refused when they run; wm has no setArgs, setProtected, setMenus, setPip, scriptMove or setCloseGuard
#   forger     its own component returns a forged host element: the host guard refuses it (its element wasn't made by
#              LordOS's React), the theme is taken off
#   throws     throws while it starts: the session goes back to LordOS's theme, it is marked failed with the error
#   nostart    never calls startShell: the same
#   untrusted  okminimal not confirmed by the person: never run
#   clicker    (build/lord-server-test with its hooks) a click on its own View, and a timer, mount new nodes: React's
#              render after the theme's setState is LordOS's (lord.hostMicrotask), never refused; the new rows draw each
#              app's Icon from useLauncherApps; a component imports '../tokens.js'; its shortcut table in the SDK's own
#              form (keys: [...]) runs, and an entry on a key every theme shares (❖Space) is dropped
# Exit 77 when something it needs is missing, 1 on a failure. Slow (each session loads two desktops on the CPU path):
# `make -C server test-full` runs it.
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -f "$ROOT/assets/theme-host.bundle.js" ] && [ -f "$ROOT/themes/default/theme.bundle.js" ] && command -v node >/dev/null ||
    { echo "skipped: needs build/lord-server, npm run build and node"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lct.XXXXXX") || exit 1
chmod 700 "$D"
P=
trap 'kill $P 2>/dev/null; rm -rf "$D"' EXIT
R=0

session() {    # $1 name, $2 fixture, $3 seconds, then install arguments
    name=$1 fix=$2 secs=$3
    shift 3
    S=$D/$name
    mkdir -p "$S/home/.config/lordos"
    chmod 700 "$S"
    node tests/community_theme_install.mjs "tests/community_themes/$fix" "$S/home" "$@" >/dev/null || { echo "FAIL $name: not installed"; R=1; return; }
    printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "theme": "%s"}\n' "$fix" >"$S/home/.config/lordos/settings.json"
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
        XDG_RUNTIME_DIR="$S" HOME="$S/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
        ./build/lord-server --headless 1024x768 --theme "$ROOT/themes/default/theme.bundle.js" --timeout "$secs" >"$S/server.log" 2>&1 &
    P=$!
    wait $P
    P=
    L=$(cat "$S/server.log" "$S/home/.cache/lordos/"*.log 2>/dev/null)
}
has() { if printf '%s\n' "$L" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1"; R=1; fi; }
hasnt() { if printf '%s\n' "$L" | grep -q -- "$2"; then echo "FAIL $1"; printf '%s\n' "$L" | grep -- "$2" | head -3; R=1; else echo "ok   $1"; fi; }
record() { cat "$S/home/.local/share/lordos/themes/$1/install.json" 2>/dev/null; }

session okminimal okminimal 40
has "okminimal starts in its own realm" "community theme okminimal started"
hasnt "okminimal: nothing it does through the SDK is refused" "community theme: lord\..* refused"
hasnt "okminimal: the watchdog leaves it on" "taken off\|the theme failed"
if record okminimal | grep -q '"failed":""'; then echo "ok   okminimal is not marked failed"; else echo "FAIL okminimal marked failed: $(record okminimal)"; R=1; fi

if record okminimal | grep -q '"crashes":0'; then echo "ok   okminimal: a session that ended normally is no crash"; else echo "FAIL okminimal crash count: $(record okminimal)"; R=1; fi

# sessions that end abnormally (lord-server killed while the theme runs) count; two in a row keep it off the third
crashrun() {
    rm -f "$S/home/.cache/lordos/"*.log
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
        XDG_RUNTIME_DIR="$S" HOME="$S/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
        ./build/lord-server --headless 1024x768 --theme "$ROOT/themes/default/theme.bundle.js" --timeout 60 >"$S/server.log" 2>&1 &
    P=$!
    n=0
    while [ $n -lt 600 ] && ! grep -q "community theme okminimal started" "$S/server.log" "$S/home/.cache/lordos/"*.log 2>/dev/null; do sleep 0.1; n=$((n + 1)); done
    kill -9 $P 2>/dev/null; wait $P 2>/dev/null
    P=
    rm -f "$S/home/.cache/lordos/"*.log
}
crashrun
if record okminimal | grep -q '"crashes":1'; then echo "ok   okminimal: a session killed under it counts once"; else echo "FAIL okminimal after one kill: $(record okminimal)"; R=1; fi
crashrun
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$S" HOME="$S/home" LORD_GPU=0 LANG=en_US.UTF-8 \
    ./build/lord-server --headless 1024x768 --theme "$ROOT/themes/default/theme.bundle.js" --timeout 20 >"$S/server.log" 2>&1
L=$(cat "$S/server.log" "$S/home/.cache/lordos/"*.log 2>/dev/null)
hasnt "okminimal: not started the third time" "community theme okminimal started"
if record okminimal | grep -q 'ended 2 times in a row'; then echo "ok   okminimal: two killed sessions in a row keep it off the next one"; else echo "FAIL okminimal after two kills: $(record okminimal)"; R=1; fi

session quitter quitter 40 --unchecked
has "quitter: it logged out by itself" "CT quitter logs out"
if record quitter | grep -q '"crashes":1'; then echo "ok   quitter: its own logout doesn't clear its crash count"; else echo "FAIL quitter: $(record quitter)"; R=1; fi

session probe probe 40 --unchecked
has "probe starts" "community theme probe started"
hasnt "probe: nothing it tried was allowed" "CT .* ALLOWED"
for c in direct settings-in-place "facade element props changed" term.create fs.write own-realm-function host-function-constructor host-proto-change promise; do has "probe: $c refused" "CT $c refused"; done
has "probe: lord.term, lord.fs, lord.admin and lord.perms don't exist in its realm" "CT lord.term undefined lord.fs undefined lord.admin undefined globalThis.lord.perms undefined"
for n in 101 102 103 104 105 106 107 108 109; do has "probe: setStyle on node $n refused" "lord.setStyle refused (node $n is not"; done
has "probe: its timers and animation frames ran" "CT raf ran"
has "probe: its own node can be restyled" "CT own-node works"
has "probe: lord.setStyle from a store's get React reads later refused" "lord.setStyle refused (node 110 is not"
hasnt "probe: no forged element (host, raw component, Provider; in children or a prop) handed it a raw node" "RAW NODE"
has "probe: forged elements were rendered (and cleaned)" "CT forged elements done"
has "probe: a LordOS function rendered as a component runs under its policy (lord.fs refused)" "lord.fs\.[a-zA-Z]* refused"
has "probe: LordOS's code read its getter, Proxy, iterator and toStringTag objects" "CT accessors done"
for n in 112 114; do has "probe: a bound native as an accessor (node $n) refused" "lord.setStyle refused (node $n is not"; done
hasnt "probe: a forged app registry is never used" "forged app RENDERED"
has "probe: a forged app doesn't open" "CT forged app opens false"
has "probe: a LordOS field's focus is a stand-in to the theme, never its real token (scan F2)" "CT focus owner stand-in"
has "probe: the theme can't take the keys from a LordOS field" "CT focus take false false"
has "probe: restoreFocus can't move the keys off a LordOS field" "CT focus kept by LordOS"
hasnt "probe: no key reached the theme" "CT focus KEY STOLEN"
has "probe: wm has no setArgs, setProtected, setMenus, setPip, scriptMove or setCloseGuard" "CT wm.setArgs undefined wm.setProtected undefined wm.setMenus undefined wm.setPip undefined wm.scriptMove undefined wm.setCloseGuard undefined"

session forger forger 40 --unchecked
hasnt "forger: its forged host element never got a raw node" "RAW NODE"
if record forger | grep -q 'View, Text and Image'; then echo "ok   forger: refused by the host guard and taken off"; else echo "FAIL forger: $(record forger)"; R=1; fi

session throws throws 40
has "throws: the session goes back to LordOS's theme" "the theme failed, going back to"
if record throws | grep -q 'community fixture throws at start'; then echo "ok   throws: marked failed with its error"; else echo "FAIL throws: $(record throws)"; R=1; fi

session nostart nostart 40 --unchecked
if record nostart | grep -q 'never called startShell'; then echo "ok   nostart: marked failed (it would draw nothing)"; else echo "FAIL nostart: $(record nostart)"; R=1; fi

session untrusted okminimal 30 --untrusted
has "untrusted: not run before the person confirms it" "not confirmed by the person"
hasnt "untrusted: its code never started" "community theme okminimal started"

# a click that mounts new nodes (Meridian's review: every community theme was taken off at its first click that opened
# something), a timer that does, launcher icons and a shortcut in the SDK's form: through lord-server-test's hooks
if [ -x build/lord-server-test ] && command -v python3 >/dev/null; then
    S=$D/clicker
    mkdir -p "$S/home/.config/lordos"
    chmod 700 "$S"
    if node tests/community_theme_install.mjs tests/community_themes/clicker "$S/home" >/dev/null; then
        # the person's own wallpaper, as Settings → Wallpaper keeps it (BUG-H of the ws171 QA)
        mkdir -p "$S/home/.local/share/lordos/wallpapers"
        cp "$ROOT/assets/desktops/default.jpg" "$S/home/.local/share/lordos/wallpapers/0123456789abcdef.jpg"
        printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "theme": "clicker", "wallpaper": "image:0123456789abcdef"}\n' >"$S/home/.config/lordos/settings.json"
        env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
            XDG_RUNTIME_DIR="$S" HOME="$S/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 \
            ./build/lord-server-test --headless 1024x768 --theme "$ROOT/themes/default/theme.bundle.js" --timeout 40 \
            --user "$(id -un)" --test-hooks >"$S/server.log" 2>&1 &
        P=$!
        n=0
        while [ $n -lt 600 ] && ! grep -q "community theme clicker started" "$S/server.log" "$S/home/.cache/lordos/"*.log 2>/dev/null; do sleep 0.1; n=$((n + 1)); done
        sleep 3
        python3 -I - "$S/lordos-test-hooks.sock" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect(sys.argv[1])
def cmd(c):
    s.send(c.encode()); s.recv(4096)
for x, y in ((500, 400), (520, 420)):
    cmd(f'ptr {x} {y}'); time.sleep(0.15); cmd('btn 0 1'); time.sleep(0.08); cmd('btn 0 0'); time.sleep(1.0)
for c in (125, 42, 37): cmd(f'key {c} 1'); time.sleep(0.03)
for c in (37, 42, 125): cmd(f'key {c} 0'); time.sleep(0.03)
time.sleep(1.0)
PY
        kill $P 2>/dev/null; wait $P 2>/dev/null
        P=
        L=$(cat "$S/server.log" "$S/home/.cache/lordos/"*.log 2>/dev/null)
        has "clicker starts" "community theme clicker started"
        has "clicker: the person's own wallpaper is found (no lord.fs from the theme's call)" "CT clicker wallpaper mine"
        hasnt "clicker: its Image of the person's wallpaper isn't refused" "setProp refused"
        has "clicker: a timer's state update mounts new nodes" "CT clicker opened timer with apps"
        has "clicker: a window's icon from windowIcon draws" "CT clicker window icons"
        has "clicker: a click mounts new nodes" "CT clicker opened click1 with apps"
        has "clicker: a second click too" "CT clicker opened click2 with apps"
        hasnt "clicker: React's render after its setState is never refused" "community theme: lord\.\(createNode\|removeChild\|appendChild\|insertBefore\|destroy\|setStyle\|setProp\|setText\) refused"
        hasnt "clicker: the launcher's icons don't take it off" "View, Text and Image"
        hasnt "clicker: never taken off" "taken off\|marked failed"
        has "clicker: its shortcut in the SDK's form (keys: [...]) runs" "CT clicker shortcut ran"
        hasnt "clicker: an entry on a key every theme shares is dropped" "BASE SHADOWED"
    else echo "FAIL clicker: not installed"; R=1; fi
else echo "skipped: clicker needs build/lord-server-test and python3"; fi

exit $R
