/* describe.c — what a program is, from dpkg's database, Debian's translations, manual pages and systemd units
 * (describe.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "describe.h"
#include "../pkg/xz.h"
#include "../sec/json.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *desc_status_path = "/var/lib/dpkg/status";
const char *desc_lists_dir = "/var/lib/apt/lists";
const char *desc_man_root = "/usr/share/man";
static const char *const unit_dirs_default[] = { "/etc/systemd/system", "/run/systemd/system", "/usr/lib/systemd/system", "/lib/systemd/system",
                                                 "~/.config/systemd/user", "/etc/systemd/user", "/usr/lib/systemd/user", NULL };
const char *const *desc_unit_dirs = unit_dirs_default;
int desc_require_root = 1;

/* ---- MD5 (RFC 1321): Debian's Description-md5 ---- */

typedef struct { uint32_t h[4]; uint64_t n; uint8_t b[64]; } Md5;
static const uint32_t MK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const uint8_t MR[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(Md5 *m, const uint8_t *p) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        uint32_t x = a + f + MK[i] + w[g];
        b = b + (x << MR[i] | x >> (32 - MR[i]));
        a = t;
    }
    m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}
static void md5_init(Md5 *m) { *m = (Md5){ { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, 0, { 0 } }; }
static void md5_update(Md5 *m, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        size_t at = (size_t)(m->n & 63), k = 64 - at < len ? 64 - at : len;
        memcpy(m->b + at, p, k);
        m->n += k; p += k; len -= k;
        if (!(m->n & 63)) md5_block(m, m->b);
    }
}
static void md5_final(Md5 *m, uint8_t out[16]) {
    uint64_t bits = m->n * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    md5_update(m, &pad, 1);
    while ((m->n & 63) != 56) md5_update(m, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i));
    md5_update(m, len, 8);
    for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(m->h[i] >> (8 * k));
}
void desc_md5_hex(const void *data, size_t len, char out[33]) {
    Md5 m;
    uint8_t d[16];
    md5_init(&m);
    md5_update(&m, data, len);
    md5_final(&m, d);
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

/* ---- shared ---- */

static int name_ok(const char *s, size_t max, const char *extra) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > max || s[0] == '.' || s[0] == '-') return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr(extra, s[i])) return 0;
    return !strstr(s, "..");
}

int desc_lang_codes(const char *lang, char out[3][16]) {
    for (int i = 0; i < 3; i++) out[i][0] = 0;
    if (!lang || !name_ok(lang, 15, "-_") || !strncmp(lang, "en", 2)) return 0;
    if (!strcmp(lang, "zh-Hans")) { snprintf(out[0], 16, "zh_CN"); return 1; }
    if (!strcmp(lang, "zh-Hant")) { snprintf(out[0], 16, "zh_TW"); snprintf(out[1], 16, "zh_HK"); return 2; }
    if (!strncmp(lang, "pt", 2)) { snprintf(out[0], 16, "pt_BR"); snprintf(out[1], 16, "pt"); return 2; }
    if (!strcmp(lang, "fil")) { snprintf(out[0], 16, "tl"); snprintf(out[1], 16, "fil"); return 2; }
    size_t k = strcspn(lang, "-_");
    snprintf(out[0], 16, "%.*s", (int)k, lang);
    return 1;
}

/* a description's line as shown: printable, one space for each run of blanks */
static void put_text(char *dst, size_t n, const char *src, size_t len) {
    size_t o = strlen(dst);
    int sp = o > 0 && dst[o - 1] != ' ' && dst[o - 1] != '\n';
    for (size_t i = 0; i < len && o + 4 < n; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\t' || ch == ' ' || ch == '\r') { sp = o > 0 && dst[o - 1] != '\n'; continue; }
        if (ch < 0x20 || ch == 0x7f) continue;
        if (sp) { dst[o++] = ' '; sp = 0; }
        dst[o++] = (char)ch;
    }
    dst[o] = 0;
    if (o + 4 >= n && o >= 3) memcpy(dst + o - 3, "...", 4);
}

/* a Debian description's continuation lines (" text", " ." for a blank line) as paragraphs */
static void put_long(DescOut *o, const char *line, size_t len) {
    size_t cur = strlen(o->lng);
    if (len >= 2 && line[1] == '.' && (len == 2 || line[2] == ' ')) {
        if (cur && cur + 3 < sizeof o->lng) strcat(o->lng, "\n\n");
        return;
    }
    put_text(o->lng, sizeof o->lng, line + 1, len - 1);
}

/* ---- "pkg": dpkg's database, then Debian's translation ---- */

