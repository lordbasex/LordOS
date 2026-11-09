/* vmodel.c — contents of an open vault (see vmodel.h). Everything lives in the secrets arena. */
#include "vmodel.h"
#include "smem.h"
#include "vcrypto.h"
#include "monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HIST_MAX 20

void vm_init(VModel *m) { memset(m, 0, sizeof *m); }

void vm_entry_free(VEntry *e) {
    if (!e) return;
    sm_free(e->cat); sm_free(e->title); sm_free(e->notes);
    for (int i = 0; i < e->nf; i++) { sm_free(e->f[i].k); sm_free(e->f[i].t); sm_free(e->f[i].l); sm_free(e->f[i].v); }
    sm_free(e->f);
    for (int i = 0; i < e->ntags; i++) sm_free(e->tags[i]);
    sm_free(e->tags);
    for (int i = 0; i < e->natt; i++) { sm_free(e->att[i].name); sm_free(e->att[i].mime); }
    sm_free(e->att);
    sm_free(e->hist); sm_free(e->x); sm_free(e->fold);
    sm_free(e);
}

void vm_free(VModel *m) {
    for (int i = 0; i < m->n; i++) vm_entry_free(m->e[i]);
    sm_free(m->e);
    sm_free(m->meta); sm_free(m->cats); sm_free(m->tags);
    memset(m, 0, sizeof *m);
}

void vm_new_id(char out[33]) {
    uint8_t r[16];
    vc_random(r, 16);
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
}

static int valid_id(const char *s) {
    if (!s || strlen(s) != 32) return 0;
    for (const char *p = s; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}

/* ---- folding for search: lowercase and without accents (Latin) ---- */

static const char *fold_cp(unsigned cp, char *tmp) {
    /* U+00C0–U+00FF and some of Latin Extended-A */
    static const char *lat1[64] = {
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","x","o","u","u","u","u","y","th","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y" };
    if (cp >= 0xc0 && cp <= 0xff) return lat1[cp - 0xc0];
    if (cp >= 0x100 && cp <= 0x17f) {
        static const char base[] = "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
        size_t i = cp - 0x100;
        if (i < sizeof base - 1) { tmp[0] = base[i]; tmp[1] = 0; return tmp; }
    }
    return NULL;
}

char *vm_fold(const char *s) {
    if (!s) return sm_strdup("");
    size_t n = strlen(s);
    char *o = sm_alloc(n * 2 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); i++; continue; }
        unsigned cp = 0;
        int len = 1;
        if ((c & 0xe0) == 0xc0 && i + 1 < n) { cp = (unsigned)(c & 0x1f) << 6 | (s[i + 1] & 0x3f); len = 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < n) { cp = (unsigned)(c & 0x0f) << 12 | (unsigned)(s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f); len = 3; }
        else if ((c & 0xf8) == 0xf0 && i + 3 < n) len = 4;
        char tmp[2];
        const char *r = cp ? fold_cp(cp, tmp) : NULL;
        if (r) { size_t rl = strlen(r); memcpy(o + k, r, rl); k += rl; }
        else { memcpy(o + k, s + i, (size_t)len); k += (size_t)len; }
        i += (size_t)len;
    }
    o[k] = 0;
    return o;
}

const char *vm_field(const VEntry *e, const char *key) {
    for (int i = 0; i < e->nf; i++) if (e->f[i].k && !strcmp(e->f[i].k, key)) return e->f[i].v;
    return NULL;
}

const char *vm_field_type(const VEntry *e, const char *type) {
    for (int i = 0; i < e->nf; i++) if (e->f[i].t && !strcmp(e->f[i].t, type) && e->f[i].v && e->f[i].v[0]) return e->f[i].v;
    return NULL;
}

static int hidden_type(const char *t) { return t && (!strcmp(t, "hidden") || !strcmp(t, "totp") || !strcmp(t, "pin")); }

