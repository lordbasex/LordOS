#!/bin/sh
# theme_switch_headless.sh — theme switches against a headless lord-server on the CPU path: a switch keeps the session,
# whatever happens to the theme asked for. Run from server/ (tests/run_suites.sh does). Exit 77 when build/lord-server
# is missing, 1 on a failure.
#
# 1. The new theme throws at its start and the running one's file is gone from under the session (as a package update
#    replaces it). lord-server used to be left with no JavaScript context at all, and its next js_tick crashed in
#    JS_GetGlobalObject(NULL) (2026-11-22 13:41); then it showed the recovery screen, whose only way on is Log Out. Now
#    the running theme comes back from the bytes verified when it loaded (js.c js_init_previous) and the session goes on.
# 2. "Package updated under the session" with a code manifest (sec/codesig.c): the LordOS folder has
#    code-manifest.json, the update replaces theme b and the manifest (as dpkg does: a new file renamed over the old
#    one). This build only trusts the LordOS release key, so the test's manifest is unsigned: a development
#    session, which never takes a manifest that changed under it (codesig_reload only replaces a signed one, after
#    checking its signature as at login; build/codesig-test covers that with a throwaway key: a good pair
#    accepted, bad signature, truncated, link, writable by others, not root's, older version all refused). Theme b
#    must be refused when lord.themes.use asks for it (js_theme_prepare), before anything is discarded: the running
#    theme is never stopped, and no switch starts.
# 3. Live, the four desktops (needs node with esbuild, npm install, and build/wlperf-client): the real Flow, Lord
#    Classic, Workspaces and Celeste with a small driver (tests/theme_switch_scene.mjs) that opens Settings
#    and Files and switches to the next one through the SDK's themes.use, round all four and back, while a Wayland
#    client draws on every frame callback. The process must stay up, each step must run the desktop asked for with the
#    two apps back, the "changing the desktop style" overlay must be up while it switches, the Wayland client must stay
#    connected to the end, and a theme that can't be switched to must answer false and leave the choice as it was.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] || { echo "needs build/lord-server"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lts.XXXXXX") || exit 1
P=
C=
trap 'kill $P $C 2>/dev/null; rm -rf "$D"' EXIT
R=0

# a theme that puts a root node up and, three seconds in, asks for theme b
theme_a() {
cat > "$1" <<'EOF'
var t0 = 0, asked = false;
globalThis.__lordTick = function (now) {
  if (!t0) t0 = now;
  if (!asked && now - t0 > 3000) { asked = true; console.log('THEME a asks for b: ' + lord.themes.use('b')); }
  return 50;
};
globalThis.__lordWantsFrame = function () { return false; };
globalThis.__lordFrame = function () {};
lord.setRoot(lord.createNode('view'));
EOF
}

run_server() {   # $1: the folder; $2: the theme bundle; $3: seconds; extra arguments after
    dir=$1 theme=$2 secs=$3
    shift 3
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
        XDG_RUNTIME_DIR="$dir" HOME="$dir/home" LORD_GPU=0 LANG=en_US.UTF-8 \
        ./build/lord-server --headless 640x480 --theme "$theme" --timeout "$secs" "$@" >"$dir/server.log" 2>&1 &
    P=$!
}

logs() { cat "$1/home/.cache/lordos/compositor.log" "$1/home/.cache/lordos/shell.log" "$1/server.log" 2>/dev/null; }
count() { printf '%s\n' "$1" | grep -c -- "$2"; }
# waits (up to $3 tenths of a second) until the logs of folder $1 hold $2 at least $4 times (default once): fixed sleeps
# failed under load (a session slow to start had theme a's file removed before it read it)
wait_log() { local n=0; while [ $n -lt "$3" ] && [ "$(count "$(logs "$1")" "$2")" -lt "${4:-1}" ]; do sleep 0.1; n=$((n + 1)); done; }