/* the stanza of pkg in a deb822 file (status or Translation): calls fn for each line of it, the field's name already
 * matched; returns 0 when found */
typedef struct { const char *want; const char *md5; const char *field; DescOut *o; Md5 m; int in, got, cur; char md5hex[33]; } Scan;

static int scan_file(const char *path, Scan *s, int translation) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 256LL * 1024 * 1024) { close(fd); return -1; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    size_t wl = strlen(s->want);
    int in = 0, desc = 0, md5ok = !translation, installed = translation;
    DescOut tmp = { 0 };
    char sh[DESC_TEXT_MAX] = "";
    Md5 m;
    md5_init(&m);
    int found = -1;
    while ((len = getline(&line, &cap, f)) > 0) {
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) {                                    /* end of a stanza */
            if (in && desc && md5ok && installed) { found = 0; break; }
            in = desc = 0; md5ok = !translation; installed = translation;
            tmp = (DescOut){ 0 }; sh[0] = 0; md5_init(&m);
            continue;
        }
        if (line[0] != ' ' && line[0] != '\t') {
            if (desc == 1) desc = 2;                     /* the description ended */
            if (!strncmp(line, "Package: ", 9)) { in = (size_t)len - 9 == wl && !memcmp(line + 9, s->want, wl); continue; }
            if (!in) continue;
            if (!translation && !strncmp(line, "Status: ", 8)) { installed = strstr(line, " installed") != NULL; continue; }
            if (translation && !strncmp(line, "Description-md5: ", 17)) { md5ok = s->md5 && !strcmp(line + 17, s->md5); continue; }
            size_t fl = strlen(s->field);
            if (!strncmp(line, s->field, fl) && line[fl] == ':' && line[fl + 1] == ' ') {
                desc = 1;
                put_text(sh, sizeof sh, line + fl + 2, (size_t)len - fl - 2);
                md5_update(&m, line + fl + 2, (size_t)len - fl - 2);
                md5_update(&m, "\n", 1);
            }
            continue;
        }
        if (in && desc == 1) {
            put_long(&tmp, line, (size_t)len);
            md5_update(&m, line, (size_t)len);
            md5_update(&m, "\n", 1);
        }
    }
    if (found && in && desc && md5ok && installed) found = 0;     /* the last stanza, with no blank line after it */
    free(line);
    fclose(f);
    if (!found) {
        snprintf(s->o->text, sizeof s->o->text, "%s", sh);
        snprintf(s->o->lng, sizeof s->o->lng, "%s", tmp.lng);
        if (!translation) {
            uint8_t d[16];
            md5_final(&m, d);
            for (int i = 0; i < 16; i++) snprintf(s->md5hex + i * 2, 3, "%02x", d[i]);
        }
    }
    return found;
}

int desc_pkg(const char *status, const char *lists, const char *pkg, const char *lang, DescOut *o) {
    *o = (DescOut){ 0 };
    if (!name_ok(pkg, 128, "+-.:")) return -1;
    char want[129];
    snprintf(want, sizeof want, "%.*s", (int)strcspn(pkg, ":"), pkg);     /* "libc6:amd64": the name */
    Scan s = { .want = want, .field = "Description", .o = o };
    if (scan_file(status, &s, 0)) return -1;
    snprintf(o->lang, sizeof o->lang, "en");
    char md5[33];
    memcpy(md5, s.md5hex, sizeof md5);
    char codes[3][16];
    int nc = desc_lang_codes(lang, codes);
    DIR *d = nc && lists ? opendir(lists) : NULL;
    if (!d) return 0;
    struct dirent *e;
    char names[16][300];
    int nn = 0;
    while ((e = readdir(d)) && nn < 16) {
        const char *t = strstr(e->d_name, "_i18n_Translation-");
        if (!t) continue;
        for (int i = 0; i < nc; i++)
            if (!strcmp(t + 18, codes[i])) { snprintf(names[nn++], sizeof names[0], "%d%s", i, e->d_name); break; }
    }
    closedir(d);
    for (int ci = 0; ci < nc; ci++) {
        for (int i = 0; i < nn; i++) {
            if (names[i][0] != '0' + ci) continue;
            char path[600], field[80];
            if ((size_t)snprintf(path, sizeof path, "%s/%s", lists, names[i] + 1) >= sizeof path) continue;
            snprintf(field, sizeof field, "Description-%s", codes[ci]);
            DescOut tr = { 0 };
            Scan ts = { .want = want, .md5 = md5, .field = field, .o = &tr };
            if (!scan_file(path, &ts, 1) && tr.text[0]) {
                memcpy(o->text, tr.text, sizeof o->text);
                memcpy(o->lng, tr.lng, sizeof o->lng);
                snprintf(o->lang, sizeof o->lang, "%s", lang);
                return 0;
            }
        }
    }
    return 0;
}