void vm_refresh(VEntry *e) {
    VjB b;
    vb_init(&b);
    vb_raw(&b, e->title ? e->title : "");
    for (int i = 0; i < e->nf; i++) {
        if (hidden_type(e->f[i].t) || !e->f[i].v) continue;
        vb_raw(&b, " ");
        vb_raw(&b, e->f[i].v);
    }
    for (int i = 0; i < e->ntags; i++) { vb_raw(&b, " #"); vb_raw(&b, e->tags[i]); }
    if (e->notes) { vb_raw(&b, " "); vb_rawn(&b, e->notes, strnlen(e->notes, 400)); }
    sm_free(e->fold);
    e->fold = vm_fold(b.p ? b.p : "");
    vb_free(&b);
    const char *pw = vm_field(e, "password");
    memset(e->pwhash, 0, sizeof e->pwhash);
    if (pw && pw[0]) crypto_blake2b_keyed(e->pwhash, 16, (const uint8_t *)"lordos-vault:reuse", 18, (const uint8_t *)pw, strlen(pw));
}

/* ---- load ---- */

static char *raw_of(const Vj *j, int tok, const char *def) {
    if (tok < 0) return sm_strdup(def);
    return sm_strndup(j->s + j->t[tok].start - (j->t[tok].type == VJ_STR ? 1 : 0),
                      j->t[tok].end - j->t[tok].start + (j->t[tok].type == VJ_STR ? 2 : 0));
}

static int hex16(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != n * 2) return -1;
    for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return 0;
}

static int load_fields(VEntry *e, const Vj *j, int arr) {
    int n = j->t[arr].count;
    if (n > 512) return -1;
    e->f = sm_alloc(sizeof(VField) * (size_t)(n ? n : 1));
    if (!e->f) return -1;
    for (int i = 0, k = arr + 1; i < n; i++, k = j->t[k].next) {
        if (vj_type(j, k) != VJ_OBJ) continue;
        VField *f = &e->f[e->nf++];
        f->k = vj_gets(j, k, "k");
        f->t = vj_gets(j, k, "t");
        f->l = vj_gets(j, k, "l");
        f->v = vj_gets(j, k, "v");
        if (!f->k) f->k = sm_strdup("");
        if (!f->t) f->t = sm_strdup("text");
        if (!f->v) f->v = sm_strdup("");
    }
    return 0;
}

static int load_tags(VEntry *e, const Vj *j, int arr) {
    if (vj_type(j, arr) != VJ_ARR) return 0;
    int n = j->t[arr].count;
    if (n > 128) n = 128;
    e->tags = sm_alloc(sizeof(char *) * (size_t)(n ? n : 1));
    if (!e->tags) return -1;
    for (int i = 0, k = arr + 1; i < j->t[arr].count; i++, k = j->t[k].next) {
        if (e->ntags >= n) break;
        char *s = vj_str(j, k);
        if (s && s[0]) e->tags[e->ntags++] = s; else sm_free(s);
    }
    return 0;
}

static int load_atts(VEntry *e, const Vj *j, int arr) {
    if (vj_type(j, arr) != VJ_ARR) return 0;
    int n = j->t[arr].count;
    if (n > 256) return -1;
    e->att = sm_alloc(sizeof(VAtt) * (size_t)(n ? n : 1));
    if (!e->att) return -1;
    for (int i = 0, k = arr + 1; i < n; i++, k = j->t[k].next) {
        VAtt *a = &e->att[e->natt];
        char buf[80];
        vj_getsbuf(j, k, "id", buf, sizeof buf);
        if (hex16(buf, a->id, 16)) continue;
        vj_getsbuf(j, k, "nonce", buf, sizeof buf);
        if (hex16(buf, a->nonce, 24)) continue;
        vj_getsbuf(j, k, "mac", buf, sizeof buf);
        if (hex16(buf, a->mac, 16)) continue;
        a->name = vj_gets(j, k, "name");
        a->mime = vj_gets(j, k, "mime");
        if (!a->name) a->name = sm_strdup("file");
        if (!a->mime) a->mime = sm_strdup("application/octet-stream");
        a->size = (uint64_t)vj_getn(j, k, "size", 0);
        e->natt++;
    }
    return 0;
}

