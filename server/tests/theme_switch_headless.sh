#!/bin/sh
# theme_switch_headless.sh — theme switches against a headless lord-server on the CPU path, when a package update
# changed LordOS's code under the running session. Run from server/ (tests/run_suites.sh does). Exit 77 when
# build/lord-server is missing, 1 on a failure.
#
# 1. Both the new theme and the one running fail to load (the running one's file changed under the session, as a
#    package update does). lord-server used to be left with no JavaScript context at all, and its next js_tick
#    crashed in JS_GetGlobalObject(NULL) (2026-11-22 13:41). It must show the recovery screen and go on.
# 2. "Package updated under the session" with a code manifest (sec/codesig.c): the LordOS folder has
#    code-manifest.json, the update replaces theme b and the manifest (as dpkg does: a new file renamed over the old
#    one). This build only trusts the LordOS release key, so the test's manifest is unsigned: a development
#    session, which never takes a manifest that changed under it (codesig_reload only replaces a signed one, after
#    checking its signature as at login; build/codesig-test covers that with a throwaway key: a good pair
#    accepted, bad signature, truncated, link, writable by others, not root's, older version all refused). Theme b
#    must be refused against the manifest read before, and the running theme must come back and keep working.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] || { echo "needs build/lord-server"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lts.XXXXXX") || exit 1
P=
trap 'kill $P 2>/dev/null; rm -rf "$D"' EXIT
R=0

# a theme that puts a root node up and, three seconds in, asks for theme b
theme_a() {
cat > "$1" <<'EOF'
var t0 = 0, asked = false;
globalThis.__lordTick = function (now) {
  if (!t0) t0 = now;
  if (!asked && now - t0 > 3000) { asked = true; lord.themes.use('b'); }
  return 50;
};
globalThis.__lordWantsFrame = function () { return false; };
globalThis.__lordFrame = function () {};
lord.setRoot(lord.createNode('view'));
EOF
}

run_server() {   # $1: the folder; $2: the theme bundle; extra arguments after
    dir=$1 theme=$2
    shift 2
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
        XDG_RUNTIME_DIR="$dir" HOME="$dir/home" LORD_GPU=0 \
        ./build/lord-server --headless 640x480 --theme "$theme" --timeout 12 "$@" >"$dir/server.log" 2>&1 &
    P=$!
}

logs() { cat "$1/home/.cache/lordos/compositor.log" "$1/server.log" 2>/dev/null; }

# ---- 1. both fail: the recovery screen ----
S=$D/one
mkdir -p "$S/home" "$S/themes/a" "$S/themes/b"
chmod 700 "$D" "$S"
theme_a "$S/themes/a/theme.bundle.js"
echo "throw new Error('theme b does not load');" > "$S/themes/b/theme.bundle.js"
run_server "$S" "$S/themes/a/theme.bundle.js"
sleep 1.5
# theme a is gone from under the running session (a package update replaced it; here it can't be read at all, as a
# refused bundle, which also leaves no JavaScript context): going back to it fails too
rm -f "$S/themes/a/theme.bundle.js"
sleep 4
if kill -0 $P 2>/dev/null; then echo "ok   lord-server is still running after the failed switch"
else echo "FAIL lord-server is gone"; R=1; fi
if logs "$S" | grep -q "did not load again either: the recovery screen"; then echo "ok   it went to the recovery screen"
else echo "FAIL no recovery screen in the log"; R=1; fi
[ $R = 0 ] || logs "$S" | tail -30
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
run_server "$S" "$S/themes/a/theme.bundle.js" --fonts "$S/assets/fonts"
sleep 1.5
# the update: theme b's new bundle and the new manifest that lists it
echo "globalThis.__lordTick = function () { return 50; }; /* 0.8.9.5 */ lord.setRoot(lord.createNode('view'));" > "$S/themes/b/theme.bundle.js.dpkg-new"
mv "$S/themes/b/theme.bundle.js.dpkg-new" "$S/themes/b/theme.bundle.js"
manifest 0.8.9.5
sleep 4
L=$(logs "$S")
R2=0
if kill -0 $P 2>/dev/null; then echo "ok   lord-server is still running after the update and the switch"
else echo "FAIL lord-server is gone after the update"; R2=1; fi
if printf '%s\n' "$L" | grep -q "code: development ($(cd "$S" && pwd -P)"; then echo "ok   the session read the folder's code manifest"
else echo "FAIL the session did not use the test folder's code manifest"; R2=1; fi
if printf '%s\n' "$L" | grep -q "REFUSED themes/b/theme.bundle.js"; then echo "ok   theme b was refused: an unsigned manifest that changed under the session is never taken"
else echo "FAIL theme b was not refused"; R2=1; fi
if printf '%s\n' "$L" | grep -q "manifest changed under this session"; then echo "FAIL a development session read the changed manifest"; R2=1
else echo "ok   no manifest was read again"; fi
if [ "$(printf '%s\n' "$L" | grep -c "hash ok themes/a/theme.bundle.js")" -ge 2 ] && ! printf '%s\n' "$L" | grep -q "did not load again either"
then echo "ok   the running theme came back and keeps working"
else echo "FAIL the running theme did not come back"; R2=1; fi
[ $R2 = 0 ] || printf '%s\n' "$L" | tail -30
[ $R2 = 0 ] || R=1
exit $R
