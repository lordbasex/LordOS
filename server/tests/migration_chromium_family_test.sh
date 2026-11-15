#!/bin/sh
# migration_chromium_family_test.sh — packaging/migrations/009-0.8.8.9-chromium-family.sh over a fake
# system tree (LORDOS_MIGRATION_ROOT), with build/browserprefs-test standing in for
# `lord-server --browser-prefs` (the same C function): installed browsers get the file, a vendor file is
# merged, others are left alone, and a second run changes nothing. Run from server/ by tests/run_suites.sh.
set -u
M=$(cd "$(dirname "$0")/../.." && pwd)/packaging/migrations/009-0.8.8.9-chromium-family.sh
BIN=$(pwd)/build/browserprefs-test
[ -x "$BIN" ] || { echo "FAIL: build/browserprefs-test is missing"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-mig009.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
R=$T/root
mkdir -p "$R/opt/google/chrome" "$R/opt/microsoft/msedge" "$R/home/user/.config/google-chrome"
printf '{\n  "distribution": {"do_not_create_desktop_shortcut": true},\n  "browser": {"custom_chrome_frame": false}\n}\n' \
    > "$R/opt/microsoft/msedge/master_preferences"
printf '{"profile": "untouched"}\n' > "$R/home/user/.config/google-chrome/Local State"
run() { LORDOS_MIGRATION_ROOT=$R LORDOS_BROWSER_PREFS_BIN=$BIN sh -eu "$M" > "$T/out" 2>&1 || fail "the migration exited with $?"; }
run
C=$R/opt/google/chrome/initial_preferences
[ -f "$C" ] || fail "chrome: no initial_preferences"
grep -q '"custom_chrome_frame": true' "$C" && grep -q '"system_theme": 1' "$C" || fail "chrome: $(cat "$C")"
E=$R/opt/microsoft/msedge/master_preferences
grep -q '"do_not_create_desktop_shortcut": true' "$E" || fail "edge: vendor key lost: $(cat "$E")"
grep -q '"custom_chrome_frame": false' "$E" || fail "edge: vendor value replaced: $(cat "$E")"
grep -q '"system_theme": 1' "$E" || fail "edge: GTK theme not added: $(cat "$E")"
[ ! -e "$R/opt/microsoft/msedge/initial_preferences" ] || fail "edge: initial_preferences would hide the vendor's file"
[ ! -e "$R/opt/brave.com" ] || fail "brave isn't installed: nothing may be created"
[ "$(cat "$R/home/user/.config/google-chrome/Local State")" = '{"profile": "untouched"}' ] || fail "a home folder was touched"
cp "$C" "$T/c1"; cp "$E" "$T/e1"
run
cmp -s "$C" "$T/c1" && cmp -s "$E" "$T/e1" || fail "the second run changed a file"
# a vendor file that isn't JSON: left alone, and the migration still succeeds
printf 'garbage' > "$C"
run
[ "$(cat "$C")" = garbage ] || fail "a non-JSON file was rewritten"
# without the binary (a tree without lord-server) it does nothing and succeeds
LORDOS_MIGRATION_ROOT=$R LORDOS_BROWSER_PREFS_BIN=$T/missing sh -eu "$M" || fail "no binary: exited with $?"
[ "$fails" -eq 0 ] && echo "migration 009 chromium-family: ok"
[ "$fails" -eq 0 ]
