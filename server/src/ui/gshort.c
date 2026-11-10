/* gshort.c — the global shortcuts portal's rules without a bus (portal.c serves the interface).
 *
 * An app proposes shortcuts with a trigger in the spec's text ("CTRL+SHIFT+a"); the person decides in
 * LordOS's own sheet and the decision is kept per app. A trigger is parsed here into the same terms
 * wl/keygrab.c uses (modifiers plus a letter or an evdev code), so a binding matches keys exactly the
 * way LordOS's own shortcuts do: letters by what the layout types, everything else by position. */
#include "gshort.h"
#include "../wl/keygrab.h"
#include "../hw/jbuf.h"
#include "../sec/json.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The keys a trigger may name besides letters, digits and F1..F24: the spec's keysym names and the names
 * sdk/shortcuts.js writes. The first name of a code is the one gs_format writes. */
static const struct { const char *name; unsigned short code; } names[] = {
    { "Space", KEY_SPACE }, { "Enter", KEY_ENTER }, { "Return", KEY_ENTER }, { "Tab", KEY_TAB },
    { "Escape", KEY_ESC }, { "Esc", KEY_ESC }, { "Backspace", KEY_BACKSPACE },
    { "Delete", KEY_DELETE }, { "Insert", KEY_INSERT }, { "Home", KEY_HOME }, { "End", KEY_END },
    { "PageUp", KEY_PAGEUP }, { "Page_Up", KEY_PAGEUP }, { "Prior", KEY_PAGEUP },
    { "PageDown", KEY_PAGEDOWN }, { "Page_Down", KEY_PAGEDOWN }, { "Next", KEY_PAGEDOWN },
    { "ArrowUp", KEY_UP }, { "Up", KEY_UP }, { "ArrowDown", KEY_DOWN }, { "Down", KEY_DOWN },
    { "ArrowLeft", KEY_LEFT }, { "Left", KEY_LEFT }, { "ArrowRight", KEY_RIGHT }, { "Right", KEY_RIGHT },
    { ",", KEY_COMMA }, { "comma", KEY_COMMA }, { ".", KEY_DOT }, { "period", KEY_DOT },
    { "/", KEY_SLASH }, { "slash", KEY_SLASH }, { ";", KEY_SEMICOLON }, { "semicolon", KEY_SEMICOLON },
    { "'", KEY_APOSTROPHE }, { "apostrophe", KEY_APOSTROPHE }, { "[", KEY_LEFTBRACE }, { "bracketleft", KEY_LEFTBRACE },
    { "]", KEY_RIGHTBRACE }, { "bracketright", KEY_RIGHTBRACE }, { "-", KEY_MINUS }, { "minus", KEY_MINUS },
    { "=", KEY_EQUAL }, { "equal", KEY_EQUAL }, { "`", KEY_GRAVE }, { "grave", KEY_GRAVE },
    { "\\", KEY_BACKSLASH }, { "backslash", KEY_BACKSLASH },
    { "Pause", KEY_PAUSE }, { "ScrollLock", KEY_SCROLLLOCK }, { "Scroll_Lock", KEY_SCROLLLOCK },
    { "PrintScreen", KEY_SYSRQ }, { "Print", KEY_SYSRQ },
};
#define NNAMES (sizeof names / sizeof names[0])

static const unsigned short digit_code[10] = { KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9 };

/* F1..F24 → its code, 0 if name is not one */
static unsigned short fkey(const char *name) {
    if ((name[0] != 'F' && name[0] != 'f') || name[1] < '1' || name[1] > '9') return 0;
    int n = name[1] - '0';
    if (name[2]) {
        if (name[2] < '0' || name[2] > '9' || name[3]) return 0;
        n = n * 10 + name[2] - '0';
    }
    if (n < 1 || n > 24) return 0;
    if (n <= 10) return (unsigned short)(KEY_F1 + n - 1);
    if (n <= 12) return (unsigned short)(KEY_F11 + n - 11);
    return (unsigned short)(KEY_F13 + n - 13);
}

static int fkey_number(unsigned short code) {
    if (code >= KEY_F1 && code <= KEY_F10) return code - KEY_F1 + 1;
    if (code == KEY_F11 || code == KEY_F12) return code - KEY_F11 + 11;
    if (code >= KEY_F13 && code <= KEY_F24) return code - KEY_F13 + 13;
    return 0;
}

static int key_of(const char *name, GsKey *k) {
    if (!name[0]) return -1;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') { k->letter = c; return 0; }
        if (c >= 'A' && c <= 'Z') { k->letter = (char)(c - 'A' + 'a'); return 0; }
        if (c >= '0' && c <= '9') { k->code = digit_code[c - '0']; return 0; }
    }
    if ((k->code = fkey(name))) return 0;
    for (size_t i = 0; i < NNAMES; i++) if (!strcasecmp(names[i].name, name)) { k->code = names[i].code; return 0; }
    return -1;
}

