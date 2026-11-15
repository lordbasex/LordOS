/* resolve.c — dependency resolver (see resolve.h).
 *
 * Each package that shows up is a node with its installed version (from dpkg), its candidate (from
 * the index) and its target: stay as is, install/upgrade to an index record, or not
 * be there. Requests change targets and queue the dependency groups of whatever changes; the
 * queue is processed until every group is satisfied. When a group can be satisfied in several
 * ways (alternatives "a | b", virtual packages) they're tried in order with backtracking, within
 * a fixed budget. Conflicts (Conflicts/Breaks, in both directions) are resolved by
 * upgrading the other package if its new version no longer clashes, and otherwise removing it;
 * removals that weren't requested need confirmation (allow_remove/cascade). At the end everything
 * that changed and whatever depends on it is reviewed until nothing else changes.
 *
 * (0.8.8.9) Multiarch: a foreign architecture's package is its own node, keyed "name:i386" in the index
 * and in dpkg's status. A relation is read in its owner's architecture, as dpkg does: an unqualified
 * Depends is satisfied by the same architecture, or by any architecture's package that is
 * "Multi-Arch: foreign"; "name:any" also by a "Multi-Arch: allowed" one; "name:native" and "name:i386"
 * name one architecture. Conflicts and Breaks without a qualifier apply to every architecture, except
 * the owner's own package in the other one. The two architectures of one package can only be present
 * together when both are "Multi-Arch: same", and then always at the same version: changing one moves
 * the other with it, and a plan that would split them is refused (pkg.multiarchSplit). */
#include "resolve.h"
#include "deb.h"
#include "../errcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_KEEP (-2)
#define T_NONE (-1)
#define MAX_ALTS 32
#define BUDGET 4000

typedef struct {
    const char *name;                   /* the key: "libc6", "libc6:i386" */
    size_t nlen, blen;                  /* blen: without ":arch" */
    const StPkg *inst;
    long cand, target;
    uint8_t requested, remove_req, touched, orphan, mark;
    char reason[200];
} Node;

typedef struct { int node; long old; uint8_t old_touched; char reason[200]; } Undo;

typedef struct { int node; long expect; const char *g, *ge; uint8_t optional; } Item;

typedef struct { const char *name; size_t nlen; const StPkg *p; const char *ver; size_t vlen; int op; } Ref;

typedef struct {
    const Idx *x;
    const Status *st;
    const PlanOpts *o;
    Node **blocks;                      /* blocks of 1024: pointers to nodes don't move */
    int nn, nblocks;
    int *ht;
    int htcap;
    Undo *log;
    int nlog, caplog;
    Item *q;
    int nq, capq;
    Ref *prov, *rdep, *conf;            /* from what's installed: provides, depends on, conflicts with */
    int nprov, nrdep, nconf;
    int budget, may_remove;
    int ma;                             /* multiarch in play: a foreign package is installed or indexed */
    int pair_block;                     /* the last option gather() left out only for its other architecture, or -1 */
    const char *native;                 /* the native architecture's name */
    char err[512];
} S;

#define FSUF ":" DEB_FOREIGN_ARCH       /* the foreign architecture's key suffix */

/* ---- nodes ---- */

static inline Node *nd(const S *s, int i) { return &s->blocks[i >> 10][i & 1023]; }

static uint32_t hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

/* apt's choice (pkgPolicy::GetCandidateVer): versions from the newest down, and only a strictly
 * higher priority replaces the one chosen, so among equals the newest wins; the installed version
 * counts at least 100 (dpkg's status, as in apt), and versions older than it only with 1000 or
 * more; a negative priority never. -1 when nothing is available or the installed version is the
 * best (it may not be in the index). */
long pkg_candidate(const Idx *x, const char *name, size_t n, const StPkg *inst) {
    long first = idx_find(x, name, n);
    if (first < 0) return -1;
    long best = -1;
    int max = 0, inst_seen = !inst;
    for (long i = first; i < (long)x->h->npkg && !strcmp(idx_s(x, x->pkg[i].name), idx_s(x, x->pkg[first].name)); i++) {
        int c = inst ? ver_cmp(idx_s(x, x->pkg[i].version), inst->version) : 1;
        if (!inst_seen && c < 0) {      /* past the installed version, which no source carries */
            inst_seen = 1;
            if (100 > max) { max = 100; best = -1; }
        }
        int p = idx_prio(x, i);
        if (p < 0) continue;
        if (c == 0) { inst_seen = 1; if (p < 100) p = 100; }
        if (c < 0 && p < 1000) continue;
        if (p > max) { max = p; best = i; }
    }
    if (!inst_seen && 100 > max) best = -1;
    return best;
}

static int node_get(S *s, const char *name, size_t n) {
    if ((s->nn + 1) * 2 > s->htcap) {
        int cap = s->htcap ? s->htcap * 2 : 4096;
        int *h = malloc(sizeof *h * (size_t)cap);
        if (!h) return -1;
        memset(h, 0xff, sizeof *h * (size_t)cap);
        for (int i = 0; i < s->nn; i++) {
            uint32_t j = hash(nd(s, i)->name, nd(s, i)->nlen) & (uint32_t)(cap - 1);
            while (h[j] >= 0) j = (j + 1) & (uint32_t)(cap - 1);
            h[j] = i;
        }
        free(s->ht);
        s->ht = h;
        s->htcap = cap;
    }
    uint32_t j = hash(name, n) & (uint32_t)(s->htcap - 1);
    while (s->ht[j] >= 0) {
        Node *e = nd(s, s->ht[j]);
        if (e->nlen == n && !memcmp(e->name, name, n)) return s->ht[j];
        j = (j + 1) & (uint32_t)(s->htcap - 1);
    }
    if ((s->nn >> 10) >= s->nblocks) {
        Node **nb = realloc(s->blocks, sizeof *nb * (size_t)(s->nblocks + 1));
        if (!nb) return -1;
        s->blocks = nb;
        if (!(s->blocks[s->nblocks] = malloc(sizeof(Node) * 1024))) return -1;
        s->nblocks++;
    }
    Node *e = nd(s, s->nn);
    memset(e, 0, sizeof *e);
    e->inst = status_find(s->st, name, n);
    e->name = e->inst ? e->inst->key : name;           /* a stable pointer */
    e->nlen = n;
    e->blen = deb_base_len(name, n);
    if (!e->inst) {
        long f = idx_find(s->x, name, n);
        if (f >= 0) e->name = idx_s(s->x, s->x->pkg[f].name);
    }
    e->cand = pkg_candidate(s->x, name, n, e->inst);
    e->target = e->inst ? T_KEEP : T_NONE;
    s->ht[j] = s->nn;
    return s->nn++;
}

static int present(const Node *n) { return n->target != T_NONE; }

static const char *node_ver(const S *s, const Node *n) {
    if (n->target == T_KEEP) return n->inst->version;
    if (n->target >= 0) return idx_s(s->x, s->x->pkg[n->target].version);
    return NULL;
}

/* ---- multiarch ---- */

/* The node's architecture suffix: "" for the native one (and "all"), ":i386" for a foreign one. */
static const char *suffix(const S *s, int i, size_t *len) {
    if (i < 0) { *len = 0; return ""; }
    const Node *n = nd(s, i);
    *len = n->nlen - n->blen;
    return n->name + n->blen;
}

/* The node of package base in the architecture of suffix suf ("" native, ":i386"); for a foreign one,
 * -1 when nothing installed or indexed has that name (no node is made for it). */
