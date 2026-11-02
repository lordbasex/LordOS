/* fwlists.c — subscribed blocklists: raw parsers, the set and the compiled file (see fwlists.h). */
#include "fwlists.h"
#include "fwmap.h"
#include "rules.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The set keeps no names: a name is its 64-bit hash (fw_hash, seeded per process, so nobody can
 * craft a name that collides on purpose), sorted for binary search, with the tag of the list it
 * came from. A list of 380,000 names takes about 4 MB instead of 50. Networks are kept whole. */
typedef struct { uint64_t h; uint32_t seq; uint16_t tag; uint16_t pad; } NameEnt;
typedef struct { unsigned char k[18]; uint16_t tag; uint32_t seq; } NetEnt;

struct FwLists {
    NameEnt *ex, *sub;                  /* exact names; names with every subdomain */
    size_t nex, nsub, capex, capsub;
    NetEnt *net;
    size_t nnet, capnet;
    uint32_t seq;
    int sorted;                         /* 0: entries were added since the last sort */
    unsigned char has4[33], has6[129];  /* prefix lengths present */
};

FwLists *fwl_new(void) { return calloc(1, sizeof(FwLists)); }

void fwl_free(FwLists *l) {
    if (!l) return;
    free(l->ex);
    free(l->sub);
    free(l->net);
    free(l);
}

FwlFormat fwl_format_from_name(const char *n) {
    if (!n) return 0;
    if (!strcmp(n, "hosts")) return FWL_FMT_HOSTS;
    if (!strcmp(n, "domains")) return FWL_FMT_DOMAINS;
    if (!strcmp(n, "ips")) return FWL_FMT_IPS;
    return 0;
}

/* ---- the set ---- */

static size_t net_key(int fam, const uint8_t *addr, int prefix, unsigned char *k) {
    memset(k, 0, 18);
    k[0] = (unsigned char)fam;
    k[1] = (unsigned char)prefix;
    int bytes = fam == 4 ? 4 : 16;
    for (int i = 0; i < bytes; i++) {
        int bits = prefix - i * 8;
        unsigned char m = bits >= 8 ? 0xff : bits <= 0 ? 0 : (unsigned char)(0xff << (8 - bits));
        k[2 + i] = addr[i] & m;
    }
    return 18;
}

