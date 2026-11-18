#!/bin/sh
# js_getter_destroy.sh — a headless lord-server runs tests/js_getter_destroy.js, whose getters destroy the node that
# lord.setProp / lord.setStyle are writing (a use-after-free before 0.8.9). Passes when the script runs to its end and
# lord-server is still alive. Built with -fsanitize=address it reports the old bug at once; the normal build catches it
# only when the freed memory is reused badly. Exit 77 when build/lord-server is missing.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] || { echo "needs build/lord-server"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/ljg.XXXXXX") || exit 1
trap '[ -n "$KEEP" ] || rm -rf "$D"' EXIT
mkdir -p "$D/home"
chmod 700 "$D"
# outside the repository: a development build runs a script there unchecked (inside it, only the manifest's files)
cp tests/js_getter_destroy.js "$D/theme.js" || exit 1
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 \
    timeout 30 ./build/lord-server --headless 640x480 --theme "$D/theme.js" --timeout 3 >"$D/out.log" 2>&1
R=$?
grep -qs 'UAF-TEST-DONE' "$D/out.log" "$D/home/.cache/lordos/shell.log" || { echo "the script did not finish (exit $R)"; tail -20 "$D/home/.cache/lordos/compositor.log" 2>/dev/null; exit 1; }
[ $R -eq 0 ] || { echo "lord-server exited with $R"; exit 1; }
echo "ok"