static int node_in(S *s, const char *base, size_t blen, const char *suf, size_t slen) {
    if (!slen) return node_get(s, base, blen);
    char k[256];
    if (blen + slen >= sizeof k) return -1;
    memcpy(k, base, blen);
    memcpy(k + blen, suf, slen);
    if (!status_find(s->st, k, blen + slen) && idx_find(s->x, k, blen + slen) < 0) return -1;
    return node_get(s, k, blen + slen);
}

/* The same package in the other architecture (native <-> i386), or -1. */
static int sibling(S *s, int i) {
    if (!s->ma) return -1;
    Node *n = nd(s, i);
    if (n->nlen > n->blen) return node_in(s, n->name, n->blen, "", 0);
    return node_in(s, n->name, n->blen, FSUF, strlen(FSUF));
}

/* Multi-Arch flags of the version a node has in the plan (installed, or the record it goes to). */
static uint8_t pflags(const S *s, const Node *n) {
    if (n->target >= 0) return s->x->pkg[n->target].flags;
    if (n->target == T_KEEP && n->inst) return n->inst->flags;
    return 0;
}

/* Does the qualifier of r name the architecture of suffix cs? */
static int qual_is(const S *s, const DebRel *r, const char *cs, size_t csl) {
    if (!csl) return r->alen == strlen(s->native) && !memcmp(r->arch, s->native, r->alen);
    return r->alen == csl - 1 && !memcmp(r->arch, cs + 1, r->alen);
}

/* May package node `cand` (whose version in question has Multi-Arch `flags`) satisfy relation r of
 * `owner` (-1: a request, read as native)? conflicts: r is a Conflicts or Breaks (dpkg: unqualified, every
 * architecture; never the owner's own package in another architecture). */
static int arch_ok(const S *s, int owner, const DebRel *r, int cand, uint8_t flags, int conflicts) {
    size_t osl, csl;
    const char *os = suffix(s, owner, &osl), *cs = suffix(s, cand, &csl);
    int same = osl == csl && !memcmp(os, cs, osl);
    int any = r->alen == 3 && !memcmp(r->arch, "any", 3), nat = r->alen == 6 && !memcmp(r->arch, "native", 6);
    if (conflicts) {
        if (owner >= 0 && cand != owner && nd(s, owner)->blen == nd(s, cand)->blen &&
            !memcmp(nd(s, owner)->name, nd(s, cand)->name, nd(s, cand)->blen)) return 0;
        if (!r->alen || any) return 1;
        return nat ? !csl : qual_is(s, r, cs, csl);
    }
    if (!r->alen) return same || (flags & IF_MA_FOREIGN);
    if (any) return same || (flags & (IF_MA_ALLOWED | IF_MA_FOREIGN));
    if (nat) return !csl;
    return qual_is(s, r, cs, csl);
}

/* The nodes named by r's package (not its providers) for owner: its architecture's first, then the
 * other one's (a foreign owner first takes a native "Multi-Arch: foreign" package, as apt does). */
static int rel_nodes(S *s, int owner, const DebRel *r, int out[2]) {
    size_t osl;
    const char *os = suffix(s, owner, &osl);
    int n = 0;
    if (r->alen && !(r->alen == 3 && !memcmp(r->arch, "any", 3))) {
        if ((r->alen == 6 && !memcmp(r->arch, "native", 6)) || (r->alen == strlen(s->native) && !memcmp(r->arch, s->native, r->alen)))
            out[n++] = node_get(s, r->name, r->nlen);
        else if (s->ma && r->alen == strlen(DEB_FOREIGN_ARCH) && !memcmp(r->arch, DEB_FOREIGN_ARCH, r->alen))
            out[n++] = node_in(s, r->name, r->nlen, FSUF, strlen(FSUF));
        return n;
    }
    out[n++] = node_in(s, r->name, r->nlen, os, osl);
    if (s->ma) out[n++] = osl ? node_get(s, r->name, r->nlen) : node_in(s, r->name, r->nlen, FSUF, strlen(FSUF));
    if (n == 2 && osl && out[1] >= 0 && nd(s, out[1])->cand >= 0 && (s->x->pkg[nd(s, out[1])->cand].flags & IF_MA_FOREIGN)) {
        int t = out[0]; out[0] = out[1]; out[1] = t;
    }
    return n;
}

/* The record of node n's package at exactly version v (any source not pinned below 0), or -1. */
static long rec_at(const S *s, const Node *n, const char *v) {
    long first = idx_find(s->x, n->name, n->nlen);
    for (long i = first; i >= 0 && i < (long)s->x->h->npkg && s->x->pkg[i].name == s->x->pkg[first].name; i++)
        if (!strcmp(idx_s(s->x, s->x->pkg[i].version), v) && idx_prio(s->x, i) >= 0) return i;
    return -1;
}

/* Could record rec of node ni sit next to its other architecture as the plan has it? Only when both are
 * "Multi-Arch: same" (dpkg refuses any other pair). */
static int pair_ok(S *s, int ni, long rec) {
    int sb = sibling(s, ni);
    if (sb < 0 || !present(nd(s, sb))) return 1;
    return (s->x->pkg[rec].flags & IF_MA_SAME) && (pflags(s, nd(s, sb)) & IF_MA_SAME);
}

enum { F_DEPENDS, F_PREDEPENDS, F_RECOMMENDS, F_PROVIDES, F_CONFLICTS, F_BREAKS };

static const char *field_of(const S *s, const Node *n, long target, int f) {
    if (target == T_KEEP) {
        const StPkg *p = n->inst;
        const char *v = f == F_DEPENDS ? p->depends : f == F_PREDEPENDS ? p->predepends : f == F_RECOMMENDS ? p->recommends :
                        f == F_PROVIDES ? p->provides : f == F_CONFLICTS ? p->conflicts : p->breaks;
        return v ? v : "";
    }
    if (target < 0) return "";
    const IdxPkg *p = &s->x->pkg[target];
    uint32_t o = f == F_DEPENDS ? p->depends : f == F_PREDEPENDS ? p->predepends : f == F_RECOMMENDS ? p->recommends :
                 f == F_PROVIDES ? p->provides : f == F_CONFLICTS ? p->conflicts : p->breaks;
    return idx_s(s->x, o);
}

/* ---- tables of what's installed ---- */

static int ref_cmp(const void *a, const void *b) {
    const Ref *x = a, *y = b;
    size_t n = x->nlen < y->nlen ? x->nlen : y->nlen;
    int c = memcmp(x->name, y->name, n);
    return c ? c : (x->nlen < y->nlen ? -1 : x->nlen > y->nlen);
}

static int refs_add(Ref **v, int *n, int *cap, const char *field, const StPkg *p) {
    if (!field) return 0;
    const char *e = field + strlen(field);
    DebRel r[MAX_ALTS];
    int k;
    while ((k = deb_next_group(&field, e, r, MAX_ALTS)) > 0)
        for (int i = 0; i < k; i++) {
            if (*n == *cap) {
                *cap = *cap ? *cap * 2 : 1024;
                Ref *nv = realloc(*v, sizeof *nv * (size_t)*cap);
                if (!nv) return -1;
                *v = nv;
            }
            (*v)[(*n)++] = (Ref){ r[i].name, r[i].nlen, p, r[i].ver, r[i].vlen, r[i].op };
        }
    return 0;
}

