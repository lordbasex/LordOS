/* fonts.c — catalog of installed fonts. See fonts.h. */
#include "fonts.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Faces, not files: a Debian desktop with fonts-noto-extra, the CJK collections or TeX Live can have more than
 * 1024, and with that old limit the scan stopped wherever readdir() happened to be, possibly before Noto
 * Sans, DejaVu Sans and Noto Color Emoji: every fallback glyph then drew as a missing box (#56). The array is
 * allocated once (callers keep pointers into it) and only the entries used are touched. */
#define MAX_ENTRIES 8192
#define MAX_FILE (64u << 20)

static FontEntry *entries;
static int nentries, scanned;
static char builtin[512];

void fonts_set_builtin_dir(const char *dir) { snprintf(builtin, sizeof builtin, "%s", dir); }

typedef struct { const uint8_t *d; size_t n; } Buf;

static unsigned u16(Buf b, size_t off) { return off + 2 <= b.n ? (unsigned)(b.d[off] << 8 | b.d[off + 1]) : 0; }
static uint32_t u32(Buf b, size_t off) {
    return off + 4 <= b.n ? (uint32_t)b.d[off] << 24 | (uint32_t)b.d[off + 1] << 16 | (uint32_t)b.d[off + 2] << 8 | b.d[off + 3] : 0;
}

static size_t table(Buf b, size_t font, const char *tag, size_t *len) {
    unsigned n = u16(b, font + 4);
    for (unsigned i = 0; i < n && i < 512; i++) {
        size_t r = font + 12 + 16 * i;
        if (r + 16 > b.n) break;
        if (!memcmp(b.d + r, tag, 4)) {
            size_t off = u32(b, r + 8), l = u32(b, r + 12);
            if (off >= b.n || l > b.n - off) return 0;
            *len = l;
            return off;
        }
    }
    return 0;
}

/* Name `id` from the name table, in UTF-8 (platform 3, UTF-16BE, preferred; otherwise platform 1, Mac Roman). */
static int name_of(Buf b, size_t name, size_t nlen, unsigned id, char *out, size_t size) {
    unsigned count = u16(b, name + 2), strs = u16(b, name + 4);
    int best = -1, best_score = -1;
    for (unsigned i = 0; i < count && i < 1024; i++) {
        size_t r = name + 6 + 12 * i;
        if (r + 12 > name + nlen) break;
        if (u16(b, r + 6) != id) continue;
        unsigned plat = u16(b, r), lang = u16(b, r + 4);
        int score = plat == 3 ? (lang == 0x409 ? 3 : 2) : plat == 1 && lang == 0 ? 1 : plat == 0 ? 2 : -1;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) return 0;
    size_t r = name + 6 + 12 * (size_t)best;
    unsigned plat = u16(b, r), len = u16(b, r + 8);
    size_t off = name + strs + u16(b, r + 10);
    if (off + len > b.n) return 0;
    size_t o = 0;
    if (plat == 3 || plat == 0) {
        for (unsigned k = 0; k + 1 < len && o + 4 < size; k += 2) {
            unsigned c = u16(b, off + k);
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800) { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3f)); }
            else { out[o++] = (char)(0xe0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 0x3f)); out[o++] = (char)(0x80 | (c & 0x3f)); }
        }
    } else {
        for (unsigned k = 0; k < len && o + 1 < size; k++) { unsigned char c = b.d[off + k]; out[o++] = c < 0x80 ? (char)c : '?'; }
    }
    out[o] = 0;
    return o > 0;
}

/* Monospaced: post.isFixedPitch or almost all advances equal (ligature fonts
 * like Fira Code have zero-width glyphs, so we look at the majority). */
static int is_mono(Buf b, size_t font) {
    size_t len, post = table(b, font, "post", &len);
    if (post && u32(b, post + 12)) return 1;
    size_t hhea = table(b, font, "hhea", &len), hmtx = table(b, font, "hmtx", &len);
    if (!hhea || !hmtx) return 0;
    unsigned n = u16(b, hhea + 34);
    unsigned common = 0, hits = 0, total = 0;
    for (unsigned i = 0; i < n && i < 4096; i++) {
        unsigned adv = u16(b, hmtx + 4 * i);
        if (!adv) continue;
        total++;
        if (!common) common = adv;
        if (adv == common) hits++;
    }
    return total > 20 && hits * 10 > total * 9;
}

