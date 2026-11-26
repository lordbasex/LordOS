/* hands.c — Hands' state and policy (see hands.h, docs/HANDS.md) */
#include "hands.h"
#include "handsmotion.h"
#include "../sec/json.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- closed words ---- */

static const char *const WHY[HANDS_WHY_COUNT] = {
    "none", "system-layer", "protected", "password", "hidden-from-agent", "allowlist", "authentication-prompt", "locked",
    "pointer-lock", "outside", "obscured", "not-editable", "protected-dialog", "sensitive-clipboard",
};
const char *hands_why_word(int why) { return why >= 0 && why < HANDS_WHY_COUNT ? WHY[why] : "none"; }

static const char *const CALL[HANDS_C_COUNT] = {
    "hello", "status", "windows", "read", "find", "pointer", "scroll", "key", "type", "set-value", "screenshot",
    "open-app", "open-path", "close-window", "app-log", "mode",
};
const char *hands_call_word(int call) { return call >= 0 && call < HANDS_C_COUNT ? CALL[call] : "call"; }

static const char *const POINTER[HANDS_P_COUNT] = {
    "", "left_click", "right_click", "middle_click", "double_click", "triple_click", "hover", "left_click_drag",
};
int hands_pointer_action(const char *s) {
    for (int i = 1; s && i < HANDS_P_COUNT; i++) if (!strcmp(s, POINTER[i])) return i;
    return HANDS_P_NONE;
}
const char *hands_pointer_word(int a) { return a > 0 && a < HANDS_P_COUNT ? POINTER[a] : ""; }

int hands_direction(const char *s) {
    static const char *const D[] = { "", "up", "down", "left", "right" };
    for (int i = 1; s && i < 5; i++) if (!strcmp(s, D[i])) return i;
    return HANDS_DIR_NONE;
}

static const char *const MODE[] = { "default", "human", "machine" };
int hands_mode_of(const char *s) {
    if (!s) return -1;
    if (!s[0]) return HANDS_MODE_DEFAULT;
    for (int i = 0; i < 3; i++) if (!strcmp(s, MODE[i])) return i;
    return -1;
}
const char *hands_mode_word(int mode) { return mode >= 0 && mode <= HANDS_MODE_MACHINE ? MODE[mode] : "default"; }

static const char *const KIND[] = { "other", "claude", "lumo" };
const char *hands_kind_word(int kind) { return kind >= 0 && kind <= HANDS_K_LUMO ? KIND[kind] : "other"; }

/* ---- pure checks ---- */

/* the code point at s (n bytes left), its length in *len; -1 not UTF-8 */
static long utf8_at(const unsigned char *s, size_t n, int *len) {
    unsigned c = s[0];
    int k = c < 0x80 ? 0 : c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
    if (k < 0 || (size_t)k >= n) return -1;
    long cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
    for (int j = 1; j <= k; j++) {
        if ((s[j] & 0xc0) != 0x80) return -1;
        cp = cp << 6 | (s[j] & 0x3f);
    }
    if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
    *len = k + 1;
    return cp;
}

/* controls (C0, DEL, C1), bidi and invisible format characters: never in a name, a value or typed text */
static int format_char(long cp) {
    return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0xad || (cp >= 0x200b && cp <= 0x200f) ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff || (cp >= 0xfff9 && cp <= 0xfffb);
}

int hands_text_ok(const char *s, size_t n, size_t max, int allow_lines) {
    if (!s || n > max || strlen(s) != n) return 0;
    for (size_t i = 0; i < n;) {
        int l = 0;
        long cp = utf8_at((const unsigned char *)s + i, n - i, &l);
        if (cp < 0) return 0;
        if (format_char(cp) && !(allow_lines && (cp == '\n' || cp == '\t'))) return 0;
        i += (size_t)l;
    }
    return 1;
}

int hands_clean_name(const char *in, char *out, size_t n) {
    size_t o = 0;
    int space = 1;
    if (!out || n < 2) return -1;
    out[0] = 0;
    if (!in) return -1;
    size_t len = strnlen(in, 4096);
    for (size_t i = 0; i < len;) {
        int l = 1;
        long cp = utf8_at((const unsigned char *)in + i, len - i, &l);
        if (cp < 0) { i++; continue; }
        if (format_char(cp) || cp == ' ' || cp == 0xa0) {
            if (!space && o + 1 < n && o < HANDS_NAME_MAX) { out[o++] = ' '; space = 1; }
            i += (size_t)l;
            continue;
        }
        if (o + (size_t)l >= n || o + (size_t)l > HANDS_NAME_MAX) break;
        memcpy(out + o, in + i, (size_t)l);
        o += (size_t)l;
        space = 0;
        i += (size_t)l;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return o ? 0 : -1;
}

int hands_kind_of(const char *name) {
    if (!name) return HANDS_K_OTHER;
    char low[HANDS_NAME_MAX + 1];
    size_t i = 0;
    for (; name[i] && i < HANDS_NAME_MAX; i++) low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    low[i] = 0;
    if (strstr(low, "claude")) return HANDS_K_CLAUDE;
    if (strstr(low, "lumo")) return HANDS_K_LUMO;
    return HANDS_K_OTHER;
}

int hands_ref_ok(const char *s) {
    if (!s) return 0;
    if (!s[0]) return 1;
    if (strncmp(s, "ref_", 4)) return 0;
    size_t d = 0;
    for (const char *p = s + 4; *p; p++, d++) if (*p < '0' || *p > '9') return 0;
    return d >= 1 && d <= 9;
}

int hands_id_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > HANDS_ID_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("._:#@+-", c))) return 0;
    }
    return 1;
}

int hands_name_ok(const char *s) { return s && s[0] && hands_text_ok(s, strlen(s), HANDS_ID_MAX, 0); }

int hands_path_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 2 || n >= HANDS_PATH_MAX || s[0] != '/' || !hands_text_ok(s, n, HANDS_PATH_MAX, 0)) return 0;
    for (const char *p = s; (p = strchr(p, '/')); p++) {
        if (!strncmp(p, "/../", 4) || !strcmp(p, "/..") || !strncmp(p, "/./", 3) || !strcmp(p, "/.") || !strncmp(p, "//", 2)) return 0;
    }
    return 1;
}

int hands_filter(const char *s) {
    if (!s || !s[0] || !strcmp(s, "all")) return 0;
    if (!strcmp(s, "interactive")) return 1;
    return -1;
}

int hands_shot_target(const char *s, uint32_t *display, char *win, size_t n) {
    *display = 0;
    if (win && n) win[0] = 0;
    if (!s) return HANDS_SHOT_NONE;
    if (!s[0] || !strcmp(s, "display")) return HANDS_SHOT_DISPLAY;
    if (!strcmp(s, "region")) return HANDS_SHOT_REGION;
    if (!strncmp(s, "display:", 8)) {
        const char *p = s + 8;
        unsigned long v = 0;
        int d = 0;
        for (; *p >= '0' && *p <= '9' && d < 10; p++, d++) v = v * 10 + (unsigned long)(*p - '0');
        if (!d || *p || v > 0xffffffffUL) return HANDS_SHOT_NONE;
        *display = (uint32_t)v;
        return HANDS_SHOT_DISPLAY;
    }
    if (!strncmp(s, "window:", 7) && hands_id_ok(s + 7) && win && strlen(s + 7) < n) {
        snprintf(win, n, "%s", s + 7);
        return HANDS_SHOT_WINDOW;
    }
    return HANDS_SHOT_NONE;
}

/* ---- key chords ---- */

static const struct { const char *name; int code; } KEYS[] = {
    { "enter", KEY_ENTER }, { "return", KEY_ENTER }, { "tab", KEY_TAB }, { "escape", KEY_ESC }, { "esc", KEY_ESC },
    { "backspace", KEY_BACKSPACE }, { "delete", KEY_DELETE }, { "del", KEY_DELETE }, { "insert", KEY_INSERT },
    { "home", KEY_HOME }, { "end", KEY_END }, { "pageup", KEY_PAGEUP }, { "page_up", KEY_PAGEUP }, { "prior", KEY_PAGEUP },
    { "pagedown", KEY_PAGEDOWN }, { "page_down", KEY_PAGEDOWN }, { "next", KEY_PAGEDOWN }, { "up", KEY_UP },
    { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "space", KEY_SPACE }, { "menu", KEY_COMPOSE },
    { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 }, { "f5", KEY_F5 }, { "f6", KEY_F6 },
    { "f7", KEY_F7 }, { "f8", KEY_F8 }, { "f9", KEY_F9 }, { "f10", KEY_F10 }, { "f11", KEY_F11 }, { "f12", KEY_F12 },
};
static const struct { const char *name; unsigned mod; } MODS[] = {
    { "ctrl", HANDS_M_CTRL }, { "control", HANDS_M_CTRL }, { "alt", HANDS_M_ALT }, { "option", HANDS_M_ALT },
    { "shift", HANDS_M_SHIFT }, { "super", HANDS_M_SUPER }, { "meta", HANDS_M_SUPER }, { "cmd", HANDS_M_SUPER },
    { "win", HANDS_M_SUPER },
};