static int name_cmp(const void *a, const void *b) {
    const NameEnt *x = a, *y = b;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int net_cmp(const void *a, const void *b) {
    const NetEnt *x = a, *y = b;
    int c = memcmp(x->k, y->k, 18);
    if (c) return c;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* Sorts and drops repeated entries (the first one added keeps its tag). Returns how many went. */
static size_t uniq_names(NameEnt *v, size_t *n) {
    if (*n < 2) return 0;
    qsort(v, *n, sizeof *v, name_cmp);
    size_t w = 1;
    for (size_t i = 1; i < *n; i++) if (v[i].h != v[w - 1].h) v[w++] = v[i];
    size_t gone = *n - w;
    *n = w;
    return gone;
}

static size_t finish(FwLists *l) {
    if (!l || l->sorted) return 0;
    size_t gone = uniq_names(l->ex, &l->nex) + uniq_names(l->sub, &l->nsub);
    if (l->nnet > 1) {
        qsort(l->net, l->nnet, sizeof *l->net, net_cmp);
        size_t w = 1;
        for (size_t i = 1; i < l->nnet; i++) if (memcmp(l->net[i].k, l->net[w - 1].k, 18)) l->net[w++] = l->net[i];
        gone += l->nnet - w;
        l->nnet = w;
    }
    l->sorted = 1;
    return gone;
}

size_t fwl_finish(FwLists *l) { return finish(l); }
size_t fwl_domains(const FwLists *l) { if (!l) return 0; finish((FwLists *)l); return l->nex + l->nsub; }
size_t fwl_nets(const FwLists *l) { if (!l) return 0; finish((FwLists *)l); return l->nnet; }

static int grow(void **v, size_t *cap, size_t n, size_t sz) {
    if (n < *cap) return 0;
    size_t c = *cap ? *cap * 2 : 1024;
    void *p = realloc(*v, c * sz);
    if (!p) return -1;
    *v = p;
    *cap = c;
    return 0;
}

/* A name with at least one dot, normalized; 0 ok. */
static int good_name(const char *in, size_t n, char *out) {
    char tmp[FW_NAME_MAX + 2];
    if (!n || n > FW_NAME_MAX) return -1;
    memcpy(tmp, in, n);
    tmp[n] = 0;
    if (fw_norm_domain(tmp, out)) return -1;
    return strchr(out, '.') ? 0 : -1;
}

static int parse_net(const char *s, size_t n, FwRule *r) {
    char tmp[80];
    if (!n || n >= sizeof tmp) return -1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    if (!strchr(tmp, ':') && !strchr(tmp, '/') && !strchr(tmp, '.')) return -1;
    if (fw_parse_dest(tmp, r) || r->kind != FW_DST_CIDR) return -1;
    return 0;
}

/* Networks a blocklist may never carry: the LAN (it would cut this machine off its own network)
 * and anything broader than /8 (IPv4) or /16 (IPv6). */
static int net_usable(const FwRule *r) {
    if (r->prefix < (r->fam == 4 ? 8 : 16)) return 0;
    return !fw_addr_is_lan(r->fam, r->addr);
}

/* What a line is: 'd'/'s' with the name in name, 'n' with the network in r, or 0 if invalid. */
static int check_line(const char *line, size_t len, char *name, FwRule *r) {
    if (!line || len < 3 || line[1] != ' ') return 0;
    const char *v = line + 2;
    size_t vn = len - 2;
    if (line[0] == 'd' || line[0] == 's') {
        if (good_name(v, vn, name) || strlen(name) != vn || memcmp(name, v, vn)) return 0;   /* already normalized */
        return line[0];
    }
    if (line[0] == 'n') return !parse_net(v, vn, r) && net_usable(r) ? 'n' : 0;
    return 0;
}

int fwl_check_line(const char *line, size_t len) {
    char name[FW_NAME_MAX];
    FwRule r;
    return check_line(line, len, name, &r) ? 0 : -1;
}

int fwl_add_line(FwLists *l, const char *line, size_t len, uint16_t tag) {
    char name[FW_NAME_MAX];
    FwRule r;
    if (!l || !tag) return -1;
    int kind = check_line(line, len, name, &r);
    if (!kind) return -1;
    if (kind == 'n') {
        if (l->nnet >= FWL_MAX_NETS * 2 || grow((void **)&l->net, &l->capnet, l->nnet, sizeof *l->net)) return -2;
        NetEnt *e = &l->net[l->nnet++];
        net_key(r.fam, r.addr, r.prefix, e->k);
        e->tag = tag;
        e->seq = l->seq++;
        if (r.fam == 4) l->has4[r.prefix] = 1;
        else l->has6[r.prefix] = 1;
    } else {
        int sub = kind == 's';
        NameEnt **v = sub ? &l->sub : &l->ex;
        size_t *n = sub ? &l->nsub : &l->nex, *cap = sub ? &l->capsub : &l->capex;
        if (l->nex + l->nsub >= FWL_MAX_DOMAINS * 2 || grow((void **)v, cap, *n, sizeof **v)) return -2;
        NameEnt *e = &(*v)[(*n)++];
        e->h = fw_hash(name, strlen(name));
        e->tag = tag;
        e->seq = l->seq++;
        e->pad = 0;
    }
    l->sorted = 0;
    return 0;
}

long fwl_add_text(FwLists *l, const char *text, size_t len, uint16_t tag) {
    long added = 0;
    for (size_t i = 0; i < len;) {
        const char *nl = memchr(text + i, '\n', len - i);
        size_t e = nl ? (size_t)(nl - text) : len;
        if (e > i) {
            int r = fwl_add_line(l, text + i, e - i, tag);
            if (r == -1) return -1;
            if (r == -2) break;
            added++;
        }
        i = e + 1;
    }
    finish(l);
    /* the caps are on unique entries */
    if (l->nex + l->nsub > FWL_MAX_DOMAINS) l->nsub = l->nex >= FWL_MAX_DOMAINS ? 0 : FWL_MAX_DOMAINS - l->nex;
    if (l->nex > FWL_MAX_DOMAINS) l->nex = FWL_MAX_DOMAINS;
    if (l->nnet > FWL_MAX_NETS) l->nnet = FWL_MAX_NETS;
    return added;
}

FwLists *fwl_load_text(const char *text, size_t len, char *err, size_t errlen) {
    size_t hl = strlen(FWL_HEADER);
    if (!text || len > FWL_FILE_MAX || memchr(text, 0, len)) { snprintf(err, errlen, "not a list file"); return NULL; }
    if (len < hl || memcmp(text, FWL_HEADER, hl) || (len > hl && text[hl] != '\n')) { snprintf(err, errlen, "no header"); return NULL; }
    FwLists *l = fwl_new();
    if (!l) { snprintf(err, errlen, "out of memory"); return NULL; }
    size_t i = hl + 1, lineno = 1;
    while (i < len) {
        const char *nl = memchr(text + i, '\n', len - i);
        size_t e = nl ? (size_t)(nl - text) : len;
        lineno++;
        int r = e > i ? fwl_add_line(l, text + i, e - i, 1) : 0;
        if (r == -1 || r == -2) {
            snprintf(err, errlen, r == -2 ? "too many entries (line %zu)" : "invalid line %zu", lineno);
            fwl_free(l);
            return NULL;
        }
        i = e + 1;
    }
    finish(l);
    if (l->nex + l->nsub > FWL_MAX_DOMAINS || l->nnet > FWL_MAX_NETS) { snprintf(err, errlen, "too many entries"); fwl_free(l); return NULL; }
    return l;
}

static uint16_t find_name(const NameEnt *v, size_t n, uint64_t h) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (v[m].h < h) lo = m + 1;
        else hi = m;
    }
    return lo < n && v[lo].h == h ? v[lo].tag : 0;
}

