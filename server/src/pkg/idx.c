/* idx.c — building and reading the package index (see idx.h). */
#include "idx.h"
#include "../errcode.h"
#include "deb.h"
#include "crypto.h"
#include "pin.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- reading ---- */

/* Each record's priority under apt's preferences (none: the index's source priorities). */
static void idx_pins(Idx *x) {
    free(x->prio);
    x->prio = NULL;
    x->pin_stamp = pin_stamp();
    PinSet *set = pin_load();
    if (!set) return;
    x->prio = malloc(sizeof *x->prio * (x->h->npkg ? x->h->npkg : 1));
    if (x->prio)
        for (long i = 0; i < (long)x->h->npkg; i++) x->prio[i] = (int16_t)pin_priority(set, x, i);
    pin_free(set);
}

int idx_open(Idx *x, const char *dir) {
    memset(x, 0, sizeof *x);
    char path[512];
    snprintf(path, sizeof path, "%s/index.bin", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(IdxHeader)) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    const IdxHeader *h = m;
    size_t len = (size_t)st.st_size;
#define SEC_OK(off, n, sz) ((uint64_t)(off) + (uint64_t)(n) * (sz) <= len && (off) % 4 == 0)
    if (memcmp(h->magic, IDX_MAGIC, 8) || !SEC_OK(h->off_pkg, h->npkg, sizeof(IdxPkg)) ||
        !SEC_OK(h->off_prov, h->nprov, sizeof(IdxProv)) || !SEC_OK(h->off_app, h->napp, sizeof(IdxApp)) ||
        !SEC_OK(h->off_src, h->nsrc, sizeof(IdxSrc)) || !SEC_OK(h->off_str, h->len_str, 1) || !h->len_str ||
        !SEC_OK(h->off_desc, h->len_desc, 1) || ((const char *)m)[h->off_str + h->len_str - 1]) {
        munmap(m, len);
        return -1;
    }
#undef SEC_OK
    x->h = h;
    x->map = m;
    x->map_len = len;
    x->pkg = (const IdxPkg *)((const char *)m + h->off_pkg);
    x->prov = (const IdxProv *)((const char *)m + h->off_prov);
    x->app = (const IdxApp *)((const char *)m + h->off_app);
    x->src = (const IdxSrc *)((const char *)m + h->off_src);
    x->str = (const char *)m + h->off_str;
    x->str_len = h->len_str;
    x->desc = (const char *)m + h->off_desc;
    x->desc_len = h->len_desc;
    x->mtime = (long)st.st_mtime;
    x->ino = (long)st.st_ino;
    idx_pins(x);
    return 0;
}

void idx_close(Idx *x) {
    if (x->map) munmap(x->map, x->map_len);
    free(x->prio);
    memset(x, 0, sizeof *x);
}

int idx_refresh(Idx *x, const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/index.bin", dir);
    struct stat st;
    if (stat(path, &st)) { if (x->map) { idx_close(x); return 1; } return 0; }
    if (x->map && (long)st.st_ino == x->ino && (long)st.st_mtime == x->mtime) {
        if (pin_stamp() == x->pin_stamp) return 0;
        idx_pins(x);                    /* the preferences changed: same index, new priorities */
        return 1;
    }
    Idx n;
    if (idx_open(&n, dir)) return 0;
    idx_close(x);
    *x = n;
    return 1;
}

static int name_cmp(const char *s, const char *k, size_t n) {
    int c = strncmp(s, k, n);
    if (c) return c;
    return s[n] ? 1 : 0;
}

long idx_find(const Idx *x, const char *name, size_t n) {
    if (!x->map) return -1;
    long lo = 0, hi = (long)x->h->npkg;
    while (lo < hi) {
        long mid = (lo + hi) / 2;
        if (name_cmp(idx_s(x, x->pkg[mid].name), name, n) < 0) lo = mid + 1; else hi = mid;
    }
    return lo < (long)x->h->npkg && !name_cmp(idx_s(x, x->pkg[lo].name), name, n) ? lo : -1;
}

void idx_providers(const Idx *x, const char *name, size_t n, long *plo, long *phi) {
    *plo = *phi = 0;
    if (!x->map) return;
    long lo = 0, hi = (long)x->h->nprov;
    while (lo < hi) {
        long mid = (lo + hi) / 2;
        if (name_cmp(idx_s(x, x->prov[mid].name), name, n) < 0) lo = mid + 1; else hi = mid;
    }
    long e = lo;
    while (e < (long)x->h->nprov && !name_cmp(idx_s(x, x->prov[e].name), name, n)) e++;
    *plo = lo;
    *phi = e;
}