/* ---- "man": the NAME section of a manual page ---- */

/* roff's escapes as plain text: \- \(em \(en \fB \fR \& \e \\ \*(…, and the rest dropped */
static void unroff(const char *s, size_t n, char *out, size_t on) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < on; i++) {
        char c = s[i];
        if (c != '\\') { out[o++] = c; continue; }
        if (++i >= n) break;
        c = s[i];
        if (c == '-' || c == 'e' || c == '\\') out[o++] = c == 'e' ? '\\' : c;
        else if (c == '(' && i + 2 < n) {
            if ((s[i + 1] == 'e' && s[i + 2] == 'm') || (s[i + 1] == 'e' && s[i + 2] == 'n')) out[o++] = '-';
            i += 2;
        } else if (c == 'f' && i + 1 < n) i += s[i + 1] == '(' ? 3 : s[i + 1] == '[' ? (size_t)(strchr(s + i, ']') ? strchr(s + i, ']') - (s + i) : 1) : 1;
        else if (c == '*' && i + 1 < n) i += s[i + 1] == '(' ? 3 : 1;
        else if (c == ' ') out[o++] = ' ';
        /* \& \| \c \% and the others: nothing */
    }
    out[o] = 0;
}

int desc_man_summary(const char *page, size_t len, char *out, size_t n) {
    out[0] = 0;
    char buf[1024] = "";
    size_t bl = 0;
    int in = 0, lines = 0;
    const char *p = page, *end = page + len;
    while (p < end && lines < 4000) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        lines++;
        int sh = l >= 3 && p[0] == '.' && (!strncmp(p, ".SH", 3) || !strncmp(p, ".Sh", 3));
        if (sh) {
            if (in) break;                          /* the next section: NAME is over */
            in = 1;                                  /* the first section is NAME, whatever its language calls it */
        } else if (in && l) {
            if (!strncmp(p, ".Nd ", 4)) {          /* mdoc: .Nm name / .Nd description */
                if (bl && bl + 4 < sizeof buf) { memcpy(buf + bl, " - ", 3); bl += 3; }
                p += 4; l -= 4;
            } else if (!strncmp(p, ".Nm", 3)) { p += 3; l -= 3; while (l && *p == ' ') { p++; l--; } }
            else if (p[0] == '.' || p[0] == '\'') {    /* another request (.B, .PD, .\"…): its text, if any */
                if (l >= 3 && p[1] == '\\' && p[2] == '"') l = 0;
                else { size_t k = 1; while (k < l && p[k] != ' ') k++; p += k; l -= k; }
            }
            if (l && bl + l + 2 < sizeof buf) {
                if (bl) buf[bl++] = ' ';
                memcpy(buf + bl, p, l);
                bl += l;
                buf[bl] = 0;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (!bl) return -1;
    char plain[1024];
    unroff(buf, bl, plain, sizeof plain);
    /* "name, other - what it does": the text after the first " - " (some pages use "--" or an en dash) */
    const char *sep = strstr(plain, " - ");
    size_t skip = 3;
    if (!sep) { sep = strstr(plain, " -- "); skip = 4; }
    if (!sep) { sep = strstr(plain, " \xe2\x80\x94 "); skip = 5; }
    if (!sep) { sep = strstr(plain, " \xe2\x80\x93 "); skip = 5; }
    if (!sep) return -1;
    out[0] = 0;
    put_text(out, n, sep + skip, strlen(sep + skip));
    return out[0] ? 0 : -1;
}

/* a file of the system's: regular, root's, not writable by others, at most max bytes; malloc'd */
static char *read_system_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (st.st_uid == 0 || !desc_require_root) && !(st.st_mode & 022) && (size_t)st.st_size <= max && (b = malloc((size_t)st.st_size + 1))) {
        ssize_t k = read(fd, b, (size_t)st.st_size);
        if (k < 0) { free(b); b = NULL; }
        else { b[k] = 0; *len = (size_t)k; }
    }
    close(fd);
    return b;
}