static int build_tables(S *s) {
    int cp = 0, cr = 0, cc = 0;
    for (int i = 0; i < s->st->n; i++) {
        const StPkg *p = &s->st->p[i];
        if (refs_add(&s->prov, &s->nprov, &cp, p->provides, p) || refs_add(&s->rdep, &s->nrdep, &cr, p->depends, p) ||
            refs_add(&s->rdep, &s->nrdep, &cr, p->predepends, p) || refs_add(&s->conf, &s->nconf, &cc, p->conflicts, p) ||
            refs_add(&s->conf, &s->nconf, &cc, p->breaks, p)) return -1;
    }
    qsort(s->prov, (size_t)s->nprov, sizeof *s->prov, ref_cmp);
    qsort(s->rdep, (size_t)s->nrdep, sizeof *s->rdep, ref_cmp);
    qsort(s->conf, (size_t)s->nconf, sizeof *s->conf, ref_cmp);
    return 0;
}

static void refs_range(const Ref *v, int n, const char *name, size_t len, int *lo, int *hi) {
    Ref key = { name, len, NULL, NULL, 0, 0 };
    int a = 0, b = n;
    while (a < b) { int m = (a + b) / 2; if (ref_cmp(&v[m], &key) < 0) a = m + 1; else b = m; }
    int e = a;
    while (e < n && !ref_cmp(&v[e], &key)) e++;
    *lo = a;
    *hi = e;
}

/* ---- undo log ---- */

static int mark(S *s, int node) {
    if (s->nlog == s->caplog) {
        int cap = s->caplog ? s->caplog * 2 : 256;
        Undo *n = realloc(s->log, sizeof *n * (size_t)cap);
        if (!n) return -1;
        s->log = n;
        s->caplog = cap;
    }
    Undo *u = &s->log[s->nlog++];
    u->node = node;
    u->old = nd(s, node)->target;
    u->old_touched = nd(s, node)->touched;
    memcpy(u->reason, nd(s, node)->reason, sizeof u->reason);
    return 0;
}

static void undo_to(S *s, int pos) {
    while (s->nlog > pos) {
        Undo *u = &s->log[--s->nlog];
        Node *n = nd(s, u->node);
        n->target = u->old;
        n->touched = u->old_touched;
        memcpy(n->reason, u->reason, sizeof n->reason);
    }
}

static int enqueue(S *s, int node, long expect, const char *field, int optional) {
    const char *e = field + strlen(field), *p = field;
    DebRel r[MAX_ALTS];
    for (;;) {
        const char *g = p;
        while (g < e && (*g == ' ' || *g == ',' || *g == '\n')) g++;
        if (deb_next_group(&p, e, r, MAX_ALTS) <= 0) break;
        if (s->nq == s->capq) {
            int cap = s->capq ? s->capq * 2 : 1024;
            Item *n = realloc(s->q, sizeof *n * (size_t)cap);
            if (!n) return -1;
            s->q = n;
            s->capq = cap;
        }
        s->q[s->nq++] = (Item){ node, expect, g, p, (uint8_t)optional };
    }
    return 0;
}

/* Does field (a relation list) name any of the k alternatives in r? */
static int names_any(const char *field, const DebRel *r, int k) {
    if (!field) return 0;
    const char *p = field, *e = field + strlen(field);
    DebRel o[MAX_ALTS];
    int m;
    while ((m = deb_next_group(&p, e, o, MAX_ALTS)) > 0)
        for (int i = 0; i < m; i++)
            for (int j = 0; j < k; j++)
                if (o[i].nlen == r[j].nlen && !memcmp(o[i].name, r[j].name, r[j].nlen)) return 1;
    return 0;
}

/* An upgrade brings the Recommends the new version adds, as apt's full upgrade does: only a group none of whose
 * names the installed version already recommended, so a recommended package the user removed stays removed.
 * Optional: one that can't be installed doesn't stop the upgrade. */
static int enqueue_new_recommends(S *s, int node, long target, const char *field, const char *had) {
    const char *e = field + strlen(field), *p = field;
    DebRel r[MAX_ALTS];
    int k;
    for (;;) {
        const char *g = p;
        while (g < e && (*g == ' ' || *g == ',' || *g == '\n')) g++;
        if ((k = deb_next_group(&p, e, r, MAX_ALTS)) <= 0) break;
        if (names_any(had, r, k)) continue;
        if (s->nq == s->capq) {
            int cap = s->capq ? s->capq * 2 : 1024;
            Item *n = realloc(s->q, sizeof *n * (size_t)cap);
            if (!n) return -1;
            s->q = n;
            s->capq = cap;
        }
        s->q[s->nq++] = (Item){ node, target, g, p, 1 };
    }
    return 0;
}

static int enqueue_deps(S *s, int node) {
    Node *n = nd(s, node);
    if (!present(n)) return 0;
    if (enqueue(s, node, n->target, field_of(s, n, n->target, F_PREDEPENDS), 0) ||
        enqueue(s, node, n->target, field_of(s, n, n->target, F_DEPENDS), 0)) return -1;
    if (n->target < 0) return 0;
    if (s->o->recommends) return enqueue(s, node, n->target, field_of(s, n, n->target, F_RECOMMENDS), 1);
    if (n->inst && ver_cmp(idx_s(s->x, s->x->pkg[n->target].version), n->inst->version) > 0)
        return enqueue_new_recommends(s, node, n->target, field_of(s, n, n->target, F_RECOMMENDS), n->inst->recommends);
    return 0;
}

/* ---- satisfaction ---- */

/* Does the provides field (of a present package) provide name, with a version that satisfies r? */
static int provides_match(const char *provides, const DebRel *r) {
    const char *e = provides + strlen(provides);
    DebRel p[MAX_ALTS];
    int k;
    while ((k = deb_next_group(&provides, e, p, MAX_ALTS)) > 0)
        for (int i = 0; i < k; i++) {
            if (p[i].nlen != r->nlen || memcmp(p[i].name, r->name, r->nlen)) continue;
            if (r->op == REL_NONE) return 1;
            if (p[i].op != REL_EQ) continue;
            char v[160];
            snprintf(v, sizeof v, "%.*s", (int)p[i].vlen, p[i].ver);
            if (ver_satisfies(v, r->op, r->ver, r->vlen)) return 1;
        }
    return 0;
}

/* Does node ni, as the plan has it, match conflict relation r of owner? */
static int rel_by_node(const S *s, int owner, int ni, const DebRel *r) {
    const Node *n = nd(s, ni);
    if (!present(n) || !arch_ok(s, owner, r, ni, pflags(s, n), 1)) return 0;
    if (n->blen == r->nlen && !memcmp(n->name, r->name, r->nlen) && ver_satisfies(node_ver(s, n), r->op, r->ver, r->vlen)) return 1;
    return provides_match(field_of(s, n, n->target, F_PROVIDES), r);
}

/* Does any present package satisfy r of owner (-1: a request)? exclude: node that doesn't count (for
 * conflicts); conflicts: r is a Conflicts or Breaks (every architecture). */