uint16_t fwl_match_name(const FwLists *l, const char *in) {
    if (!l || !in) return 0;
    finish((FwLists *)l);
    if (!l->nex && !l->nsub) return 0;
    char name[FW_NAME_MAX];
    if (strnlen(in, FW_NAME_MAX + 1) > FW_NAME_MAX || fw_norm_domain(in, name)) return 0;
    uint16_t t = find_name(l->ex, l->nex, fw_hash(name, strlen(name)));
    if (t) return t;
    for (const char *sfx = name; sfx;) {
        t = find_name(l->sub, l->nsub, fw_hash(sfx, strlen(sfx)));
        if (t) return t;
        sfx = strchr(sfx, '.');
        if (sfx) sfx++;
    }
    return 0;
}

uint16_t fwl_match_addr(const FwLists *l, int fam, const uint8_t *addr) {
    if (!l || !addr || (fam != 4 && fam != 6)) return 0;
    finish((FwLists *)l);
    if (!l->nnet) return 0;
    uint8_t a[16] = { 0 };
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (fam == 6 && !memcmp(addr, mapped, 12)) { fam = 4; memcpy(a, addr + 12, 4); }
    else memcpy(a, addr, fam == 4 ? 4 : 16);
    const unsigned char *has = fam == 4 ? l->has4 : l->has6;
    for (int p = fam == 4 ? 32 : 128; p >= 0; p--) {
        if (!has[p]) continue;
        unsigned char k[18];
        net_key(fam, a, p, k);
        size_t lo = 0, hi = l->nnet;
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (memcmp(l->net[m].k, k, 18) < 0) lo = m + 1;
            else hi = m;
        }
        if (lo < l->nnet && !memcmp(l->net[lo].k, k, 18)) return l->net[lo].tag;
    }
    return 0;
}

void fwl_net_each(const FwLists *l, int (*cb)(void *u, int fam, const uint8_t *addr, int prefix), void *u) {
    if (!l) return;
    finish((FwLists *)l);
    for (size_t i = 0; i < l->nnet; i++) {
        uint8_t a[16] = { 0 };
        memcpy(a, l->net[i].k + 2, l->net[i].k[0] == 4 ? 4 : 16);
        if (cb(u, l->net[i].k[0], a, l->net[i].k[1])) return;
    }
}

uint16_t fwl_block_names(const FwLists *l, const char *const *names, int n) {
    uint16_t first = 0;
    for (int i = 0; i < n; i++) {
        uint16_t t = fwl_match_name(l, names[i]);
        if (!t) return 0;               /* an unlisted name shares the address */
        if (!first) first = t;
    }
    return first;
}

