/* rules.c — store of per-app network rules and the decider (see rules.h and docs/FIREWALL.md).
 *
 * In-memory structure:
 *   FwStore.apps   app id → App
 *   FwStore.ids    rule id (uint32) → RNode
 *   App.exact      exact domain → list of RNode
 *   App.wild       suffix of "*.suffix" → list of RNode
 *   App.trie[0|1]  binary prefix trie for IPv4 / IPv6; each node has its list
 *   App.any/lan/inet  short lists for the destinations without an address
 * Each list groups the rules with the same destination (they differ in direction, ports or protocol). */
#ifndef _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE                /* O_NOFOLLOW with _POSIX_C_SOURCE on BSD-libc hosts (tests) */
#endif
#include "fw/rules.h"
#include "fw/fwmap.h"
#include "sec/json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct RNode RNode;
typedef struct App App;

typedef struct TNode {
    struct TNode *kid[2], *parent;
    RNode *head;
} TNode;

struct RNode {
    FwRule r;
    RNode *next;        /* in its destination's list */
    App *app;
};

struct App {
    char id[FW_APP_MAX];
    int trusted;
    uint32_t restricted;
    size_t nrules;
    FwMap exact, wild;
    TNode *trie[2];
    RNode *any, *lan, *inet;
};

struct FwStore {
    FwMap apps, ids;
    size_t nrules;
    uint32_t next_id;
    FwMode mode;
};

/* ---- text and addresses ---------------------------------------------------------------------- */

static int id_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == ':' || c == '@' || c == '+' || c == '~';
}

static int ends_with(const char *s, size_t n, const char *suf) {
    size_t k = strlen(suf);
    return n >= k && !memcmp(s + n - k, suf, k);
}

int fw_component_ok(const char *name) {
    static const char *const v[] = FW_COMPONENTS;
    for (int i = 0; name && v[i]; i++) if (!strcmp(name, v[i])) return 1;
    return 0;
}

int fw_component_scope(const char *scope, char *app, size_t n) {
    size_t l = scope ? strlen(scope) : 0;
    if (l <= 17 || strncmp(scope, "lordos-net-", 11) || strcmp(scope + l - 6, ".scope")) return -1;
    char comp[32];
    size_t cl = l - 11 - 6;
    if (cl >= sizeof comp) return -1;
    memcpy(comp, scope + 11, cl);
    comp[cl] = 0;
    if (!fw_component_ok(comp)) return -1;
    int w = snprintf(app, n, "lordos:%s", comp);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int fw_app_canon(const char *in, char *out, size_t n) {
    if (!in || !out || !n) return -1;
    size_t len = strnlen(in, FW_APP_MAX);
    if (!len || len >= FW_APP_MAX) return -1;
    char buf[FW_APP_MAX];
    const char *pre = NULL, *mid = in;
    size_t mlen = len;
    /* cgroup scopes (sec/cgroup.h): the .lpk one first, since it's the longest */
    if (ends_with(in, len, ".scope") && !strncmp(in, "app-lordos-lpk-", 15) && len > 15 + 6) {
        pre = "lpk:"; mid = in + 15; mlen = len - 15 - 6;
    } else if (ends_with(in, len, ".scope") && !strncmp(in, "app-lordos-", 11) && len > 11 + 6) {
        pre = "desktop:"; mid = in + 11; mlen = len - 11 - 6;
    }
    if (pre) {
        for (size_t i = 0; i < mlen; i++) if (!id_char((unsigned char)mid[i]) || mid[i] == ':') return -1;
        int w = snprintf(buf, sizeof buf, "%s%.*s", pre, (int)mlen, mid);
        if (w < 0 || (size_t)w >= sizeof buf) return -1;
    } else if (in[0] == '/') {
        /* absolute path of an executable (what the Monitor uses if there's no .desktop) */
        const char *seg = in + 1;
        for (size_t i = 1; i <= len; i++) {
            if (i == len || in[i] == '/') {
                size_t sl = (size_t)(in + i - seg);
                if (!sl || (sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.')) return -1;
                seg = in + i + 1;
            } else if (!id_char((unsigned char)in[i])) return -1;
        }
        memcpy(buf, in, len + 1);
    } else {
        for (size_t i = 0; i < len; i++) if (!id_char((unsigned char)in[i])) return -1;
        const char *c = memchr(in, ':', len);
        if (c && (c == in || c == in + len - 1)) return -1;   /* "desktop:" without an id, ":x" */
        memcpy(buf, in, len + 1);
    }
    size_t bl = strlen(buf);
    if (bl + 1 > n) return -1;
    memcpy(out, buf, bl + 1);
    return 0;
}

/* Domain: ASCII lowercase, labels of 1 to 63 from [a-z0-9-_], 253 in total, an optional trailing
 * dot; the last label can't be only digits (that's a mistyped IP). */
static int norm_domain(const char *in, char *out) {
    size_t len = strnlen(in, FW_NAME_MAX + 1);
    if (len && in[len - 1] == '.') len--;
    if (!len || len > 253) return -1;
    size_t lab = 0, last = 0;
    int digits = 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '.') {
            if (!lab) return -1;
            lab = 0; digits = 1; last = i + 1;
            out[i] = '.';
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return -1;
        if (!(c >= '0' && c <= '9')) digits = 0;
        if (++lab > 63) return -1;
        out[i] = (char)c;
    }
    if (!lab || (digits && len - last > 0)) return -1;
    out[len] = 0;
    return 0;
}

int fw_norm_domain(const char *in, char *out) {
    if (!in || !out || strnlen(in, FW_NAME_MAX + 1) > FW_NAME_MAX) return -1;
    return norm_domain(in, out);
}

static int labels(const char *d) {
    int n = 1;
    for (; *d; d++) n += *d == '.';
    return n;
}

static int is_mapped4(const uint8_t *a) {
    static const uint8_t pre[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    return !memcmp(a, pre, 12);
}

int fw_addr_is_lan(int fam, const uint8_t *a) {
    if (!a) return 0;
    if (fam == 6 && is_mapped4(a)) return fw_addr_is_lan(4, a + 12);
    if (fam == 4) {
        return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 100 && (a[1] & 0xc0) == 64) ||
               (a[0] == 169 && a[1] == 254) || (a[0] == 172 && (a[1] & 0xf0) == 16) ||
               (a[0] == 192 && a[1] == 168) || (a[0] & 0xf0) == 224 ||
               (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255);
    }
    if (fam == 6) {
        static const uint8_t zero[16];
        if (!memcmp(a, zero, 15) && (a[15] == 0 || a[15] == 1)) return 1;     /* :: and ::1 */
        return (a[0] & 0xfe) == 0xfc || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) || a[0] == 0xff;
    }
    return 0;
}

int fw_addr_is_loopback(int fam, const uint8_t *a) {
    if (!a) return 0;
    if (fam == 6 && is_mapped4(a)) return a[12] == 127;
    if (fam == 4) return a[0] == 127;
    if (fam == 6) {
        static const uint8_t zero[15];
        return !memcmp(a, zero, 15) && a[15] == 1;
    }
    return 0;
}

static int bit_at(const uint8_t *a, int i) { return a[i >> 3] >> (7 - (i & 7)) & 1; }

static int host_bits_zero(const uint8_t *a, int bytes, int prefix) {
    for (int i = prefix; i < bytes * 8; i++) if (bit_at(a, i)) return 0;
    return 1;
}

static int parse_uint(const char *s, size_t n, unsigned max, unsigned *out) {
    if (!n || n > 5 || (n > 1 && s[0] == '0')) return -1;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v > max) return -1;
    *out = v;
    return 0;
}