int hands_chord(const char *s, HandsChord *c) {
    memset(c, 0, sizeof *c);
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 64 || !hands_text_ok(s, n, 64, 0)) return -1;
    if (s[0] == '+' || (s[n - 1] == '+' && !(n >= 2 && s[n - 2] == '+'))) return -1;
    char buf[65];
    memcpy(buf, s, n + 1);
    int have_key = 0;
    unsigned lone_mod = 0;
    char *save = NULL;
    /* "+" alone is the plus key: "ctrl++" */
    for (char *part = strtok_r(buf, "+", &save); part; part = strtok_r(NULL, "+", &save)) {
        if (have_key) return -1;                        /* the key comes last */
        int found = 0;
        for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++)
            if (!strcasecmp(part, MODS[i].name)) {
                if (c->mods & MODS[i].mod) return -1;
                c->mods |= MODS[i].mod;
                lone_mod = MODS[i].mod;
                found = 1;
            }
        if (found) continue;
        lone_mod = 0;
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0] && !found; i++)
            if (!strcasecmp(part, KEYS[i].name)) { c->code = KEYS[i].code; found = 1; }
        if (!found) {
            int l = 0;
            long cp = utf8_at((const unsigned char *)part, strlen(part), &l);
            if (cp <= ' ' || (size_t)l != strlen(part) || format_char(cp)) return -1;
            if (cp >= 'A' && cp <= 'Z') { part[0] = (char)(cp + 32); if (!c->mods) c->mods |= HANDS_M_SHIFT; }
            memcpy(c->ch, part, (size_t)l);
            c->ch[l] = 0;
        }
        have_key = 1;
    }
    if (n && s[n - 1] == '+' && n >= 2 && s[n - 2] == '+' && !have_key) { strcpy(c->ch, "+"); have_key = 1; }
    if (!have_key) {
        /* a modifier alone ("super", "shift"): pressed and released as a key of its own */
        if (!lone_mod || (c->mods & (c->mods - 1))) return -1;
        c->code = lone_mod == HANDS_M_CTRL ? KEY_LEFTCTRL : lone_mod == HANDS_M_ALT ? KEY_LEFTALT : lone_mod == HANDS_M_SHIFT ? KEY_LEFTSHIFT : KEY_LEFTMETA;
        c->mods = 0;
    }
    /* the physical keyboard's own chords: ending the session, a text console, the panic key, Lord Flow's rescue */
    unsigned ca = HANDS_M_CTRL | HANDS_M_ALT;
    if ((c->mods & ca) == ca) {
        if (c->code == KEY_BACKSPACE || c->code == KEY_ESC || c->code == KEY_DELETE) return -1;
        if (c->code >= KEY_F1 && c->code <= KEY_F10) return -1;
        if (c->code == KEY_F11 || c->code == KEY_F12) return -1;
    }
    return 0;
}

int hands_chords(const char *s, HandsChord *c, int max, int *n) {
    *n = 0;
    size_t len = s ? strlen(s) : 0;
    if (len < 1 || len > 256) return -1;
    char buf[257];
    memcpy(buf, s, len + 1);
    char *save = NULL;
    for (char *w = strtok_r(buf, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (*n >= max || hands_chord(w, &c[*n])) return -1;
        (*n)++;
    }
    return *n ? 0 : -1;
}

/* ---- the security-state natives an agent's input never sets off (js.c prof_call) ----
 * the vault's requests (vault.request: reveal, copy, edit…), the keyring, filling a password into a terminal, the
 * firewall's changes, permissions, administrator actions (admin.request), Sentinela's
 * blocklist, theme trust and removal, signing keys, screen-sharing answers, Hands' own switch, power; lord.clipboard.get
 * answers "" while the clipboard holds a secret. A name ending in "." covers its whole namespace. */
int hands_native_guard(const char *name) {
    static const char *const refuse[] = {
        "vault.request", "vault.fidoAttach", "vault.promptDone", "keyring.request", "admin.request", "term.fill", "term.saveSecret", "term.dropSecret", "perms.set",
        "fw.setList", "fw.addList", "fw.removeList", "fw.setAppRule", "fw.setTrusted", "fw.setRestricted", "fw.setMode",
        "fw.addRule", "fw.removeRule", "fw.updateRule", "sentinela.block", "sentinela.unblock",
        "themes.trust", "themes.remove", "sign.share", "sign.restore", "share.answer", "hands.serve", "hands.answer",
        "hands.motion", "hands.cursor",
        "session.power",
    };
    if (!name) return HANDS_G_NONE;
    if (!strcmp(name, "clipboard.get")) return HANDS_G_CLIP;
    for (size_t i = 0; i < sizeof refuse / sizeof refuse[0]; i++) {
        size_t l = strlen(refuse[i]);
        if (refuse[i][l - 1] == '.' ? !strncmp(name, refuse[i], l) : !strcmp(name, refuse[i])) return HANDS_G_REFUSE;
    }
    return HANDS_G_NONE;
}

/* ---- rate ---- */

int hands_rate_take(HandsBucket *b, double now, double per_s, double burst) {
    if (!b->init) { b->init = 1; b->tokens = burst; b->last = now; }
    if (now > b->last) { b->tokens += (now - b->last) * per_s / 1000.0; b->last = now; }
    if (b->tokens > burst) b->tokens = burst;
    if (b->tokens < 1) return 0;
    b->tokens -= 1;
    return 1;
}

static const char *result_word(int r) {
    switch (r) {
    case HANDS_OK: return "ok";
    case HANDS_E_OFF: return "off";
    case HANDS_E_ASKING: return "asking";
    case HANDS_E_DENIED: return "denied";
    case HANDS_E_STOPPED: return "stopped";
    case HANDS_E_ACTIVE: return "user-active";
    case HANDS_E_REFUSED: return "refused";
    case HANDS_E_RATE: return "rate-limited";
    case HANDS_E_NOTFOUND: return "not-found";
    case HANDS_E_ARGS: return "bad-arguments";
    case HANDS_E_BUSY: return "busy";
    default: return "failed";
    }
}

size_t hands_log_line(char *out, size_t n, int kind, int call, int action, int result, int why) {
    if (!out || !n) return 0;
    int k = snprintf(out, n, "hands: %s %s%s%s %s%s%s", hands_kind_word(kind), hands_call_word(call),
                     call == HANDS_C_POINTER && action > 0 ? " " : "", call == HANDS_C_POINTER ? hands_pointer_word(action) : "",
                     result_word(result), result == HANDS_E_REFUSED ? " " : "", result == HANDS_E_REFUSED ? hands_why_word(why) : "");
    return k < 0 ? 0 : (size_t)k < n ? (size_t)k : n - 1;
}

/* ---- JSON text ---- */

typedef struct { char *p; size_t n, cap; int oom; } Jb;
static void jb_put(Jb *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void jb_s(Jb *b, const char *s) { jb_put(b, s, strlen(s)); }
static void jb_q(Jb *b, const char *s) {
    jb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char e[8];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = (char)*p; jb_put(b, e, 2); }
        else if (*p < 0x20) { snprintf(e, sizeof e, "\\u%04x", *p); jb_put(b, e, 6); }
        else jb_put(b, (const char *)p, 1);
    }
    jb_put(b, "\"", 1);
}
static void jb_num(Jb *b, double v) { char t[48]; snprintf(t, sizeof t, "%.2f", v); jb_s(b, t); }
/* the text, now the caller's (b is empty again) */
static char *jb_take(Jb *b) {
    char *p = b->p;
    int oom = b->oom;
    memset(b, 0, sizeof *b);
    if (oom) { free(p); return NULL; }
    return p ? p : strdup("");
}

/* ---- state ---- */

enum { CS_FREE = 0, CS_ASKING, CS_GRANTED, CS_DENIED, CS_STOPPED };
static const char *const CSW[] = { "free", "asking", "granted", "denied", "stopped" };

/* A client is one bus connection: its grant is that connection's and dies with it (a new connection, even with the
 * same name, asks again). The name it gives and the program path handsbus.c read are shown in the prompt as reported,
 * never as verified: any program of the same user can claim any name or run inside any program (docs/HANDS.md "Threat
 * model"). The control is the person's answer, the banner, Stop and the panic key. */
typedef struct {
    char bus[64], name[HANDS_NAME_MAX + 1], exe[HANDS_EXE_MAX];
    int kind, st;
    int mode;                           /* SetMode: HANDS_MODE_DEFAULT follows the desktop's */
    unsigned long actions;
} Client;

