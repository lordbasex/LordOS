/* webedit.c — see webedit.h. */
#include "webedit.h"

#include <string.h>

static const char *const EDIT_NAMES[WEBEDIT_N] = { "undo", "redo", "cut", "copy", "paste", "selectAll" };
static const char *const EDIT_WEBKIT[WEBEDIT_N] = { "Undo", "Redo", "Cut", "Copy", "Paste", "SelectAll" };
static const char *const MENU_NAMES[WEBMENU_N] = { "cut", "copy", "paste", "selectAll", "copyLink", "openLink", "copyImage" };

static int find(const char *const *list, int n, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(list[i], name)) return i;
    return -1;
}

int webedit_command(const char *name) { return find(EDIT_NAMES, WEBEDIT_N, name); }
const char *webedit_name(int cmd) { return cmd >= 0 && cmd < WEBEDIT_N ? EDIT_NAMES[cmd] : NULL; }
const char *webedit_webkit(int cmd) { return cmd >= 0 && cmd < WEBEDIT_N ? EDIT_WEBKIT[cmd] : NULL; }

int webmenu_item(const char *name) { return find(MENU_NAMES, WEBMENU_N, name); }
const char *webmenu_name(int item) { return item >= 0 && item < WEBMENU_N ? MENU_NAMES[item] : NULL; }

int webmenu_items(unsigned at, int out[WEBMENU_N]) {
    int n = 0;
    int editable = (at & WEBMENU_AT_EDITABLE) != 0, sel = (at & WEBMENU_AT_SELECTION) != 0;
    if (editable && sel) out[n++] = WEBMENU_CUT;
    if (sel) out[n++] = WEBMENU_COPY;
    if (editable) out[n++] = WEBMENU_PASTE;
    out[n++] = WEBMENU_SELECT_ALL;
    if (at & WEBMENU_AT_LINK) { out[n++] = WEBMENU_COPY_LINK; out[n++] = WEBMENU_OPEN_LINK; }
    if (at & WEBMENU_AT_IMAGE) out[n++] = WEBMENU_COPY_IMAGE;
    return n;
}

int webmenu_parse(const char *const *names, int n, int out[WEBMENU_N]) {
    if (!names || n < 1 || n > WEBMENU_N) return -1;
    unsigned seen = 0;
    for (int i = 0; i < n; i++) {
        int k = webmenu_item(names[i]);
        if (k < 0 || (seen & (1u << k))) return -1;
        seen |= 1u << k;
        out[i] = k;
    }
    return n;
}