static int man_page(const char *root, const char *code, const char *name, DescOut *o, int depth) {
    static const char secs[] = "186";
    for (const char *sec = secs; *sec; sec++) {
        for (int gz = 1; gz >= 0; gz--) {
            char path[600];
            if ((size_t)snprintf(path, sizeof path, "%s/%s%sman%c/%s.%c%s", root, code, code[0] ? "/" : "", *sec, name, *sec, gz ? ".gz" : "") >= sizeof path) continue;
            size_t len = 0;
            char *raw = read_system_file(path, 2 * 1024 * 1024, &len), *page = raw;
            if (!raw) continue;
            size_t plen = len;
            if (gz) {
                uint8_t *out = NULL;
                size_t ol = 0;
                if (gz_decode((const uint8_t *)raw, len, &out, &ol, 8 * 1024 * 1024)) { free(raw); continue; }
                free(raw);
                page = (char *)out;
                plen = ol;
            }
            /* ".so man1/other.1": the page is another one's (followed once) */
            if (depth == 0 && plen > 4 && !strncmp(page, ".so ", 4)) {
                char other[128];
                size_t k = strcspn(page + 4, "\n");
                snprintf(other, sizeof other, "%.*s", (int)(k < 127 ? k : 127), page + 4);
                free(page);
                const char *base = strrchr(other, '/');
                base = base ? base + 1 : other;
                char nm[128];
                snprintf(nm, sizeof nm, "%s", base);
                char *dot = strrchr(nm, '.');
                if (dot && !strcmp(dot, ".gz")) { *dot = 0; dot = strrchr(nm, '.'); }
                if (dot) *dot = 0;
                return name_ok(nm, 64, "._+-") ? man_page(root, code, nm, o, 1) : -1;
            }
            int r = desc_man_summary(page, plen, o->text, sizeof o->text);
            free(page);
            if (!r) return 0;
        }
    }
    return -1;
}

int desc_man(const char *root, const char *name, const char *lang, DescOut *o) {
    *o = (DescOut){ 0 };
    if (!name_ok(name, 64, "._+-")) return -1;
    char codes[3][16];
    int nc = desc_lang_codes(lang, codes);
    for (int i = 0; i < nc; i++) {
        if (!man_page(root, codes[i], name, o, 0)) { snprintf(o->lang, sizeof o->lang, "%s", lang); return 0; }
        /* "es_ES" pages under "es": tried already; "pt_BR" then "pt" */
    }
    if (!man_page(root, "", name, o, 0)) { snprintf(o->lang, sizeof o->lang, "en"); return 0; }
    return -1;
}

/* ---- "unit": a systemd unit's Description= ---- */

int desc_unit(const char *const *dirs, const char *unit, DescOut *o) {
    *o = (DescOut){ 0 };
    if (!name_ok(unit, 200, "-_.@:\\") || !strchr(unit, '.')) return -1;
    const char *ext = strrchr(unit, '.');
    if (strcmp(ext, ".service") && strcmp(ext, ".socket") && strcmp(ext, ".timer") && strcmp(ext, ".scope") && strcmp(ext, ".slice") && strcmp(ext, ".mount"))
        return -1;
    char tmpl[256] = "";
    const char *at = strchr(unit, '@');
    if (at) snprintf(tmpl, sizeof tmpl, "%.*s@%s", (int)(at - unit), unit, ext);      /* "getty@tty1.service" → "getty@.service" */
    const char *home = getenv("HOME");
    uid_t me = getuid();
    for (int pass = 0; pass < (tmpl[0] ? 2 : 1); pass++) {
        for (const char *const *d = dirs; *d; d++) {
            char path[600];
            if ((*d)[0] == '~') {
                if (!home || home[0] != '/') continue;
                if ((size_t)snprintf(path, sizeof path, "%s%s/%s", home, *d + 1, pass ? tmpl : unit) >= sizeof path) continue;
            } else if ((size_t)snprintf(path, sizeof path, "%s/%s", *d, pass ? tmpl : unit) >= sizeof path) continue;
            int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
            if (fd < 0) continue;
            struct stat st;
            char buf[65537];
            ssize_t k = -1;
            if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (st.st_uid == 0 || st.st_uid == me) && !(st.st_mode & 002)) k = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (k <= 0) continue;
            buf[k] = 0;
            int in_unit = 0;
            for (char *line = buf, *next; line && *line; line = next) {
                next = strchr(line, '\n');
                if (next) *next++ = 0;
                while (*line == ' ' || *line == '\t') line++;
                if (line[0] == '[') { in_unit = !strncmp(line, "[Unit]", 6); continue; }
                if (in_unit && !strncmp(line, "Description=", 12) && line[12]) {
                    put_text(o->text, sizeof o->text, line + 12, strlen(line + 12));
                    snprintf(o->lang, sizeof o->lang, "en");
                    return o->text[0] ? 0 : -1;
                }
            }
        }
    }
    return -1;
}

/* ---- the cache and its worker ---- */