/* A human-mode action in progress (one at a time: there is one agent cursor). Its request is a copy (its text owned);
 * every step goes through the same gate as a machine-mode action, at the moment it happens. */
enum { PH_GLIDE = 1, PH_HOVER, PH_DRAG, PH_SCROLL, PH_TYPE };
typedef struct {
    int on;
    char bus[64];
    HandsReq r;
    char *text;                         /* r.text, owned */
    int phase;
    double t0, next;
    HandsGlide g;
    double tx, ty, width;               /* the target */
    int steps[HANDS_SCROLL_STEPS], nsteps, step;
    size_t off, chars;                  /* typing */
    int held;                           /* a drag's button is down */
} Plan;

static struct {
    const HandsHost *h;
    int on, qa, injecting;
    char allow[HANDS_ALLOW_MAX][HANDS_ID_MAX + 1];
    int nallow;
    Client c[HANDS_CLIENTS];
    double person;
    int person_seen;
    double last_agent;                  /* when the agent's last input was delivered */
    double person_press;                /* the person's last real click or key (physical, or a remote viewer's) */
    unsigned long seq, agent_seq, press_seq;   /* which came last: the agent's input or the person's click or key */
    unsigned long events;
    HandsBucket input, shot, read, hello;
    int motion, reduce;                 /* Settings → AI → Hands → Agent motion; reduce motion */
    double cx, cy;                      /* the agent cursor */
    int cur_shown;
    double fbox[4];                     /* the field the keys go to (the shell's focus), w 0 unknown */
    HandsDone done;
    uint32_t seed;
    Plan P;
} S;

static void plan_end(int rc, int why, const char *out, int reply);
static void cur(int what, double x, double y, double w, double h);

void hands_init(const HandsHost *h) { S.h = h; }
void hands_on_done(HandsDone fn) { S.done = fn; }
void hands_motion(int mode, int reduce) {
    S.motion = mode == HANDS_MODE_MACHINE ? HANDS_MODE_MACHINE : HANDS_MODE_HUMAN;
    S.reduce = !!reduce;
}
int hands_motion_default(void) { return S.qa ? HANDS_MODE_MACHINE : S.motion == HANDS_MODE_MACHINE ? HANDS_MODE_MACHINE : HANDS_MODE_HUMAN; }
int hands_acting(void) { return S.P.on; }
const HandsHost *hands_host(void) { return S.h; }
int hands_on(void) { return S.on; }
int hands_qa_mode(void) { return S.qa; }
static double now(void) { return S.h && S.h->now ? S.h->now() : 0; }
int hands_injecting(void) { return S.injecting; }

/* A Hands client is granted and connected (the banner shows) */
int hands_active(void) {
    for (int i = 0; i < HANDS_CLIENTS; i++) if (S.c[i].st == CS_GRANTED) return 1;
    return 0;
}

/* Fail closed: the input that set something off is the agent's unless the person's own click or key came after the
 * agent's last input (never mere motion, never a time that passed, never the agent leaving). lord-server's
 * security-state actions refuse then (hands_native_guard in js.c, the broker, term.fill, the clipboard). */
int hands_agent_owns_input(void) {
    if (S.injecting) return 1;
    /* sticky: the agent's last input stays its own until the person's click or key, even after it disconnects (a
     * deferred action of its own never runs as the person's) */
    return S.agent_seq && S.press_seq < S.agent_seq;
}
int hands_agent_recent(void) { return hands_agent_owns_input(); }

void hands_person_press(void) {
    if (S.injecting) return;
    S.person_press = now();
    S.press_seq = ++S.seq;
}
unsigned long hands_ai_events(void) { return S.events; }


char *hands_status_json(void) {
    Jb b = { 0 };
    jb_s(&b, "{\"on\":");
    jb_s(&b, S.on ? "true" : "false");
    jb_s(&b, ",\"qa\":");
    jb_s(&b, S.qa ? "true" : "false");
    jb_s(&b, ",\"allow\":[");
    for (int i = 0; i < S.nallow; i++) { if (i) jb_s(&b, ","); jb_q(&b, S.allow[i]); }
    jb_s(&b, "],\"clients\":[");
    int first = 1;
    for (int i = 0; i < HANDS_CLIENTS; i++) {
        const Client *k = &S.c[i];
        if (k->st == CS_FREE) continue;
        if (!first) jb_s(&b, ",");
        first = 0;
        char a[32];
        snprintf(a, sizeof a, "%lu", k->actions);
        jb_s(&b, "{\"bus\":");
        jb_q(&b, k->bus);
        jb_s(&b, ",\"name\":");
        jb_q(&b, k->name);
        jb_s(&b, ",\"exe\":");
        jb_q(&b, k->exe);
        jb_s(&b, ",\"kind\":");
        jb_q(&b, hands_kind_word(k->kind));
        jb_s(&b, ",\"state\":");
        jb_q(&b, CSW[k->st]);
        jb_s(&b, ",\"actions\":");
        jb_s(&b, a);
        jb_s(&b, "}");
    }
    char e[96];
    snprintf(e, sizeof e, "],\"motion\":\"%s\",\"events\":%lu}", hands_mode_word(hands_motion_default()), S.events);
    jb_s(&b, e);
    return jb_take(&b);
}

static void tell(void) {
    if (!S.h || !S.h->event) return;
    char *st = hands_status_json();
    if (!st) return;
    Jb b = { 0 };
    jb_s(&b, "{\"t\":\"clients\",\"status\":");
    jb_s(&b, st);
    jb_s(&b, "}");
    free(st);
    char *j = jb_take(&b);
    if (j) S.h->event(j);
    free(j);
}

static void log_call(int kind, int call, int action, int result, int why) {
    if (!S.h || !S.h->event) return;
    char line[160], j[256];
    hands_log_line(line, sizeof line, kind, call, action, result, why);
    snprintf(j, sizeof j, "{\"t\":\"log\",\"line\":\"%s\"}", line);   /* closed words only: no quoting needed */
    S.h->event(j);
}

static Client *client_of(const char *bus) {
    for (int i = 0; bus && i < HANDS_CLIENTS; i++) if (S.c[i].st != CS_FREE && !strcmp(S.c[i].bus, bus)) return &S.c[i];
    return NULL;
}


void hands_serve(int on, const char *const *allow, int nallow) {
    int was = S.on;
    S.on = !!on;
    S.nallow = 0;
    for (int i = 0; allow && i < nallow && S.nallow < HANDS_ALLOW_MAX; i++)
        if (hands_id_ok(allow[i])) snprintf(S.allow[S.nallow++], sizeof S.allow[0], "%s", allow[i]);
    if (was && !S.on) hands_stop("off");
    else tell();
}

void hands_answer(const char *bus, int allow) {
    Client *k = client_of(bus);
    if (!k || k->st != CS_ASKING) return;
    k->st = allow ? CS_GRANTED : CS_DENIED;
    tell();
}

void hands_stop(const char *why_word) {
    (void)why_word;
    if (S.P.on) plan_end(HANDS_E_STOPPED, 0, NULL, 1);
    cur(HANDS_CUR_HIDE, 0, 0, 0, 0);
    for (int i = 0; i < HANDS_CLIENTS; i++) if (S.c[i].st != CS_FREE) S.c[i].st = CS_STOPPED;
    /* a stopped connection never acts again: the next one asks again */
    if (S.h && S.h->event) {
        char j[96];
        snprintf(j, sizeof j, "{\"t\":\"stopped\",\"why\":\"%s\"}", why_word && hands_id_ok(why_word) ? why_word : "stop");
        S.h->event(j);
    }
    tell();
}

void hands_panic(void) {
    S.on = 0;
    hands_stop("panic");
}

void hands_away(void) { if (!S.qa) hands_stop("locked"); }

void hands_person_input(void) {
    if (S.injecting) return;
    S.person = now();
    S.person_seen = 1;
    /* the person's own input wins at once: an agent's glide, drag, scroll or typing in progress stops where it is */
    if (S.P.on && !S.qa) plan_end(HANDS_E_ACTIVE, 0, NULL, 1);
}

void hands_qa(int on) {
    S.qa = !!on;
    if (on) S.on = 1;
    tell();
}