static VEntry *load_entry(const Vj *j, int o) {
    VEntry *e = sm_alloc(sizeof *e);
    if (!e) return NULL;
    vj_getsbuf(j, o, "id", e->id, sizeof e->id);
    if (!valid_id(e->id)) vm_new_id(e->id);
    e->cat = vj_gets(j, o, "cat");
    e->title = vj_gets(j, o, "title");
    e->notes = vj_gets(j, o, "notes");
    if (!e->cat) e->cat = sm_strdup("general");
    if (!e->title) e->title = sm_strdup("");
    e->fav = vj_getb(j, o, "fav", 0);
    e->arch = vj_getb(j, o, "arch", 0);
    e->rotate = (int)vj_getn(j, o, "rotate", 0);
    e->trash = (int64_t)vj_getn(j, o, "trash", 0);
    e->created = (int64_t)vj_getn(j, o, "created", 0);
    e->modified = (int64_t)vj_getn(j, o, "modified", 0);
    e->pwch = (int64_t)vj_getn(j, o, "pwch", 0);
    int f = vj_get(j, o, "f");
    if ((vj_type(j, f) == VJ_ARR && load_fields(e, j, f)) || load_tags(e, j, vj_get(j, o, "tags")) || load_atts(e, j, vj_get(j, o, "att"))) {
        vm_entry_free(e);
        return NULL;
    }
    int h = vj_get(j, o, "hist");
    e->hist = vj_type(j, h) == VJ_ARR ? raw_of(j, h, "[]") : sm_strdup("[]");
    int x = vj_get(j, o, "x");
    e->x = vj_type(j, x) == VJ_OBJ ? raw_of(j, x, "{}") : NULL;
    vm_refresh(e);
    return e;
}

static int push(VModel *m, VEntry *e) {
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 64;
        VEntry **ne = sm_realloc(m->e, sizeof(VEntry *) * (size_t)nc);
        if (!ne) return -1;
        m->e = ne;
        m->cap = nc;
    }
    m->e[m->n++] = e;
    return 0;
}

int vm_load(VModel *m, const char *json, size_t len) {
    vm_init(m);
    Vj j;
    if (vj_parse(&j, json, len) || vj_type(&j, 0) != VJ_OBJ) { vj_free(&j); return -1; }
    int v = (int)vj_getn(&j, 0, "v", 0);
    if (v != 1) { vj_free(&j); return -1; }
    int meta = vj_get(&j, 0, "meta"), cats = vj_get(&j, 0, "cats"), tags = vj_get(&j, 0, "tags");
    m->meta = vj_type(&j, meta) == VJ_OBJ ? raw_of(&j, meta, "{}") : sm_strdup("{}");
    m->cats = vj_type(&j, cats) == VJ_ARR ? raw_of(&j, cats, "[]") : sm_strdup("[]");
    m->tags = vj_type(&j, tags) == VJ_ARR ? raw_of(&j, tags, "[]") : sm_strdup("[]");
    int es = vj_get(&j, 0, "entries");
    if (vj_type(&j, es) == VJ_ARR) {
        for (int i = 0, k = es + 1; i < j.t[es].count; i++, k = j.t[k].next) {
            if (vj_type(&j, k) != VJ_OBJ) continue;
            VEntry *e = load_entry(&j, k);
            if (!e || push(m, e)) { vm_entry_free(e); vj_free(&j); vm_free(m); return -1; }
        }
    }
    vj_free(&j);
    return 0;
}

/* ---- save ---- */

static void hexs(VjB *b, const uint8_t *p, size_t n) {
    char h[130];
    for (size_t i = 0; i < n && i < 64; i++) snprintf(h + 2 * i, 3, "%02x", p[i]);
    h[2 * (n < 64 ? n : 64)] = 0;
    vb_str(b, h);
}

static void fields_json(VjB *b, const VEntry *e, int reveal) {
    vb_key(b, "f");
    vb_open(b, '[');
    for (int i = 0; i < e->nf; i++) {
        const VField *f = &e->f[i];
        vb_sep(b);
        vb_open(b, '{');
        vb_ks(b, "k", f->k);
        vb_ks(b, "t", f->t);
        if (f->l) vb_ks(b, "l", f->l);
        if (reveal || !hidden_type(f->t)) vb_ks(b, "v", f->v);
        else vb_kb(b, "has", f->v && f->v[0]);
        vb_close(b, '}');
    }
    vb_close(b, ']');
}

static void tags_json(VjB *b, const VEntry *e) {
    vb_key(b, "tags");
    vb_open(b, '[');
    for (int i = 0; i < e->ntags; i++) { vb_sep(b); vb_str(b, e->tags[i]); }
    vb_close(b, ']');
}