# ---- 1. the new theme throws, the running one's file is gone: the running one comes back ----
S=$D/one
mkdir -p "$S/home" "$S/themes/a" "$S/themes/b"
chmod 700 "$D" "$S"
theme_a "$S/themes/a/theme.bundle.js"
echo "throw new Error('theme b does not load');" > "$S/themes/b/theme.bundle.js"
run_server "$S" "$S/themes/a/theme.bundle.js" 12
wait_log "$S" "js: theme $S/themes/a/theme.bundle.js loaded" 80
# theme a is gone from under the running session (a package update replaced it; here it can't be read at all)
rm -f "$S/themes/a/theme.bundle.js"
wait_log "$S" "js: theme $S/themes/a/theme.bundle.js loaded" 100 2
sleep 0.5
L=$(logs "$S")
if kill -0 $P 2>/dev/null; then echo "ok   lord-server is still running after the failed switch"
else echo "FAIL lord-server is gone"; R=1; fi
if printf '%s\n' "$L" | grep -q "the theme failed, going back to"; then echo "ok   theme b failed and lord-server went back"
else echo "FAIL theme b did not fail as it should"; R=1; fi
if [ "$(count "$L" "js: theme $S/themes/a/theme.bundle.js loaded")" -ge 2 ] && ! printf '%s\n' "$L" | grep -q "recovery screen"
then echo "ok   theme a runs again from the bytes verified at its start, no recovery screen"
else echo "FAIL theme a did not come back (or the recovery screen was shown)"; R=1; fi
[ $R = 0 ] || printf '%s\n' "$L" | tail -30
kill $P 2>/dev/null
wait $P 2>/dev/null

# ---- 2. the package updated under the session, with a code manifest ----
S=$D/two
mkdir -p "$S/home" "$S/themes/a" "$S/themes/b" "$S/assets"
chmod 700 "$S"
cp -r ../assets/fonts "$S/assets/fonts" || exit 1
theme_a "$S/themes/a/theme.bundle.js"
echo "globalThis.__lordTick = function () { return 50; }; lord.setRoot(lord.createNode('view'));" > "$S/themes/b/theme.bundle.js"
entry() { printf '    { "path": "%s", "size": %s, "sha256": "%s" }' "$1" "$(wc -c < "$S/$1" | tr -d ' ')" "$(sha256sum "$S/$1" | cut -c1-64)"; }
manifest() {   # $1: version; written next to the old one and renamed over it
    { printf '{\n  "format": 1,\n  "kind": "lordos-system-code",\n  "version": "%s",\n  "files": [\n' "$1"
      entry themes/a/theme.bundle.js; printf ',\n'
      entry themes/b/theme.bundle.js; printf '\n  ]\n}\n'; } > "$S/code-manifest.json.dpkg-new"
    mv "$S/code-manifest.json.dpkg-new" "$S/code-manifest.json"
}
manifest 0.8.9.4
run_server "$S" "$S/themes/a/theme.bundle.js" 12 --fonts "$S/assets/fonts"
wait_log "$S" "js: theme $S/themes/a/theme.bundle.js loaded" 80
# the update: theme b's new bundle and the new manifest that lists it
echo "globalThis.__lordTick = function () { return 50; }; /* 0.8.9.5 */ lord.setRoot(lord.createNode('view'));" > "$S/themes/b/theme.bundle.js.dpkg-new"
mv "$S/themes/b/theme.bundle.js.dpkg-new" "$S/themes/b/theme.bundle.js"
manifest 0.8.9.5
wait_log "$S" "THEME a asks for b:" 100
sleep 0.5
L=$(logs "$S")
R2=0
if kill -0 $P 2>/dev/null; then echo "ok   lord-server is still running after the update and the refused switch"
else echo "FAIL lord-server is gone after the update"; R2=1; fi
if printf '%s\n' "$L" | grep -q "code: development ($(cd "$S" && pwd -P)"; then echo "ok   the session read the folder's code manifest"
else echo "FAIL the session did not use the test folder's code manifest"; R2=1; fi
if printf '%s\n' "$L" | grep -q "REFUSED themes/b/theme.bundle.js" && printf '%s\n' "$L" | grep -q "THEME a asks for b: false"
then echo "ok   theme b was refused when it was asked for: lord.themes.use answered false"
else echo "FAIL theme b was not refused at lord.themes.use"; R2=1; fi
if printf '%s\n' "$L" | grep -q "manifest changed under this session"; then echo "FAIL a development session read the changed manifest"; R2=1
else echo "ok   no manifest was read again"; fi
if [ "$(count "$L" "hash ok themes/a/theme.bundle.js")" -eq 1 ] && ! printf '%s\n' "$L" | grep -q "cambiando a tema"
then echo "ok   the running theme was never stopped: no switch started"
else echo "FAIL the running theme was discarded for a refused theme"; R2=1; fi
[ $R2 = 0 ] || printf '%s\n' "$L" | tail -30
[ $R2 = 0 ] || R=1
kill $P 2>/dev/null
wait $P 2>/dev/null