static void add_face(Buf b, size_t font, const char *path, int index) {
    if (nentries >= MAX_ENTRIES) return;
    size_t nlen, name = table(b, font, "name", &nlen);
    if (!name) return;
    FontEntry e;
    memset(&e, 0, sizeof e);
    if (!name_of(b, name, nlen, 16, e.family, sizeof e.family) && !name_of(b, name, nlen, 1, e.family, sizeof e.family)) return;
    if (!name_of(b, name, nlen, 17, e.style, sizeof e.style) && !name_of(b, name, nlen, 2, e.style, sizeof e.style))
        snprintf(e.style, sizeof e.style, "Regular");
    name_of(b, name, nlen, 6, e.ps, sizeof e.ps);
    size_t len, os2 = table(b, font, "OS/2", &len);
    e.weight = os2 ? (int)u16(b, os2 + 4) : 400;
    unsigned sel = os2 ? u16(b, os2 + 62) : 0;
    e.italic = (sel & 1) || (sel & 0x200) || strcasestr(e.style, "italic") || strcasestr(e.style, "oblique");
    if (!os2 && strcasestr(e.style, "bold")) e.weight = 700;
    e.color = table(b, font, "CBDT", &len) != 0;
    e.mono = !e.color && is_mono(b, font);
    if (!table(b, font, "glyf", &len) && !table(b, font, "CFF ", &len) && !e.color) return;
    snprintf(e.path, sizeof e.path, "%s", path);
    e.index = index;
    for (int i = 0; i < nentries; i++)     /* the same font installed twice */
        if (!strcmp(entries[i].family, e.family) && !strcmp(entries[i].style, e.style)) return;
    entries[nentries++] = e;
}

static void scan_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 12 || (size_t)st.st_size > MAX_FILE) { close(fd); return; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    Buf b = { m, (size_t)st.st_size };
    if (!memcmp(b.d, "ttcf", 4)) {
        unsigned n = (unsigned)u32(b, 8);
        for (unsigned i = 0; i < n && i < 64; i++) add_face(b, u32(b, 12 + 4 * i), path, (int)i);
    } else add_face(b, 0, path, 0);
    munmap(m, (size_t)st.st_size);
}

static int font_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") || !strcasecmp(dot, ".ttc"));
}

static void scan_dir(const char *dir, int depth) {
    if (depth > 5) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nentries < MAX_ENTRIES) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(path, depth + 1);
        else if (font_ext(e->d_name)) scan_file(path);
    }
    closedir(d);
}

static int cmp_entry(const void *a, const void *b) {
    const FontEntry *x = a, *y = b;
    int c = strcasecmp(x->family, y->family);
    if (c) return c;
    if (x->italic != y->italic) return x->italic - y->italic;
    return x->weight - y->weight;
}

const FontEntry *fonts_list(int force, int *count) {
    if (!scanned || force) {
        if (!entries) entries = calloc(MAX_ENTRIES, sizeof *entries);
        nentries = 0;
        if (entries) {
            if (builtin[0]) scan_dir(builtin, 0);
            scan_dir("/usr/share/fonts", 0);
            scan_dir("/usr/local/share/fonts", 0);
            const char *home = getenv("HOME");
            if (home) {
                char p[600];
                snprintf(p, sizeof p, "%s/.local/share/fonts", home);
                scan_dir(p, 0);
                snprintf(p, sizeof p, "%s/.fonts", home);
                scan_dir(p, 0);
            }
            qsort(entries, (size_t)nentries, sizeof *entries, cmp_entry);
        }
        scanned = 1;
    }
    *count = nentries;
    return entries;
}

const FontEntry *fonts_pick(const char *family, int bold, int italic) {
    int n;
    const FontEntry *l = fonts_list(0, &n), *best = NULL;
    int best_score = 1 << 30;
    int want = bold ? 700 : 400;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(l[i].family, family)) continue;
        int score = abs(l[i].weight - want) + (l[i].italic != italic) * 2000;
        if (score < best_score) { best_score = score; best = &l[i]; }
    }
    return best;
}

const FontEntry *fonts_by_ps(const char *ps) {
    int n;
    const FontEntry *l = fonts_list(0, &n);
    for (int i = 0; i < n; i++) if (!strcasecmp(l[i].ps, ps)) return &l[i];
    /* some terminal emulators save the name without hyphens: "JetBrainsMonoNL-Regular" vs family */
    char fam[64];
    snprintf(fam, sizeof fam, "%s", ps);
    char *dash = strchr(fam, '-');
    if (dash) *dash = 0;
    for (int i = 0; i < n; i++) {
        char flat[64];
        size_t k = 0;
        for (const char *s = l[i].family; *s && k + 1 < sizeof flat; s++) if (*s != ' ') flat[k++] = *s;
        flat[k] = 0;
        if (!strcasecmp(flat, fam)) return fonts_pick(l[i].family, dash && strcasestr(dash + 1, "bold"), dash && strcasestr(dash + 1, "italic"));
    }
    return NULL;
}

const FontEntry *fonts_color(void) {
    int n;
    const FontEntry *l = fonts_list(0, &n);
    for (int i = 0; i < n; i++) if (l[i].color) return &l[i];
    return NULL;
}