static void atts_json(VjB *b, const VEntry *e, int full) {
    vb_key(b, "att");
    vb_open(b, '[');
    for (int i = 0; i < e->natt; i++) {
        const VAtt *a = &e->att[i];
        vb_sep(b);
        vb_open(b, '{');
        vb_key(b, "id"); hexs(b, a->id, 16);
        vb_ks(b, "name", a->name);
        vb_ks(b, "mime", a->mime);
        vb_kn(b, "size", (double)a->size);
        if (full) { vb_key(b, "nonce"); hexs(b, a->nonce, 24); vb_key(b, "mac"); hexs(b, a->mac, 16); }
        vb_close(b, '}');
    }
    vb_close(b, ']');
}

static void entry_common(VjB *b, const VEntry *e) {
    vb_ks(b, "id", e->id);
    vb_ks(b, "cat", e->cat);
    vb_ks(b, "title", e->title);
    if (e->notes && e->notes[0]) vb_ks(b, "notes", e->notes);
    vb_kb(b, "fav", e->fav);
    vb_kb(b, "arch", e->arch);
    vb_kn(b, "trash", (double)e->trash);
    vb_kn(b, "created", (double)e->created);
    vb_kn(b, "modified", (double)e->modified);
    vb_kn(b, "pwch", (double)e->pwch);
    vb_kn(b, "rotate", e->rotate);
    tags_json(b, e);
}

char *vm_dump(const VModel *m, size_t *len) {
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_ki(&b, "v", 1);
    vb_key(&b, "meta"); vb_raw(&b, m->meta ? m->meta : "{}");
    vb_key(&b, "cats"); vb_raw(&b, m->cats ? m->cats : "[]");
    vb_key(&b, "tags"); vb_raw(&b, m->tags ? m->tags : "[]");
    vb_key(&b, "entries");
    vb_open(&b, '[');
    for (int i = 0; i < m->n; i++) {
        const VEntry *e = m->e[i];
        vb_sep(&b);
        vb_open(&b, '{');
        entry_common(&b, e);
        fields_json(&b, e, 1);
        atts_json(&b, e, 1);
        vb_key(&b, "hist"); vb_raw(&b, e->hist ? e->hist : "[]");
        if (e->x) { vb_key(&b, "x"); vb_raw(&b, e->x); }
        vb_close(&b, '}');
    }
    vb_close(&b, ']');
    vb_close(&b, '}');
    if (b.oom) { vb_free(&b); return NULL; }
    *len = b.n;
    return b.p;
}

/* ---- for the interface ---- */

/* The history keeps the hidden values; without reveal they are removed (the JSON is rewritten). */
static void hist_json(VjB *b, const char *hist, int reveal) {
    if (reveal || !hist) { vb_raw(b, hist ? hist : "[]"); return; }
    Vj j;
    if (vj_parse(&j, hist, strlen(hist)) || vj_type(&j, 0) != VJ_ARR) { vj_free(&j); vb_raw(b, "[]"); return; }
    vb_open(b, '[');
    for (int i = 0, k = 1; i < j.t[0].count; i++, k = j.t[k].next) {
        vb_sep(b);
        vb_open(b, '{');
        vb_kn(b, "t", vj_getn(&j, k, "t", 0));
        char *title = vj_gets(&j, k, "title");
        vb_ks(b, "title", title ? title : "");
        sm_free(title);
        int f = vj_get(&j, k, "f");
        vb_key(b, "f");
        vb_open(b, '[');
        for (int a = 0, q = f + 1; vj_type(&j, f) == VJ_ARR && a < j.t[f].count; a++, q = j.t[q].next) {
            char *fk = vj_gets(&j, q, "k"), *ft = vj_gets(&j, q, "t"), *fl = vj_gets(&j, q, "l"), *fv = vj_gets(&j, q, "v");
            vb_sep(b);
            vb_open(b, '{');
            vb_ks(b, "k", fk ? fk : "");
            vb_ks(b, "t", ft ? ft : "text");
            if (fl) vb_ks(b, "l", fl);
            if (hidden_type(ft)) vb_kb(b, "has", fv && fv[0]); else vb_ks(b, "v", fv ? fv : "");
            vb_close(b, '}');
            sm_free(fk); sm_free(ft); sm_free(fl); sm_free(fv);
        }
        vb_close(b, ']');
        vb_close(b, '}');
    }
    vb_close(b, ']');
    vj_free(&j);
}