static int parse_cidr(const char *s, FwRule *r) {
    char ip[64];
    const char *slash = strchr(s, '/');
    size_t il = slash ? (size_t)(slash - s) : strlen(s);
    if (!il || il >= sizeof ip) return -1;
    memcpy(ip, s, il);
    ip[il] = 0;
    uint8_t a[16] = { 0 };
    int fam;
    if (strchr(ip, ':')) { if (inet_pton(AF_INET6, ip, a) != 1) return -1; fam = 6; }
    else { if (inet_pton(AF_INET, ip, a) != 1) return -1; fam = 4; }
    unsigned bits = fam == 4 ? 32 : 128, pfx = bits;
    if (slash && parse_uint(slash + 1, strlen(slash + 1), bits, &pfx)) return -1;
    if (fam == 6 && is_mapped4(a) && pfx >= 96) {   /* ::ffff:a.b.c.d/n is IPv4 */
        memmove(a, a + 12, 4);
        memset(a + 4, 0, 12);
        fam = 4;
        pfx -= 96;
    }
    if (!host_bits_zero(a, fam == 4 ? 4 : 16, (int)pfx)) return -1;
    r->kind = FW_DST_CIDR;
    r->fam = fam;
    memcpy(r->addr, a, 16);
    r->prefix = (int)pfx;
    return 0;
}

int fw_parse_dest(const char *s, FwRule *r) {
    if (!s || !r) return -1;
    r->wildcard = 0;
    r->domain[0] = 0;
    r->fam = 0;
    r->prefix = 0;
    memset(r->addr, 0, sizeof r->addr);
    if (!strcmp(s, "any")) { r->kind = FW_DST_ANY; return 0; }
    if (!strcmp(s, "lan")) { r->kind = FW_DST_LAN; return 0; }
    if (!strcmp(s, "internet")) { r->kind = FW_DST_INTERNET; return 0; }
    if (strchr(s, ':') || strchr(s, '/')) return parse_cidr(s, r);
    if (s[0] == '*') {
        if (s[1] != '.' || norm_domain(s + 2, r->domain)) return -1;
        r->kind = FW_DST_DOMAIN;
        r->wildcard = 1;
        return 0;
    }
    /* "1.2.3.4" is an address; if it doesn't parse as one, norm_domain rejects it anyway */
    if (!parse_cidr(s, r)) return 0;
    if (norm_domain(s, r->domain)) return -1;
    r->kind = FW_DST_DOMAIN;
    return 0;
}

int fw_format_dest(const FwRule *r, char *out, size_t n) {
    int w = -1;
    switch (r->kind) {
    case FW_DST_ANY: w = snprintf(out, n, "any"); break;
    case FW_DST_LAN: w = snprintf(out, n, "lan"); break;
    case FW_DST_INTERNET: w = snprintf(out, n, "internet"); break;
    case FW_DST_DOMAIN: w = snprintf(out, n, "%s%s", r->wildcard ? "*." : "", r->domain); break;
    case FW_DST_CIDR: {
        char ip[64];
        if (!inet_ntop(r->fam == 4 ? AF_INET : AF_INET6, r->addr, ip, sizeof ip)) return -1;
        if (r->prefix == (r->fam == 4 ? 32 : 128)) w = snprintf(out, n, "%s", ip);
        else w = snprintf(out, n, "%s/%d", ip, r->prefix);
        break;
    }
    }
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* ---- restrictions ----------------------------------------------------------------------------- */

static const char *const FLAG_NAMES[] = {
    "no-internet", "no-lan", "no-network", "no-incoming", "no-camera", "no-mic", "no-screen",
    "no-usb", "no-bluetooth", "no-files", "no-notifications", "no-clipboard",
};
#define NFLAGS (int)(sizeof FLAG_NAMES / sizeof *FLAG_NAMES)

const char *fw_flag_name(uint32_t flag) {
    for (int i = 0; i < NFLAGS; i++) if (flag == 1u << i) return FLAG_NAMES[i];
    return NULL;
}

uint32_t fw_flag_from_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < NFLAGS; i++) if (!strcmp(name, FLAG_NAMES[i])) return 1u << i;
    return 0;
}

