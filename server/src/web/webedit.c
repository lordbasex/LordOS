/* webedit.c — see webedit.h. */
#include "webedit.h"

#include <string.h>

int webai_clean(const char *in, char *out) {
    if (!in) return -1;
    size_t n = strlen(in), o = 0, chars = 0;
    if (!n || n > WEBAI_MAX) return -1;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)in[i];
        int k = c < 0x80 ? 0 : c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : -1;
        if (k < 0 || i + (size_t)k >= n + (k ? 0 : 1)) return -1;
        unsigned cp = k ? c & (0x3f >> k) : c;
        for (int j = 1; j <= k; j++) { if (((unsigned char)in[i + j] & 0xc0) != 0x80) return -1; cp = cp << 6 | ((unsigned char)in[i + j] & 0x3f); }
        if ((k == 1 && cp < 0x80) || (k == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff))) return -1;
        int drop = (cp < 0x20 && cp != '\n' && cp != '\t') || (cp >= 0x7f && cp <= 0x9f) || cp == 0x200e || cp == 0x200f ||
                   (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069);
        if (!drop) { memcpy(out + o, in + i, (size_t)k + 1); o += (size_t)k + 1; if (++chars > WEBAI_CHARS) return -1; }
        i += (size_t)k + 1;
    }
    out[o] = 0;
    return o ? 0 : -1;
}

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

/* ---- ✨ AI's Replace in the page (webedit.h) ---- */

const char WEBAI_MARK_JS[] =
    "const a = document.activeElement;\n"
    "const ok = !!a && (a instanceof HTMLTextAreaElement || a instanceof HTMLInputElement || a.isContentEditable);\n"
    "globalThis.__lordosAiField = ok ? a : null;\n"
    "return ok;\n";

const char WEBAI_REPLACE_JS[] =
    "const f = globalThis.__lordosAiField || null;\n"
    "globalThis.__lordosAiField = null;\n"
    "if (!f || !f.isConnected) return 'gone';\n"
    "if (document.activeElement !== f) return 'other-field';\n"
    "const TEXT = ['', 'text', 'search', 'url', 'tel', 'email'];\n"
    "const plain = f instanceof HTMLTextAreaElement || f instanceof HTMLInputElement;\n"
    "let sel, read;\n"
    "if (plain) {\n"
    "  if (f instanceof HTMLInputElement && f.type === 'password') return 'password';\n"
    "  if (f instanceof HTMLInputElement && !TEXT.includes(f.type)) return 'not-editable';\n"
    "  if (f.readOnly || f.disabled) return 'read-only';\n"
    "  sel = f.value.slice(f.selectionStart, f.selectionEnd);\n"
    "  read = () => f.value;\n"
    "} else {\n"
    "  if (!f.isContentEditable) return 'not-editable';\n"
    "  const s = getSelection();\n"
    "  if (!s || !s.rangeCount || !f.contains(s.anchorNode) || !f.contains(s.focusNode)) return 'selection-changed';\n"
    "  sel = String(s);\n"
    "  read = () => f.textContent;\n"
    "}\n"
    "if (!sel) return 'no-selection';\n"
    "if (sel !== orig) return 'selection-changed';\n"
    "const before = read();\n"
    "document.execCommand('insertText', false, text);\n"
    "// a rich editor (Lexical, ProseMirror…) may apply it in its next update\n"
    "for (let i = 0; i < 6 && read() === before; i++) await new Promise((r) => setTimeout(r, 25));\n"
    "return read() !== before ? 'done' : 'fallback';\n";

const char *webai_refusal(const char *word) {
    static const char *const W[][2] = {
        { "gone", "the field it was in is gone" },
        { "other-field", "another field has the focus" },
        { "password", "a password field" },
        { "read-only", "the field is read-only" },
        { "not-editable", "the focus isn't in a text field" },
        { "no-selection", "nothing is selected any more" },
        { "selection-changed", "the selection changed" },
        { "unchanged", "the field took nothing" },
        { "no-menu", "nothing to replace" },
        { "no-focus", "the window didn't get the keyboard back" },
        { "bad-text", "the text isn't clean UTF-8" },
        { "no-answer", "the page didn't answer" },
    };
    if (word && (!strcmp(word, "done") || !strcmp(word, "fallback"))) return NULL;
    for (size_t i = 0; word && i < sizeof W / sizeof W[0]; i++) if (!strcmp(word, W[i][0])) return W[i][1];
    return W[sizeof W / sizeof W[0] - 1][1];
}