static int who_satisfies(S *s, int owner, const DebRel *r, int exclude, int *out, int max, int conflicts) {
    int found = 0, c[2], nc = rel_nodes(s, owner, r, c);
    for (int k = 0; k < nc; k++) {
        int ni = c[k];
        if (ni < 0 || ni == exclude || !present(nd(s, ni)) || !ver_satisfies(node_ver(s, nd(s, ni)), r->op, r->ver, r->vlen) ||
            !arch_ok(s, owner, r, ni, pflags(s, nd(s, ni)), conflicts) || (found && out[0] == ni)) continue;
        if (found < max) out[found] = ni;
        found++;
    }
    int lo, hi;
    refs_range(s->prov, s->nprov, r->name, r->nlen, &lo, &hi);
    for (int i = lo; i < hi && found < max; i++) {
        int pi = node_get(s, s->prov[i].p->key, strlen(s->prov[i].p->key));
        if (pi < 0 || pi == exclude || nd(s, pi)->target != T_KEEP) continue;
        if (!provides_match(nd(s, pi)->inst->provides, r) || !arch_ok(s, owner, r, pi, nd(s, pi)->inst->flags, conflicts)) continue;
        int dup = 0;
        for (int k = 0; k < found; k++) if (out[k] == pi) dup = 1;
        if (!dup) out[found++] = pi;
    }
    long plo, phi;
    idx_providers(s->x, r->name, r->nlen, &plo, &phi);
    for (long i = plo; i < phi && found < max; i++) {
        long rec = s->x->prov[i].pkg;
        const char *pn = idx_s(s->x, s->x->pkg[rec].name);
        int pi = node_get(s, pn, strlen(pn));
        if (pi < 0 || pi == exclude || nd(s, pi)->target != rec) continue;
        if (!provides_match(idx_s(s->x, s->x->pkg[rec].provides), r) || !arch_ok(s, owner, r, pi, s->x->pkg[rec].flags, conflicts)) continue;
        int dup = 0;
        for (int k = 0; k < found; k++) if (out[k] == pi) dup = 1;
        if (!dup) out[found++] = pi;
    }
    return found;
}

static int group_ok(S *s, int owner, const DebRel *alts, int n) {
    int who[2];
    for (int i = 0; i < n; i++) if (who_satisfies(s, owner, &alts[i], -1, who, 2, 0)) return 1;
    return 0;
}

/* ---- applying changes ---- */

static int apply_install(S *s, int ni, long rec, const char *why);

static int removable(const Node *n) {
    return !n->inst || !(n->inst->flags & (IF_ESSENTIAL | IF_PROTECTED));
}

static int apply_remove(S *s, int ni, const char *why) {
    Node *n = nd(s, ni);
    if (!removable(n)) { lerr_set(s->err, sizeof s->err, "pkg.removeEssential", "name", n->name, NULL); return 0; }
    if (mark(s, ni)) return 0;
    n->target = T_NONE;
    n->touched = 1;
    if (!n->remove_req) snprintf(n->reason, sizeof n->reason, "%s", why);
    /* what's installed that depended on this package (or on something it provided) is rechecked */
    const char *names[MAX_ALTS + 1];
    size_t lens[MAX_ALTS + 1];
    int nn = 0;
    names[nn] = n->name; lens[nn++] = n->blen;
    if (n->inst && n->inst->provides) {
        const char *p = n->inst->provides, *e = p + strlen(p);
        DebRel r[MAX_ALTS];
        int k;
        while ((k = deb_next_group(&p, e, r, MAX_ALTS)) > 0 && nn < MAX_ALTS)
            for (int i = 0; i < k && nn < MAX_ALTS; i++) { names[nn] = r[i].name; lens[nn++] = r[i].nlen; }
    }
    for (int k = 0; k < nn; k++) {
        int lo, hi;
        refs_range(s->rdep, s->nrdep, names[k], lens[k], &lo, &hi);
        for (int i = lo; i < hi; i++) {
            int di = node_get(s, s->rdep[i].p->key, strlen(s->rdep[i].p->key));
            if (di >= 0 && nd(s, di)->target == T_KEEP && enqueue_deps(s, di)) return 0;
        }
    }
    return 1;
}

/* Do record rec (if it were a's target) and node b as it stands clash? */
static int clash(S *s, int a, long rec, int b) {
    Node *na = nd(s, a), *nb = nd(s, b);
    if (!present(nb) || a == b) return 0;
    for (int f = F_CONFLICTS; f <= F_BREAKS; f++) {
        const char *p = field_of(s, na, rec, f), *e = p + strlen(p);
        DebRel r[MAX_ALTS];
        int k;
        while ((k = deb_next_group(&p, e, r, MAX_ALTS)) > 0)
            for (int i = 0; i < k; i++) if (rel_by_node(s, a, b, &r[i])) return 1;
        /* and the other way around: what b rejects against a's version rec */
        p = field_of(s, nb, nb->target, f);
        e = p + strlen(p);
        uint8_t af = rec == T_KEEP ? na->inst->flags : rec >= 0 ? s->x->pkg[rec].flags : 0;
        while ((k = deb_next_group(&p, e, r, MAX_ALTS)) > 0)
            for (int i = 0; i < k; i++) {
                if (!arch_ok(s, b, &r[i], a, af, 1)) continue;
                if (r[i].nlen == na->blen && !memcmp(r[i].name, na->name, na->blen)) {
                    const char *v = rec == T_KEEP ? na->inst->version : idx_s(s->x, s->x->pkg[rec].version);
                    if (ver_satisfies(v, r[i].op, r[i].ver, r[i].vlen)) return 1;
                }
                if (provides_match(field_of(s, na, rec, F_PROVIDES), &r[i]) && r[i].op == REL_NONE) return 1;
            }
    }
    return 0;
}

static int fix_clash(S *s, int ni, int ti) {
    Node *n = nd(s, ni), *t = nd(s, ti);
    char why[200];
    if (t->target == T_KEEP) {
        if (t->cand >= 0 && ver_cmp(idx_s(s->x, s->x->pkg[t->cand].version), t->inst->version) > 0 && !clash(s, ti, t->cand, ni)) {
            lerr_set(why, sizeof why, "pkg.why.upgradedFor", "name", n->name, NULL);
            return apply_install(s, ti, t->cand, why);
        }
        if (s->may_remove && removable(t) && !t->requested) {
            lerr_set(why, sizeof why, "pkg.why.conflictsWith", "name", n->name, NULL);
            return apply_remove(s, ti, why);
        }
        lerr_set(s->err, sizeof s->err, "pkg.conflictInstalled", "name", n->name, "other", t->name, NULL);
        return 0;
    }
    lerr_set(s->err, sizeof s->err, "pkg.conflict", "name", n->name, "other", t->name, NULL);
    return 0;
}

/* A plan error naming a package by its name without the architecture. */
static void err_base(S *s, const char *code, const Node *n) {
    char b[160];
    snprintf(b, sizeof b, "%.*s", (int)(n->blen < 150 ? n->blen : 150), n->name);
    lerr_set(s->err, sizeof s->err, code, "name", b, NULL);
}