int hands_hello(const char *bus, const char *name, const char *exe) {
    if (!bus || !bus[0] || strlen(bus) >= sizeof S.c[0].bus) return HANDS_E_ARGS;
    if (!S.h || !hands_rate_take(&S.hello, now(), 2, 8)) return HANDS_E_RATE;
    if (!S.on) return HANDS_E_OFF;
    char clean[HANDS_NAME_MAX + 1];
    if (hands_clean_name(name, clean, sizeof clean)) return HANDS_E_ARGS;
    Client *k = client_of(bus);
    if (k && k->st == CS_STOPPED) return HANDS_E_STOPPED;
    if (k && strcmp(k->name, clean)) return HANDS_E_ARGS;      /* one name per connection */
    if (!k) {
        for (int i = 0; i < HANDS_CLIENTS && !k; i++) if (S.c[i].st == CS_FREE) k = &S.c[i];
        if (!k) return HANDS_E_BUSY;
        memset(k, 0, sizeof *k);
        snprintf(k->bus, sizeof k->bus, "%s", bus);
        snprintf(k->name, sizeof k->name, "%s", clean);
        snprintf(k->exe, sizeof k->exe, "%s", exe && exe[0] == '/' && hands_text_ok(exe, strlen(exe), HANDS_EXE_MAX - 1, 0) ? exe : "");
        k->kind = hands_kind_of(clean);
        k->st = S.qa ? CS_GRANTED : CS_ASKING;      /* every new connection asks: a grant is never a name's */
        log_call(k->kind, HANDS_C_HELLO, 0, k->st == CS_GRANTED ? HANDS_OK : k->st == CS_DENIED ? HANDS_E_DENIED : HANDS_E_ASKING, 0);
        tell();
    }
    return k->st == CS_GRANTED ? HANDS_OK : k->st == CS_DENIED ? HANDS_E_DENIED : k->st == CS_STOPPED ? HANDS_E_STOPPED : HANDS_E_ASKING;
}

void hands_gone(const char *bus) {
    Client *k = client_of(bus);
    if (!k) return;
    if (S.P.on && !strcmp(S.P.bus, bus)) plan_end(HANDS_E_STOPPED, 0, NULL, 0);     /* no one to answer */
    memset(k, 0, sizeof *k);
    if (!hands_active()) cur(HANDS_CUR_HIDE, 0, 0, 0, 0);
    tell();
}

void hands_lost(void) {
    if (S.P.on) plan_end(HANDS_E_STOPPED, 0, NULL, 0);
    memset(S.c, 0, sizeof S.c);
    cur(HANDS_CUR_HIDE, 0, 0, 0, 0);
    tell();
}

static int is_input(int call) {
    return call == HANDS_C_POINTER || call == HANDS_C_SCROLL || call == HANDS_C_KEY || call == HANDS_C_TYPE ||
           call == HANDS_C_SETVALUE || call == HANDS_C_OPENAPP || call == HANDS_C_OPENPATH || call == HANDS_C_CLOSE;
}

int hands_may(const char *bus, int call) {
    if (!S.h) return HANDS_E_FAILED;
    if (!S.on) return HANDS_E_OFF;
    Client *k = client_of(bus);
    if (!k) return HANDS_E_DENIED;                  /* Hello first */
    if (k->st == CS_ASKING) return HANDS_E_ASKING;
    if (k->st == CS_DENIED) return HANDS_E_DENIED;
    if (k->st == CS_STOPPED) return HANDS_E_STOPPED;
    double t = now();
    if (is_input(call) && !S.qa && S.person_seen && t - S.person < HANDS_YIELD_MS) return HANDS_E_ACTIVE;
    int ok = 1;
    if (call == HANDS_C_SHOT) ok = hands_rate_take(&S.shot, t, 2, 4);
    else if (is_input(call)) ok = hands_rate_take(&S.input, t, 8, 16);
    else ok = hands_rate_take(&S.read, t, 10, 20);
    return ok ? HANDS_OK : HANDS_E_RATE;
}

/* ---- the calls ---- */

static void allow_json(Jb *b) {
    jb_s(b, "\"allow\":[");
    for (int i = 0; i < S.nallow; i++) { if (i) jb_s(b, ","); jb_q(b, S.allow[i]); }
    jb_s(b, "]");
}

/* the shell's answer to op: 0 with its JSON in *d, or an error (its word into *why) */
static int ask(const char *op, Jb *args, JDoc *d, int *why) {
    char *a = jb_take(args);
    char *r = a && S.h->shell ? S.h->shell(op, a) : NULL;
    free(a);
    memset(d, 0, sizeof *d);
    if (!r) return HANDS_E_FAILED;
    int bad = json_parse(r, strlen(r), d, NULL, 0) || !d->root || d->root->type != J_OBJ;
    free(r);
    if (bad) { json_free(d); return HANDS_E_FAILED; }
    const char *e = json_str(d->root, "error");
    if (!e) return HANDS_OK;
    int code = HANDS_E_FAILED;
    if (!strcmp(e, "not-found")) code = HANDS_E_NOTFOUND;
    for (int w = 1; w < HANDS_WHY_COUNT; w++) if (!strcmp(e, WHY[w])) { code = HANDS_E_REFUSED; *why = w; }
    json_free(d);
    return code;
}

/* a call to the shell whose JSON answer goes back to the agent as it is */
static int shell_pass(const char *op, Jb *args, char **out, int *why) {
    char *a = jb_take(args);
    char *r = a && S.h->shell ? S.h->shell(op, a) : NULL;
    free(a);
    if (!r) return HANDS_E_FAILED;
    JDoc d;
    if (json_parse(r, strlen(r), &d, NULL, 0) || !d.root || d.root->type != J_OBJ) { free(r); return HANDS_E_FAILED; }
    const char *e = json_str(d.root, "error");
    int code = HANDS_OK;
    if (e) {
        code = HANDS_E_FAILED;
        if (!strcmp(e, "not-found")) code = HANDS_E_NOTFOUND;
        for (int w = 1; w < HANDS_WHY_COUNT; w++) if (!strcmp(e, WHY[w])) { code = HANDS_E_REFUSED; *why = w; }
    } else {
        const char *t = json_str(d.root, "text");
        *out = t ? strdup(t) : NULL;
    }
    json_free(&d);
    if (code == HANDS_OK && !*out) *out = r;
    else free(r);
    return code;
}

/* where a ref is: its node, the centre of its box, and whether it is a secret field */
static int resolve(const char *ref, int *node, double *x, double *y, double box[4], int *secure, int *why) {
    Jb a = { 0 };
    jb_s(&a, "{\"ref\":");
    jb_q(&a, ref);
    jb_s(&a, ",");
    allow_json(&a);
    jb_s(&a, "}");
    JDoc d;
    int r = ask("resolve", &a, &d, why);
    if (r) return r;
    *node = (int)json_num(d.root, "node", 0);
    if (secure) *secure = json_bool(d.root, "secure", 0);
    double bx = json_num(d.root, "x", -1), by = json_num(d.root, "y", -1), bw = json_num(d.root, "w", 0), bh = json_num(d.root, "h", 0);
    json_free(&d);
    if (*node <= 0 || bw < 1 || bh < 1) return HANDS_E_NOTFOUND;
    if (box) { box[0] = bx; box[1] = by; box[2] = bw; box[3] = bh; }
    *x = bx + bw / 2;
    *y = by + bh / 2;
    return HANDS_OK;
}

/* the app of what is at node (its window's, or the window in front for the bar and the menus) is in the allowlist */
static int allowlist_node(int node, int *why) {
    if (!S.nallow) return HANDS_OK;
    Jb a = { 0 };
    char t[64];
    snprintf(t, sizeof t, "{\"node\":%d,", node);
    jb_s(&a, t);
    allow_json(&a);
    jb_s(&a, "}");
    JDoc d;
    int r = ask("target", &a, &d, why);
    if (r) return r;
    int ok = json_bool(d.root, "allowed", 0);
    json_free(&d);
    if (!ok) { *why = HANDS_WHY_ALLOWLIST; return HANDS_E_REFUSED; }
    return HANDS_OK;
}

/* ---- the gate: every action passes here (pointer, drag, scroll, set-value, keys, typing) ----
 * GATE_POINT: a place on the screen and, when a ref named it, that ref's own node: inside the desktop; not the system
 * band, a protected or handsDeny node or an authentication prompt (the ref's node and the node hit there both); the ref
 * really under the point (not covered by something else); never a secret field; in the allowlist. GATE_KEYS: where a
 * key would go now: no prompt, protected window, password field or app outside the allowlist. A screenshot has its own
 * checks (do_shot) on top of the same node places. */
enum { GATE_POINT = 1, GATE_KEYS = 2 };
typedef struct {
    double x, y;
    int ref_node;               /* 0: a bare point */
    int secure;                 /* the ref is a secret field (the shell's resolve) */
    int pointer;                /* the pointer moves: refused while a pointer lock holds it */
    int node;                   /* out: the node hit */
    double bw, bh;              /* out: the ref's box (a glide's target size), 0 a bare point */
} Gate;