/* ---- building ---- */

struct IdxBuilder {
    char *str;
    size_t slen, scap;
    uint32_t *ht;                       /* interned strings (UINT32_MAX = free) */
    size_t htcap, htn;
    IdxPkg *pk;
    uint8_t (*md5)[16];
    uint8_t *hasmd5;
    size_t npk, cappk, capmd5;
    int32_t *mnext, *mht;               /* md5 → packages */
    size_t mhtcap;
    IdxSrc src[64];
    int nsrc;
    char *desc;
    size_t dlen, dcap;
    IdxApp *app;
    size_t napp, capapp;
    uint32_t *icons;                    /* set of icon names */
    size_t iconcap, nicons;
};

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static int grow(void **p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    void *n = realloc(*p, c * elem);
    if (!n) return -1;
    *p = n;
    *cap = c;
    return 0;
}

static int ht_rehash(IdxBuilder *b) {
    size_t cap = b->htcap ? b->htcap * 2 : 1 << 16;
    uint32_t *n = malloc(cap * sizeof *n);
    if (!n) return -1;
    memset(n, 0xff, cap * sizeof *n);
    for (size_t i = 0; i < b->htcap; i++) {
        uint32_t o = b->ht[i];
        if (o == UINT32_MAX) continue;
        size_t j = fnv(b->str + o, strlen(b->str + o)) & (cap - 1);
        while (n[j] != UINT32_MAX) j = (j + 1) & (cap - 1);
        n[j] = o;
    }
    free(b->ht);
    b->ht = n;
    b->htcap = cap;
    return 0;
}

/* Interns a string (without repeats); returns its offset or UINT32_MAX if out of memory. */
static uint32_t intern(IdxBuilder *b, const char *s, size_t n) {
    if (!n) return 0;
    if ((b->htn + 1) * 2 > b->htcap && ht_rehash(b)) return UINT32_MAX;
    size_t j = fnv(s, n) & (b->htcap - 1);
    while (b->ht[j] != UINT32_MAX) {
        const char *t = b->str + b->ht[j];
        if (!strncmp(t, s, n) && !t[n]) return b->ht[j];
        j = (j + 1) & (b->htcap - 1);
    }
    if (b->slen + n + 1 > UINT32_MAX - 1 || grow((void **)&b->str, &b->scap, b->slen + n + 1, 1)) return UINT32_MAX;
    uint32_t o = (uint32_t)b->slen;
    memcpy(b->str + b->slen, s, n);
    b->str[b->slen + n] = 0;
    b->slen += n + 1;
    b->ht[j] = o;
    b->htn++;
    return o;
}

static uint32_t intern_z(IdxBuilder *b, const char *s) { return intern(b, s, s ? strlen(s) : 0); }

IdxBuilder *idxb_new(void) {
    IdxBuilder *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    if (grow((void **)&b->str, &b->scap, 1, 1)) { free(b); return NULL; }
    b->str[0] = 0;
    b->slen = 1;
    return b;
}

void idxb_free(IdxBuilder *b) {
    if (!b) return;
    free(b->str); free(b->ht); free(b->pk); free(b->md5); free(b->hasmd5); free(b->mnext); free(b->mht);
    free(b->desc); free(b->app); free(b->icons);
    free(b);
}

uint32_t idxb_count(const IdxBuilder *b) { return (uint32_t)b->npk; }

int idxb_source(IdxBuilder *b, const char *uri, const char *suite, const char *component, const char *codename,
                const char *origin, const char *label, int64_t date, int64_t valid_until, int prio) {
    if (b->nsrc >= 64) return -1;
    IdxSrc *s = &b->src[b->nsrc];
    memset(s, 0, sizeof *s);
    s->uri = intern_z(b, uri);
    s->suite = intern_z(b, suite);
    s->component = intern_z(b, component);
    s->codename = intern_z(b, codename);
    s->origin = intern_z(b, origin);
    s->label = intern_z(b, label);
    s->date = date;
    s->valid_until = valid_until;
    s->prio = (uint32_t)prio;
    return b->nsrc++;
}

static uint32_t put_desc(IdxBuilder *b, const char *s, size_t n, uint32_t *len) {
    /* Debian "Description": lines with a leading space; " ." is an empty paragraph */
    if (grow((void **)&b->desc, &b->dcap, b->dlen + n + 1, 1)) { *len = 0; return 0; }
    uint32_t start = (uint32_t)b->dlen;
    const char *p = s, *e = s + n;
    int first = 1;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        if (!nl) nl = e;
        const char *l = p;
        size_t ll = (size_t)(nl - p);
        if (ll && *l == ' ') { l++; ll--; }
        if (ll == 1 && *l == '.') ll = 0;
        if (!first) b->desc[b->dlen++] = '\n';
        first = 0;
        memcpy(b->desc + b->dlen, l, ll);
        b->dlen += ll;
        p = nl + 1;
    }
    *len = (uint32_t)(b->dlen - start);
    return start;
}