typedef struct { char *key; char *json; int state; long long mt; } Ent;     /* state: -1 queued, 0 none, 1 known */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Ent ents[DESC_CACHE];
static int nents, queued, started;

static long long mtime_of(const char *p) {
    struct stat st;
    return stat(p, &st) ? -1 : st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

static char *to_json(const DescOut *o) {
    size_t cap = (strlen(o->text) + strlen(o->lng)) * 6 + 96;
    char *j = malloc(cap);
    if (!j) return NULL;
    size_t k = (size_t)snprintf(j, cap, "{\"text\":");
    k += json_quote(j + k, cap - k, o->text);
    k += (size_t)snprintf(j + k, cap - k, ",\"long\":");
    k += json_quote(j + k, cap - k, o->lng);
    k += (size_t)snprintf(j + k, cap - k, ",\"lang\":");
    k += json_quote(j + k, cap - k, o->lang);
    snprintf(j + k, cap - k, "}");
    return j;
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!queued) pthread_cond_wait(&cv, &mu);
        char *key = NULL;
        for (int i = 0; i < nents && !key; i++) if (ents[i].state < 0) key = strdup(ents[i].key);
        if (!key) { queued = 0; pthread_mutex_unlock(&mu); continue; }
        pthread_mutex_unlock(&mu);
        /* "kind\x1farg\x1flang" */
        char *kind = key, *a = strchr(kind, 0x1f), *lang = a ? strchr(a + 1, 0x1f) : NULL;
        DescOut o;
        int r = -1;
        long long mt = -1;
        if (a && lang) {
            *a++ = 0;
            *lang++ = 0;
            if (!strcmp(kind, "pkg")) { mt = mtime_of(desc_status_path); r = desc_pkg(desc_status_path, desc_lists_dir, a, lang, &o); }
            else if (!strcmp(kind, "man")) r = desc_man(desc_man_root, a, lang, &o);
            else if (!strcmp(kind, "unit")) r = desc_unit(desc_unit_dirs, a, &o);
            a[-1] = 0x1f;
            lang[-1] = 0x1f;
        }
        char *json = !r ? to_json(&o) : NULL;
        pthread_mutex_lock(&mu);
        int put = 0;
        for (int i = 0; i < nents; i++)
            if (ents[i].state < 0 && !strcmp(ents[i].key, key)) {
                free(ents[i].json);
                ents[i].json = json;
                ents[i].state = json ? 1 : 0;
                ents[i].mt = mt;
                put = 1;
                break;
            }
        if (!put) free(json);
        pthread_mutex_unlock(&mu);
        free(key);
    }
    return NULL;
}

int desc_lookup(const char *kind, const char *arg, const char *lang, char **json) {
    *json = NULL;
    if (!kind || !arg || !lang || (strcmp(kind, "pkg") && strcmp(kind, "man") && strcmp(kind, "unit"))) return -1;
    if (strlen(arg) > 200 || strlen(lang) > 15 || !name_ok(lang, 15, "-_")) return -1;
    for (const char *p = arg; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x1f || *p == '/') return -1;
    char key[256];
    snprintf(key, sizeof key, "%s\x1f%s\x1f%s", kind, arg, lang);
    pthread_mutex_lock(&mu);
    Ent *e = NULL;
    for (int i = 0; i < nents && !e; i++) if (!strcmp(ents[i].key, key)) e = &ents[i];
    /* a package's answer is asked again when dpkg's database changed (an update) */
    if (e && e->state >= 0 && !strcmp(kind, "pkg") && e->mt != mtime_of(desc_status_path)) { e->state = -1; queued = 1; }
    int r = -1;
    if (e && e->state >= 0) {
        r = e->state;
        if (r && e->json) *json = strdup(e->json);
        if (r && !*json) r = -1;
    } else if (!e) {
        if (nents == DESC_CACHE) {                  /* full: forget the oldest half */
            for (int i = 0; i < DESC_CACHE / 2; i++) { free(ents[i].key); free(ents[i].json); }
            memmove(ents, ents + DESC_CACHE / 2, sizeof ents[0] * (DESC_CACHE - DESC_CACHE / 2));
            nents = DESC_CACHE - DESC_CACHE / 2;
        }
        char *k = strdup(key);
        if (k) { ents[nents++] = (Ent){ k, NULL, -1, -1 }; queued = 1; }
    }
    if (queued) {
        if (!started) {
            pthread_t th;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&at, 512 * 1024);
            if (!pthread_create(&th, &at, worker, NULL)) started = 1;
            pthread_attr_destroy(&at);
        }
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
    return r;
}