/* ---- raw lists ------------------------------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

/* Names a hosts file maps to itself, not blocklist entries. */
static int hosts_noise(const char *name) {
    static const char *const v[] = { "localhost", "localhost.localdomain", "local", "broadcasthost", "ip6-localhost",
                                     "ip6-loopback", "ip6-localnet", "ip6-mcastprefix", "ip6-allnodes", "ip6-allrouters",
                                     "ip6-allhosts", "0.0.0.0", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(name, v[i])) return 1;
    return 0;
}

/* The sink-hole addresses a hosts-format blocklist points names to. */
static int hosts_target(const char *s, size_t n) {
    static const char *const v[] = { "0.0.0.0", "127.0.0.1", "::", "::1", NULL };
    for (int i = 0; v[i]; i++) if (strlen(v[i]) == n && !memcmp(s, v[i], n)) return 1;
    return 0;
}

static int set_err(char *err, size_t errlen, const char *code) {
    if (err && errlen) snprintf(err, errlen, "%s", code);
    return -1;
}

/* What the parser writes: the normalized lines, each once (a set of their hashes). */
typedef struct {
    char *p;
    size_t n, cap;
    uint64_t *set;
    size_t nset, capset;                /* capset: a power of 2 */
    int err;
} Raw;

static void raw_put(Raw *o, const char *s, size_t n) {
    if (o->err) return;
    if (o->n + n + 2 > o->cap) {
        size_t c = o->cap ? o->cap : 65536;
        while (c < o->n + n + 2) c *= 2;
        char *p = realloc(o->p, c);
        if (!p) { o->err = 1; return; }
        o->p = p;
        o->cap = c;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

/* 1 if new (and now remembered), 0 if already there, -1 out of memory. */
static int raw_seen(Raw *o, uint64_t h) {
    if (!h) h = 1;
    if ((o->nset + 1) * 2 > o->capset) {
        size_t c = o->capset ? o->capset * 2 : 4096;
        uint64_t *v = calloc(c, sizeof *v);
        if (!v) return -1;
        for (size_t i = 0; i < o->capset; i++) {
            if (!o->set[i]) continue;
            size_t k = (size_t)o->set[i] & (c - 1);
            while (v[k]) k = (k + 1) & (c - 1);
            v[k] = o->set[i];
        }
        free(o->set);
        o->set = v;
        o->capset = c;
    }
    size_t k = (size_t)h & (o->capset - 1);
    while (o->set[k]) {
        if (o->set[k] == h) return 0;
        k = (k + 1) & (o->capset - 1);
    }
    o->set[k] = h;
    o->nset++;
    return 1;
}

static int raw_line(Raw *o, const char *line, size_t n, size_t *count, FwlStats *st) {
    int r = raw_seen(o, fw_hash(line, n));
    if (r < 0) { o->err = 1; return -1; }
    if (!r) { st->dups++; return 0; }
    raw_put(o, line, n);
    raw_put(o, "\n", 1);
    (*count)++;
    return 1;
}

/* One name from a list: written (d/s) or counted as why not. 1 written, 0 not, -1 stop. */
static int raw_name(Raw *o, const char *s, size_t n, int sub, FwlStats *st) {
    char name[FW_NAME_MAX];
    if (n > 2 && s[0] == '*' && s[1] == '.') { s += 2; n -= 2; sub = 1; }
    if (n > 3 && s[0] == '|' && s[1] == '|' && s[n - 1] == '^') { s += 2; n -= 3; sub = 1; }
    char tmp[FW_NAME_MAX + 2];
    if (!n || n > FW_NAME_MAX) { st->bad++; return 0; }
    memcpy(tmp, s, n);
    tmp[n] = 0;
    for (char *c = tmp; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    if (hosts_noise(tmp)) { st->skipped++; return 0; }
    if (fw_norm_domain(tmp, name)) { st->bad++; return 0; }
    if (!strchr(name, '.') || hosts_noise(name)) { st->skipped++; return 0; }
    if (st->domains >= FWL_LIST_DOMAINS) return -1;
    char line[FW_NAME_MAX + 4];
    int ln = snprintf(line, sizeof line, "%c %s", sub ? 's' : 'd', name);
    return raw_line(o, line, (size_t)ln, &st->domains, st);
}

static int raw_net(Raw *o, const char *s, size_t n, FwlStats *st) {
    FwRule r;
    if (parse_net(s, n, &r)) { st->bad++; return 0; }
    if (!net_usable(&r)) { st->skipped++; return 0; }
    if (st->nets >= FWL_LIST_NETS) return -1;
    char ip[INET6_ADDRSTRLEN], line[80];
    inet_ntop(r.fam == 4 ? AF_INET : AF_INET6, r.addr, ip, sizeof ip);
    int ln = r.prefix == (r.fam == 4 ? 32 : 128) ? snprintf(line, sizeof line, "n %s", ip) : snprintf(line, sizeof line, "n %s/%d", ip, r.prefix);
    return raw_line(o, line, (size_t)ln, &st->nets, st);
}

int fwl_parse_raw(const char *raw, size_t len, FwlFormat fmt, int sub, char **out, size_t *outlen,
                  FwlStats *st, char *err, size_t errlen) {
    FwlStats z = { 0 };
    if (!st) st = &z;
    memset(st, 0, sizeof *st);
    *out = NULL;
    *outlen = 0;
    if (fmt != FWL_FMT_HOSTS && fmt != FWL_FMT_DOMAINS && fmt != FWL_FMT_IPS) return set_err(err, errlen, "format");
    if (len > FWL_RAW_MAX) return set_err(err, errlen, "big");
    if (memchr(raw, 0, len)) return set_err(err, errlen, "nul");
    Raw o = { 0 };
    Raw *l = &o;
    int full = 0;
    size_t i = 0;
    /* a UTF-8 byte order mark at the start */
    if (len >= 3 && (unsigned char)raw[0] == 0xef && (unsigned char)raw[1] == 0xbb && (unsigned char)raw[2] == 0xbf) i = 3;
    while (i < len && !full) {
        const char *nl = memchr(raw + i, '\n', len - i);
        size_t e = nl ? (size_t)(nl - raw) : len;
        if (e - i > FWL_LINE_MAX) { free(o.p); free(o.set); return set_err(err, errlen, "line"); }
        const char *s = raw + i;
        size_t n = e - i;
        i = e + 1;
        /* comments: whole-line (#, !, ;) and, after a space, inline (#) */
        while (n && is_space(*s)) { s++; n--; }
        if (!n || *s == '#' || *s == '!' || *s == ';' || (n >= 2 && s[0] == '/' && s[1] == '/')) continue;
        for (size_t k = 1; k < n; k++) if (s[k] == '#' && is_space(s[k - 1])) { n = k; break; }
        if (fmt == FWL_FMT_IPS) for (size_t k = 0; k < n; k++) if (s[k] == ';') { n = k; break; }
        while (n && is_space(s[n - 1])) n--;
        if (!n) continue;
        st->lines++;
        /* tokens */
        const char *tok[16];
        size_t tl[16];
        int nt = 0, over = 0;
        for (size_t k = 0; k < n;) {
            while (k < n && is_space(s[k])) k++;
            if (k >= n) break;
            size_t b = k;
            while (k < n && !is_space(s[k])) k++;
            if (nt == 16) { over = 1; break; }
            tok[nt] = s + b;
            tl[nt++] = k - b;
        }
        for (size_t k = 0; k < n; k++) if ((unsigned char)s[k] < 0x20 && !is_space(s[k])) { over = 1; break; }
        if (over || !nt) { st->bad++; continue; }
        int r = 0;
        if (fmt == FWL_FMT_HOSTS) {
            if (nt < 2 || !hosts_target(tok[0], tl[0])) { st->bad++; continue; }
            for (int t = 1; t < nt && r >= 0; t++) r = raw_name(l, tok[t], tl[t], sub, st);
        } else if (fmt == FWL_FMT_DOMAINS) {
            if (nt != 1) { st->bad++; continue; }
            r = raw_name(l, tok[0], tl[0], sub, st);
        } else {
            r = raw_net(l, tok[0], tl[0], st);     /* other columns (counts, dates) are ignored */
        }
        if (r < 0) { full = 1; st->truncated = !o.err; }
    }
    free(o.set);
    if (o.err) { free(o.p); return set_err(err, errlen, "oom"); }
    if (st->bad > 20 && st->bad * 10 > st->lines) { free(o.p); return set_err(err, errlen, "format"); }
    if (!st->domains && !st->nets) { free(o.p); return set_err(err, errlen, "empty"); }
    *out = o.p;
    *outlen = o.n;
    return 0;
}