static int prio_of(const char *v, size_t n) {
    static const char *const names[] = { "required", "important", "standard", "optional", "extra" };
    for (int i = 0; i < 5; i++) if (strlen(names[i]) == n && !strncmp(v, names[i], n)) return i + 1;
    return PRIO_UNKNOWN;
}

#define FIELD(name) (d822_is(&f, name))

int idxb_packages(IdxBuilder *b, int src, const char *buf, size_t len, const char *arch) {
    const char *pos = buf, *end = buf + len, *st, *se;
    size_t archlen = strlen(arch);
    uint32_t count = 0;
    while (d822_next_stanza(&pos, end, &st, &se)) {
        D822 it = { st, se };
        D822Field f;
        IdxPkg p;
        memset(&p, 0, sizeof p);
        p.app = UINT32_MAX;
        p.src = (uint16_t)src;
        uint8_t md5[16];
        int hasmd5 = 0, ok_arch = 0, has_sha = 0;
        const char *name = NULL, *desc = NULL;
        size_t namelen = 0, desclen = 0;
        while (d822_next_field(&it, &f)) {
            const char *v = f.value;
            size_t n = f.vlen;
            switch (f.name[0] | 0x20) {
            case 'a':
                if (FIELD("Architecture")) { p.arch = intern(b, v, n); ok_arch = (n == archlen && !strncmp(v, arch, n)) || (n == 3 && !strncmp(v, "all", 3)); }
                break;
            case 'b':
                if (FIELD("Breaks")) p.breaks = intern(b, v, n);
                break;
            case 'c':
                if (FIELD("Conflicts")) p.conflicts = intern(b, v, n);
                break;
            case 'd':
                if (FIELD("Depends")) p.depends = intern(b, v, n);
                else if (FIELD("Description")) { desc = v; desclen = n; }
                else if (FIELD("Description-md5") && n == 32 && !hex_decode(v, md5, 16)) hasmd5 = 1;
                break;
            case 'e':
                if (FIELD("Essential") && n == 3 && !strncmp(v, "yes", 3)) p.flags |= IF_ESSENTIAL;
                break;
            case 'f':
                if (FIELD("Filename")) p.filename = intern(b, v, n);
                break;
            case 'h':
                if (FIELD("Homepage")) p.homepage = intern(b, v, n);
                break;
            case 'i':
                if (FIELD("Installed-Size")) p.isize = (uint32_t)strtoul(v, NULL, 10);
                break;
            case 'm':
                if (FIELD("Multi-Arch")) {
                    if (n == 4 && !strncmp(v, "same", 4)) p.flags |= IF_MA_SAME;
                    else if (n == 7 && !strncmp(v, "foreign", 7)) p.flags |= IF_MA_FOREIGN;
                    else if (n == 7 && !strncmp(v, "allowed", 7)) p.flags |= IF_MA_ALLOWED;
                }
                break;
            case 'p':
                if (FIELD("Package")) { name = v; namelen = n; }
                else if (FIELD("Pre-Depends")) p.predepends = intern(b, v, n);
                else if (FIELD("Provides")) p.provides = intern(b, v, n);
                else if (FIELD("Priority")) p.prio = (uint8_t)prio_of(v, n);
                else if (FIELD("Protected") && n == 3 && !strncmp(v, "yes", 3)) p.flags |= IF_PROTECTED;
                break;
            case 'r':
                if (FIELD("Recommends")) p.recommends = intern(b, v, n);
                else if (FIELD("Replaces")) p.replaces = intern(b, v, n);
                break;
            case 's':
                if (FIELD("Size")) p.size = (uint32_t)strtoul(v, NULL, 10);
                else if (FIELD("SHA256") && n == 64 && !hex_decode(v, p.sha256, 32)) has_sha = 1;
                else if (FIELD("Section")) p.section = intern(b, v, n);
                else if (FIELD("Suggests")) p.suggests = intern(b, v, n);
                else if (FIELD("Source")) p.source = intern(b, v, n);
                break;
            case 'v':
                if (FIELD("Version")) p.version = intern(b, v, n);
                break;
            }
        }
        if (!name || !ok_arch || !p.version || !p.filename || !has_sha || !deb_valid_name(name, namelen)) continue;
        /* names with a slash or .. in Filename are rejected: they're downloaded to a fixed name, but still */
        const char *fn = b->str + p.filename;
        if (strstr(fn, "..") || fn[0] == '/') continue;
        p.name = intern(b, name, namelen);
        if (desc) {
            const char *nl = memchr(desc, '\n', desclen);
            size_t sl = nl ? (size_t)(nl - desc) : desclen;
            p.summary = intern(b, desc, sl);
            if (nl) p.desc = put_desc(b, nl + 1, desclen - sl - 1, &p.desclen);
        }
        if (grow((void **)&b->pk, &b->cappk, b->npk + 1, sizeof *b->pk)) return -1;
        if (b->cappk > b->capmd5) {         /* parallel arrays with the same capacity */
            uint8_t (*nm)[16] = realloc(b->md5, b->cappk * 16);
            if (!nm) return -1;
            b->md5 = nm;
            uint8_t *nh = realloc(b->hasmd5, b->cappk);
            if (!nh) return -1;
            b->hasmd5 = nh;
            b->capmd5 = b->cappk;
        }
        b->pk[b->npk] = p;
        if (hasmd5) memcpy(b->md5[b->npk], md5, 16);
        b->hasmd5[b->npk] = (uint8_t)(hasmd5 && !p.desclen);
        b->npk++;
        count++;
        if (p.name == UINT32_MAX || p.version == UINT32_MAX) return -1;
    }
    b->src[src].npkgs += count;
    return 0;
}

