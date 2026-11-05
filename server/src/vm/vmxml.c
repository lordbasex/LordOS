/* vmxml.c — the bounded XML reader of vmxml.h. The input is copied once; names, attribute values and text are
 * decoded in that copy (decoding never makes text longer: "&lt;" is 1 byte, "&#65536;" is 4), and nodes come from a
 * bump allocator freed in one go. No recursion: the open elements are a fixed stack of VX_MAX_DEPTH. */
#include "vmxml.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Chunk { struct Chunk *next; size_t used, cap; char data[]; } Chunk;
typedef struct {
    VxNode root;                      /* first, so a VxNode * of the root is the document */
    char *buf;
    Chunk *chunks;
    size_t total;
} Doc;

#define ARENA_MAX ((size_t)64 << 20)

static void *arena(Doc *d, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!d->chunks || d->chunks->cap - d->chunks->used < n) {
        size_t cap = n > 65536 ? n : 65536;
        if (d->total + cap > ARENA_MAX) return NULL;
        Chunk *c = malloc(sizeof *c + cap);
        if (!c) return NULL;
        c->next = d->chunks; c->used = 0; c->cap = cap;
        d->chunks = c;
        d->total += cap;
    }
    void *p = d->chunks->data + d->chunks->used;
    d->chunks->used += n;
    memset(p, 0, n);
    return p;
}

