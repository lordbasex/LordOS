/* webedit.c — see webedit.h. */
#include "webedit.h"

#include <string.h>

static const char *const EDIT_NAMES[WEBEDIT_N] = { "undo", "redo", "cut", "copy", "paste", "selectAll" };
static const char *const EDIT_WEBKIT[WEBEDIT_N] = { "Undo", "Redo", "Cut", "Copy", "Paste", "SelectAll" };
static const char *const MENU_NAMES[WEBMENU_N] = {
    "spell1", "spell2", "spell3", "spell4", "spell5", "ignoreSpelling", "learnSpelling",
    "undo", "redo", "cut", "copy", "paste", "pastePlain", "selectAll", "emoji",
    "copyHighlight", "search", "speech",
    "openLink", "copyLink",
    "openImage", "saveImage", "copyImage", "copyImageAddress",
    "play", "pause", "mute", "unmute", "loop", "controls", "saveVideo", "saveAudio",
    "back", "forward", "reload",
    "inspect",
};

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

int webmenu_shell_only(int item) { return item == WEBMENU_EMOJI || item == WEBMENU_SPEECH || item == WEBMENU_INSPECT; }

int webmenu_items(unsigned at, int out[WEBMENU_N]) {
    int n = 0;
    int editable = (at & WEBMENU_AT_EDITABLE) != 0, sel = (at & WEBMENU_AT_SELECTION) != 0;
    int link = (at & WEBMENU_AT_LINK) != 0, image = (at & WEBMENU_AT_IMAGE) != 0, media = (at & WEBMENU_AT_MEDIA) != 0;
    if (editable) {
        for (int g = 0; g < WEBMENU_GUESSES_OF(at); g++) out[n++] = WEBMENU_SPELL_1 + g;
        if (at & WEBMENU_AT_IGNORE) out[n++] = WEBMENU_IGNORE_SPELLING;
        if (at & WEBMENU_AT_LEARN) out[n++] = WEBMENU_LEARN_SPELLING;
        out[n++] = WEBMENU_UNDO;
        out[n++] = WEBMENU_REDO;
        if (sel) { out[n++] = WEBMENU_CUT; out[n++] = WEBMENU_COPY; }
        out[n++] = WEBMENU_PASTE;
        out[n++] = WEBMENU_PASTE_PLAIN;
        out[n++] = WEBMENU_SELECT_ALL;
        out[n++] = WEBMENU_EMOJI;
    }
    if (sel) {
        if (!editable) out[n++] = WEBMENU_COPY;
        if (!editable && (at & WEBMENU_AT_HIGHLIGHT)) out[n++] = WEBMENU_COPY_HIGHLIGHT;
        if (at & WEBMENU_AT_SEARCH) out[n++] = WEBMENU_SEARCH;
        out[n++] = WEBMENU_SPEECH;
    }
    if (link) { out[n++] = WEBMENU_OPEN_LINK; out[n++] = WEBMENU_COPY_LINK; }
    if (image) {
        if (at & WEBMENU_AT_IMAGE_WEB) out[n++] = WEBMENU_OPEN_IMAGE;
        if (at & WEBMENU_AT_IMAGE_SAVE) out[n++] = WEBMENU_SAVE_IMAGE;
        out[n++] = WEBMENU_COPY_IMAGE;
        if (at & WEBMENU_AT_IMAGE_WEB) out[n++] = WEBMENU_COPY_IMAGE_ADDRESS;
    }
    if (media) {
        out[n++] = at & WEBMENU_AT_PAUSED ? WEBMENU_PLAY : WEBMENU_PAUSE;
        out[n++] = at & WEBMENU_AT_MUTED ? WEBMENU_UNMUTE : WEBMENU_MUTE;
        out[n++] = WEBMENU_LOOP;
        out[n++] = WEBMENU_CONTROLS;
        if (at & WEBMENU_AT_MEDIA_SAVE) out[n++] = at & WEBMENU_AT_VIDEO ? WEBMENU_SAVE_VIDEO : WEBMENU_SAVE_AUDIO;
    }
    if (!editable && !sel && !link && !image && !media) {
        if (at & WEBMENU_AT_BACK) out[n++] = WEBMENU_BACK;
        if (at & WEBMENU_AT_FORWARD) out[n++] = WEBMENU_FORWARD;
        out[n++] = WEBMENU_RELOAD;
        out[n++] = WEBMENU_SELECT_ALL;
    }
    out[n++] = WEBMENU_INSPECT;
    return n;
}

unsigned webmenu_checked(unsigned at, const int *items, int n) {
    unsigned m = 0;
    for (int i = 0; items && i < n && i < 32; i++)
        if ((items[i] == WEBMENU_LOOP && (at & WEBMENU_AT_LOOP)) || (items[i] == WEBMENU_CONTROLS && (at & WEBMENU_AT_CONTROLS))) m |= 1u << i;
    return m;
}

int webmenu_parse(const char *const *names, int n, int out[WEBMENU_N]) {
    if (!names || n < 1 || n > WEBMENU_N) return -1;
    unsigned long long seen = 0;
    for (int i = 0; i < n; i++) {
        int k = webmenu_item(names[i]);
        if (k < 0 || (seen & (1ull << k))) return -1;
        seen |= 1ull << k;
        out[i] = k;
    }
    return n;
}