static uint32_t md5h(const uint8_t *m) { return (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24; }

static int build_md5_map(IdxBuilder *b) {
    if (b->mht) return 0;
    size_t cap = 1;
    while (cap < b->npk * 2) cap <<= 1;
    b->mht = malloc(cap * sizeof *b->mht);
    b->mnext = malloc((b->npk ? b->npk : 1) * sizeof *b->mnext);
    if (!b->mht || !b->mnext) return -1;
    memset(b->mht, 0xff, cap * sizeof *b->mht);
    b->mhtcap = cap;
    for (size_t i = 0; i < b->npk; i++) {
        b->mnext[i] = -1;
        if (!b->hasmd5[i]) continue;
        uint32_t h = md5h(b->md5[i]) & (uint32_t)(cap - 1);
        b->mnext[i] = b->mht[h];
        b->mht[h] = (int32_t)i;
    }
    return 0;
}

int idxb_translation(IdxBuilder *b, const char *buf, size_t len) {
    if (build_md5_map(b)) return -1;
    const char *pos = buf, *end = buf + len, *st, *se;
    while (d822_next_stanza(&pos, end, &st, &se)) {
        D822 it = { st, se };
        D822Field f;
        uint8_t md5[16];
        int has = 0;
        const char *d = NULL;
        size_t dl = 0;
        while (d822_next_field(&it, &f)) {
            if (d822_is(&f, "Description-md5") && f.vlen == 32 && !hex_decode(f.value, md5, 16)) has = 1;
            else if (d822_is(&f, "Description-en")) { d = f.value; dl = f.vlen; }
        }
        if (!has || !d) continue;
        const char *nl = memchr(d, '\n', dl);
        if (!nl) continue;
        uint32_t off = 0, olen = 0;
        int done = 0;
        for (int32_t i = b->mht[md5h(md5) & (uint32_t)(b->mhtcap - 1)]; i >= 0; i = b->mnext[i]) {
            if (!b->hasmd5[i] || memcmp(b->md5[i], md5, 16)) continue;
            if (!done) { off = put_desc(b, nl + 1, dl - (size_t)(nl + 1 - d), &olen); done = 1; }
            b->pk[i].desc = off;
            b->pk[i].desclen = olen;
            b->hasmd5[i] = 0;
        }
    }
    return 0;
}

/* ---- DEP-11 (AppStream) ---- */

/* Trims trailing spaces and leaves exactly n line breaks (if there's already text). */
static void line_break(char *out, size_t *o, int n) {
    while (*o && (out[*o - 1] == ' ' || out[*o - 1] == '\n')) (*o)--;
    if (*o) while (n--) out[(*o)++] = '\n';
}

/* AppStream HTML (<p>, <ul><li>, <em>, <code>) to plain text. */
static size_t html_text(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    int par = 0;                        /* a paragraph has to be started before the next text */
    for (size_t i = 0; i < n && o + 8 < cap;) {
        if (s[i] == '<') {
            const char *gt = memchr(s + i, '>', n - i);
            if (!gt) break;
            size_t tl = (size_t)(gt - (s + i)) + 1;
            if (!strncmp(s + i, "<p>", 3) || !strncmp(s + i, "<ul>", 4) || !strncmp(s + i, "<ol>", 4)) par = 1;
            else if (!strncmp(s + i, "<li>", 4)) {
                line_break(out, &o, par ? 2 : 1);
                par = 0;
                memcpy(out + o, "\xe2\x80\xa2 ", 4);         /* "• " */
                o += 4;
            }
            i += tl;
            continue;
        }
        char c = s[i];
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
        if (c == ' ' && (!o || out[o - 1] == ' ' || out[o - 1] == '\n')) { i++; continue; }
        if (par) { line_break(out, &o, 2); par = 0; }
        if (c == '&') {
            static const struct { const char *e; char c; } ent[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' }, { "&apos;", '\'' } };
            int hit = 0;
            for (size_t k = 0; k < sizeof ent / sizeof *ent; k++) {
                size_t el = strlen(ent[k].e);
                if (n - i >= el && !strncmp(s + i, ent[k].e, el)) { out[o++] = ent[k].c; i += el; hit = 1; break; }
            }
            if (hit) continue;
        }
        out[o++] = c;
        i++;
    }
    line_break(out, &o, 0);
    out[o] = 0;
    return o;
}