/* the agent cursor: where it is, and what the person sees of an action (HANDS_CUR_*) */
static void cur(int what, double x, double y, double w, double h) {
    if (!S.h || !S.h->cursor) return;
    if (what == HANDS_CUR_HIDE) {
        if (!S.cur_shown) return;
        S.cur_shown = 0;
    } else {
        if (what != HANDS_CUR_PULSE) { S.cx = x; S.cy = y; }
        S.cur_shown = 1;
    }
    S.h->cursor(what, x, y, w, h);
}
/* where the cursor rests while it types: inside the field's end (its box from the shell's focus), else where it is */
static void type_spot(double *x, double *y) {
    if (S.fbox[2] >= 4 && S.fbox[3] >= 4) {
        double in = S.fbox[2] > 40 ? 14 : S.fbox[2] / 3;
        *x = S.fbox[0] + S.fbox[2] - in;
        *y = S.fbox[1] + S.fbox[3] * 0.7;
    } else { *x = S.cx; *y = S.cy; }
}
static void pulse(void) {
    double x, y;
    type_spot(&x, &y);
    /* never shown yet and no field known: it appears where the person's pointer is, not in a corner */
    if (!S.cur_shown && !(S.fbox[2] >= 4) && !(S.h->pointer_at && S.h->pointer_at(&x, &y))) return;
    if ((!S.P.on || !S.cur_shown) && (x != S.cx || y != S.cy || !S.cur_shown)) cur(HANDS_CUR_MOVE, x, y, 0, 0);
    cur(HANDS_CUR_PULSE, S.fbox[0], S.fbox[1], S.fbox[2], S.fbox[3]);
}

static int gate(int kind, Gate *g, int *why) {
    if (kind == GATE_KEYS) {
        int w = S.h->keys_place ? S.h->keys_place() : 0;
        if (w) { *why = w; return HANDS_E_REFUSED; }
        Jb a = { 0 };
        jb_s(&a, "{");
        allow_json(&a);
        jb_s(&a, "}");
        JDoc d;
        int r = ask("focus", &a, &d, why);
        if (r) return r;
        int secure = json_bool(d.root, "secure", 0), denied = json_bool(d.root, "denied", 0), ok = json_bool(d.root, "allowed", 1);
        /* the field's box: where the agent cursor rests and pulses while it types (only a box, never what is in it) */
        const JVal *fb = json_get(d.root, "box");
        memset(S.fbox, 0, sizeof S.fbox);
        if (fb && fb->type == J_ARR && fb->len == 4) {
            int i = 0;
            for (const JVal *v = fb->kid; v && i < 4; v = v->next, i++) S.fbox[i] = v->type == J_NUM && v->num > -1e6 && v->num < 1e6 ? v->num : 0;
        }
        /* the field and the node with the keyboard focus, checked here too (the system band, protected, handsDeny and
         * every node they are in): the shell's word is not the only one */
        int nw = 0;
        const JVal *l = json_get(d.root, "nodes");
        for (const JVal *v = l && l->type == J_ARR ? l->kid : NULL; v && !nw; v = v->next)
            if (v->type == J_NUM && v->num > 0 && v->num < 1e9 && S.h->node_place) nw = S.h->node_place((int)v->num);
        /* a dialog that owns the keyboard and is the person's own (handsDeny, protected, the system band): every key refused,
         * whatever the shell said, its root checked here too */
        int dialog = json_bool(d.root, "dialog", 0);
        double dn = json_num(d.root, "dialogNode", 0);
        if (!dialog && dn > 0 && dn < 1e9 && S.h->node_place && S.h->node_place((int)dn)) dialog = 1;
        json_free(&d);
        if (dialog) { *why = HANDS_WHY_DIALOG; return HANDS_E_REFUSED; }
        if (nw) { *why = nw == HANDS_WHY_PROTECTED ? HANDS_WHY_PASSWORD : nw; return HANDS_E_REFUSED; }
        if (secure) { *why = HANDS_WHY_PASSWORD; return HANDS_E_REFUSED; }
        if (denied) { *why = HANDS_WHY_DENIED_NODE; return HANDS_E_REFUSED; }
        if (!ok) { *why = HANDS_WHY_ALLOWLIST; return HANDS_E_REFUSED; }
        return HANDS_OK;
    }
    if (g->pointer && S.h->pointer_locked && S.h->pointer_locked()) { *why = HANDS_WHY_POINTER_LOCK; return HANDS_E_REFUSED; }
    if (g->secure) { *why = HANDS_WHY_PASSWORD; return HANDS_E_REFUSED; }
    if (g->ref_node && S.h->node_place) {
        int w = S.h->node_place(g->ref_node);
        if (w) { *why = w; return HANDS_E_REFUSED; }
    }
    int W = 0, H = 0;
    S.h->size(&W, &H);
    if (!(g->x >= 0 && g->y >= 0 && g->x < W && g->y < H)) { *why = HANDS_WHY_OUTSIDE; return HANDS_E_REFUSED; }
    g->node = 0;
    int w = S.h->place(g->x, g->y, &g->node);
    if (w) { *why = w; return HANDS_E_REFUSED; }
    if (g->ref_node && (!S.h->same_target || !S.h->same_target(g->node, g->ref_node))) { *why = HANDS_WHY_OBSCURED; return HANDS_E_REFUSED; }
    return allowlist_node(g->node, why);
}

/* a ref's place, or a bare point's, through the gate */
static int gate_ref(const char *ref, int pointer, Gate *g, int *why) {
    memset(g, 0, sizeof *g);
    g->pointer = pointer;
    double box[4] = { 0 };
    int rc = resolve(ref, &g->ref_node, &g->x, &g->y, box, &g->secure, why);
    g->bw = box[2];
    g->bh = box[3];
    return rc ? rc : gate(GATE_POINT, g, why);
}
static int gate_xy(double x, double y, int pointer, Gate *g, int *why) {
    memset(g, 0, sizeof *g);
    g->x = x;
    g->y = y;
    g->pointer = pointer;
    return gate(GATE_POINT, g, why);
}

static int do_pointer(const HandsReq *r, char **out, int *why) {
    Gate g, e;
    /* a middle click pastes the primary selection: never while the clipboard holds a secret */
    if (r->action == HANDS_P_MIDDLE && S.h->clip_sensitive && S.h->clip_sensitive()) { *why = HANDS_WHY_SENSITIVE_CLIP; return HANDS_E_REFUSED; }
    int rc = r->ref[0] ? gate_ref(r->ref, 1, &g, why) : gate_xy(r->x, r->y, 1, &g, why);
    if (rc) return rc;
    if (r->action == HANDS_P_DRAG && (rc = gate_xy(r->x2, r->y2, 1, &e, why))) return rc;
    double x = g.x, y = g.y;
    S.injecting = 1;
    S.h->move(x, y);
    int n = 1;
    switch (r->action) {
    case HANDS_P_HOVER: break;
    case HANDS_P_DRAG:
        S.h->button(0, 1);
        for (int i = 1; i <= 8; i++) S.h->move(x + (r->x2 - x) * i / 8, y + (r->y2 - y) * i / 8);
        S.h->button(0, 0);
        n = 10;
        break;
    default: {
        int b = r->action == HANDS_P_RIGHT ? 2 : r->action == HANDS_P_MIDDLE ? 1 : 0;
        int times = r->action == HANDS_P_DOUBLE ? 2 : r->action == HANDS_P_TRIPLE ? 3 : 1;
        for (int i = 0; i < times; i++) { S.h->button(b, 1); S.h->button(b, 0); }
        n = 1 + 2 * times;
    }
    }
    S.injecting = 0;
    S.last_agent = now();
    S.agent_seq = ++S.seq;
    S.events += (unsigned long)n;
    /* what the person sees: in human mode a ripple where it clicks (the cursor is already there), in machine mode a
     * flash where it landed */
    if (r->action == HANDS_P_DRAG) cur(S.P.on ? HANDS_CUR_MOVE : HANDS_CUR_FLASH, r->x2, r->y2, 0, 0);
    else if (r->action == HANDS_P_HOVER) cur(S.P.on ? HANDS_CUR_MOVE : HANDS_CUR_FLASH, x, y, 0, 0);
    else cur(S.P.on ? HANDS_CUR_CLICK : HANDS_CUR_FLASH, x, y, 0, 0);
    Jb o = { 0 };
    jb_s(&o, "{\"x\":");
    jb_num(&o, x);
    jb_s(&o, ",\"y\":");
    jb_num(&o, y);
    jb_s(&o, "}");
    *out = jb_take(&o);
    return HANDS_OK;
}

static int do_scroll(const HandsReq *r, int *why) {
    Gate g;
    int rc = r->ref[0] ? gate_ref(r->ref, 1, &g, why) : gate_xy(r->x, r->y, 1, &g, why);
    if (rc) return rc;
    double x = g.x, y = g.y;
    int k = r->amount < 1 ? 3 : r->amount;
    S.injecting = 1;
    S.h->move(x, y);
    int dx = r->dir == HANDS_DIR_LEFT ? -k : r->dir == HANDS_DIR_RIGHT ? k : 0;
    int dy = r->dir == HANDS_DIR_UP ? k : r->dir == HANDS_DIR_DOWN ? -k : 0;
    S.h->wheel(dx, dy);
    S.injecting = 0;
    S.last_agent = now();
    S.agent_seq = ++S.seq;
    S.events += 2;
    cur(S.P.on ? HANDS_CUR_MOVE : HANDS_CUR_FLASH, x, y, 0, 0);
    return HANDS_OK;
}