/* ---- store ------------------------------------------------------------------------------------- */

FwStore *fw_store_new(void) {
    FwStore *s = calloc(1, sizeof *s);
    if (s) s->mode = FW_MODE_SILENT;
    return s;
}

static void trie_free(TNode *t) {
    if (!t) return;
    trie_free(t->kid[0]);
    trie_free(t->kid[1]);
    free(t);
}

static void list_free(RNode *n) {
    while (n) { RNode *x = n->next; free(n); n = x; }
}

static void list_free_v(void *v) { list_free(v); }

static void trie_free_rules(TNode *t) {
    if (!t) return;
    list_free(t->head);
    trie_free_rules(t->kid[0]);
    trie_free_rules(t->kid[1]);
}

static void app_free(void *v) {
    App *a = v;
    if (!a) return;
    fwmap_free(&a->exact, list_free_v);
    fwmap_free(&a->wild, list_free_v);
    for (int f = 0; f < 2; f++) { trie_free_rules(a->trie[f]); trie_free(a->trie[f]); }
    list_free(a->any);
    list_free(a->lan);
    list_free(a->inet);
    free(a);
}

void fw_store_free(FwStore *s) {
    if (!s) return;
    fwmap_free(&s->ids, NULL);          /* the RNodes are freed by their App */
    fwmap_free(&s->apps, app_free);
    free(s);
}

static App *app_get(const FwStore *s, const char *canon) { return fwmap_get(&s->apps, canon, strlen(canon)); }

static App *app_make(FwStore *s, const char *canon) {
    App *a = app_get(s, canon);
    if (a) return a;
    if (s->apps.n >= FW_MAX_APPS) return NULL;
    void **slot = fwmap_slot(&s->apps, canon, strlen(canon), 1);
    if (!slot) return NULL;
    a = calloc(1, sizeof *a);
    if (!a) { fwmap_take(&s->apps, canon, strlen(canon)); return NULL; }
    snprintf(a->id, sizeof a->id, "%s", canon);
    *slot = a;
    return a;
}

/* Releases the App if it no longer holds anything. */
static void app_prune(FwStore *s, App *a) {
    if (a->nrules || a->trusted || a->restricted) return;
    fwmap_take(&s->apps, a->id, strlen(a->id));
    app_free(a);
}

static App *app_find(const FwStore *s, const char *app) {
    char c[FW_APP_MAX];
    if (!s || fw_app_canon(app, c, sizeof c)) return NULL;
    return app_get(s, c);
}

/* Slot of r's destination list. create: creates whatever is missing (trie nodes, keys). */
static RNode **loc(App *a, const FwRule *r, int create, TNode **tnode) {
    if (tnode) *tnode = NULL;
    switch (r->kind) {
    case FW_DST_ANY: return &a->any;
    case FW_DST_LAN: return &a->lan;
    case FW_DST_INTERNET: return &a->inet;
    case FW_DST_DOMAIN:
        return (RNode **)fwmap_slot(r->wildcard ? &a->wild : &a->exact, r->domain, strlen(r->domain), create);
    case FW_DST_CIDR: {
        int f = r->fam == 6;
        TNode **pp = &a->trie[f], *parent = NULL;
        for (int i = 0;; i++) {
            if (!*pp) {
                if (!create) return NULL;
                *pp = calloc(1, sizeof **pp);
                if (!*pp) return NULL;
                (*pp)->parent = parent;
            }
            if (i == r->prefix) break;
            parent = *pp;
            pp = &(*pp)->kid[bit_at(r->addr, i)];
        }
        if (tnode) *tnode = *pp;
        return &(*pp)->head;
    }
    }
    return NULL;
}

/* Deletes whatever was left empty after removing a rule. */
static void loc_prune(App *a, const FwRule *r, TNode *t) {
    if (r->kind == FW_DST_DOMAIN) {
        FwMap *m = r->wildcard ? &a->wild : &a->exact;
        RNode **h = (RNode **)fwmap_slot(m, r->domain, strlen(r->domain), 0);
        if (h && !*h) fwmap_take(m, r->domain, strlen(r->domain));
    } else if (r->kind == FW_DST_CIDR) {
        while (t && !t->head && !t->kid[0] && !t->kid[1]) {
            TNode *p = t->parent;
            if (p) p->kid[p->kid[1] == t] = NULL;
            else a->trie[r->fam == 6] = NULL;
            free(t);
            t = p;
        }
    }
}

static int same_match(const FwRule *x, const FwRule *y) {
    return x->dir == y->dir && x->port_lo == y->port_lo && x->port_hi == y->port_hi && x->proto == y->proto;
}

