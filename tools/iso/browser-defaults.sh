#!/bin/sh
# browser-defaults.sh — the preinstalled browsers' LordOS defaults, written into a system tree being
# built (the live ISO, tools/iso/build.sh, and the Raspberry Pi image, tools/rpi/build-img.sh).
#
#   sh tools/iso/browser-defaults.sh <root>
#
# - Chromium: new profiles draw their own bar (tabs in the title bar) with the GTK theme, so the
#   window buttons are LordOS's traffic lights from LordOS's GTK theme (docs/WAYLAND.md). The user
#   goes back to the LordOS frame with «Use system title bar and borders» (Settings → Appearance).
# - Firefox ESR: its own bar too (the user changes it in Customize Toolbar > Title Bar).
# The GTK themes, the video extension and the video settings (the video-optimize action) come with
# the lordos package; installed systems get these two defaults from migration 005.
set -eu
R=${1:?usage: browser-defaults.sh <root>}
[ -d "$R/etc" ] || { echo "browser-defaults.sh: $R is not a system tree" >&2; exit 1; }
if [ -f "$R/etc/chromium/master_preferences" ]; then
python3 - "$R/etc/chromium/master_preferences" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
d.setdefault("browser", {})["custom_chrome_frame"] = True
d.setdefault("extensions", {}).setdefault("theme", {})["system_theme"] = 1   # 1 = GTK
json.dump(d, open(p, "w"), indent=2)
PY
fi
if [ -d "$R/usr/lib/firefox-esr" ] || [ -d "$R/etc/firefox-esr" ]; then
    install -d "$R/etc/firefox-esr"
    printf '%s\n' '// LordOS: Firefox draws its own bar, with the window buttons in LordOS'"'"'s order (see' \
      '// docs/WAYLAND.md); 0 gives it the LordOS frame instead (Customize Toolbar > Title Bar).' \
      'pref("browser.tabs.inTitlebar", 1);' > "$R/etc/firefox-esr/lordos.js"
fi