void vm_entry_json(VjB *b, const VEntry *e, int reveal, int hist) {
    vb_open(b, '{');
    entry_common(b, e);
    fields_json(b, e, reveal);
    atts_json(b, e, 0);
    if (hist) { vb_key(b, "hist"); hist_json(b, e->hist, reveal); }
    if (e->x) { vb_key(b, "x"); vb_raw(b, e->x); }
    vb_close(b, '}');
}

/* list subtitle: username, email, or the last 4 digits of a card */
static const char *subtitle(const VEntry *e, char *tmp, size_t n) {
    const char *u = vm_field(e, "username");
    if (u && u[0]) return u;
    u = vm_field(e, "email");
    if (u && u[0]) return u;
    u = vm_field(e, "number");
    if (u && u[0] && (!strcmp(e->cat, "cards"))) {
        char d[32];
        int k = 0;
        for (const char *p = u; *p && k < 31; p++) if (*p >= '0' && *p <= '9') d[k++] = *p;
        d[k] = 0;
        snprintf(tmp, n, "•••• %s", k >= 4 ? d + k - 4 : d);
        return tmp;
    }
    /* documents: the number (if it is not hidden) */
    for (int i = 0; i < e->nf; i++)
        if (!strcmp(e->f[i].k, "number") && !strcmp(e->f[i].t, "text") && e->f[i].v[0]) return e->f[i].v;
    for (const char *key = "ssid"; key; key = !strcmp(key, "ssid") ? "host" : !strcmp(key, "host") ? "product" : !strcmp(key, "product") ? "service" : !strcmp(key, "service") ? "account" : NULL) {
        u = vm_field(e, key);
        if (u && u[0]) return u;
    }
    return "";
}

void vm_list_json(VjB *b, const VModel *m) {
    vb_open(b, '[');
    char tmp[64];
    for (int i = 0; i < m->n; i++) {
        const VEntry *e = m->e[i];
        vb_sep(b);
        vb_open(b, '{');
        vb_ks(b, "id", e->id);
        vb_ks(b, "cat", e->cat);
        vb_ks(b, "title", e->title);
        vb_ks(b, "sub", subtitle(e, tmp, sizeof tmp));
        const char *url = vm_field(e, "url");
        if (url && url[0]) vb_ks(b, "url", url);
        const char *host = vm_field(e, "host");     /* Network, SSH and Database entries: autofill matches it */
        if (host && host[0]) vb_ks(b, "host", host);
        if (e->fav) vb_kb(b, "fav", 1);
        if (e->arch) vb_kb(b, "arch", 1);
        if (e->trash) vb_kn(b, "trash", (double)e->trash);
        vb_kn(b, "modified", (double)e->modified);
        if (e->pwch) vb_kn(b, "pwch", (double)e->pwch);
        if (e->rotate) vb_kn(b, "rotate", e->rotate);
        if (vm_field_type(e, "totp")) vb_kb(b, "totp", 1);
        if (e->natt) vb_kn(b, "natt", e->natt);
        const char *exp = vm_field(e, "expiry");
        if (exp && exp[0]) vb_ks(b, "exp", exp);
        if (e->ntags) tags_json(b, e);
        vb_close(b, '}');
    }
    vb_close(b, ']');
}

void vm_search_json(VjB *b, const VModel *m, const char *q, int max) {
    char *fq = vm_fold(q ? q : "");
    char *words[16];
    int nw = 0;
    for (char *p = fq; p && *p && nw < 16;) {
        while (*p == ' ') p++;
        if (!*p) break;
        words[nw++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    vb_open(b, '[');
    int found = 0;
    for (int i = 0; i < m->n && found < max; i++) {
        const char *s = m->e[i]->fold;
        if (!s) continue;
        int ok = 1;
        for (int w = 0; w < nw && ok; w++) if (!strstr(s, words[w])) ok = 0;
        if (!ok) continue;
        vb_sep(b);
        vb_str(b, m->e[i]->id);
        found++;
    }
    vb_close(b, ']');
    sm_free(fq);
}

VEntry *vm_find(const VModel *m, const char *id) {
    int i = vm_index(m, id);
    return i >= 0 ? m->e[i] : NULL;
}

int vm_index(const VModel *m, const char *id) {
    if (!id) return -1;
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i]->id, id)) return i;
    return -1;
}