/* Validates and normalizes a rule into out. */
static int validate(const FwRule *in, FwRule *out) {
    memset(out, 0, sizeof *out);
    if (fw_app_canon(in->app, out->app, sizeof out->app)) return -1;
    if (in->dir != FW_OUT && in->dir != FW_IN) return -1;
    if (in->proto != FW_PROTO_ANY && in->proto != FW_PROTO_TCP && in->proto != FW_PROTO_UDP) return -1;
    if (in->action != FW_ALLOW && in->action != FW_DENY) return -1;
    if (in->life != FW_LIFE_FOREVER && in->life != FW_LIFE_UNTIL_QUIT && in->life != FW_LIFE_ONCE) return -1;
    if (in->port_lo > in->port_hi) return -1;
    out->dir = in->dir; out->proto = in->proto; out->action = in->action; out->life = in->life;
    out->port_lo = in->port_lo; out->port_hi = in->port_hi;
    if (!in->port_lo && !in->port_hi) out->port_hi = 65535;     /* no ports = any */
    out->when = in->when < 0 ? 0 : in->when;
    out->kind = in->kind;
    switch (in->kind) {
    case FW_DST_ANY: case FW_DST_LAN: case FW_DST_INTERNET: return 0;
    case FW_DST_DOMAIN: {
        if (strnlen(in->domain, FW_NAME_MAX) >= FW_NAME_MAX || norm_domain(in->domain, out->domain)) return -1;
        out->wildcard = in->wildcard ? 1 : 0;
        return 0;
    }
    case FW_DST_CIDR:
        if (in->fam != 4 && in->fam != 6) return -1;
        if (in->prefix < 0 || in->prefix > (in->fam == 4 ? 32 : 128)) return -1;
        if (!host_bits_zero(in->addr, in->fam == 4 ? 4 : 16, in->prefix)) return -1;
        if (in->fam == 4) for (int i = 4; i < 16; i++) if (in->addr[i]) return -1;
        out->fam = in->fam; out->prefix = in->prefix;
        memcpy(out->addr, in->addr, 16);
        return 0;
    }
    return -1;
}

int fw_rule_add(FwStore *s, const FwRule *in, uint32_t *id_out) {
    FwRule r;
    if (!s || !in || validate(in, &r)) return -1;
    App *a = app_make(s, r.app);
    if (!a) return -1;
    RNode **head = loc(a, &r, 1, NULL);
    if (!head) { app_prune(s, a); return -1; }
    for (RNode *n = *head; n; n = n->next) {
        if (!same_match(&n->r, &r)) continue;
        r.id = n->r.id;
        n->r = r;               /* the new decision replaces the previous one */
        if (id_out) *id_out = r.id;
        return 0;
    }
    if (s->nrules >= FW_MAX_RULES) goto fail;
    do { if (!++s->next_id) s->next_id = 1; } while (fwmap_get(&s->ids, &s->next_id, sizeof s->next_id));
    r.id = s->next_id;
    RNode *n = calloc(1, sizeof *n);
    if (!n) goto fail;
    void **slot = fwmap_slot(&s->ids, &r.id, sizeof r.id, 1);
    if (!slot) { free(n); goto fail; }
    n->r = r;
    n->app = a;
    n->next = *head;
    *head = n;
    *slot = n;
    a->nrules++;
    s->nrules++;
    if (id_out) *id_out = r.id;
    return 0;
fail: {
        TNode *t;
        RNode **h = loc(a, &r, 0, &t);
        if (h && !*h) loc_prune(a, &r, t);
        app_prune(s, a);
        return -1;
    }
}

int fw_rule_remove(FwStore *s, uint32_t id) {
    if (!s) return -1;
    RNode *n = fwmap_take(&s->ids, &id, sizeof id);
    if (!n) return -1;
    App *a = n->app;
    TNode *t;
    RNode **pp = loc(a, &n->r, 0, &t);
    for (; pp && *pp; pp = &(*pp)->next) if (*pp == n) { *pp = n->next; break; }
    FwRule r = n->r;
    free(n);
    loc_prune(a, &r, t);
    a->nrules--;
    s->nrules--;
    app_prune(s, a);
    return 0;
}

const FwRule *fw_rule_get(const FwStore *s, uint32_t id) {
    RNode *n = s ? fwmap_get(&s->ids, &id, sizeof id) : NULL;
    return n ? &n->r : NULL;
}

size_t fw_rule_count(const FwStore *s) { return s ? s->nrules : 0; }

static int cmp_rule_id(const void *x, const void *y) {
    uint32_t a = (*(RNode *const *)x)->r.id, b = (*(RNode *const *)y)->r.id;
    return a < b ? -1 : a > b;
}

/* The rules (of one app or all) sorted by id, in a malloc'd array. */
static RNode **sorted_rules(const FwStore *s, const App *only, size_t *count) {
    *count = 0;
    RNode **v = malloc((s->nrules ? s->nrules : 1) * sizeof *v);
    if (!v) return NULL;
    size_t k = 0;
    FWMAP_EACH(&s->ids, e) {
        RNode *n = e->val;
        if (!only || n->app == only) v[k++] = n;
    }
    qsort(v, k, sizeof *v, cmp_rule_id);
    *count = k;
    return v;
}

int fw_rule_list(const FwStore *s, const char *app, FwRule *out, int max) {
    if (!s) return 0;
    const App *a = NULL;
    if (app && !(a = app_find(s, app))) return 0;
    size_t k;
    RNode **v = sorted_rules(s, a, &k);
    if (!v) return -1;
    for (size_t i = 0; i < k && (int)i < max; i++) out[i] = v[i]->r;
    free(v);
    return (int)k;
}

