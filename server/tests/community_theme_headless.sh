#!/bin/sh
# community_theme_headless.sh — community themes in a headless lord-server (docs/THEMES-GUIDE.md, "Community themes";
# AGENTS.md "Community themes"). Each fixture of tests/community_themes/ is put into a private home's per-user themes
# folder the way an install does (community_theme_install.mjs, the same compiler), chosen in settings.json, and the
# session runs it from LordOS's default theme:
#   okminimal  the minimal example as a community theme: it starts in its own realm and draws (no watchdog), and
#              nothing it does through the SDK is refused
#   probe      (installed unchecked) every way to reach past its policy: lord.term/fs/admin don't exist in its realm,
#              code from text gives nothing, LordOS's built-ins are frozen, and lord.setStyle on a node not its own is
#              refused directly, from Promise.then, queueMicrotask, setTimeout (bound and with arguments),
#              requestAnimationFrame, a store subscription, an effect and a ref (a bound restricted native handed to
#              LordOS's code each time: nodes 101…109), and from a theme store's get React reads later (110); on its own
#              node it works; forged elements (a host element, LordOS's raw component, a context's Provider holding a
#              host element; in children, another prop, a nested object, an iterable, a lazy element, a thenable or a store's
#              value) never hand it a raw node; a forged app registry is never
#              used; wm has no setArgs, setProtected, setMenus, setPip, scriptMove or setCloseGuard
#   forger     its own component returns a forged host element: the host guard refuses it (its element wasn't made by
#              LordOS's React), the theme is taken off
#   throws     throws while it starts: the session goes back to LordOS's theme, it is marked failed with the error
#   nostart    never calls startShell: the same
#   untrusted  okminimal not confirmed by the person: never run
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

session probe probe 40 --unchecked
has "probe starts" "community theme probe started"
hasnt "probe: nothing it tried was allowed" "CT .* ALLOWED"
for c in direct settings-in-place term.create fs.write own-realm-function host-function-constructor host-proto-change promise; do has "probe: $c refused" "CT $c refused"; done
has "probe: lord.term, lord.fs, lord.admin and lord.perms don't exist in its realm" "CT lord.term undefined lord.fs undefined lord.admin undefined globalThis.lord.perms undefined"
for n in 101 102 103 104 105 106 107 108 109; do has "probe: setStyle on node $n refused" "lord.setStyle refused (node $n is not"; done
has "probe: its timers and animation frames ran" "CT raf ran"
has "probe: its own node can be restyled" "CT own-node works"
has "probe: lord.setStyle from a store's get React reads later refused" "lord.setStyle refused (node 110 is not"
hasnt "probe: no forged element (host, raw component, Provider; in children or a prop) handed it a raw node" "RAW NODE"
has "probe: forged elements were rendered (and cleaned)" "CT forged elements done"
hasnt "probe: a forged app registry is never used" "forged app RENDERED"
has "probe: a forged app doesn't open" "CT forged app opens false"
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

exit $R