/* YAML scalar value on one line: unquoted or with '…' / "…". */
static size_t yaml_scalar(const char *v, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (n && v[0] == '\'') {
        for (size_t i = 1; i < n && o + 1 < cap; i++) {
            if (v[i] == '\'') { if (i + 1 < n && v[i + 1] == '\'') { out[o++] = '\''; i++; continue; } break; }
            out[o++] = v[i];
        }
    } else if (n && v[0] == '"') {
        for (size_t i = 1; i < n && o + 1 < cap; i++) {
            if (v[i] == '"') break;
            if (v[i] == '\\' && i + 1 < n) { i++; out[o++] = v[i] == 'n' ? '\n' : v[i] == 't' ? '\t' : v[i]; continue; }
            out[o++] = v[i];
        }
    } else {
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\r')) n--;
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        o = n;
    }
    out[o] = 0;
    return o;
}

typedef struct {
    char type[32], id[256], package[128], cats[256], icon[256];
    char name[3][256], summary[3][512];
    char *desc;
    size_t dlen, dcap;
    int icon_w;
} App;

static void app_store(IdxBuilder *b, App *a) {
    if (!a->id[0] || !a->package[0] || !deb_valid_name(a->package, strlen(a->package))) return;
    if (grow((void **)&b->app, &b->capapp, b->napp + 1, sizeof *b->app)) return;
    IdxApp *x = &b->app[b->napp++];
    memset(x, 0, sizeof *x);
    x->id = intern_z(b, a->id);
    x->package = intern_z(b, a->package);
    x->type = intern_z(b, a->type);
    x->name = intern_z(b, a->name[0]);
    x->summary = intern_z(b, a->summary[0]);
    x->name_es = intern_z(b, a->name[1]);
    x->summary_es = intern_z(b, a->summary[1]);
    x->name_pt = intern_z(b, a->name[2]);
    x->summary_pt = intern_z(b, a->summary[2]);
    x->categories = intern_z(b, a->cats);
    x->icon = intern_z(b, a->icon);
    x->desc = UINT32_MAX;
    if (a->dlen) {                      /* in the descriptions block, 0-terminated */
        char *t = malloc(a->dlen + 16);
        if (t) {
            size_t tl = html_text(a->desc, a->dlen, t, a->dlen + 16);
            if (tl && !grow((void **)&b->desc, &b->dcap, b->dlen + tl + 1, 1)) {
                x->desc = (uint32_t)b->dlen;
                memcpy(b->desc + b->dlen, t, tl + 1);
                b->dlen += tl + 1;
            }
            free(t);
        }
    }
    if (a->icon[0]) {
        uint32_t o = x->icon;
        if ((b->nicons + 1) * 2 > b->iconcap) {
            size_t cap = b->iconcap ? b->iconcap * 2 : 4096;
            uint32_t *n = malloc(cap * sizeof *n);
            if (!n) return;
            memset(n, 0xff, cap * sizeof *n);
            for (size_t i = 0; i < b->iconcap; i++) if (b->icons[i] != UINT32_MAX) {
                size_t j = fnv(b->str + b->icons[i], strlen(b->str + b->icons[i])) & (cap - 1);
                while (n[j] != UINT32_MAX) j = (j + 1) & (cap - 1);
                n[j] = b->icons[i];
            }
            free(b->icons);
            b->icons = n;
            b->iconcap = cap;
        }
        size_t j = fnv(b->str + o, strlen(b->str + o)) & (b->iconcap - 1);
        while (b->icons[j] != UINT32_MAX && b->icons[j] != o) j = (j + 1) & (b->iconcap - 1);
        if (b->icons[j] == UINT32_MAX) { b->icons[j] = o; b->nicons++; }
    }
}

