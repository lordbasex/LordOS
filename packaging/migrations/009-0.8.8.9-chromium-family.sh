# 0.8.8.9: the Chromium-based browsers from the Store (Google Chrome, Brave, Microsoft Edge; the closed
# list of server/src/pkg/browserprefs.c) get the same first-run defaults as Debian's Chromium: their own
# bar and the GTK theme, so the window buttons are LordOS's traffic lights (docs/WAYLAND.md, "Frame and
# colors"). From 0.8.8.9 the Store's package engine writes them after it installs or upgrades one of
# them; this does it once for a browser that is already installed.
#
# The file is the browser's initial_preferences next to its binary (or its master_preferences, when
# that is the one it reads). A file the vendor ships is merged, never replaced: its keys stay and
# LordOS's two keys are added only where absent; a file that isn't a JSON object, or a symlink, is left
# alone. The C code does it (lord-server --browser-prefs, the same code as the engine) and always exits
# 0: a browser default never fails an upgrade. Running it again changes nothing.
#
# Home folders are never touched: these are initial preferences, so only profiles created from now on
# get them. An existing profile switches in the browser's Settings → Appearance → Theme → GTK.
# LORDOS_MIGRATION_ROOT and LORDOS_BROWSER_PREFS_BIN are for tests only
# (server/tests/migration_chromium_family_test.sh).
R=${LORDOS_MIGRATION_ROOT:-}
B=${LORDOS_BROWSER_PREFS_BIN:-/usr/lib/lordos/bin/lord-server}
[ -x "$B" ] || exit 0
"$B" --browser-prefs "${R:-/}" || true
exit 0