static int name_start(unsigned char c) { return (c | 32) >= 'a' && (c | 32) <= 'z' ? 1 : c == '_' || c == ':' || c >= 0x80; }
static int name_char(unsigned char c) { return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
static int space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static size_t utf8(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* decodes s[0..n) into out (which may be s itself: it never grows); -1 on a bad or unknown entity */
static long decode(const char *s, size_t n, char *out) {
    size_t i = 0, o = 0;
    while (i < n) {
        if (s[i] != '&') { out[o++] = s[i++]; continue; }
        const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) return -1;
        size_t k = (size_t)(semi - (s + i)) + 1;
        const char *e = s + i + 1;
        size_t el = k - 2;
        if (el == 2 && !memcmp(e, "lt", 2)) out[o++] = '<';
        else if (el == 2 && !memcmp(e, "gt", 2)) out[o++] = '>';
        else if (el == 3 && !memcmp(e, "amp", 3)) out[o++] = '&';
        else if (el == 4 && !memcmp(e, "quot", 4)) out[o++] = '"';
        else if (el == 4 && !memcmp(e, "apos", 4)) out[o++] = '\'';
        else if (el >= 2 && e[0] == '#') {
            uint32_t cp = 0;
            int hex = e[1] == 'x', digits = 0;
            for (size_t j = hex ? 2 : 1; j < el; j++, digits++) {
                char c = e[j];
                int v = c >= '0' && c <= '9' ? c - '0' : hex && (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
                if (v < 0 || cp > 0x10ffff) return -1;
                cp = cp * (hex ? 16 : 10) + (uint32_t)v;
            }
            if (!digits || !cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
            char tmp[4];
            size_t u = utf8(cp, tmp);
            if (u > k) return -1;                       /* can't happen: the shortest form is longer than its bytes */
            memcpy(out + o, tmp, u);
            o += u;
        } else return -1;
        i += k;
    }
    return (long)o;
}

/* the first `pat` at or after s[from], or NULL: scans only as far as the match (a library strstr may measure the whole
 * rest of the input first, which made a file of many small comments cost the square of its size) */
static char *find(char *s, size_t from, size_t len, const char *pat) {
    size_t pl = strlen(pat);
    for (size_t i = from; i + pl <= len; i++) if (s[i] == pat[0] && !memcmp(s + i, pat, pl)) return s + i;
    return NULL;
}

static const char *local(const char *q) { const char *c = strrchr(q, ':'); return c ? c + 1 : q; }

/* have: the text's length so far, kept by the caller (a strlen here made many small pieces cost the square of the text) */
static int add_text(Doc *d, VxNode *n, const char *s, size_t len, size_t *cap, size_t *havep) {
    if (!len) return 0;
    size_t have = *havep;
    if (have + len > (1u << 20)) return -1;
    if (!n->text || have + len + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        while (c < have + len + 1) c *= 2;
        char *t = arena(d, c);
        if (!t) return -1;
        if (have) memcpy(t, n->text, have);
        n->text = t;
        *cap = c;
    }
    memcpy(n->text + have, s, len);
    n->text[have + len] = 0;
    *havep = have + len;
    return 0;
}

static void trim_all(VxNode *root) {
    for (VxNode *n = root; n; ) {
        if (n->text) {
            while (space((unsigned char)*n->text)) n->text++;
            size_t l = strlen(n->text);
            while (l && space((unsigned char)n->text[l - 1])) n->text[--l] = 0;
        }
        if (n->kid) n = n->kid;
        else {
            while (n && !n->next) n = n->parent;
            if (n) n = n->next;
        }
    }
}

void vx_free(VxNode *root) {
    if (!root) return;
    Doc *d = (Doc *)root;
    for (Chunk *c = d->chunks; c; ) { Chunk *nx = c->next; free(c); c = nx; }
    free(d->buf);
    free(d);
}

#define FAIL(m) do { if (err) *err = (m); goto bad; } while (0)

VxNode *vx_parse(const char *src, size_t len, const char **err) {
    if (err) *err = NULL;
    if (!src || len > VX_MAX_INPUT) { if (err) *err = "too large"; return NULL; }
    if (memchr(src, 0, len)) { if (err) *err = "NUL byte"; return NULL; }
    Doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->buf = malloc(len + 1);
    if (!d->buf) { free(d); return NULL; }
    memcpy(d->buf, src, len);
    d->buf[len] = 0;
    char *s = d->buf;
    size_t i = 0;
    if (len >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) i = 3;
    VxNode *stack[VX_MAX_DEPTH];
    const char *qn[VX_MAX_DEPTH];
    size_t tcap[VX_MAX_DEPTH], tlen[VX_MAX_DEPTH];
    int depth = 0, nodes = 0, have_root = 0;
    VxAttr at[VX_MAX_ATTRS];
    while (i < len) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < len && s[j] != '<') j++;
            if (!depth) {
                for (size_t k = i; k < j; k++) if (!space((unsigned char)s[k])) FAIL("text outside the root");
            } else {
                int blank = 1;
                for (size_t k = i; k < j; k++) if (!space((unsigned char)s[k])) { blank = 0; break; }
                if (!blank) {
                    long o = decode(s + i, j - i, s + i);
                    if (o < 0) FAIL("bad entity");
                    if (add_text(d, stack[depth - 1], s + i, (size_t)o, &tcap[depth - 1], &tlen[depth - 1])) FAIL("text too long");
                }
            }
            i = j;
            continue;
        }
        /* s[i] == '<' */
        if (i + 1 >= len) FAIL("truncated");
        if (s[i + 1] == '?') {
            char *e = find(s, i + 2, len, "?>");
            if (!e) FAIL("unclosed processing instruction");
            i = (size_t)(e - s) + 2;
            continue;
        }
        if (s[i + 1] == '!') {
            if (len - i >= 4 && !memcmp(s + i, "<!--", 4)) {
                char *e = find(s, i + 4, len, "-->");
                if (!e) FAIL("unclosed comment");
                i = (size_t)(e - s) + 3;
                continue;
            }
            if (len - i >= 9 && !memcmp(s + i, "<![CDATA[", 9)) {
                if (!depth) FAIL("CDATA outside the root");
                char *e = find(s, i + 9, len, "]]>");
                if (!e) FAIL("unclosed CDATA");
                if (add_text(d, stack[depth - 1], s + i + 9, (size_t)(e - (s + i + 9)), &tcap[depth - 1], &tlen[depth - 1])) FAIL("text too long");
                i = (size_t)(e - s) + 3;
                continue;
            }
            FAIL("declarations (DOCTYPE, ENTITY) are refused");
        }
        if (s[i + 1] == '/') {
            size_t j = i + 2, st = j;
            while (j < len && name_char((unsigned char)s[j])) j++;
            if (!depth || j == st) FAIL("unexpected end tag");
            size_t nl = j - st;
            if (strlen(qn[depth - 1]) != nl || memcmp(qn[depth - 1], s + st, nl)) FAIL("mismatched end tag");
            while (j < len && space((unsigned char)s[j])) j++;
            if (j >= len || s[j] != '>') FAIL("bad end tag");
            i = j + 1;
            depth--;
            continue;
        }
        /* a start tag */
        size_t j = i + 1, st = j;
        if (!name_start((unsigned char)s[j])) FAIL("bad element name");
        while (j < len && name_char((unsigned char)s[j])) j++;
        if (j >= len) FAIL("truncated");
        if (depth >= VX_MAX_DEPTH) FAIL("too deep");
        if (!depth && have_root) FAIL("a second root");
        if (++nodes > VX_MAX_NODES) FAIL("too many elements");
        char *qname = s + st;
        size_t qlen = j - st;
        int na = 0, empty = 0;
        for (;;) {
            size_t ws = j;
            while (j < len && space((unsigned char)s[j])) j++;
            if (j >= len) FAIL("truncated");
            if (s[j] == '>') { j++; break; }
            if (s[j] == '/') { if (j + 1 >= len || s[j + 1] != '>') FAIL("bad empty tag"); empty = 1; j += 2; break; }
            if (j == ws || !name_start((unsigned char)s[j])) FAIL("bad attribute");
            size_t as = j;
            while (j < len && name_char((unsigned char)s[j])) j++;
            size_t al = j - as;
            while (j < len && space((unsigned char)s[j])) j++;
            if (j >= len || s[j] != '=') FAIL("attribute without a value");
            j++;
            while (j < len && space((unsigned char)s[j])) j++;
            if (j >= len || (s[j] != '"' && s[j] != '\'')) FAIL("unquoted attribute");
            char q = s[j++];
            size_t vs = j;
            while (j < len && s[j] != q && s[j] != '<') j++;
            if (j >= len || s[j] != q) FAIL("bad attribute value");
            if (na >= VX_MAX_ATTRS) FAIL("too many attributes");
            long o = decode(s + vs, j - vs, s + vs);
            if (o < 0) FAIL("bad entity");
            s[vs + (size_t)o] = 0;
            /* the name ends where '=' or a space was; terminate it (that byte is never needed again) */
            char save = s[as + al];
            (void)save;
            s[as + al] = 0;
            for (int k = 0; k < na; k++) if (!strcmp(at[k].name, s + as)) FAIL("duplicate attribute");
            at[na].name = s + as;
            at[na].value = s + vs;
            na++;
            j++;                                    /* the closing quote */
            if (j < len && !space((unsigned char)s[j]) && s[j] != '>' && s[j] != '/') FAIL("attributes need a space between them");
        }
        qname[qlen] = 0;                            /* after the attributes: s[st + qlen] was a space, '>' or '/', all consumed */
        VxNode *n = depth ? arena(d, sizeof *n) : &d->root;
        if (!n) FAIL("out of memory");
        n->name = local(qname);
        if (na) {
            n->attrs = arena(d, sizeof(VxAttr) * (size_t)na);
            if (!n->attrs) FAIL("out of memory");
            for (int k = 0; k < na; k++) { n->attrs[k].name = local(at[k].name); n->attrs[k].value = at[k].value; }
            n->nattrs = na;
        }
        if (depth) {
            VxNode *p = stack[depth - 1];
            n->parent = p;
            if (p->last) p->last->next = n; else p->kid = n;
            p->last = n;
        } else have_root = 1;
        i = j;
        if (!empty) { stack[depth] = n; qn[depth] = qname; tcap[depth] = 0; tlen[depth] = 0; depth++; }
    }
    if (depth || !have_root) FAIL(have_root ? "unclosed element" : "no root element");
    trim_all(&d->root);
    return &d->root;
bad:
    if (err && !*err) *err = "bad XML";
    for (Chunk *c = d->chunks; c; ) { Chunk *nx = c->next; free(c); c = nx; }
    free(d->buf);
    free(d);
    return NULL;
}

VxNode *vx_child(const VxNode *n, const char *name) {
    for (VxNode *k = n ? n->kid : NULL; k; k = k->next) if (!strcmp(k->name, name)) return k;
    return NULL;
}
VxNode *vx_next(const VxNode *n, const char *name) {
    for (VxNode *k = n ? n->next : NULL; k; k = k->next) if (!strcmp(k->name, name)) return k;
    return NULL;
}
VxNode *vx_find(const VxNode *top, const char *name) {
    if (!top) return NULL;
    for (const VxNode *n = top->kid; n; ) {
        if (!strcmp(n->name, name)) return (VxNode *)n;
        if (n->kid) n = n->kid;
        else {
            while (n && n != top && !n->next) n = n->parent;
            if (!n || n == top) break;
            n = n->next;
        }
    }
    return NULL;
}
const char *vx_attr(const VxNode *n, const char *name) {
    for (int i = 0; n && i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}
const char *vx_text(const VxNode *n) { return n && n->text ? n->text : ""; }
const char *vx_child_text(const VxNode *n, const char *name) { return vx_text(vx_child(n, name)); }