int vm_remove(VModel *m, const char *id) {
    int i = vm_index(m, id);
    if (i < 0) return -1;
    vm_entry_free(m->e[i]);
    memmove(m->e + i, m->e + i + 1, sizeof(VEntry *) * (size_t)(m->n - i - 1));
    m->n--;
    m->dirty = 1;
    return 0;
}

/* ---- create/update from the interface ---- */

static int str_eq(const char *a, const char *b) { return !strcmp(a ? a : "", b ? b : ""); }

static int fields_differ(const VEntry *a, const VEntry *b) {
    if (a->nf != b->nf || !str_eq(a->title, b->title)) return 1;
    for (int i = 0; i < a->nf; i++)
        if (!str_eq(a->f[i].k, b->f[i].k) || !str_eq(a->f[i].v, b->f[i].v) || !str_eq(a->f[i].l, b->f[i].l) || !str_eq(a->f[i].t, b->f[i].t)) return 1;
    return 0;
}

static int valid_ftype(const char *t) {
    static const char *ok[] = { "text", "hidden", "url", "date", "number", "file", "totp", "email", "multiline", "pin", "bool", NULL };
    for (int i = 0; ok[i]; i++) if (!strcmp(t, ok[i])) return 1;
    return 0;
}

/* new entry built from the JSON; old (if any) for the "keep" fields */
static VEntry *entry_from_ui(const Vj *j, int o, const VEntry *old, char *err, size_t errn) {
    VEntry *e = sm_alloc(sizeof *e);
    if (!e) { snprintf(err, errn, "memory"); return NULL; }
    e->cat = vj_gets(j, o, "cat");
    e->title = vj_gets(j, o, "title");
    e->notes = vj_gets(j, o, "notes");
    if (!e->cat || !e->cat[0] || strlen(e->cat) > 64) { sm_free(e->cat); e->cat = sm_strdup(old ? old->cat : "general"); }
    if (!e->title) e->title = sm_strdup("");
    if (strlen(e->title) > 1024 || (e->notes && strlen(e->notes) > (1u << 20))) { vm_entry_free(e); snprintf(err, errn, "too-big"); return NULL; }
    e->fav = vj_getb(j, o, "fav", old ? old->fav : 0);
    e->arch = vj_getb(j, o, "arch", old ? old->arch : 0);
    e->rotate = (int)vj_getn(j, o, "rotate", old ? old->rotate : 0);
    if (e->rotate < 0 || e->rotate > 3650) e->rotate = 0;
    int f = vj_get(j, o, "f");
    int nf = vj_type(j, f) == VJ_ARR ? j->t[f].count : 0;
    if (nf > 256) { vm_entry_free(e); snprintf(err, errn, "too-many-fields"); return NULL; }
    e->f = sm_alloc(sizeof(VField) * (size_t)(nf ? nf : 1));
    if (!e->f) { vm_entry_free(e); snprintf(err, errn, "memory"); return NULL; }
    for (int i = 0, k = f + 1; i < nf; i++, k = j->t[k].next) {
        if (vj_type(j, k) != VJ_OBJ) continue;
        VField *d = &e->f[e->nf];
        d->k = vj_gets(j, k, "k");
        d->t = vj_gets(j, k, "t");
        d->l = vj_gets(j, k, "l");
        if (!d->k || !d->k[0] || strlen(d->k) > 64) { sm_free(d->k); d->k = sm_strdup("field"); }
        if (!d->t || !valid_ftype(d->t)) { sm_free(d->t); d->t = sm_strdup("text"); }
        if (vj_getb(j, k, "keep", 0) && old) {
            /* keep the previous value of the field with the same key (and the same position if it repeats) */
            const char *pv = NULL;
            int seen = 0;
            for (int q = 0; q < i; q++) if (e->f[q].k && !strcmp(e->f[q].k, d->k)) seen++;
            for (int q = 0; q < old->nf; q++) if (!strcmp(old->f[q].k, d->k) && seen-- == 0) { pv = old->f[q].v; break; }
            d->v = sm_strdup(pv ? pv : "");
        } else d->v = vj_gets(j, k, "v");
        if (!d->v) d->v = sm_strdup("");
        if (strlen(d->v) > (1u << 20)) { e->nf++; vm_entry_free(e); snprintf(err, errn, "too-big"); return NULL; }
        e->nf++;
    }
    if (load_tags(e, j, vj_get(j, o, "tags"))) { vm_entry_free(e); snprintf(err, errn, "memory"); return NULL; }
    int x = vj_get(j, o, "x");
    if (vj_type(j, x) == VJ_OBJ) e->x = raw_of(j, x, "{}");
    else if (old && old->x) e->x = sm_strdup(old->x);
    return e;
}