/* Ctrl+V, Ctrl+Shift+V, Shift+Insert, Super+V: a paste */
int hands_paste_chord(const HandsChord *c) {
    if (c->code == KEY_INSERT && (c->mods & HANDS_M_SHIFT)) return 1;
    return (c->ch[0] == 'v' || c->ch[0] == 'V') && !c->ch[1] && (c->mods & (HANDS_M_CTRL | HANDS_M_SUPER));
}

static int do_key(const HandsReq *r, int *why) {
    HandsChord c[8];
    int n = 0;
    if (hands_chords(r->text, c, 8, &n)) return HANDS_E_ARGS;
    int rc = 0;
    int rep = r->repeat < 1 ? 1 : r->repeat;
    /* the whole key gate before every chord and every repeat: a Tab may have moved the focus into a denied control */
    for (int k = 0; k < rep && !rc; k++)
        for (int i = 0; i < n && !rc; i++) {
            if ((rc = gate(GATE_KEYS, NULL, why))) break;
            pulse();
            if (hands_paste_chord(&c[i]) && S.h->clip_sensitive && S.h->clip_sensitive()) { *why = HANDS_WHY_SENSITIVE_CLIP; rc = HANDS_E_REFUSED; break; }
            S.injecting = 1;
            int kr = S.h->key(&c[i]);
            S.injecting = 0;
            if (kr == -2) { *why = HANDS_WHY_SENSITIVE_CLIP; rc = HANDS_E_REFUSED; }
            else if (kr) rc = HANDS_E_ARGS;
            S.last_agent = now();
    S.agent_seq = ++S.seq;
            S.events += 2;
        }
    S.last_agent = now();
    S.agent_seq = ++S.seq;
    return rc;
}

/* the text in pieces: a line break or a tab is a key of its own (Enter, Tab), and the field with the keyboard is checked
 * again after each one (a Tab may have moved it to a password field) */
static int do_type(const HandsReq *r, int *why) {
    int rc = gate(GATE_KEYS, NULL, why);
    if (rc) return rc;
    pulse();
    const char *t = r->text;
    size_t n = r->text_len;
    for (size_t off = 0; off < n && !rc;) {
        size_t end = off;
        while (end < n && t[end] != '\n' && t[end] != '\t') end++;
        S.injecting = 1;
        int w = end > off ? S.h->type(t + off, end - off) : 0;
        S.injecting = 0;
        S.last_agent = now();
    S.agent_seq = ++S.seq;
        S.events += end - off;
        if (w) { *why = w; return HANDS_E_REFUSED; }
        if (end < n) {
            /* the field is checked again before the Enter or Tab, then after it (the Tab may have moved the focus) */
            if ((rc = gate(GATE_KEYS, NULL, why))) break;
            HandsChord c = { 0 };
            c.code = t[end] == '\n' ? KEY_ENTER : KEY_TAB;
            S.injecting = 1;
            S.h->key(&c);
            S.injecting = 0;
            S.last_agent = now();
    S.agent_seq = ++S.seq;
            S.events++;
        }
        off = end + 1;
        if (end < n && off < n) rc = gate(GATE_KEYS, NULL, why);
    }
    return rc;
}

static int do_setvalue(const HandsReq *r, int *why) {
    Gate g;
    int rc = gate_ref(r->ref, 0, &g, why);      /* the ref's own node, the node under it, a secret field, the allowlist */
    if (rc) return rc;
    Jb a = { 0 };
    jb_s(&a, "{\"ref\":");
    jb_q(&a, r->ref);
    jb_s(&a, ",\"value\":");
    jb_q(&a, r->text);
    jb_s(&a, ",");
    allow_json(&a);
    jb_s(&a, "}");
    JDoc d;
    S.injecting = 1;
    rc = ask("setvalue", &a, &d, why);
    S.injecting = 0;
    S.last_agent = now();
    S.agent_seq = ++S.seq;
    if (rc) return rc;
    json_free(&d);
    S.events++;
    cur(S.P.on ? HANDS_CUR_CLICK : HANDS_CUR_FLASH, g.x, g.y, 0, 0);
    return HANDS_OK;
}

/* With an allowlist a screenshot shows only allowed apps: a window of one, or a region that shows some of one (a window,
 * or a menu, popover or sheet tied to it by handsWin); everything else in it (other apps' windows, the Dock, the
 * wallpaper) is painted black as the system band is, so the region's rules are the screenshot's: what can't be shown is
 * black, never a refusal of what can. */
static int blackout(int *nodes, int max, int *n, int *why) {
    *n = 0;
    if (!S.nallow) return HANDS_OK;
    Jb a = { 0 };
    jb_s(&a, "{");
    allow_json(&a);
    jb_s(&a, "}");
    JDoc d;
    int rc = ask("blackout", &a, &d, why);
    if (rc) return rc;
    const JVal *l = json_get(d.root, "nodes");
    for (const JVal *v = l && l->type == J_ARR ? l->kid : NULL; v && *n < max; v = v->next)
        if (v->type == J_NUM && v->num > 0 && v->num < 1e9) nodes[(*n)++] = (int)v->num;
    int over = !l || l->type != J_ARR || (int)l->len > max;
    json_free(&d);
    if (over) { *why = HANDS_WHY_ALLOWLIST; return HANDS_E_REFUSED; }     /* can't black out all of them: nothing taken */
    return HANDS_OK;
}

static int do_shot(const HandsReq *r, char **out, uint8_t **png, size_t *len, int *why) {
    int node = 0, rc;
    double x = r->x, y = r->y, w = r->w, h = r->h;
    int also[8], nalso = 0;
    if (r->shot == HANDS_SHOT_WINDOW) {
        Jb a = { 0 };
        jb_s(&a, "{\"window\":");
        jb_q(&a, r->window);
        jb_s(&a, ",");
        allow_json(&a);
        jb_s(&a, "}");
        JDoc d;
        if ((rc = ask("window", &a, &d, why))) return rc;
        node = (int)json_num(d.root, "node", 0);
        x = json_num(d.root, "x", 0);
        y = json_num(d.root, "y", 0);
        w = json_num(d.root, "w", 0);
        h = json_num(d.root, "h", 0);
        /* its sheets (Settings → Wallpaper's sheet, a file dialog: wm.addSheet), painted over it as Screenshots does */
        const JVal *sl = json_get(d.root, "sheets");
        for (const JVal *v = sl && sl->type == J_ARR ? sl->kid : NULL; v && nalso < 8; v = v->next)
            if (v->type == J_NUM && v->num > 0 && v->num < 1e9) also[nalso++] = (int)v->num;
        json_free(&d);
        if (node <= 0 || w < 1 || h < 1) return HANDS_E_NOTFOUND;
        if (S.h->node_place && (rc = S.h->node_place(node))) { *why = rc; return HANDS_E_REFUSED; }
    } else if (r->shot == HANDS_SHOT_REGION) {
        int W = 0, H = 0;
        S.h->size(&W, &H);
        /* the part on the desktop: what lies off it is not there to show (a zoom at the edge of a window that runs off the
         * screen); refused only when nothing of it is on the desktop */
        double x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
        if (!(w >= 1 && h >= 1) || !(x1 - x0 >= 1) || !(y1 - y0 >= 1)) { *why = HANDS_WHY_OUTSIDE; return HANDS_E_REFUSED; }
        x = x0; y = y0; w = x1 - x0; h = y1 - y0;
        if (S.nallow) {
            char t[160];
            Jb a = { 0 };
            snprintf(t, sizeof t, "{\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f,", x, y, w, h);
            jb_s(&a, t);
            allow_json(&a);
            jb_s(&a, "}");
            JDoc d;
            if ((rc = ask("region", &a, &d, why))) return rc;
            int shows = json_bool(d.root, "shows", 0);
            json_free(&d);
            if (!shows) { *why = HANDS_WHY_ALLOWLIST; return HANDS_E_REFUSED; }
        }
    } else if (S.nallow) {
        /* with an allowlist, the whole screen is no window's: only a window or a region of one */
        *why = HANDS_WHY_ALLOWLIST;
        return HANDS_E_REFUSED;
    }
    static int black[HANDS_BLACK_MAX];
    int nblack = 0;
    if ((rc = blackout(black, HANDS_BLACK_MAX, &nblack, why))) return rc;
    char meta[256] = "";
    int iw = 0, ih = 0;
    *png = S.h->shot(r->shot, r->display, node, x, y, w, h, r->maxdim, black, nblack, also, nalso, len, &iw, &ih, meta, sizeof meta);
    if (!*png) return HANDS_E_FAILED;
    *out = strdup(meta[0] ? meta : "{}");
    return HANDS_OK;
}