int idxb_icon_wanted(IdxBuilder *b, const char *name) {
    if (!b->iconcap) return 0;
    size_t n = strlen(name);
    size_t j = fnv(name, n) & (b->iconcap - 1);
    while (b->icons[j] != UINT32_MAX) {
        if (!strcmp(b->str + b->icons[j], name)) return 1;
        j = (j + 1) & (b->iconcap - 1);
    }
    return 0;
}

static int lang_slot(const char *k, size_t n) {
    if (n == 1 && k[0] == 'C') return 0;
    if (n == 2 && !strncmp(k, "es", 2)) return 1;
    if (n == 2 && !strncmp(k, "pt", 2)) return 2;
    if (n == 5 && !strncmp(k, "pt-BR", 5)) return 3;       /* if there's no pt */
    return -1;
}

int idxb_appstream(IdxBuilder *b, const char *buf, size_t len) {
    App a;
    memset(&a, 0, sizeof a);
    const char *p = buf, *end = buf + len;
    char top[32] = "";
    int in_cached = 0, block = 0, block_desc = 0, cont_slot = -1, cont_is_name = 0, cur_icon_set = 0;
    char cur_icon[256] = "";
    for (;;) {
        const char *nl = p < end ? memchr(p, '\n', (size_t)(end - p)) : NULL;
        const char *le = nl ? nl : end;
        size_t ll = (size_t)(le - p);
        int doc_end = p >= end || (ll >= 3 && !strncmp(p, "---", 3));
        size_t ind = 0;
        while (ind < ll && p[ind] == ' ') ind++;
        /* literal block (>- / |): empty lines or with indentation ≥ 4 */
        if (block && !doc_end && (ind == ll || ind >= 4)) {
            if (block_desc) {
                const char *t = p + (ind < 4 ? ind : 4);
                size_t tl = (size_t)(le - t);
                if (!grow((void **)&a.desc, &a.dcap, a.dlen + tl + 2, 1)) {
                    if (a.dlen) a.desc[a.dlen++] = ind == ll ? '\n' : ' ';
                    memcpy(a.desc + a.dlen, t, tl);
                    a.dlen += tl;
                }
            }
            goto next;
        }
        block = block_desc = 0;
        if (cont_slot >= 0 && !doc_end && ind >= 4 && ind < ll) {   /* split plain scalar */
            char *dst = cont_is_name ? a.name[cont_slot] : a.summary[cont_slot];
            size_t cap = cont_is_name ? sizeof a.name[0] : sizeof a.summary[0], dl = strlen(dst);
            snprintf(dst + dl, cap - dl, " %.*s", (int)(ll - ind), p + ind);
            goto next;
        }
        cont_slot = -1;
        if (doc_end) {
            if (cur_icon_set && !a.icon[0]) snprintf(a.icon, sizeof a.icon, "%s", cur_icon);
            if (a.type[0]) app_store(b, &a);
            char *keep = a.desc;
            size_t kc = a.dcap;
            memset(&a, 0, sizeof a);
            a.desc = keep;
            a.dcap = kc;
            top[0] = 0;
            in_cached = cur_icon_set = 0;
            cur_icon[0] = 0;
            if (p >= end) break;
            goto next;
        }
        if (ind == 0) {
            if (ll >= 2 && p[0] == '-' && p[1] == ' ') {
                if (!strcmp(top, "Categories")) {
                    size_t cl = strlen(a.cats);
                    snprintf(a.cats + cl, sizeof a.cats - cl, "%s%.*s", cl ? ";" : "", (int)(ll - 2), p + 2);
                }
                goto next;
            }
            const char *colon = memchr(p, ':', ll);
            if (!colon) goto next;
            size_t kl = (size_t)(colon - p);
            snprintf(top, sizeof top, "%.*s", (int)(kl < 31 ? kl : 31), p);
            const char *v = colon + 1;
            while (v < le && *v == ' ') v++;
            size_t vl = (size_t)(le - v);
            in_cached = 0;
            if (!strcmp(top, "Type")) yaml_scalar(v, vl, a.type, sizeof a.type);
            else if (!strcmp(top, "ID")) yaml_scalar(v, vl, a.id, sizeof a.id);
            else if (!strcmp(top, "Package")) yaml_scalar(v, vl, a.package, sizeof a.package);
            goto next;
        }
        if (ind == 2 && (!strcmp(top, "Name") || !strcmp(top, "Summary") || !strcmp(top, "Description"))) {
            const char *k = p + 2, *colon = memchr(k, ':', (size_t)(le - k));
            if (!colon) goto next;
            int slot = lang_slot(k, (size_t)(colon - k));
            const char *v = colon + 1;
            while (v < le && *v == ' ') v++;
            size_t vl = (size_t)(le - v);
            if (vl && (v[0] == '>' || v[0] == '|')) {
                block = 1;
                block_desc = !strcmp(top, "Description") && slot == 0;
                if (block_desc) a.dlen = 0;
                goto next;
            }
            if (slot < 0 || !strcmp(top, "Description")) goto next;
            int is_name = !strcmp(top, "Name");
            if (slot == 3) {                    /* pt-BR: only if there's no pt */
                char *dst = is_name ? a.name[2] : a.summary[2];
                if (dst[0]) goto next;
                slot = 2;
            }
            if (is_name) yaml_scalar(v, vl, a.name[slot], sizeof a.name[0]);
            else yaml_scalar(v, vl, a.summary[slot], sizeof a.summary[0]);
            if (vl && v[0] != '\'' && v[0] != '"') { cont_slot = slot; cont_is_name = is_name; }
            goto next;
        }
        if (!strcmp(top, "Icon")) {
            if (ind == 2 && ll >= 9 && !strncmp(p + 2, "cached:", 7)) { in_cached = 1; goto next; }
            if (ind == 2 && p[2] != '-') { in_cached = 0; goto next; }
            if (!in_cached) goto next;
            if (ind == 2 && ll > 10 && !strncmp(p + 2, "- name:", 7)) {
                const char *v = p + 9;
                while (v < le && *v == ' ') v++;
                yaml_scalar(v, (size_t)(le - v), cur_icon, sizeof cur_icon);
                cur_icon_set = 1;
                goto next;
            }
            if (ind == 4 && ll > 10 && !strncmp(p + 4, "width:", 6)) {
                if (atoi(p + 10) == 64 && cur_icon[0]) snprintf(a.icon, sizeof a.icon, "%s", cur_icon);
            }
        }
    next:
        p = nl ? nl + 1 : end;
    }
    free(a.desc);
    return 0;
}