static int mod_of(const char *m) {
    if (!strcasecmp(m, "CTRL") || !strcasecmp(m, "CONTROL")) return KG_CTRL;
    if (!strcasecmp(m, "ALT")) return KG_ALT;
    if (!strcasecmp(m, "SHIFT")) return KG_SHIFT;
    if (!strcasecmp(m, "LOGO") || !strcasecmp(m, "META") || !strcasecmp(m, "SUPER")) return KG_META;
    return 0;                               /* NUM (Num Lock held) and anything else */
}

int gs_parse(const char *text, GsKey *k) {
    memset(k, 0, sizeof *k);
    if (!text) return -1;
    size_t len = strlen(text);
    if (!len || len > 63) return -1;
    char buf[64];
    memcpy(buf, text, len + 1);
    char *part = buf;
    for (;;) {
        char *plus = strchr(part, '+');
        if (!plus) break;
        *plus = 0;
        int m = mod_of(part);
        if (!m || (k->mods & m)) { memset(k, 0, sizeof *k); return -1; }
        k->mods |= (unsigned char)m;
        part = plus + 1;
    }
    if (key_of(part, k)) { memset(k, 0, sizeof *k); return -1; }
    return 0;
}

void gs_format(const GsKey *k, char *out, size_t n) {
    char key[16] = "";
    if (k->letter) snprintf(key, sizeof key, "%c", k->letter - 'a' + 'A');
    else if (fkey_number(k->code)) snprintf(key, sizeof key, "F%d", fkey_number(k->code));
    else {
        for (int d = 0; d < 10 && !key[0]; d++) if (digit_code[d] == k->code) snprintf(key, sizeof key, "%d", d);
        for (size_t i = 0; i < NNAMES && !key[0]; i++) if (names[i].code == k->code) snprintf(key, sizeof key, "%s", names[i].name);
    }
    snprintf(out, n, "%s%s%s%s%s", k->mods & KG_CTRL ? "Ctrl+" : "", k->mods & KG_ALT ? "Alt+" : "",
             k->mods & KG_SHIFT ? "Shift+" : "", k->mods & KG_META ? "Meta+" : "", key);
}

int gs_same(const GsKey *a, const GsKey *b) {
    return a->mods == b->mods && a->letter == b->letter && (a->letter || a->code == b->code);
}

int gs_match(const GsKey *k, int code, char letter, unsigned mods) {
    if (k->mods != mods) return 0;
    return k->letter ? k->letter == letter : !letter && k->code == code;
}

int gs_kept(const GsKey *k) {
    if (k->mods & KG_META) return GS_LOGO;
    unsigned short c = k->letter ? 0 : k->code;
    if (c && keygrab_system_key(c)) return GS_SYSTEM;
    if (c == KEY_TAB || c == KEY_ESC) return GS_SYSTEM;
    if ((k->mods & (KG_CTRL | KG_ALT)) == (KG_CTRL | KG_ALT)
        && ((c >= KEY_F1 && c <= KEY_F10) || c == KEY_F11 || c == KEY_F12 || c == KEY_BACKSPACE || c == KEY_DELETE)) return GS_SYSTEM;
    if (!(k->mods & (KG_CTRL | KG_ALT)) && !fkey_number(c) && c != KEY_PAUSE && c != KEY_SCROLLLOCK) return GS_TYPING;
    if (keygrab_match(c, k->letter, k->mods) != KG_PASS) return GS_THEME;
    return GS_FREE;
}

/*            CREATE    BIND        ANSWER      CANCEL    LIST        KEY         CLOSE */
static const signed char steps[5][7] = {
    [GSS_NONE]   = { GSS_NEW, -1,         -1,         -1,      -1,         -1,         -1 },
    [GSS_NEW]    = { -1,      GSS_ASKING, -1,         -1,      GSS_NEW,    -1,         GSS_CLOSED },
    [GSS_ASKING] = { -1,      -1,         GSS_BOUND,  GSS_NEW, GSS_ASKING, -1,         GSS_CLOSED },
    [GSS_BOUND]  = { -1,      -1,         -1,         -1,      GSS_BOUND,  GSS_BOUND,  GSS_CLOSED },
    [GSS_CLOSED] = { -1,      -1,         -1,         -1,      -1,         -1,         -1 },
};

int gs_step(int state, int op) {
    if (state < GSS_NONE || state > GSS_CLOSED || op < GSO_CREATE || op > GSO_CLOSE) return -1;
    return steps[state][op];
}

int gs_id_valid(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n >= GS_ID_MAX) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)id[i] <= 0x20 || (unsigned char)id[i] >= 0x7f) return 0;
    return 1;
}