/* ---- human mode: an action over the next frames (handsmotion.h, docs/HANDS.md "The agent's cursor") ----
 * The cursor glides to the target, hovers, then the action happens through the same do_* path as machine mode, with
 * every gate checked again at that moment (the glide itself injects nothing: only the cursor moves). A drag's moves,
 * a scroll's steps and each typed character are gated as they happen. The person's input, Stop, the panic key, the
 * lock and the switch end it at once (plan_end). */

static int mode_of(const char *bus) {
    Client *k = client_of(bus);
    int m = k ? k->mode : HANDS_MODE_DEFAULT;
    return m ? m : hands_motion_default();
}

/* where a glide starts: the agent cursor, else the person's pointer, else the middle of the desktop */
static void glide_from(double *x, double *y) {
    if (S.cur_shown) { *x = S.cx; *y = S.cy; return; }
    if (S.h->pointer_at && S.h->pointer_at(x, y)) return;
    int W = 0, H = 0;
    S.h->size(&W, &H);
    *x = W / 2.0;
    *y = H / 2.0;
}

static void plan_glide(double tx, double ty, double width) {
    double fx, fy;
    glide_from(&fx, &fy);
    S.P.tx = tx;
    S.P.ty = ty;
    S.P.width = width;
    hands_glide_make(&S.P.g, fx, fy, tx, ty, width, S.reduce, &S.seed);
    S.P.t0 = now();
    S.P.phase = PH_GLIDE;
    cur(HANDS_CUR_MOVE, fx, fy, 0, 0);
}

/* a pending action ends: its answer through the done callback (when reply), its copy freed; a drag's button still down
 * is let go where the cursor is */
static void plan_end(int rc, int why, const char *out, int reply) {
    if (!S.P.on) return;
    if (S.P.held) {
        S.injecting = 1;
        S.h->button(0, 0);
        S.injecting = 0;
        S.P.held = 0;
        S.last_agent = now();
        S.agent_seq = ++S.seq;
    }
    char bus[64];
    memcpy(bus, S.P.bus, sizeof bus);
    int call = S.P.r.call, action = S.P.r.action;
    free(S.P.text);
    memset(&S.P, 0, sizeof S.P);
    Client *k = client_of(bus);
    if (!rc && k) { k->actions++; tell(); }
    log_call(k ? k->kind : HANDS_K_OTHER, call, action, rc, why);
    if (reply && S.done) S.done(bus, call, rc, why, out);
}

static void wheel_of(int dir, int k, int *dx, int *dy) {
    *dx = dir == HANDS_DIR_LEFT ? -k : dir == HANDS_DIR_RIGHT ? k : 0;
    *dy = dir == HANDS_DIR_UP ? k : dir == HANDS_DIR_DOWN ? -k : 0;
}

/* the target again, through the whole gate, now (it may have moved, been covered or become protected meanwhile) */
static int regate(int pointer, Gate *g, int *why) {
    int rc = S.P.r.ref[0] ? gate_ref(S.P.r.ref, pointer, g, why) : gate_xy(S.P.tx, S.P.ty, pointer, g, why);
    if (!rc && (g->x != S.cx || g->y != S.cy)) cur(HANDS_CUR_MOVE, g->x, g->y, 0, 0);
    return rc;
}

static size_t utf8_chars(const char *t, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)t[i] & 0xc0) != 0x80) c++;
    return c;
}

static int plan_start(const char *bus, const HandsReq *r, int *why) {
    Gate g, e;
    int rc;
    memset(&g, 0, sizeof g);
    switch (r->call) {
    case HANDS_C_POINTER:
        if (r->action == HANDS_P_MIDDLE && S.h->clip_sensitive && S.h->clip_sensitive()) { *why = HANDS_WHY_SENSITIVE_CLIP; return HANDS_E_REFUSED; }
        rc = r->ref[0] ? gate_ref(r->ref, 1, &g, why) : gate_xy(r->x, r->y, 1, &g, why);
        if (!rc && r->action == HANDS_P_DRAG) rc = gate_xy(r->x2, r->y2, 1, &e, why);
        break;
    case HANDS_C_SCROLL: rc = r->ref[0] ? gate_ref(r->ref, 1, &g, why) : gate_xy(r->x, r->y, 1, &g, why); break;
    case HANDS_C_SETVALUE: rc = gate_ref(r->ref, 0, &g, why); break;
    case HANDS_C_TYPE: rc = gate(GATE_KEYS, NULL, why); type_spot(&g.x, &g.y); break;
    default: return HANDS_E_ARGS;
    }
    if (rc) return rc;
    memset(&S.P, 0, sizeof S.P);
    S.P.r = *r;
    if (r->text) {
        S.P.text = malloc(r->text_len + 1);
        if (!S.P.text) return HANDS_E_FAILED;
        memcpy(S.P.text, r->text, r->text_len);
        S.P.text[r->text_len] = 0;
    }
    S.P.r.text = S.P.text;
    snprintf(S.P.bus, sizeof S.P.bus, "%s", bus);
    S.P.on = 1;
    if (!S.seed) S.seed = (uint32_t)now() ^ 0x9e3779b9u;
    S.P.chars = r->call == HANDS_C_TYPE ? utf8_chars(r->text, r->text_len) : 0;
    double width = g.bw >= 1 && g.bh >= 1 ? (g.bw < g.bh ? g.bw : g.bh) : 24;
    if (r->call == HANDS_C_TYPE && !(S.fbox[2] >= 4)) { S.P.phase = PH_TYPE; S.P.next = now(); return HANDS_E_PENDING; }
    plan_glide(g.x, g.y, width);
    return HANDS_E_PENDING;
}

/* the hover is over: the action itself */
static void plan_act(double t) {
    int why = 0, rc;
    char *out = NULL;
    Gate g, e;
    switch (S.P.r.call) {
    case HANDS_C_POINTER:
        if (S.P.r.action == HANDS_P_DRAG) {
            if ((rc = regate(1, &g, &why)) || (rc = gate_xy(S.P.r.x2, S.P.r.y2, 1, &e, &why))) { plan_end(rc, why, NULL, 1); return; }
            S.injecting = 1;
            S.h->move(g.x, g.y);
            S.h->button(0, 1);
            S.injecting = 0;
            S.P.held = 1;
            S.P.tx = g.x;                       /* where it began: a drop the end refuses goes back there */
            S.P.ty = g.y;
            S.last_agent = now();
            S.agent_seq = ++S.seq;
            S.events += 2;
            hands_glide_make(&S.P.g, g.x, g.y, S.P.r.x2, S.P.r.y2, 24, S.reduce, &S.seed);
            S.P.t0 = t;
            S.P.phase = PH_DRAG;
            return;
        }
        rc = do_pointer(&S.P.r, &out, &why);
        plan_end(rc, why, out, 1);
        free(out);
        return;
    case HANDS_C_SCROLL:
        S.P.nsteps = hands_scroll_steps(S.P.r.amount < 1 ? 3 : S.P.r.amount, S.P.steps);
        S.P.step = 0;
        S.P.phase = PH_SCROLL;
        S.P.next = t;
        return;
    case HANDS_C_SETVALUE:
        rc = do_setvalue(&S.P.r, &why);
        plan_end(rc, why, NULL, 1);
        return;
    default:
        plan_end(HANDS_E_ARGS, 0, NULL, 1);
    }
}