static int apply_install(S *s, int ni, long rec, const char *why) {
    Node *n = nd(s, ni);
    if (n->remove_req) { lerr_set(s->err, sizeof s->err, "pkg.neededButRemoved", "name", n->name, NULL); return 0; }
    /* multiarch: the other architecture of this package, if the plan has it */
    int sb = sibling(s, ni), cmp = 0;
    if (sb >= 0 && present(nd(s, sb))) {
        Node *b = nd(s, sb);
        if (!(s->x->pkg[rec].flags & IF_MA_SAME) || !(pflags(s, b) & IF_MA_SAME)) { err_base(s, "pkg.multiarchConflict", n); return 0; }
        const char *bv = node_ver(s, b);
        cmp = ver_cmp(idx_s(s->x, s->x->pkg[rec].version), bv);
        if (cmp < 0) {                  /* never moves the other one down: this one goes to its version instead */
            long r2 = rec_at(s, n, bv);
            if (r2 >= 0 && (!n->inst || ver_cmp(bv, n->inst->version) >= 0)) { rec = r2; cmp = 0; }
            else { err_base(s, "pkg.multiarchSplit", n); return 0; }
        }
    }
    if (mark(s, ni)) return 0;
    n->target = rec;
    n->touched = 1;
    if (!n->requested && why) snprintf(n->reason, sizeof n->reason, "%s", why);
    if (cmp > 0) {                      /* the other architecture moves up to the same version, in the same plan */
        Node *b = nd(s, sb);
        long r2 = rec_at(s, b, idx_s(s->x, s->x->pkg[rec].version));
        char w[200];
        lerr_set(w, sizeof w, "pkg.why.multiarchPair", "name", n->name, NULL);
        if (r2 < 0 || b->remove_req) { err_base(s, "pkg.multiarchSplit", n); return 0; }
        if (b->target != r2 && !apply_install(s, sb, r2, w)) return 0;
    }
    if (enqueue_deps(s, ni)) return 0;
    /* conflicts with everything present named in its Conflicts/Breaks */
    for (int f = F_CONFLICTS; f <= F_BREAKS; f++) {
        const char *p = field_of(s, n, rec, f), *e = p + strlen(p);
        DebRel r[MAX_ALTS];
        int k;
        while ((k = deb_next_group(&p, e, r, MAX_ALTS)) > 0)
            for (int i = 0; i < k; i++) {
                int who[16];
                int c = who_satisfies(s, ni, &r[i], ni, who, 16, 1);
                for (int w = 0; w < c && w < 16; w++)
                    if (nd(s, ni)->target == rec && present(nd(s, who[w])) && clash(s, ni, rec, who[w]) && !fix_clash(s, ni, who[w])) return 0;
            }
    }
    /* and what's installed that rejects this package (by name or by what it provides) */
    const char *names[MAX_ALTS + 1];
    size_t lens[MAX_ALTS + 1];
    int nn = 0;
    names[nn] = n->name; lens[nn++] = n->blen;
    {
        const char *p = idx_s(s->x, s->x->pkg[rec].provides), *e = p + strlen(p);
        DebRel r[MAX_ALTS];
        int k;
        while ((k = deb_next_group(&p, e, r, MAX_ALTS)) > 0 && nn < MAX_ALTS)
            for (int i = 0; i < k && nn < MAX_ALTS; i++) { names[nn] = r[i].name; lens[nn++] = r[i].nlen; }
    }
    for (int k = 0; k < nn; k++) {
        int lo, hi;
        refs_range(s->conf, s->nconf, names[k], lens[k], &lo, &hi);
        for (int i = lo; i < hi; i++) {
            int ti = node_get(s, s->conf[i].p->key, strlen(s->conf[i].p->key));
            if (ti < 0 || ti == ni || nd(s, ti)->target != T_KEEP) continue;
            if (nd(s, ni)->target == rec && clash(s, ni, rec, ti) && !fix_clash(s, ni, ti)) return 0;
        }
    }
    /* what's new in this same operation */
    for (int t = 0; t < s->nn; t++)
        if (t != ni && nd(s, t)->touched && nd(s, t)->target >= 0 && nd(s, ni)->target == rec && clash(s, ni, rec, t) && !fix_clash(s, ni, t)) return 0;
    return 1;
}

/* ---- search ---- */

typedef struct { int node; long rec; } Opt;

static int prov_rank(const S *s, long rec) {
    int p = s->x->pkg[rec].prio;
    return p ? p : 9;
}

static int gather(S *s, int owner, const DebRel *alts, int n, Opt *opt, int max) {
    int k = 0;
    for (int a = 0; a < n && k < max; a++) {
        int c[2], nc = rel_nodes(s, owner, &alts[a], c);
        for (int j = 0; j < nc && k < max; j++) {
            int ni = c[j];
            if (ni < 0) continue;
            Node *an = nd(s, ni);
            /* what this same operation removed isn't installed again */
            if (an->cand >= 0 && !an->remove_req && an->target != an->cand && !(an->inst && an->target == T_NONE) &&
                ver_satisfies(idx_s(s->x, s->x->pkg[an->cand].version), alts[a].op, alts[a].ver, alts[a].vlen) &&
                arch_ok(s, owner, &alts[a], ni, s->x->pkg[an->cand].flags, 0)) {
                if (pair_ok(s, ni, an->cand)) opt[k++] = (Opt){ ni, an->cand };
                else s->pair_block = ni;
            }
        }
        long lo, hi;
        idx_providers(s->x, alts[a].name, alts[a].nlen, &lo, &hi);
        int first = k;
        for (long i = lo; i < hi && k < max; i++) {
            long rec = s->x->prov[i].pkg;
            const char *pn = idx_s(s->x, s->x->pkg[rec].name);
            if (!s->ma && strchr(pn, ':')) continue;
            int pi = node_get(s, pn, strlen(pn));
            if (pi < 0) continue;
            Node *pd = nd(s, pi);
            if (pd->cand != rec || pd->remove_req || pd->target == rec || (pd->inst && pd->target == T_NONE)) continue;
            if (!provides_match(idx_s(s->x, s->x->pkg[rec].provides), &alts[a]) || !arch_ok(s, owner, &alts[a], pi, s->x->pkg[rec].flags, 0)) continue;
            if (!pair_ok(s, pi, rec)) { s->pair_block = pi; continue; }
            int dup = 0;
            for (int j = 0; j < k; j++) if (opt[j].node == pi) dup = 1;
            if (!dup) opt[k++] = (Opt){ pi, rec };
        }
        /* providers: the highest priority first (required, important…), then by name */
        for (int i = first + 1; i < k; i++)
            for (int j = i; j > first; j--) {
                int ra = prov_rank(s, opt[j - 1].rec), rb = prov_rank(s, opt[j].rec);
                if (ra < rb || (ra == rb && strcmp(nd(s, opt[j - 1].node)->name, nd(s, opt[j].node)->name) <= 0)) break;
                Opt t = opt[j]; opt[j] = opt[j - 1]; opt[j - 1] = t;
            }
    }
    return k;
}

/* Length of a group's text without the comma or trailing spaces. */
static int group_len(const char *g, const char *ge) {
    while (ge > g && (ge[-1] == ',' || ge[-1] == ' ' || ge[-1] == '\n')) ge--;
    return (int)(ge - g > 120 ? 120 : ge - g);
}

static void describe_missing(S *s, const Node *owner, const char *g, const char *ge) {
    char dep[128];
    snprintf(dep, sizeof dep, "%.*s", group_len(g, ge), g);
    lerr_set(s->err, sizeof s->err, "pkg.dependencyUninstallable", "name", owner->name, "dep", dep, NULL);
}