/* ---- writing ---- */

static const IdxBuilder *sort_b;

static int pkg_order(const void *pa, const void *pb) {
    const IdxPkg *a = &sort_b->pk[*(const uint32_t *)pa], *b = &sort_b->pk[*(const uint32_t *)pb];
    int c = strcmp(sort_b->str + a->name, sort_b->str + b->name);
    if (c) return c;
    c = ver_cmp(sort_b->str + b->version, sort_b->str + a->version);      /* newest first */
    if (c) return c;
    return (int)sort_b->src[b->src].prio - (int)sort_b->src[a->src].prio;
}

static int prov_order(const void *pa, const void *pb) {
    const IdxProv *a = pa, *b = pb;
    int c = strcmp(sort_b->str + a->name, sort_b->str + b->name);
    if (c) return c;
    return a->pkg < b->pkg ? -1 : a->pkg > b->pkg;
}

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static int pad4(int fd, size_t *off) {
    static const char z[4] = { 0 };
    size_t p = (4 - *off % 4) % 4;
    *off += p;
    return p ? write_all(fd, z, p) : 0;
}

int idxb_write(IdxBuilder *b, const char *dir, const char *arch, char *err, size_t errlen) {
    if (b->npk >= UINT32_MAX / 2) { lerr_set(err, errlen, "pkg.index.tooManyPackages", NULL); return -1; }
    uint32_t *perm = malloc((b->npk ? b->npk : 1) * sizeof *perm), *inv = malloc((b->npk ? b->npk : 1) * sizeof *inv);
    if (!perm || !inv) { free(perm); free(inv); lerr_set(err, errlen, "sys.outOfMemory", NULL); return -1; }
    for (size_t i = 0; i < b->npk; i++) perm[i] = (uint32_t)i;
    sort_b = b;
    qsort(perm, b->npk, sizeof *perm, pkg_order);
    for (size_t i = 0; i < b->npk; i++) inv[perm[i]] = (uint32_t)i;
    IdxPkg *sorted = malloc((b->npk ? b->npk : 1) * sizeof *sorted);
    if (!sorted) { free(perm); free(inv); lerr_set(err, errlen, "sys.outOfMemory", NULL); return -1; }
    for (size_t i = 0; i < b->npk; i++) sorted[i] = b->pk[perm[i]];

    /* provides */
    IdxProv *prov = NULL;
    size_t nprov = 0, capprov = 0;
    for (size_t i = 0; i < b->npk; i++) {
        if (!sorted[i].provides) continue;
        const char *s = b->str + sorted[i].provides, *e = s + strlen(s);
        DebRel r[8];
        int n;
        while ((n = deb_next_group(&s, e, r, 8)) > 0) {
            if (grow((void **)&prov, &capprov, nprov + (size_t)n, sizeof *prov)) break;
            for (int k = 0; k < n; k++) {
                IdxProv *x = &prov[nprov++];
                x->name = intern(b, r[k].name, r[k].nlen);
                x->version = r[k].op == REL_EQ ? intern(b, r[k].ver, r[k].vlen) : 0;
                x->pkg = (uint32_t)i;
            }
        }
    }
    sort_b = b;
    qsort(prov, nprov, sizeof *prov, prov_order);

    /* apps → packages (all versions with that name) */
    for (size_t a = 0; a < b->napp; a++) {
        const char *pn = b->str + b->app[a].package;
        size_t lo = 0, hi = b->npk;
        while (lo < hi) { size_t m = (lo + hi) / 2; if (strcmp(b->str + sorted[m].name, pn) < 0) lo = m + 1; else hi = m; }
        for (size_t i = lo; i < b->npk && !strcmp(b->str + sorted[i].name, pn); i++) {
            /* if the package has several, the first desktop application wins */
            if (sorted[i].app == UINT32_MAX || (strcmp(b->str + b->app[sorted[i].app].type, "desktop-application") &&
                                               !strcmp(b->str + b->app[a].type, "desktop-application")))
                sorted[i].app = (uint32_t)a;
        }
    }

    IdxHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, IDX_MAGIC, 8);
    snprintf(h.arch, sizeof h.arch, "%s", arch);
    h.npkg = (uint32_t)b->npk;
    h.nprov = (uint32_t)nprov;
    h.napp = (uint32_t)b->napp;
    h.nsrc = (uint32_t)b->nsrc;
    h.built = (int64_t)time(NULL);
    size_t off = sizeof h;
    h.off_pkg = (uint32_t)off; off += b->npk * sizeof(IdxPkg);
    h.off_prov = (uint32_t)off; off += nprov * sizeof(IdxProv);
    h.off_app = (uint32_t)off; off += b->napp * sizeof(IdxApp);
    off = (off + 7) & ~(size_t)7;
    h.off_src = (uint32_t)off; off += (size_t)b->nsrc * sizeof(IdxSrc);
    h.off_str = (uint32_t)off; h.len_str = (uint32_t)b->slen; off += b->slen;
    off = (off + 3) & ~(size_t)3;
    h.off_desc = (uint32_t)off; h.len_desc = (uint32_t)b->dlen; off += b->dlen;
    if (off > UINT32_MAX) { free(perm); free(inv); free(sorted); free(prov); lerr_set(err, errlen, "pkg.index.tooLarge", NULL); return -1; }

    char tmp[512], path[512];
    snprintf(tmp, sizeof tmp, "%s/index.bin.new", dir);
    snprintf(path, sizeof path, "%s/index.bin", dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    int ok = fd >= 0;
    size_t w = 0;
    if (ok) ok = !write_all(fd, &h, sizeof h);
    w += sizeof h;
    if (ok) ok = !write_all(fd, sorted, b->npk * sizeof(IdxPkg));
    w += b->npk * sizeof(IdxPkg);
    if (ok) ok = !write_all(fd, prov, nprov * sizeof(IdxProv));
    w += nprov * sizeof(IdxProv);
    if (ok) ok = !write_all(fd, b->app, b->napp * sizeof(IdxApp));
    w += b->napp * sizeof(IdxApp);
    while (ok && w % 8) { ok = !write_all(fd, "", 1); w++; }
    if (ok) ok = !write_all(fd, b->src, (size_t)b->nsrc * sizeof(IdxSrc));
    w += (size_t)b->nsrc * sizeof(IdxSrc);
    if (ok) ok = !write_all(fd, b->str, b->slen);
    w += b->slen;
    if (ok) ok = !pad4(fd, &w);
    if (ok) ok = !write_all(fd, b->desc, b->dlen);
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = 0;
    free(perm); free(inv); free(sorted); free(prov);
    if (!ok || rename(tmp, path)) { unlink(tmp); lerr_set(err, errlen, "pkg.index.writeFailed", "path", path, "errno", lerr_errno(errno), NULL); return -1; }
    return 0;
}