static void remove_where(FwStore *s, App *a, int only_until_quit) {
    size_t k;
    RNode **v = sorted_rules(s, a, &k);
    if (!v) return;
    for (size_t i = 0; i < k; i++)
        if (!only_until_quit || v[i]->r.life == FW_LIFE_UNTIL_QUIT) {
            /* the last fw_rule_remove may free the App: don't touch it afterwards */
            fw_rule_remove(s, v[i]->r.id);
        }
    free(v);
}

void fw_app_quit(FwStore *s, const char *app) {
    App *a = app_find(s, app);
    if (a) remove_where(s, a, 1);
}

void fw_app_forget(FwStore *s, const char *app) {
    App *a = app_find(s, app);
    if (!a) return;
    a->trusted = 0;
    a->restricted = 0;
    if (a->nrules) remove_where(s, a, 0);   /* the last rule releases the App */
    else app_prune(s, a);
}

int fw_set_trusted(FwStore *s, const char *app, int on) {
    char c[FW_APP_MAX];
    if (!s || fw_app_canon(app, c, sizeof c)) return -1;
    App *a = on ? app_make(s, c) : app_get(s, c);
    if (!a) return on ? -1 : 0;
    a->trusted = on ? 1 : 0;
    if (on) a->restricted = 0;
    app_prune(s, a);
    return 0;
}

int fw_is_trusted(const FwStore *s, const char *app) {
    App *a = app_find(s, app);
    return a && a->trusted;
}

int fw_set_restricted(FwStore *s, const char *app, uint32_t flags) {
    char c[FW_APP_MAX];
    if (!s || fw_app_canon(app, c, sizeof c) || (flags & ~(uint32_t)FW_R_ALL)) return -1;
    App *a = flags ? app_make(s, c) : app_get(s, c);
    if (!a) return flags ? -1 : 0;
    a->restricted = flags;
    if (flags) a->trusted = 0;
    app_prune(s, a);
    return 0;
}

uint32_t fw_restricted(const FwStore *s, const char *app) {
    App *a = app_find(s, app);
    return a ? a->restricted : 0;
}

int fw_blocks(const FwStore *s, const char *app, uint32_t flag) { return (fw_restricted(s, app) & flag) != 0; }

void fw_app_each(const FwStore *s, int (*cb)(void *u, const char *app, int trusted, uint32_t flags), void *u) {
    if (!s || !cb) return;
    FWMAP_EACH(&s->apps, e) {
        App *a = e->val;
        if (cb(u, a->id, a->trusted, a->restricted)) return;
    }
}

void fw_set_mode(FwStore *s, FwMode m) {
    if (s && (m == FW_MODE_SILENT || m == FW_MODE_ASK || m == FW_MODE_OFF || m == FW_MODE_DENY)) s->mode = m;
}

FwMode fw_mode(const FwStore *s) { return s ? s->mode : FW_MODE_SILENT; }

/* ---- deciding ---------------------------------------------------------------------------------- */

/* Destination score: exact domain > wildcard (more labels, more specific) > network (longest
 * prefix) > LAN/internet > any. An IPv4 /32 and an IPv6 /128 never compete. */
enum { SC_ANY = 0, SC_NET = 500, SC_CIDR = 1000, SC_WILD = 2000, SC_EXACT = 3000 };

typedef struct {
    const FwQuery *q;
    RNode *best;
    int dst, port, proto;
} Pick;

/* Is b better than the current pick? Destination, then smaller port range, concrete protocol,
 * deny before allow and, last, the newest decision. */
static void consider(Pick *p, RNode *list, int dst) {
    for (RNode *n = list; n; n = n->next) {
        const FwRule *r = &n->r;
        if (r->dir != p->q->dir) continue;
        if (r->proto != FW_PROTO_ANY && r->proto != p->q->proto) continue;
        if (p->q->port < r->port_lo || p->q->port > r->port_hi) continue;
        int port = 65535 - (r->port_hi - r->port_lo), proto = r->proto != FW_PROTO_ANY;
        if (p->best) {
            const FwRule *b = &p->best->r;
            if (dst != p->dst) { if (dst < p->dst) continue; }
            else if (port != p->port) { if (port < p->port) continue; }
            else if (proto != p->proto) { if (proto < p->proto) continue; }
            else if (r->action != b->action) { if (r->action != FW_DENY) continue; }
            else if (r->id < b->id) continue;
        }
        p->best = n; p->dst = dst; p->port = port; p->proto = proto;
    }
}