void hands_tick(void) {
    if (!S.P.on || !S.h) return;
    Client *k = client_of(S.P.bus);
    if (!S.on || !k || k->st != CS_GRANTED) { plan_end(HANDS_E_STOPPED, 0, NULL, 1); return; }
    double t = now(), x, y;
    int why = 0, rc;
    Gate g;
    switch (S.P.phase) {
    case PH_GLIDE:
        if (!hands_glide_at(&S.P.g, t - S.P.t0, &x, &y)) { cur(HANDS_CUR_MOVE, x, y, 0, 0); return; }
        cur(HANDS_CUR_MOVE, x, y, 0, 0);
        if (S.P.r.call == HANDS_C_TYPE) { S.P.phase = PH_TYPE; S.P.next = t; return; }
        /* the pointer over the target (a person's hover: its highlight shows), gated, then a short pause */
        if (S.P.r.call != HANDS_C_SETVALUE) {
            if ((rc = regate(1, &g, &why))) { plan_end(rc, why, NULL, 1); return; }
            S.injecting = 1;
            S.h->move(g.x, g.y);
            S.injecting = 0;
            S.last_agent = now();
            S.agent_seq = ++S.seq;
            S.events++;
        }
        if (S.P.r.call == HANDS_C_POINTER && S.P.r.action == HANDS_P_HOVER) {
            char *out = NULL;
            rc = do_pointer(&S.P.r, &out, &why);
            plan_end(rc, why, out, 1);
            free(out);
            return;
        }
        S.P.phase = PH_HOVER;
        S.P.next = t + hands_hover_ms(S.reduce, &S.seed);
        return;
    case PH_HOVER:
        if (t >= S.P.next) plan_act(t);
        return;
    case PH_DRAG: {
        int arrived = hands_glide_at(&S.P.g, t - S.P.t0, &x, &y);
        if (arrived && (rc = gate_xy(S.P.r.x2, S.P.r.y2, 1, &g, &why))) {
            /* the drop's place is refused now: let go where the drag began (allowed when it began) */
            x = S.P.tx;
            y = S.P.ty;
        } else rc = 0;
        S.injecting = 1;
        S.h->move(x, y);
        S.injecting = 0;
        S.last_agent = now();
        S.agent_seq = ++S.seq;
        S.events++;
        cur(HANDS_CUR_MOVE, x, y, 0, 0);
        if (!arrived) return;
        char out[96];
        snprintf(out, sizeof out, "{\"x\":%.2f,\"y\":%.2f}", x, y);
        plan_end(rc, why, rc ? NULL : out, 1);     /* releases the button */
        return;
    }
    case PH_SCROLL: {
        if (t < S.P.next) return;
        if ((rc = regate(1, &g, &why))) { plan_end(rc, why, NULL, 1); return; }
        int dx, dy;
        wheel_of(S.P.r.dir, S.P.steps[S.P.step], &dx, &dy);
        S.injecting = 1;
        S.h->move(g.x, g.y);
        S.h->wheel(dx, dy);
        S.injecting = 0;
        S.last_agent = now();
        S.agent_seq = ++S.seq;
        S.events += 2;
        if (++S.P.step >= S.P.nsteps) { plan_end(HANDS_OK, 0, NULL, 1); return; }
        S.P.next = t + (S.reduce ? 30 : HANDS_SCROLL_STEP);
        return;
    }
    case PH_TYPE: {
        if (t < S.P.next) return;
        const char *s = S.P.text ? S.P.text : "";
        size_t n = S.P.r.text_len, off = S.P.off;
        if (off >= n) { plan_end(HANDS_OK, 0, NULL, 1); return; }
        /* the whole key gate before every character: a Tab, a dialog or a click may have moved the keys meanwhile */
        if ((rc = gate(GATE_KEYS, NULL, &why))) { plan_end(rc, why, NULL, 1); return; }
        pulse();
        size_t l = 1;
        while (off + l < n && ((unsigned char)s[off + l] & 0xc0) == 0x80) l++;
        int w = 0;
        S.injecting = 1;
        if (s[off] == '\n' || s[off] == '\t') {
            HandsChord c = { 0 };
            c.code = s[off] == '\n' ? KEY_ENTER : KEY_TAB;
            S.h->key(&c);
        } else w = S.h->type(s + off, l);
        S.injecting = 0;
        S.last_agent = now();
        S.agent_seq = ++S.seq;
        S.events++;
        if (w) { plan_end(HANDS_E_REFUSED, w, NULL, 1); return; }
        S.P.off = off + l;
        if (S.P.off >= n) { plan_end(HANDS_OK, 0, NULL, 1); return; }
        S.P.next = t + hands_type_ms((unsigned char)s[off], S.P.chars, &S.seed);
        return;
    }
    }
}

double hands_timeout(void) {
    if (!S.P.on) return -1;
    if (S.P.phase == PH_GLIDE || S.P.phase == PH_DRAG) return 8;     /* a frame (the loop paints at the display's pace) */
    double d = S.P.next - now();
    return d < 0 ? 0 : d;
}

int hands_call(const char *bus, const HandsReq *r, char **out, uint8_t **png, size_t *png_len, int *why) {
    *out = NULL;
    if (png) *png = NULL;
    if (png_len) *png_len = 0;
    *why = 0;
    Client *k = client_of(bus);
    int kind = k ? k->kind : HANDS_K_OTHER;
    int rc = hands_may(bus, r->call);
    if (rc) {
        if (rc != HANDS_E_RATE) log_call(kind, r->call, r->action, rc, 0);
        return rc;
    }
    /* one action at a time: there is one agent cursor (another client's input waits for it) */
    if (S.P.on && is_input(r->call)) { log_call(kind, r->call, r->action, HANDS_E_BUSY, 0); return HANDS_E_BUSY; }
    if ((r->call == HANDS_C_POINTER || r->call == HANDS_C_SCROLL || r->call == HANDS_C_TYPE || r->call == HANDS_C_SETVALUE) &&
        mode_of(bus) == HANDS_MODE_HUMAN) {
        rc = plan_start(bus, r, why);
        if (rc == HANDS_E_PENDING) return rc;
        log_call(kind, r->call, r->action, rc, *why);
        return rc;
    }
    Jb a = { 0 };
    switch (r->call) {
    case HANDS_C_STATUS: {
        jb_s(&a, "{");
        allow_json(&a);
        jb_s(&a, "}");
        rc = shell_pass("status", &a, out, why);
        break;
    }
    case HANDS_C_WINDOWS:
        jb_s(&a, "{");
        allow_json(&a);
        jb_s(&a, "}");
        rc = shell_pass("windows", &a, out, why);
        break;
    case HANDS_C_READ: {
        char t[96];
        jb_s(&a, "{\"window\":");
        jb_q(&a, r->window);
        snprintf(t, sizeof t, ",\"filter\":\"%s\",\"depth\":%d,", r->filter ? "interactive" : "all", r->depth);
        jb_s(&a, t);
        allow_json(&a);
        jb_s(&a, "}");
        rc = shell_pass("read", &a, out, why);
        break;
    }
    case HANDS_C_FIND:
        jb_s(&a, "{\"window\":");
        jb_q(&a, r->window);
        jb_s(&a, ",\"query\":");
        jb_q(&a, r->text);
        jb_s(&a, ",");
        allow_json(&a);
        jb_s(&a, "}");
        rc = shell_pass("find", &a, out, why);
        break;
    case HANDS_C_POINTER: rc = do_pointer(r, out, why); break;
    case HANDS_C_SCROLL: rc = do_scroll(r, why); break;
    case HANDS_C_KEY: rc = do_key(r, why); break;
    case HANDS_C_TYPE: rc = do_type(r, why); break;
    case HANDS_C_SETVALUE: rc = do_setvalue(r, why); break;
    case HANDS_C_SHOT: rc = do_shot(r, out, png, png_len, why); break;
    case HANDS_C_OPENAPP:
        jb_s(&a, "{\"app\":");
        jb_q(&a, r->app);
        jb_s(&a, ",");
        allow_json(&a);
        jb_s(&a, "}");
        S.injecting = 1;
        rc = shell_pass("open", &a, out, why);
        S.injecting = 0;
    S.last_agent = now();
    S.agent_seq = ++S.seq;
        break;
    case HANDS_C_OPENPATH:
        jb_s(&a, "{\"path\":");
        jb_q(&a, r->text);
        jb_s(&a, ",");
        allow_json(&a);
        jb_s(&a, "}");
        S.injecting = 1;
        rc = shell_pass("openPath", &a, out, why);
        S.injecting = 0;
    S.last_agent = now();
    S.agent_seq = ++S.seq;
        break;
    case HANDS_C_CLOSE:
        jb_s(&a, "{\"window\":");
        jb_q(&a, r->window);
        jb_s(&a, ",");
        allow_json(&a);
        jb_s(&a, "}");
        S.injecting = 1;
        rc = shell_pass("close", &a, out, why);
        S.injecting = 0;
    S.last_agent = now();
    S.agent_seq = ++S.seq;
        break;
    case HANDS_C_APPLOG: {
        char t[64];
        jb_s(&a, "{\"app\":");
        jb_q(&a, r->app);
        jb_s(&a, ",\"pattern\":");
        jb_q(&a, r->text ? r->text : "");
        snprintf(t, sizeof t, ",\"max\":%d,", r->amount);
        jb_s(&a, t);
        allow_json(&a);
        jb_s(&a, "}");
        rc = shell_pass("log", &a, out, why);
        break;
    }
    case HANDS_C_MODE: {
        /* the connection's own motion (lordos-mcp's hands_mode, a call's mode): default follows the desktop's */
        if (k) k->mode = r->mode;
        char t[96];
        snprintf(t, sizeof t, "{\"mode\":\"%s\",\"effective\":\"%s\"}", hands_mode_word(r->mode), hands_mode_word(mode_of(bus)));
        *out = strdup(t);
        rc = *out ? HANDS_OK : HANDS_E_FAILED;
        break;
    }
    default: rc = HANDS_E_ARGS;
    }
    free(a.p);
    if (!rc && k) { k->actions++; if (is_input(r->call)) tell(); }
    if (rc && *out) { free(*out); *out = NULL; }
    log_call(kind, r->call, r->action, rc, *why);
    return rc;
}
