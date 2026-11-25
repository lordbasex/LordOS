/* vmcli.c — the `machines` command's session channel: request and answer lines (vmcli.h) */
#include "vmcli.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const VERBS[] = { NULL, "open", "start", "start-detached", "release" };

const char *vmcli_verb_name(int verb) {
    return verb >= VMCLI_OPEN && verb <= VMCLI_RELEASE ? VERBS[verb] : NULL;
}

/* the folder's form: absolute, no control bytes, no "//", no "." or ".." component, no trailing '/', ".lordvm" at the
 * end with a name before it */
static int dir_ok(const char *p, size_t n) {
    if (n < 2 || n > VMCLI_DIR_MAX || p[0] != '/' || p[n - 1] == '/') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch < 0x20 || ch == 0x7f) return 0;
        if (ch != '/') continue;
        /* the component after this slash */
        size_t j = i + 1;
        while (j < n && p[j] != '/') j++;
        size_t len = j - i - 1;
        if (!len) return 0;                                                    /* "//" */
        if ((len == 1 && p[i + 1] == '.') || (len == 2 && p[i + 1] == '.' && p[i + 2] == '.')) return 0;
    }
    size_t b = n;                                       /* p isn't NUL-terminated: the last component, within n */
    while (p[b - 1] != '/') b--;
    const char *base = p + b;
    size_t bl = n - b;
    /* a machine's folder, or a machine group's (docs/VIRTUALIZATION.md 19.9; approved by the maintainer 2026-11-23) */
    return (bl > 7 && !memcmp(base + bl - 7, ".lordvm", 7)) || (bl > 10 && !memcmp(base + bl - 10, ".lordgroup", 10));
}

int vmcli_req_parse(const char *buf, size_t n, VmCliReq *r) {
    if (!buf || !r || n < 2 || n > VMCLI_LINE_MAX || buf[n - 1] != '\n') return -1;
    if (memchr(buf, '\n', n - 1) || memchr(buf, 0, n)) return -1;
    const char *sp = memchr(buf, ' ', n - 1);
    if (!sp) return -1;
    size_t vl = (size_t)(sp - buf);
    int verb = 0;
    for (int v = VMCLI_OPEN; v <= VMCLI_RELEASE; v++)
        if (strlen(VERBS[v]) == vl && !memcmp(buf, VERBS[v], vl)) verb = v;
    if (!verb) return -1;
    const char *d = sp + 1;
    size_t dl = (size_t)(buf + n - 1 - d);
    if (!dir_ok(d, dl)) return -1;
    r->verb = verb;
    memcpy(r->dir, d, dl);
    r->dir[dl] = 0;
    return 0;
}

size_t vmcli_req_format(int verb, const char *dir, char *out, size_t n) {
    const char *v = vmcli_verb_name(verb);
    if (!v || !dir || !out) return 0;
    int k = snprintf(out, n, "%s %s\n", v, dir);
    if (k <= 0 || (size_t)k >= n) return 0;
    VmCliReq r;
    return vmcli_req_parse(out, (size_t)k, &r) ? 0 : (size_t)k;
}

/* the length of one valid UTF-8 character at s (at most left bytes), or 0 */
static size_t utf8_one(const unsigned char *s, size_t left) {
    unsigned char c = s[0];
    size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (!len || len > left) return 0;
    for (size_t i = 1; i < len; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    uint32_t cp = len == 1 ? c : len == 2 ? (uint32_t)(c & 0x1f) << 6 | (s[1] & 0x3f)
                : len == 3 ? (uint32_t)(c & 0x0f) << 12 | (uint32_t)(s[1] & 0x3f) << 6 | (s[2] & 0x3f)
                : (uint32_t)(c & 0x07) << 18 | (uint32_t)(s[1] & 0x3f) << 12 | (uint32_t)(s[2] & 0x3f) << 6 | (s[3] & 0x3f);
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    return len;
}

size_t vmcli_answer_format(int code, const char *text, char *out, size_t n) {
    if (!out || n < 4) return 0;
    if (code < 0 || code > 9) code = VMCLI_FAILED;
    size_t o = 0;
    out[o++] = (char)('0' + code);
    out[o++] = ' ';
    const unsigned char *s = (const unsigned char *)(text ? text : "");
    size_t left = strlen((const char *)s), used = 0;
    while (left) {
        size_t k = utf8_one(s, left);
        const unsigned char *src = s;
        unsigned char sub;
        size_t w = k;
        if (!k) { sub = '?'; src = &sub; w = 1; k = 1; }
        else if (k == 1 && (s[0] < 0x20 || s[0] == 0x7f)) { sub = ' '; src = &sub; }
        if (used + w > VMCLI_TEXT_MAX || o + w + 2 > n) break;
        memcpy(out + o, src, w);
        o += w; used += w; s += k; left -= k;
    }
    out[o++] = '\n';
    out[o] = 0;
    return o;
}

int vmcli_answer_parse(const char *buf, size_t n, int *code, char *text, size_t tn) {
    if (!buf || n < 3 || buf[n - 1] != '\n' || buf[0] < '0' || buf[0] > '9' || buf[1] != ' ') return -1;
    size_t tl = n - 3;
    if (tl > VMCLI_TEXT_MAX || memchr(buf, '\n', n - 1)) return -1;
    const unsigned char *s = (const unsigned char *)buf + 2;
    for (size_t i = 0; i < tl;) {
        size_t k = utf8_one(s + i, tl - i);
        if (!k || (k == 1 && (s[i] < 0x20 || s[i] == 0x7f))) return -1;
        i += k;
    }
    if (code) *code = buf[0] - '0';
    if (text && tn) {
        size_t c = tl < tn - 1 ? tl : tn - 1;
        memcpy(text, s, c);
        text[c] = 0;
    }
    return 0;
}