static int solve(S *s, int qpos) {
    while (qpos < s->nq) {
        Item it = s->q[qpos++];
        Node *owner = nd(s, it.node);
        if (owner->target != it.expect) continue;           /* it changed: its dependencies were already queued */
        DebRel alts[MAX_ALTS];
        const char *p = it.g;
        int na = deb_next_group(&p, it.ge, alts, MAX_ALTS);
        if (na <= 0 || group_ok(s, it.node, alts, na)) continue;
        Opt opt[MAX_ALTS + 1];
        int no = 0;
        /* something installed that was left without what it needs: first, upgrade it */
        if (owner->target == T_KEEP && owner->cand >= 0 && !owner->remove_req &&
            ver_cmp(idx_s(s->x, s->x->pkg[owner->cand].version), owner->inst->version) > 0)
            opt[no++] = (Opt){ it.node, owner->cand };
        s->pair_block = -1;
        no += gather(s, it.node, alts, na, opt + no, MAX_ALTS);
        char why[200];
        lerr_set(why, sizeof why, "pkg.why.dependencyOf", "name", owner->name, NULL);
        /* if nothing works and it's something installed, it's removed too (with confirmation) */
        int can_drop = owner->target == T_KEEP && s->may_remove && !owner->requested && removable(owner);
        char w2[200];
        { char dep[128]; snprintf(dep, sizeof dep, "%.*s", group_len(it.g, it.ge), it.g); lerr_set(w2, sizeof w2, "pkg.why.lostDependency", "dep", dep, NULL); }
        if (!no) {
            if (it.optional) continue;
            if (can_drop) { if (!apply_remove(s, it.node, w2)) return 0; continue; }
            if (s->pair_block >= 0) err_base(s, "pkg.multiarchConflict", nd(s, s->pair_block));   /* only its other architecture is in the way */
            else describe_missing(s, owner, it.g, it.ge);
            return 0;
        }
        /* a single option and nothing else to try: no backtracking. If removing the owner is another
         * way out, the option is tried with the rest of the queue and if it fails the owner is removed. */
        if (no == 1 && !can_drop) {
            int lm = s->nlog, qm = s->nq;
            if (!apply_install(s, opt[0].node, opt[0].rec, opt[0].node == it.node ? "pkg.why.upgrade" : why)) {
                undo_to(s, lm);
                s->nq = qm;
                if (it.optional) continue;
                return 0;
            }
            continue;
        }
        for (int k = 0; k < no; k++) {
            if (--s->budget <= 0) { lerr_set(s->err, sizeof s->err, "pkg.tooManyCombinations", NULL); return 0; }
            int lm = s->nlog, qm = s->nq;
            if (apply_install(s, opt[k].node, opt[k].rec, opt[k].node == it.node ? "pkg.why.upgrade" : why) && solve(s, qpos)) return 1;
            undo_to(s, lm);
            s->nq = qm;
        }
        if (it.optional) continue;
        if (can_drop) return apply_remove(s, it.node, w2) && solve(s, qpos);
        return 0;
    }
    return 1;
}

/* Reviews what changed and whatever depends on it until nothing else changes. */
static int settle(S *s) {
    for (int round = 0; round < 8; round++) {
        int before = s->nlog, qstart = s->nq;
        for (int i = 0; i < s->nn; i++) {
            Node *n = nd(s, i);
            if (!n->touched) continue;
            if (present(n) && enqueue_deps(s, i)) return 0;
            if (n->inst) {
                int lo, hi;
                refs_range(s->rdep, s->nrdep, n->name, n->blen, &lo, &hi);
                for (int k = lo; k < hi; k++) {
                    int di = node_get(s, s->rdep[k].p->key, strlen(s->rdep[k].p->key));
                    if (di >= 0 && nd(s, di)->target == T_KEEP && enqueue_deps(s, di)) return 0;
                }
            }
        }
        if (!solve(s, qstart)) return 0;
        if (s->nlog == before) return 1;
    }
    return 1;
}

/* ---- orphans (like apt autoremove) ---- */

static int is_auto(const PlanOpts *o, const char *name) {
    for (int i = 0; i < o->nautos; i++) if (!strcmp(o->autos[i], name)) return 1;
    return 0;
}

/* Marks from the roots (what was installed by hand, what's essential and what's being installed
 * now) everything needed via Pre-Depends, Depends or Recommends (like apt: recommends count);
 * whatever was installed as a dependency and is left unmarked gets removed. */
static void mark_orphans(S *s) {
    int *stack = malloc(sizeof *stack * (size_t)(s->st->n + s->nn + 16)), sp = 0;
    if (!stack) return;
    int cap = s->st->n + s->nn + 16;
    for (int i = 0; i < s->nn; i++) nd(s, i)->mark = 0;
    for (int i = 0; i < s->st->n; i++) {
        const StPkg *p = &s->st->p[i];
        int k = node_get(s, p->key, strlen(p->key));
        if (k < 0) continue;
        Node *n = nd(s, k);
        if (present(n) && (!is_auto(s->o, n->name) || !removable(n) || n->target >= 0)) { n->mark = 1; stack[sp++] = k; }
    }
    for (int i = 0; i < s->nn; i++)
        if (nd(s, i)->target >= 0 && !nd(s, i)->mark && sp < cap) { nd(s, i)->mark = 1; stack[sp++] = i; }
    while (sp > 0) {
        int k = stack[--sp];
        Node *n = nd(s, k);
        for (int f = F_DEPENDS; f <= F_RECOMMENDS; f++) {
            const char *p = field_of(s, n, n->target, f), *e = p + strlen(p);
            DebRel r[MAX_ALTS];
            int c;
            while ((c = deb_next_group(&p, e, r, MAX_ALTS)) > 0)
                for (int j = 0; j < c; j++) {
                    int who[16];
                    int w = who_satisfies(s, k, &r[j], -1, who, 16, 0);
                    for (int q = 0; q < w && q < 16; q++) {
                        Node *d = nd(s, who[q]);
                        if (d->mark) continue;
                        d->mark = 1;
                        if (sp == cap) {
                            int *ns = realloc(stack, sizeof *ns * (size_t)(cap * 2));
                            if (!ns) { free(stack); return; }
                            stack = ns;
                            cap *= 2;
                        }
                        stack[sp++] = who[q];
                    }
                }
        }
    }
    free(stack);
    for (int i = 0; i < s->nn; i++) {
        Node *n = nd(s, i);
        if (n->target == T_KEEP && !n->mark && removable(n) && is_auto(s->o, n->name)) {
            n->target = T_NONE;
            n->orphan = 1;
            n->touched = 1;
        }
    }
}

/* The last check before a plan is accepted: every package whose two architectures are present after it,
 * and that this plan changes, is "Multi-Arch: same" in both at the same version. */
static int pairs_ok(S *s) {
    if (!s->ma) return 1;
    int nn = s->nn;                     /* sibling() may add nodes; those aren't touched */
    for (int i = 0; i < nn; i++) {
        Node *n = nd(s, i);
        if (!n->touched || !present(n)) continue;
        int sb = sibling(s, i);
        if (sb < 0 || !present(nd(s, sb))) continue;
        n = nd(s, i);
        Node *b = nd(s, sb);
        if (!(pflags(s, n) & IF_MA_SAME) || !(pflags(s, b) & IF_MA_SAME)) { err_base(s, "pkg.multiarchConflict", n); return 0; }
        if (strcmp(node_ver(s, n), node_ver(s, b))) { err_base(s, "pkg.multiarchSplit", n); return 0; }
    }
    return 1;
}

/* ---- plan ---- */

static int item_cmp(const void *a, const void *b) {
    const PlanItem *x = a, *y = b;
    if (x->action != y->action) return x->action - y->action;
    return strcmp(x->name, y->name);
}

void plan_free(Plan *p) {
    free(p->items);
    memset(p, 0, sizeof *p);
}