void gs_text(char *out, size_t n, const char *in) {
    size_t o = 0;
    if (!n) return;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; ) {
        size_t len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if (!len) { p++; continue; }                    /* a stray continuation byte */
        size_t got = 1;
        while (got < len && (p[got] & 0xc0) == 0x80) got++;
        if (got < len) { p += got; continue; }          /* a cut sequence */
        if (len == 1 && (*p < 0x20 || *p == 0x7f)) { p++; continue; }
        if (o + len >= n) break;
        memcpy(out + o, p, len);
        o += len;
        p += len;
    }
    out[o] = 0;
}

/* ---- the remembered decisions ---- */

GsEntry *gs_store_find(GsStore *s, const char *app, const char *id) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->e[i].app, app) && !strcmp(s->e[i].id, id)) return &s->e[i];
    return NULL;
}

const GsEntry *gs_store_owner(const GsStore *s, const GsKey *k, const char *app, const char *id) {
    for (int i = 0; i < s->n; i++) {
        const GsEntry *e = &s->e[i];
        if (!e->bound || !gs_same(&e->key, k)) continue;
        if (!strcmp(e->app, app) && !strcmp(e->id, id)) continue;
        return e;
    }
    return NULL;
}

GsEntry *gs_store_set(GsStore *s, const char *app, const char *id, const char *desc, const GsKey *key) {
    if (!app[0] || strlen(app) >= GS_APP_MAX || !gs_id_valid(id)) return NULL;
    GsEntry *e = gs_store_find(s, app, id);
    if (!e) {
        if (s->n >= GS_MAX_ENTRIES) return NULL;
        e = &s->e[s->n++];
        memset(e, 0, sizeof *e);
        snprintf(e->app, sizeof e->app, "%s", app);
        snprintf(e->id, sizeof e->id, "%s", id);
    }
    if (desc) gs_text(e->desc, sizeof e->desc, desc);
    e->bound = key != NULL;
    if (key) e->key = *key; else memset(&e->key, 0, sizeof e->key);
    return e;
}

int gs_store_forget(GsStore *s, const char *app) {
    int gone = 0;
    for (int i = s->n; i-- > 0;)
        if (!strcmp(s->e[i].app, app)) {
            memmove(&s->e[i], &s->e[i + 1], sizeof s->e[0] * (size_t)(s->n - i - 1));
            s->n--;
            gone++;
        }
    return gone;
}

int gs_store_load(GsStore *s, const char *text, size_t len) {
    s->n = 0;
    JDoc d;
    if (json_parse(text, len, &d, NULL, 0)) return -1;
    const JVal *apps = json_get(d.root, "apps");
    for (const JVal *a = apps && apps->type == J_OBJ ? apps->kid : NULL; a; a = a->next) {
        if (a->type != J_OBJ || !a->key[0] || strlen(a->key) >= GS_APP_MAX) continue;
        for (const JVal *sc = a->kid; sc; sc = sc->next) {
            if (sc->type != J_OBJ || !gs_id_valid(sc->key)) continue;
            const char *trigger = json_str(sc, "trigger"), *desc = json_str(sc, "description");
            GsKey k;
            int bound = trigger && trigger[0] && !gs_parse(trigger, &k) && gs_kept(&k) == GS_FREE
                        && !gs_store_owner(s, &k, a->key, sc->key);
            gs_store_set(s, a->key, sc->key, desc ? desc : "", bound ? &k : NULL);
        }
    }
    json_free(&d);
    return 0;
}

char *gs_store_json(const GsStore *s) {
    HBuf b = { 0 };
    hb_printf(&b, "{\"apps\":{");
    int first_app = 1;
    for (int i = 0; i < s->n; i++) {
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = !strcmp(s->e[j].app, s->e[i].app);
        if (seen) continue;
        hb_printf(&b, "%s\n", first_app ? "" : ",");
        first_app = 0;
        hb_str(&b, s->e[i].app);
        hb_printf(&b, ":{");
        int first = 1;
        for (int j = i; j < s->n; j++) {
            const GsEntry *e = &s->e[j];
            if (strcmp(e->app, s->e[i].app)) continue;
            char trig[64] = "";
            if (e->bound) gs_format(&e->key, trig, sizeof trig);
            hb_printf(&b, "%s", first ? "" : ",");
            first = 0;
            hb_str(&b, e->id);
            hb_printf(&b, ":{\"description\":");
            hb_str(&b, e->desc);
            hb_printf(&b, ",\"trigger\":");
            hb_str(&b, trig);
            hb_printf(&b, "}");
        }
        hb_printf(&b, "}");
    }
    hb_printf(&b, "}}\n");
    return hb_take(&b);
}