# ---- 3. live, the four desktops in one session ----
S=$D/three
mkdir -p "$S/home/.config/lordos"
chmod 700 "$S"
if ! command -v node >/dev/null 2>&1 || [ ! -x build/wlperf-client ] || ! node tests/theme_switch_scene.mjs "$S/themes" >"$S/scene.log" 2>&1; then
    echo "skipped: the live round needs node with esbuild (npm install) and build/wlperf-client"
    exit $R
fi
printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en", "theme": "default"}\n' >"$S/home/.config/lordos/settings.json"
run_server "$S" "$S/themes/default/theme.bundle.js" 60
i=0
while [ $i -lt 40 ] && [ ! -S "$S/wayland-0" ]; do sleep 0.25; i=$((i + 1)); done
# as long as the session (60 s): the round takes over 45 s on a loaded machine, and a client that ended on its own
# (exit 0) is not one the switches dropped
XDG_RUNTIME_DIR="$S" WAYLAND_DISPLAY=wayland-0 ./build/wlperf-client frames 58 >"$S/client.log" 2>&1 &
C=$!
i=0
while [ $i -lt 120 ] && ! grep -q "SCENE 4 " "$S/home/.cache/lordos/shell.log" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
sleep 1
L=$(logs "$S")
R3=0
if kill -0 $P 2>/dev/null; then echo "ok   lord-server is still running after four live switches"
else echo "FAIL lord-server is gone"; R3=1; fi
seen=$(printf '%s\n' "$L" | sed -n 's/.*SCENE \([0-9]\) current=\([a-z]*\) apps=\(.*\)$/\2 \3/p')
n=$(printf '%s\n' "$seen" | grep -c .)
if [ "$n" -eq 5 ]; then echo "ok   five steps, one per JS context"; else echo "FAIL $n steps instead of 5"; R3=1; fi
for th in default classic workspaces celeste; do
    if printf '%s\n' "$seen" | grep -q "^$th "; then echo "ok   $th ran live"; else echo "FAIL $th never ran"; R3=1; fi
done
if [ "$(printf '%s\n' "$seen" | grep -c 'files.*settings')" -eq 5 ]; then echo "ok   Settings and Files came back in every desktop"
else echo "FAIL the apps did not come back in every desktop"; R3=1; fi
if [ "$(count "$L" "SCENE switch .*: true overlay=1")" -eq 4 ]; then echo "ok   every switch was accepted with the overlay up"
else echo "FAIL a switch was not accepted, or without its overlay"; R3=1; fi
if printf '%s\n' "$L" | grep -q "SCENE refused false theme=default"; then echo "ok   a theme that can't be switched to answers false and the choice stays"
else echo "FAIL a theme that can't be switched to changed the choice"; R3=1; fi
if kill -0 $C 2>/dev/null; then echo "ok   the Wayland client is still connected"
else wait $C; echo "FAIL the Wayland client ended ($?)"; R3=1; fi
kill $C 2>/dev/null
wait $C 2>/dev/null
C=
if printf '%s\n' "$L" | grep -q -e "crashed:" -e "recovery screen"; then echo "FAIL a crash or the recovery screen"; R3=1; fi
[ $R3 = 0 ] || { printf '%s\n' "$L" | grep -v stall | tail -40; tail -5 "$S/client.log"; }
[ $R3 = 0 ] || R=1
exit $R