int plan_make(const Idx *x, const Status *st, const char *const *install, int ni,
              const char *const *remove, int nr, const PlanOpts *o, Plan *out) {
    memset(out, 0, sizeof *out);
    S s;
    memset(&s, 0, sizeof s);
    s.x = x;
    s.st = st;
    s.o = o;
    s.budget = BUDGET;
    s.may_remove = 1;                   /* the extra removals are computed and then confirmed */
    s.pair_block = -1;
    s.native = x->map && x->h->arch[0] ? x->h->arch : pkg_native_arch();
    /* multiarch is in play when a foreign package is installed or indexed (else nothing changes) */
    for (int i = 0; i < st->n && !s.ma; i++) if (!st->p[i].native) s.ma = 1;
    for (long i = 0; x->map && i < (long)x->h->npkg && !s.ma; i++) if (strchr(idx_s(x, x->pkg[i].name), ':')) s.ma = 1;
    int ok = !build_tables(&s);
    if (!ok) lerr_set(s.err, sizeof s.err, "sys.outOfMemory", NULL);
    if (!x->map && (ni || o->upgrade_all)) { ok = 0; lerr_set(s.err, sizeof s.err, "pkg.noIndex", NULL); }

    for (int i = 0; ok && i < nr; i++) {
        int k = node_get(&s, remove[i], strlen(remove[i]));
        if (k < 0) { ok = 0; break; }
        Node *n = nd(&s, k);
        if (!n->inst) { ok = 0; lerr_set(s.err, sizeof s.err, "pkg.notInstalled", "name", remove[i], NULL); break; }
        n->remove_req = 1;
        n->requested = 1;
        ok = apply_remove(&s, k, "pkg.why.requested");
    }
    for (int i = 0; ok && i < ni; i++) {
        int k = node_get(&s, install[i], strlen(install[i]));
        if (k < 0) { ok = 0; break; }
        Node *n = nd(&s, k);
        if (n->cand < 0 && !n->inst) {
            /* virtual? a single provider, or one has to be chosen ("name:i386": one of that architecture) */
            size_t il = strlen(install[i]), bl = deb_base_len(install[i], il);
            DebRel r = { install[i], NULL, bl, 0, REL_NONE, bl < il ? install[i] + bl + 1 : NULL, bl < il ? il - bl - 1 : 0 };
            Opt opt[MAX_ALTS];
            int no = gather(&s, -1, &r, 1, opt, MAX_ALTS);
            if (no == 1) { k = opt[0].node; n = nd(&s, k); }
            else if (no > 1) {
                char names[300] = "";
                for (int j = 0; j < no && strlen(names) < sizeof names - 70; j++)
                    snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", j ? ", " : "", nd(&s, opt[j].node)->name);
                lerr_set(s.err, sizeof s.err, "pkg.virtual", "name", install[i], "options", names, NULL);
                ok = 0;
                break;
            } else { ok = 0; lerr_set(s.err, sizeof s.err, "pkg.notFound", "name", install[i], NULL); break; }
        }
        n->requested = 1;
        if (n->remove_req) { ok = 0; lerr_set(s.err, sizeof s.err, "pkg.installAndRemove", "name", n->name, NULL); break; }
        if (n->cand < 0 || (n->inst && !ver_cmp(idx_s(x, x->pkg[n->cand].version), n->inst->version))) continue;   /* already there */
        ok = apply_install(&s, k, n->cand, NULL);
    }
    if (ok) ok = solve(&s, 0);
    if (ok && o->upgrade_all) {
        s.may_remove = o->allow_remove || o->cascade;
        for (int i = 0; i < st->n; i++) {
            const StPkg *p = &st->p[i];
            int k = node_get(&s, p->key, strlen(p->key));
            if (k < 0) continue;
            Node *n = nd(&s, k);
            if (n->target != T_KEEP || n->cand < 0 || ver_cmp(idx_s(x, x->pkg[n->cand].version), p->version) <= 0) continue;
            int lm = s.nlog, qm = s.nq;
            s.budget = BUDGET;
            if (apply_install(&s, k, n->cand, "pkg.why.upgrade") && solve(&s, qm)) continue;
            undo_to(&s, lm);
            s.nq = qm;
            if (out->nheld < 32) snprintf(out->held[out->nheld++], sizeof out->held[0], "%s", p->key);
        }
        s.err[0] = 0;
        s.may_remove = 1;
    }
    if (ok) ok = settle(&s);
    if (ok && o->orphans) mark_orphans(&s);
    if (ok) ok = pairs_ok(&s);

    /* result */
    int count = 0;
    for (int i = 0; i < s.nn; i++) {
        Node *n = nd(&s, i);
        int changed = (n->target >= 0 && (!n->inst || ver_cmp(idx_s(x, x->pkg[n->target].version), n->inst->version))) ||
                      (n->target == T_NONE && n->inst);
        if (changed) count++;
    }
    out->items = calloc((size_t)(count ? count : 1), sizeof *out->items);
    if (!out->items) { ok = 0; lerr_set(s.err, sizeof s.err, "sys.outOfMemory", NULL); }
    int extra_removals = 0;
    char extra[300] = "";
    for (int i = 0; out->items && i < s.nn; i++) {
        Node *n = nd(&s, i);
        PlanItem it;
        memset(&it, 0, sizeof it);
        it.rec = -1;
        if (n->target >= 0 && (!n->inst || ver_cmp(idx_s(x, x->pkg[n->target].version), n->inst->version))) {
            const IdxPkg *r = &x->pkg[n->target];
            it.action = n->inst ? PA_UPGRADE : PA_INSTALL;
            it.version = idx_s(x, r->version);
            it.old_version = n->inst ? n->inst->version : NULL;
            it.rec = n->target;
            out->download += r->size;
            out->isize_delta += (int64_t)r->isize - (n->inst ? (int64_t)n->inst->isize : 0);
        } else if (n->target == T_NONE && n->inst) {
            it.action = PA_REMOVE;
            it.version = n->inst->version;
            out->isize_delta -= (int64_t)n->inst->isize;
            if (!n->remove_req && !n->orphan) {
                extra_removals++;
                size_t l = strlen(extra);
                if (l < sizeof extra - 70) snprintf(extra + l, sizeof extra - l, "%s%s", l ? ", " : "", n->name);
            }
        } else continue;
        it.name = n->name;
        it.requested = n->requested;
        it.orphan = n->orphan;
        snprintf(it.reason, sizeof it.reason, "%s", n->requested ? "pkg.why.requested" : n->orphan ? "pkg.why.auto" : n->reason);
        out->items[out->n++] = it;
    }
    if (out->items) qsort(out->items, (size_t)out->n, sizeof *out->items, item_cmp);
    if (ok && extra_removals && !o->allow_remove && !o->cascade) {
        ok = 0;
        lerr_set(s.err, sizeof s.err, "pkg.removeAlso", "names", extra, NULL);
    }
    out->ok = ok;
    snprintf(out->err, sizeof out->err, "%s", ok ? "" : s.err[0] ? s.err : "pkg.unresolvable");
    for (int i = 0; i < s.nblocks; i++) free(s.blocks[i]);
    free(s.blocks); free(s.ht); free(s.log); free(s.q); free(s.prov); free(s.rdep); free(s.conf);
    return ok ? 0 : -1;
}

/* ---- order for dpkg ---- */

