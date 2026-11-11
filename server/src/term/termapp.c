/* termapp.c — terminal apps (.desktop with Terminal=true: htop, mc, ranger, vim's entry).
 *
 * Approved by the maintainer on 2026-11-10 as part of the lord.term.create exception (AGENTS.md):
 *   - lord.apps.launch(id) of such an entry never runs it: apps.c parses its Exec line (field codes
 *     removed, no caller arguments, no file) and stores that argv here, in a one-shot slot named by a
 *     random 128-bit token (getrandom), valid for 10 s, used once, at most 4 pending (the oldest goes);
 *   - the Terminal passes that token, and only that token, as lord.term.create({ app: token }): the tab
 *     starts the same login shell as always (term/pty.c) and lord-server types the line built here into
 *     it once, when the first prompt is shown;
 *   - the line is built in C: each element single-quoted (POSIX: ' becomes '\''), no control character
 *     allowed, and then the profile's exit rule, one of three fixed suffixes. JS never sees or chooses
 *     the command; with no valid token nothing is typed.
 * No process is started here, and nothing is read from the caller but the token. */
#include "termapp.h"

#include <stdint.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    int used;
    double created;
    char token[TERMAPP_TOKEN_HEX + 1];
    char args[TERMAPP_ARGV_MAX];        /* the elements, each ended by a NUL */
    int argc;
} Slot;

static Slot slots[TERMAPP_MAX_PENDING];

double termapp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void drop(Slot *s) {
    memset(s, 0, sizeof *s);
}

static void expire(double now) {
    for (int i = 0; i < TERMAPP_MAX_PENDING; i++)
        if (slots[i].used && (now - slots[i].created >= TERMAPP_TTL_MS || now < slots[i].created)) drop(&slots[i]);
}

int termapp_arg_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        if (c < 0x20 || c == 0x7f) return 0;
        if (c < 0x80) { p++; continue; }
        int len = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!len) return 0;
        uint32_t cp = c & (len == 2 ? 0x1f : len == 3 ? 0x0f : 0x07);
        for (int k = 1; k < len; k++) {
            if ((p[k] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[k] & 0x3f);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if (cp >= 0x80 && cp <= 0x9f) return 0;          /* C1 controls (CSI, OSC… in 8 bits) */
        if (cp == 0x2028 || cp == 0x2029) return 0;      /* line and paragraph separators */
        p += len;
    }
    return 1;
}

int termapp_quote(char *const *argv, int argc, int fish, char *out, size_t n) {
    size_t o = 0;
    if (!n) return -1;
    out[0] = 0;
    if (argc <= 0) return -1;
    for (int i = 0; i < argc; i++) {
        if (!argv[i] || !termapp_arg_ok(argv[i])) { out[0] = 0; return -1; }
        if (i) { if (o + 1 >= n) goto full; out[o++] = ' '; }
        if (o + 1 >= n) goto full;
        out[o++] = '\'';
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '\'') {
                if (o + 4 >= n) goto full;
                memcpy(out + o, "'\\''", 4);
                o += 4;
            } else if (*p == '\\' && fish) {
                if (o + 2 >= n) goto full;
                out[o++] = '\\';
                out[o++] = '\\';
            } else {
                if (o + 1 >= n) goto full;
                out[o++] = *p;
            }
        }
        if (o + 1 >= n) goto full;
        out[o++] = '\'';
    }
    out[o] = 0;
    return 0;
full:
    out[0] = 0;
    return -1;
}

const char *termapp_exit_suffix(const char *rule) {
    if (rule && !strcmp(rule, "keep")) return "";
    if (rule && !strcmp(rule, "closeIfOk")) return " && exit";
    return "; exit";
}

int termapp_put(char *const *argv, int argc, double now, char *token) {
    token[0] = 0;
    if (argc <= 0 || !argv) return -1;
    Slot s;
    memset(&s, 0, sizeof s);
    size_t o = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i] || !termapp_arg_ok(argv[i])) return -1;
        size_t l = strlen(argv[i]);
        if (o + l + 1 > sizeof s.args) return -1;
        memcpy(s.args + o, argv[i], l + 1);
        o += l + 1;
    }
    /* the line it will become must fit too (the worst exit rule included) */
    char line[TERMAPP_LINE_MAX];
    if (termapp_quote(argv, argc, 1, line, sizeof line - 16)) return -1;
    unsigned char rnd[TERMAPP_TOKEN_HEX / 2];
    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) return -1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof rnd; i++) { s.token[2 * i] = hex[rnd[i] >> 4]; s.token[2 * i + 1] = hex[rnd[i] & 15]; }
    s.token[TERMAPP_TOKEN_HEX] = 0;
    s.used = 1;
    s.created = now;
    s.argc = argc;
    expire(now);
    Slot *dst = NULL;
    for (int i = 0; i < TERMAPP_MAX_PENDING && !dst; i++) if (!slots[i].used) dst = &slots[i];
    if (!dst) {                         /* full: the oldest goes */
        dst = &slots[0];
        for (int i = 1; i < TERMAPP_MAX_PENDING; i++) if (slots[i].created < dst->created) dst = &slots[i];
    }
    *dst = s;
    memcpy(token, s.token, sizeof s.token);
    memset(&s, 0, sizeof s);
    return 0;
}

/* Compares the whole token, whatever matches first. */
static int same_token(const char *a, const char *b) {
    unsigned d = 0;
    for (int i = 0; i < TERMAPP_TOKEN_HEX; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

static int token_ok(const char *t) {
    if (!t || strlen(t) != TERMAPP_TOKEN_HEX) return 0;
    for (const char *p = t; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}

int termapp_take(const char *token, double now, int fish, const char *exit_rule, char *line, size_t n) {
    if (n) line[0] = 0;
    expire(now);
    if (!token_ok(token)) return -1;
    Slot *s = NULL;
    for (int i = 0; i < TERMAPP_MAX_PENDING; i++) if (slots[i].used && same_token(slots[i].token, token)) s = &slots[i];
    if (!s) return -1;
    char *argv[TERMAPP_ARGV_MAX / 2];
    int argc = 0;
    for (size_t o = 0; argc < s->argc && o < sizeof s->args; argc++) {
        argv[argc] = s->args + o;
        o += strlen(s->args + o) + 1;
    }
    const char *suffix = termapp_exit_suffix(exit_rule);
    int r = termapp_quote(argv, argc, fish, line, n);
    size_t l = r ? 0 : strlen(line), sl = strlen(suffix);
    if (!r && l + sl + 1 > n) r = -1;
    if (!r) memcpy(line + l, suffix, sl + 1);
    else if (n) line[0] = 0;
    drop(s);                            /* used once, whatever happened */
    return r;
}

int termapp_pending(double now) {
    expire(now);
    int k = 0;
    for (int i = 0; i < TERMAPP_MAX_PENDING; i++) k += slots[i].used;
    return k;
}

void termapp_reset(void) {
    for (int i = 0; i < TERMAPP_MAX_PENDING; i++) drop(&slots[i]);
}