FwDecision fw_decide(FwStore *s, const FwQuery *q) {
    FwDecision d = { FW_V_DENY, FW_BY_RESTRICTED, 0, FW_R_NO_NETWORK, 0 };
    char c[FW_APP_MAX];
    if (!s || !q || fw_app_canon(q->app, c, sizeof c)) return d;   /* no valid app: no */
    App *a = app_get(s, c);

    int fam = q->fam;
    uint8_t addr[16] = { 0 };
    if (fam == 4) memcpy(addr, q->addr, 4);
    else if (fam == 6) {
        memcpy(addr, q->addr, 16);
        if (is_mapped4(addr)) { memmove(addr, addr + 12, 4); memset(addr + 4, 0, 12); fam = 4; }
    } else fam = 0;
    int lan = fam ? fw_addr_is_lan(fam, addr) : -1;   /* -1: unknown address */

    /* 1. restrictions */
    uint32_t rf = a ? a->restricted : 0;
    uint32_t hit = 0;
    if (rf & FW_R_NO_NETWORK) hit = FW_R_NO_NETWORK;
    else if (q->dir == FW_IN && (rf & FW_R_NO_INCOMING)) hit = FW_R_NO_INCOMING;
    else if ((rf & FW_R_NO_INTERNET) && lan != 1) hit = FW_R_NO_INTERNET;   /* unknown: internet */
    else if ((rf & FW_R_NO_LAN) && lan == 1) hit = FW_R_NO_LAN;
    if (hit) { d.flag = hit; d.record = 1; return d; }
    d.flag = 0;

    /* 2. explicit rules */
    if (a && a->nrules) {
        Pick p = { q, NULL, 0, 0, 0 };
        for (int h = 0; h < q->nhosts && q->hosts; h++) {
            char name[FW_NAME_MAX];
            if (!q->hosts[h] || strnlen(q->hosts[h], FW_NAME_MAX + 1) > FW_NAME_MAX || norm_domain(q->hosts[h], name)) continue;
            int nl = labels(name);
            if (a->exact.n) consider(&p, fwmap_get(&a->exact, name, strlen(name)), SC_EXACT + nl);
            if (a->wild.n) {
                /* labels from right to left: each suffix of the name, from the longest to the TLD */
                for (const char *sfx = name; sfx; nl--) {
                    consider(&p, fwmap_get(&a->wild, sfx, strlen(sfx)), SC_WILD + nl);
                    sfx = strchr(sfx, '.');
                    if (sfx) sfx++;
                }
            }
        }
        if (fam) {
            int bits = fam == 4 ? 32 : 128;
            TNode *t = a->trie[fam == 6];
            for (int i = 0; t; i++) {
                consider(&p, t->head, SC_CIDR + i);
                if (i == bits) break;
                t = t->kid[bit_at(addr, i)];
            }
        }
        if (lan == 1) consider(&p, a->lan, SC_NET);
        else if (lan == 0) consider(&p, a->inet, SC_NET);
        consider(&p, a->any, SC_ANY);
        if (p.best && (p.dst >= SC_CIDR || !q->listed)) {
            d.verdict = p.best->r.action == FW_DENY ? FW_V_DENY : FW_V_ALLOW;
            d.reason = FW_BY_RULE;
            d.rule_id = p.best->r.id;
            if (p.best->r.life == FW_LIFE_ONCE) fw_rule_remove(s, d.rule_id);
            return d;
        }
    }

    /* 2b. a subscribed blocklist: only a rule that names the destination (above) beats it */
    if (q->listed) { d.verdict = FW_V_DENY; d.reason = FW_BY_LIST; d.record = 1; return d; }

    /* 3. trusted */
    if (a && a->trusted) { d.verdict = FW_V_ALLOW; d.reason = FW_BY_TRUSTED; return d; }

    /* 4. mode */
    d.reason = FW_BY_MODE;
    switch (s->mode) {
    case FW_MODE_SILENT: d.verdict = FW_V_ALLOW; d.record = 1; break;
    case FW_MODE_ASK: d.verdict = FW_V_ASK; break;
    case FW_MODE_OFF: d.verdict = FW_V_ALLOW; break;
    case FW_MODE_DENY:
        d.verdict = fam && fw_addr_is_loopback(fam, addr) ? FW_V_ALLOW : FW_V_DENY;
        d.record = 1;
        break;
    }
    return d;
}

/* ---- JSON -------------------------------------------------------------------------------------- */

typedef struct { char *p; size_t n, cap; int err; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->err) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->err = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

static void quote(Buf *b, const char *s) {
    char q[6 * FW_NAME_MAX + 8];
    size_t n = json_quote(q, sizeof q, s);
    put(b, q, n);
}

static void putf(Buf *b, const char *fmt, ...) {
    char t[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof t) { b->err = 1; return; }
    put(b, t, (size_t)n);
}

static const char *const MODE_NAMES[] = { "silent", "ask", "off", "deny" };

static int cmp_app(const void *x, const void *y) { return strcmp((*(App *const *)x)->id, (*(App *const *)y)->id); }

char *fw_to_json(const FwStore *s, size_t *len) {
    if (!s) return NULL;
    Buf b = { 0 };
    App **apps = malloc((s->apps.n ? s->apps.n : 1) * sizeof *apps);
    size_t na = 0, nr = 0;
    RNode **rules = sorted_rules(s, NULL, &nr);
    if (!apps || !rules) { free(apps); free(rules); return NULL; }
    FWMAP_EACH(&s->apps, e) apps[na++] = e->val;
    qsort(apps, na, sizeof *apps, cmp_app);

    putf(&b, "{\"v\":1,\"mode\":\"%s\",\"trusted\":[", MODE_NAMES[s->mode]);
    int first = 1;
    for (size_t i = 0; i < na; i++) {
        if (!apps[i]->trusted) continue;
        if (!first) puts_(&b, ",");
        first = 0;
        quote(&b, apps[i]->id);
    }
    puts_(&b, "],\"restricted\":{");
    first = 1;
    for (size_t i = 0; i < na; i++) {
        if (!apps[i]->restricted) continue;
        if (!first) puts_(&b, ",");
        first = 0;
        quote(&b, apps[i]->id);
        puts_(&b, ":[");
        int f1 = 1;
        for (int k = 0; k < NFLAGS; k++)
            if (apps[i]->restricted & (1u << k)) { putf(&b, "%s\"%s\"", f1 ? "" : ",", FLAG_NAMES[k]); f1 = 0; }
        puts_(&b, "]");
    }
    puts_(&b, "},\"rules\":[");
    first = 1;
    for (size_t i = 0; i < nr; i++) {
        const FwRule *r = &rules[i]->r;
        if (r->life != FW_LIFE_FOREVER) continue;     /* the others don't survive the session */
        char dst[FW_NAME_MAX + 8];
        if (fw_format_dest(r, dst, sizeof dst)) { b.err = 1; break; }
        puts_(&b, first ? "{\"app\":" : ",{\"app\":");
        first = 0;
        quote(&b, r->app);
        putf(&b, ",\"dir\":\"%s\",\"dst\":", r->dir == FW_IN ? "in" : "out");
        quote(&b, dst);
        if (r->port_lo != 0 || r->port_hi != 65535) {
            if (r->port_lo == r->port_hi) putf(&b, ",\"port\":\"%u\"", r->port_lo);
            else putf(&b, ",\"port\":\"%u-%u\"", r->port_lo, r->port_hi);
        }
        if (r->proto != FW_PROTO_ANY) putf(&b, ",\"proto\":\"%s\"", r->proto == FW_PROTO_TCP ? "tcp" : "udp");
        putf(&b, ",\"action\":\"%s\",\"when\":%lld}", r->action == FW_DENY ? "deny" : "allow", r->when);
    }
    puts_(&b, "]}\n");
    free(apps);
    free(rules);
    if (b.err) { free(b.p); return NULL; }
    if (len) *len = b.n;
    return b.p;
}

