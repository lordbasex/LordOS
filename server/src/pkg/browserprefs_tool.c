/* browserprefs_tool.c — lord-browser-prefs (0.8.8.9): LordOS's initial preferences for the installed
 * Chromium-based browsers of browserprefs.c's closed list, for a browser installed before the package
 * engine wrote them. Run as root only by migration 009 (packaging/migrations/009-0.8.8.9-chromium-family.sh,
 * from the lordos package's postinst). Built from browserprefs.c and sec/json.c only (libc): it takes no
 * argument, reads no environment and runs nothing; the paths and the text are the closed list's.
 * Always exits 0 (2 when given an argument): a browser default never fails an upgrade. */
#include "browserprefs.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 1) { fprintf(stderr, "usage: %s (no arguments)\n", argv[0]); return 2; }
    browser_prefs_apply_all("");
    return 0;
}
