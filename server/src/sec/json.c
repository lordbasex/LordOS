/* json.c — strict JSON reader (see json.h). Recursive descent; values and strings are
 * stored in chained blocks that are freed together with json_free. */
#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32

typedef struct Block { struct Block *next; size_t used, cap; unsigned char data[]; } Block;

typedef struct {
    const char *s, *end, *start;
    Block *blocks;
    int depth, max_depth;
    const char *why;
} P;

static void *alloc(P *p, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!p->blocks || p->blocks->used + n > p->blocks->cap) {
        size_t cap = n > 16384 ? n : 16384;
        Block *b = malloc(sizeof *b + cap);
        if (!b) return NULL;
        b->next = p->blocks;
        b->used = 0;
        b->cap = cap;
        p->blocks = b;
    }
    void *r = p->blocks->data + p->blocks->used;
    p->blocks->used += n;
    return r;
}

static void ws(P *p) { while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) p->s++; }

static int fail(P *p, const char *why) { if (!p->why) p->why = why; return -1; }

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_put(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3f)); o[2] = (char)(0x80 | (cp >> 6 & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* Quoted string (p->s points at the opening quote). */
static int string(P *p, const char **out, size_t *outlen) {
    const char *s = ++p->s;
    size_t raw = 0;
    while (s + raw < p->end && s[raw] != '"') { if (s[raw] == '\\') raw++; raw++; }
    if (s + raw >= p->end) return fail(p, "unterminated string");
    char *o = alloc(p, raw + 1);
    if (!o) return fail(p, "out of memory");
    size_t n = 0;
    while (*p->s != '"') {
        unsigned char c = (unsigned char)*p->s;
        if (c < 0x20) return fail(p, "control character in a string");
        if (c != '\\') { o[n++] = (char)c; p->s++; continue; }
        p->s++;
        switch (*p->s) {
        case '"': o[n++] = '"'; break;
        case '\\': o[n++] = '\\'; break;
        case '/': o[n++] = '/'; break;
        case 'b': o[n++] = '\b'; break;
        case 'f': o[n++] = '\f'; break;
        case 'n': o[n++] = '\n'; break;
        case 'r': o[n++] = '\r'; break;
        case 't': o[n++] = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (p->end - p->s < 5 || hex4(p->s + 1, &cp)) return fail(p, "invalid \\u");
            p->s += 4;
            if (cp >= 0xd800 && cp < 0xdc00) {
                if (p->end - p->s < 7 || p->s[1] != '\\' || p->s[2] != 'u' || hex4(p->s + 3, &lo) || lo < 0xdc00 || lo > 0xdfff)
                    return fail(p, "invalid surrogate pair");
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                p->s += 6;
            } else if (cp >= 0xdc00 && cp < 0xe000) return fail(p, "invalid surrogate pair");
            if (!cp) return fail(p, "\\u0000 is not accepted");
            n += utf8_put(o + n, cp);
            break;
        }
        default: return fail(p, "invalid escape");
        }
        p->s++;
    }
    p->s++;
    o[n] = 0;
    *out = o;
    *outlen = n;
    return 0;
}

static int number(P *p, double *out) {
    const char *s = p->s;
    if (s < p->end && *s == '-') s++;
    if (s >= p->end) return fail(p, "invalid number");
    if (*s == '0') s++;
    else if (*s >= '1' && *s <= '9') while (s < p->end && *s >= '0' && *s <= '9') s++;
    else return fail(p, "invalid number");
    if (s < p->end && *s == '.') {
        s++;
        if (s >= p->end || *s < '0' || *s > '9') return fail(p, "invalid number");
        while (s < p->end && *s >= '0' && *s <= '9') s++;
    }
    if (s < p->end && (*s == 'e' || *s == 'E')) {
        s++;
        if (s < p->end && (*s == '+' || *s == '-')) s++;
        if (s >= p->end || *s < '0' || *s > '9') return fail(p, "invalid number");
        while (s < p->end && *s >= '0' && *s <= '9') s++;
    }
    char buf[64];
    size_t n = (size_t)(s - p->s);
    if (n >= sizeof buf) return fail(p, "number too long");
    memcpy(buf, p->s, n);
    buf[n] = 0;
    *out = strtod(buf, NULL);
    p->s = s;
    return 0;
}

static int lit(P *p, const char *w) {
    size_t n = strlen(w);
    if ((size_t)(p->end - p->s) < n || memcmp(p->s, w, n)) return fail(p, "invalid value");
    p->s += n;
    return 0;
}

static JVal *value(P *p);

static JVal *container(P *p, int obj) {
    JVal *v = alloc(p, sizeof *v);
    if (!v) { fail(p, "out of memory"); return NULL; }
    memset(v, 0, sizeof *v);
    v->type = obj ? J_OBJ : J_ARR;
    if (++p->depth > p->max_depth) { fail(p, "nested too deeply"); return NULL; }
    p->s++;
    ws(p);
    JVal **tail = &v->kid;
    if (p->s < p->end && *p->s == (obj ? '}' : ']')) { p->s++; p->depth--; return v; }
    for (;;) {
        const char *key = NULL;
        size_t klen = 0;
        if (obj) {
            ws(p);
            if (p->s >= p->end || *p->s != '"') { fail(p, "expected a key"); return NULL; }
            if (string(p, &key, &klen)) return NULL;
            if (strlen(key) != klen) { fail(p, "invalid key"); return NULL; }
            for (JVal *k = v->kid; k; k = k->next) if (!strcmp(k->key, key)) { fail(p, "clave repetida"); return NULL; }
            ws(p);
            if (p->s >= p->end || *p->s != ':') { fail(p, "expected ':'"); return NULL; }
            p->s++;
        }
        JVal *k = value(p);
        if (!k) return NULL;
        k->key = key;
        *tail = k;
        tail = &k->next;
        v->len++;
        ws(p);
        if (p->s < p->end && *p->s == ',') { p->s++; continue; }
        if (p->s < p->end && *p->s == (obj ? '}' : ']')) { p->s++; break; }
        fail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
        return NULL;
    }
    p->depth--;
    return v;
}

static JVal *value(P *p) {
    ws(p);
    if (p->s >= p->end) { fail(p, "missing value"); return NULL; }
    char c = *p->s;
    if (c == '{' || c == '[') return container(p, c == '{');
    JVal *v = alloc(p, sizeof *v);
    if (!v) { fail(p, "out of memory"); return NULL; }
    memset(v, 0, sizeof *v);
    int r;
    if (c == '"') { v->type = J_STR; r = string(p, &v->str, &v->len); }
    else if (c == 't') { v->type = J_BOOL; v->num = 1; r = lit(p, "true"); }
    else if (c == 'f') { v->type = J_BOOL; r = lit(p, "false"); }
    else if (c == 'n') { v->type = J_NULL; r = lit(p, "null"); }
    else { v->type = J_NUM; r = number(p, &v->num); }
    return r ? NULL : v;
}

int json_parse(const char *text, size_t len, JDoc *doc, char *err, size_t errlen) {
    return json_parse_depth(text, len, doc, err, errlen, MAX_DEPTH);
}

int json_parse_depth(const char *text, size_t len, JDoc *doc, char *err, size_t errlen, int max_depth) {
    P p = { .s = text, .end = text + len, .start = text, .max_depth = max_depth > 0 ? max_depth : MAX_DEPTH };
    doc->root = NULL;
    doc->mem = NULL;
    JVal *v = value(&p);
    if (v) { ws(&p); if (p.s != p.end) { fail(&p, "trailing garbage"); v = NULL; } }
    if (!v) {
        if (err && errlen) snprintf(err, errlen, "%s (byte %ld)", p.why ? p.why : "invalid JSON", (long)(p.s - text));
        doc->mem = p.blocks;
        json_free(doc);
        return -1;
    }
    doc->root = v;
    doc->mem = p.blocks;
    return 0;
}

void json_free(JDoc *doc) {
    for (Block *b = doc->mem, *n; b; b = n) { n = b->next; free(b); }
    doc->mem = NULL;
    doc->root = NULL;
}

const JVal *json_get(const JVal *o, const char *key) {
    if (!o || o->type != J_OBJ) return NULL;
    for (const JVal *k = o->kid; k; k = k->next) if (!strcmp(k->key, key)) return k;
    return NULL;
}

const char *json_str(const JVal *o, const char *key) {
    const JVal *v = json_get(o, key);
    return v && v->type == J_STR ? v->str : NULL;
}

double json_num(const JVal *o, const char *key, double def) {
    const JVal *v = json_get(o, key);
    return v && v->type == J_NUM ? v->num : def;
}

int json_bool(const JVal *o, const char *key, int def) {
    const JVal *v = json_get(o, key);
    return v && v->type == J_BOOL ? (int)v->num : def;
}

size_t json_quote(char *out, size_t n, const char *s) {
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return 0; }
    out[o++] = '"';
    for (; *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
    return o;
}