static int jerr(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int only_keys(const JVal *o, const char *const *keys, const char **bad) {
    for (const JVal *k = o->kid; k; k = k->next) {
        int ok = 0;
        for (int i = 0; keys[i]; i++) if (!strcmp(k->key, keys[i])) { ok = 1; break; }
        if (!ok) { *bad = k->key; return -1; }
    }
    return 0;
}

/* A JSON text with no 0 inside and of reasonable length. */
static const char *jstr(const JVal *v, size_t max) {
    if (!v || v->type != J_STR || v->len >= max || strlen(v->str) != v->len) return NULL;
    return v->str;
}

static int parse_ports(const JVal *v, FwRule *r) {
    unsigned lo, hi;
    if (v->type == J_NUM) {
        if (v->num < 1 || v->num > 65535 || v->num != (double)(unsigned)v->num) return -1;
        r->port_lo = r->port_hi = (uint16_t)v->num;
        return 0;
    }
    const char *s = jstr(v, 16);
    if (!s) return -1;
    const char *dash = strchr(s, '-');
    if (dash) {
        if (parse_uint(s, (size_t)(dash - s), 65535, &lo) || parse_uint(dash + 1, strlen(dash + 1), 65535, &hi) || lo > hi) return -1;
    } else {
        if (parse_uint(s, strlen(s), 65535, &lo)) return -1;
        hi = lo;
    }
    if (!lo) return -1;                     /* port 0 doesn't exist; "any" = no "port" */
    r->port_lo = (uint16_t)lo;
    r->port_hi = (uint16_t)hi;
    return 0;
}

static int load_rule(FwStore *t, const JVal *o, int idx, char *err, size_t errlen) {
    static const char *const keys[] = { "app", "dir", "dst", "port", "proto", "action", "when", NULL };
    const char *bad;
    if (o->type != J_OBJ) return jerr(err, errlen, "rule %d: not an object", idx);
    if (only_keys(o, keys, &bad)) return jerr(err, errlen, "rule %d: unknown key \"%.32s\"", idx, bad);
    FwRule r;
    memset(&r, 0, sizeof r);
    r.port_hi = 65535;
    const char *app = jstr(json_get(o, "app"), FW_APP_MAX);
    if (!app || fw_app_canon(app, r.app, sizeof r.app) || strcmp(app, r.app))
        return jerr(err, errlen, "rule %d: invalid app", idx);
    const JVal *v;
    if ((v = json_get(o, "dir"))) {
        const char *d = jstr(v, 8);
        if (d && !strcmp(d, "out")) r.dir = FW_OUT;
        else if (d && !strcmp(d, "in")) r.dir = FW_IN;
        else return jerr(err, errlen, "rule %d: invalid dir", idx);
    }
    const char *dst = jstr(json_get(o, "dst"), FW_NAME_MAX + 4);
    if (!dst || fw_parse_dest(dst, &r)) return jerr(err, errlen, "rule %d: invalid dst", idx);
    if ((v = json_get(o, "port")) && parse_ports(v, &r)) return jerr(err, errlen, "rule %d: invalid port", idx);
    if ((v = json_get(o, "proto"))) {
        const char *p = jstr(v, 8);
        if (p && !strcmp(p, "tcp")) r.proto = FW_PROTO_TCP;
        else if (p && !strcmp(p, "udp")) r.proto = FW_PROTO_UDP;
        else if (p && !strcmp(p, "any")) r.proto = FW_PROTO_ANY;
        else return jerr(err, errlen, "rule %d: invalid proto", idx);
    }
    const char *act = jstr(json_get(o, "action"), 8);
    if (act && !strcmp(act, "allow")) r.action = FW_ALLOW;
    else if (act && !strcmp(act, "deny")) r.action = FW_DENY;
    else return jerr(err, errlen, "rule %d: invalid action", idx);
    if ((v = json_get(o, "when"))) {
        if (v->type != J_NUM || v->num < 0 || v->num > 9007199254740992.0 || v->num != (double)(long long)v->num)
            return jerr(err, errlen, "rule %d: invalid when", idx);
        r.when = (long long)v->num;
    }
    r.life = FW_LIFE_FOREVER;
    if (fw_rule_add(t, &r, NULL)) return jerr(err, errlen, "rule %d: doesn't fit", idx);
    return 0;
}

static int load_doc(FwStore *t, const JVal *root, char *err, size_t errlen) {
    static const char *const keys[] = { "v", "mode", "trusted", "restricted", "rules", NULL };
    const char *bad;
    if (!root || root->type != J_OBJ) return jerr(err, errlen, "the root is not an object");
    if (only_keys(root, keys, &bad)) return jerr(err, errlen, "unknown key \"%.32s\"", bad);
    const JVal *v = json_get(root, "v");
    if (!v || v->type != J_NUM || v->num != 1) return jerr(err, errlen, "unsupported version");

    if ((v = json_get(root, "mode"))) {
        const char *m = jstr(v, 8);
        int k = -1;
        for (int i = 0; m && i < 4; i++) if (!strcmp(m, MODE_NAMES[i])) k = i;
        if (k < 0) return jerr(err, errlen, "invalid mode");
        t->mode = (FwMode)k;
    }
    if ((v = json_get(root, "trusted"))) {
        if (v->type != J_ARR) return jerr(err, errlen, "trusted is not a list");
        if (v->len > FW_MAX_APPS) return jerr(err, errlen, "trusted: too many apps");
        int i = 0;
        for (const JVal *k = v->kid; k; k = k->next, i++) {
            const char *app = jstr(k, FW_APP_MAX);
            char c[FW_APP_MAX];
            if (!app || fw_app_canon(app, c, sizeof c) || strcmp(app, c)) return jerr(err, errlen, "trusted %d: invalid app", i);
            if (fw_is_trusted(t, c)) return jerr(err, errlen, "trusted %d: repeated", i);
            if (fw_set_trusted(t, c, 1)) return jerr(err, errlen, "trusted %d: doesn't fit", i);
        }
    }
    if ((v = json_get(root, "restricted"))) {
        if (v->type != J_OBJ) return jerr(err, errlen, "restricted is not an object");
        if (v->len > FW_MAX_APPS) return jerr(err, errlen, "restricted: too many apps");
        for (const JVal *k = v->kid; k; k = k->next) {
            char c[FW_APP_MAX];
            if (strlen(k->key) >= FW_APP_MAX || fw_app_canon(k->key, c, sizeof c) || strcmp(k->key, c))
                return jerr(err, errlen, "restricted: invalid app");
            if (fw_is_trusted(t, c)) return jerr(err, errlen, "restricted: %.64s is also trusted", c);
            if (k->type != J_ARR || !k->len || k->len > NFLAGS) return jerr(err, errlen, "restricted %.64s: invalid list", c);
            uint32_t flags = 0;
            for (const JVal *f = k->kid; f; f = f->next) {
                uint32_t bit = fw_flag_from_name(jstr(f, 32));
                if (!bit || (flags & bit)) return jerr(err, errlen, "restricted %.64s: invalid or repeated restriction", c);
                flags |= bit;
            }
            if (fw_set_restricted(t, c, flags)) return jerr(err, errlen, "restricted %.64s: doesn't fit", c);
        }
    }
    if ((v = json_get(root, "rules"))) {
        if (v->type != J_ARR) return jerr(err, errlen, "rules is not a list");
        if (v->len > FW_MAX_RULES) return jerr(err, errlen, "too many rules");
        int i = 0;
        for (const JVal *k = v->kid; k; k = k->next, i++)
            if (load_rule(t, k, i, err, errlen)) return -1;
    }
    return 0;
}

int fw_from_json(FwStore *s, const char *text, size_t len, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!s || !text) return jerr(err, errlen, "no data");
    if (len > FW_FILE_MAX) return jerr(err, errlen, "file too large");
    if (memchr(text, 0, len)) return jerr(err, errlen, "byte 0 in the file");
    JDoc doc;
    char jmsg[128];
    if (json_parse(text, len, &doc, jmsg, sizeof jmsg)) return jerr(err, errlen, "invalid JSON: %s", jmsg);
    FwStore *t = fw_store_new();
    if (!t) { json_free(&doc); return jerr(err, errlen, "out of memory"); }
    int r = load_doc(t, doc.root, err, errlen);
    json_free(&doc);
    if (r) { fw_store_free(t); return -1; }
    /* everything validated: the contents are swapped in one go */
    FwStore tmp = *s;
    *s = *t;
    *t = tmp;
    fw_store_free(t);
    return 0;
}

int fw_save(const FwStore *s, const char *path) {
    if (!s || !path) return -1;
    size_t len;
    char *text = fw_to_json(s, &len);
    if (!text) return -1;
    char tmp[4096];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) { free(text); return -1; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { free(text); return -1; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
    free(text);
    int ok = off == len && !fsync(fd);
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int fw_load(FwStore *s, const char *path, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!s || !path) return jerr(err, errlen, "no path");
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 1;      /* there's no file yet: s stays as it is */
        return jerr(err, errlen, "could not open: %s", strerror(errno));
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (unsigned long long)st.st_size > FW_FILE_MAX) {
        close(fd);
        return jerr(err, errlen, "not a regular file or too large");
    }
    size_t want = (size_t)st.st_size, got = 0;
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return jerr(err, errlen, "out of memory"); }
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != want) { free(buf); return jerr(err, errlen, "incomplete read"); }
    int r = fw_from_json(s, buf, got, err, errlen);
    free(buf);
    return r;
}