static void add_history(VEntry *e, const VEntry *old, int64_t now) {
    VjB b;
    vb_init(&b);
    vb_open(&b, '[');
    vb_sep(&b);
    vb_open(&b, '{');
    vb_kn(&b, "t", (double)(old->modified ? old->modified : now));
    vb_ks(&b, "title", old->title);
    fields_json(&b, old, 1);
    vb_close(&b, '}');
    /* the previous ones, up to HIST_MAX */
    Vj j;
    if (old->hist && !vj_parse(&j, old->hist, strlen(old->hist)) && vj_type(&j, 0) == VJ_ARR) {
        for (int i = 0, k = 1; i < j.t[0].count && i < HIST_MAX - 1; i++, k = j.t[k].next) {
            vb_sep(&b);
            vb_rawn(&b, j.s + j.t[k].start, j.t[k].end - j.t[k].start);
        }
        vj_free(&j);
    }
    vb_close(&b, ']');
    sm_free(e->hist);
    e->hist = b.oom ? sm_strdup("[]") : b.p;
    if (b.oom) vb_free(&b);
}

int vm_upsert(VModel *m, const Vj *j, int tok, VEntry **out, char *err, size_t errn) {
    if (vj_type(j, tok) != VJ_OBJ) { snprintf(err, errn, "bad-entry"); return -1; }
    char id[40] = "";
    vj_getsbuf(j, tok, "id", id, sizeof id);
    int idx = id[0] ? vm_index(m, id) : -1;
    if (id[0] && idx < 0 && !valid_id(id)) { snprintf(err, errn, "bad-id"); return -1; }
    VEntry *old = idx >= 0 ? m->e[idx] : NULL;
    VEntry *e = entry_from_ui(j, tok, old, err, errn);
    if (!e) return -1;
    int64_t now = (int64_t)time(NULL);
    if (old) {
        memcpy(e->id, old->id, sizeof e->id);
        e->created = old->created;
        e->trash = (int64_t)vj_getn(j, tok, "trash", (double)old->trash);
        e->pwch = old->pwch;
        /* attachments: kept (they are added and removed with their own requests) */
        e->att = old->att; e->natt = old->natt;
        VEntry *o2 = old;
        o2->att = NULL; o2->natt = 0;
        if (fields_differ(e, old)) {
            add_history(e, old, now);
            e->modified = now;
        } else {
            e->hist = sm_strdup(old->hist ? old->hist : "[]");
            int meta_change = e->fav != old->fav || e->arch != old->arch || e->trash != old->trash || !str_eq(e->cat, old->cat) || !str_eq(e->notes, old->notes) || e->ntags != old->ntags || e->rotate != old->rotate;
            for (int i = 0; !meta_change && i < e->ntags; i++) if (strcmp(e->tags[i], old->tags[i])) meta_change = 1;
            e->modified = meta_change ? now : old->modified;
            if (!str_eq(e->notes, old->notes)) e->modified = now;
        }
        if (!str_eq(vm_field(e, "password"), vm_field(old, "password"))) e->pwch = now;
        vm_refresh(e);
        vm_entry_free(old);
        m->e[idx] = e;
    } else {
        if (id[0] && valid_id(id)) snprintf(e->id, sizeof e->id, "%s", id); else vm_new_id(e->id);
        e->created = (int64_t)vj_getn(j, tok, "created", (double)now);
        e->modified = (int64_t)vj_getn(j, tok, "modified", (double)now);
        e->pwch = vm_field(e, "password") ? e->modified : 0;
        e->trash = 0;
        e->hist = sm_strdup("[]");
        vm_refresh(e);
        if (push(m, e)) { vm_entry_free(e); snprintf(err, errn, "memory"); return -1; }
    }
    m->dirty = 1;
    if (out) *out = e;
    return 0;
}