typedef struct { const Idx *x; const Plan *p; int *state, *order, n; } Topo;

/* The item a relation of item `from` names: that package in from's architecture first, then in the other
 * one, then something in the batch that provides it. */
static int find_item(const Topo *t, int from, const char *name, size_t n) {
    const char *fn = from >= 0 ? t->p->items[from].name : "";
    const char *fs = strchr(fn, ':');
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < t->p->n; i++) {
            const PlanItem *it = &t->p->items[i];
            if (it->rec < 0) continue;
            size_t l = strlen(it->name), b = deb_base_len(it->name, l);
            if (b != n || memcmp(it->name, name, n)) continue;
            const char *is = b < l ? it->name + b : "";
            if (pass || !strcmp(is, fs ? fs : "")) return i;
        }
    for (int i = 0; i < t->p->n; i++) {             /* or something in the batch that provides it */
        const PlanItem *it = &t->p->items[i];
        if (it->rec < 0) continue;
        DebRel r = { name, NULL, n, 0, REL_NONE, NULL, 0 };
        if (provides_match(idx_s(t->x, t->x->pkg[it->rec].provides), &r)) return i;
    }
    return -1;
}

/* The other architecture of item i's package in the plan, or -1 (a "Multi-Arch: same" pair). */
static int pair_item(const Topo *t, int i) {
    const char *a = t->p->items[i].name;
    size_t al = strlen(a), ab = deb_base_len(a, al);
    for (int j = 0; j < t->p->n; j++) {
        const char *b = t->p->items[j].name;
        if (j == i || t->p->items[j].rec < 0) continue;
        size_t bl = strlen(b), bb = deb_base_len(b, bl);
        if (ab == bb && !memcmp(a, b, ab) && (al == ab) != (bl == bb)) return j;
    }
    return -1;
}

static void visit(Topo *t, int i);

static void visit_deps(Topo *t, int i, int skip) {
    const IdxPkg *r = &t->x->pkg[t->p->items[i].rec];
    for (int f = 0; f < 2; f++) {
        const char *s = idx_s(t->x, f ? r->depends : r->predepends), *e = s + strlen(s);
        DebRel a[MAX_ALTS];
        int k;
        while ((k = deb_next_group(&s, e, a, MAX_ALTS)) > 0)
            for (int j = 0; j < k; j++) {
                int d = find_item(t, i, a[j].name, a[j].nlen);
                if (d >= 0 && d != i && d != skip) visit(t, d);
            }
    }
}

static void visit(Topo *t, int i) {
    if (t->state[i]) return;                /* 1: in progress (cycle: cut), 2: done */
    /* the two architectures of a package go side by side, so they're unpacked by the same dpkg run */
    int pj = pair_item(t, i);
    if (pj >= 0 && t->state[pj]) pj = -1;
    t->state[i] = 1;
    if (pj >= 0) t->state[pj] = 1;
    visit_deps(t, i, pj);
    if (pj >= 0) visit_deps(t, pj, i);
    t->state[i] = 2;
    t->order[t->n++] = i;
    if (pj >= 0) { t->state[pj] = 2; t->order[t->n++] = pj; }
}

/* Does item a's new version Break or Conflict with item b's installed version (b is an upgrade)? */
static int breaks_installed(const Topo *t, int a, int b) {
    const PlanItem *ib = &t->p->items[b];
    if (!ib->old_version || a == b) return 0;
    size_t bl = strlen(ib->name), bb = deb_base_len(ib->name, bl);
    const IdxPkg *r = &t->x->pkg[t->p->items[a].rec];
    for (int f = 0; f < 2; f++) {
        const char *s = idx_s(t->x, f ? r->conflicts : r->breaks), *e = s + strlen(s);
        DebRel rel[MAX_ALTS];
        int k;
        while ((k = deb_next_group(&s, e, rel, MAX_ALTS)) > 0)
            for (int j = 0; j < k; j++)
                if (rel[j].nlen == bb && !memcmp(rel[j].name, ib->name, bb) &&
                    (rel[j].op == REL_NONE || ver_satisfies(ib->old_version, rel[j].op, rel[j].ver, rel[j].vlen))) return 1;
    }
    return 0;
}

/* (0.8.8.9) Inside one dpkg --install run, dpkg unpacks in argument order and checks each Breaks/Conflicts when it
 * unpacks, against what's installed at that moment; Depends wait for the configure step, after every unpack. So a
 * package whose installed version another one in the run breaks goes first: lordos "Breaks: lordos-web (<< V)" and
 * lordos-web "Depends: lordos (= V)" put lordos first in dependency order, and dpkg refused it ("installing lordos
 * would break lordos-web, and deconfiguration is not permitted"). The two architectures of a pair move together.
 * Bounded: a cycle (each breaks the other's installed version) keeps the order it has. */
static void breaks_first(Topo *t, int *group) {
    int *o = t->order, n = t->n;
    for (int start = 0; start < n;) {
        int end = start;
        while (end < n && group[end] == group[start]) end++;
        int budget = (end - start) * (end - start) + 1;
        for (int moved = 1; moved && budget-- > 0;) {
            moved = 0;
            for (int k = start; k < end && !moved; k++)
                for (int m = k + 1; m < end && !moved; m++) {
                    if (!breaks_installed(t, o[k], o[m])) continue;
                    int lo = m, hi = m;             /* the block that moves: m, and its pair beside it */
                    int pj = pair_item(t, o[m]);
                    if (pj >= 0 && m > k + 1 && o[m - 1] == pj) lo = m - 1;
                    else if (pj >= 0 && m + 1 < end && o[m + 1] == pj) hi = m + 1;
                    int kk = k;                     /* never between the two of a pair */
                    if (kk > start && pair_item(t, o[kk]) == o[kk - 1]) kk--;
                    int len = hi - lo + 1, tmp[2];
                    memcpy(tmp, &o[lo], (size_t)len * sizeof *o);
                    memmove(&o[kk + len], &o[kk], (size_t)(lo - kk) * sizeof *o);
                    memcpy(&o[kk], tmp, (size_t)len * sizeof *o);
                    moved = 1;
                }
        }
        start = end;
    }
}

int plan_order(const Idx *x, const Plan *p, int *order, int *group) {
    int *state = calloc((size_t)(p->n ? p->n : 1), sizeof *state);
    if (!state) return -1;
    Topo t = { x, p, state, order, 0 };
    for (int i = 0; i < p->n; i++) if (p->items[i].rec >= 0) visit(&t, i);
    /* batches: a Pre-Depends on something in the current batch forces configuring it first */
    int g = 0, start = 0;
    for (int k = 0; k < t.n; k++) {
        const IdxPkg *r = &x->pkg[p->items[order[k]].rec];
        const char *s = idx_s(x, r->predepends), *e = s + strlen(s);
        DebRel a[MAX_ALTS];
        int n, cut = 0;
        while (!cut && (n = deb_next_group(&s, e, a, MAX_ALTS)) > 0)
            for (int j = 0; j < n && !cut; j++) {
                int d = find_item(&t, order[k], a[j].name, a[j].nlen);
                for (int m = start; m < k && !cut; m++) if (order[m] == d) cut = 1;
            }
        if (cut && k > start && pair_item(&t, order[k]) == order[k - 1]) cut = 0;   /* never between the two of a pair */
        if (cut) { g++; start = k; }
        group[k] = g;
    }
    breaks_first(&t, group);
    free(state);
    return t.n;
}
