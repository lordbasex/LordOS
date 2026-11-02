/* js_pkg.c — lord.pkg: queries about Debian packages (read-only).
 *
 * Reads the index built by the helper (/var/lib/lordos/pkg/index.bin, via mmap) and the dpkg
 * database (/var/lib/dpkg/status). None of this modifies the system: installing, removing and upgrading
 * are actions of the privileged helper (admin.run('pkg-install', …) in the SDK). See
 * docs/PACKAGES.md. */
#include "js_pkg.h"
#include "../pkg/deb.h"
#include "../pkg/idx.h"
#include "../pkg/resolve.h"
#include "../pkg/status.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ICON_DIR PKG_DIR "/icons/64"

static Idx X;
static Status ST;
static char **autos;                    /* installed as a dependency (sorted) */
static int nautos;
static long auto_mtime = -1;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void refresh(void) {
    idx_refresh(&X, PKG_DIR);
    status_load(&ST, DPKG_STATUS, pkg_native_arch());
    struct stat st;
    long mt = stat(PKG_DIR "/auto", &st) ? 0 : (long)st.st_mtime;
    if (mt == auto_mtime) return;
    auto_mtime = mt;
    for (int i = 0; i < nautos; i++) free(autos[i]);
    free(autos);
    autos = NULL;
    nautos = 0;
    FILE *f = mt ? fopen(PKG_DIR "/auto", "r") : NULL;
    char line[256];
    int cap = 0;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        if (nautos == cap) { cap = cap ? cap * 2 : 256; char **n = realloc(autos, sizeof *n * (size_t)cap); if (!n) break; autos = n; }
        autos[nautos++] = strdup(line);
    }
    if (f) fclose(f);
    qsort(autos, (size_t)nautos, sizeof *autos, cmp_str);
}

static int is_auto(const char *name) {
    return nautos && bsearch(&name, autos, (size_t)nautos, sizeof *autos, cmp_str) != NULL;
}

/* ---- options ---- */

static int opt_int(JSContext *c, JSValueConst o, const char *k, int def, int lo, int hi) {
    if (!JS_IsObject(o)) return def;
    JSValue v = JS_GetPropertyStr(c, o, k);
    int32_t r = def;
    if (JS_IsNumber(v)) JS_ToInt32(c, &r, v);
    JS_FreeValue(c, v);
    return r < lo ? lo : r > hi ? hi : r;
}

static int opt_bool(JSContext *c, JSValueConst o, const char *k) {
    if (!JS_IsObject(o)) return 0;
    JSValue v = JS_GetPropertyStr(c, o, k);
    int r = JS_ToBool(c, v) > 0;
    JS_FreeValue(c, v);
    return r;
}

static void opt_str(JSContext *c, JSValueConst o, const char *k, char *out, size_t n) {
    out[0] = 0;
    if (!JS_IsObject(o)) return;
    JSValue v = JS_GetPropertyStr(c, o, k);
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s) { snprintf(out, n, "%s", s); JS_FreeCString(c, s); }
    }
    JS_FreeValue(c, v);
}

static void set_s(JSContext *c, JSValue o, const char *k, const char *v) {
    JS_SetPropertyStr(c, o, k, v && v[0] ? JS_NewString(c, v) : JS_NULL);
}

/* ---- objects ---- */

static int lang_of(const char *lang) {
    if (!strncmp(lang, "es", 2)) return 1;
    if (!strncmp(lang, "pt", 2)) return 2;
    return 0;
}

static const char *app_name(const IdxApp *a, int l) {
    const char *s = l == 1 ? idx_s(&X, a->name_es) : l == 2 ? idx_s(&X, a->name_pt) : "";
    return s[0] ? s : idx_s(&X, a->name);
}

static const char *app_summary(const IdxApp *a, int l) {
    const char *s = l == 1 ? idx_s(&X, a->summary_es) : l == 2 ? idx_s(&X, a->summary_pt) : "";
    return s[0] ? s : idx_s(&X, a->summary);
}

static JSValue app_obj(JSContext *c, const IdxApp *a, int l) {
    JSValue o = JS_NewObject(c);
    set_s(c, o, "id", idx_s(&X, a->id));
    set_s(c, o, "type", idx_s(&X, a->type));
    set_s(c, o, "name", app_name(a, l));
    set_s(c, o, "summary", app_summary(a, l));
    char path[600];
    const char *icon = idx_s(&X, a->icon);
    snprintf(path, sizeof path, ICON_DIR "/%s", icon);
    set_s(c, o, "icon", icon[0] && !access(path, R_OK) ? path : NULL);
    JSValue cats = JS_NewArray(c);
    const char *s = idx_s(&X, a->categories);
    uint32_t n = 0;
    while (*s) {
        const char *e = strchr(s, ';');
        size_t l2 = e ? (size_t)(e - s) : strlen(s);
        JS_SetPropertyUint32(c, cats, n++, JS_NewStringLen(c, s, l2));
        s += l2 + (e ? 1 : 0);
    }
    JS_SetPropertyStr(c, o, "categories", cats);
    return o;
}

/* Summary of a package: rec = first record with that name (or -1 if it is only installed). */
static JSValue summary_obj(JSContext *c, const char *name, long rec, const StPkg *inst, int l) {
    JSValue o = JS_NewObject(c);
    long cand = rec >= 0 ? pkg_candidate(&X, name, strlen(name), inst) : -1;
    const IdxPkg *p = cand >= 0 ? &X.pkg[cand] : rec >= 0 ? &X.pkg[rec] : NULL;
    set_s(c, o, "name", name);
    set_s(c, o, "version", p ? idx_s(&X, p->version) : inst ? inst->version : NULL);
    set_s(c, o, "summary", p ? idx_s(&X, p->summary) : inst ? inst->summary : NULL);
    set_s(c, o, "section", p ? idx_s(&X, p->section) : inst ? inst->section : NULL);
    JS_SetPropertyStr(c, o, "size", JS_NewFloat64(c, p ? p->size : 0));
    JS_SetPropertyStr(c, o, "installedSize", JS_NewFloat64(c, 1024.0 * (p ? p->isize : inst ? inst->isize : 0)));
    set_s(c, o, "installed", inst ? inst->version : NULL);
    JS_SetPropertyStr(c, o, "upgradable", JS_NewBool(c, inst && cand >= 0 && ver_cmp(idx_s(&X, X.pkg[cand].version), inst->version) > 0));
    JS_SetPropertyStr(c, o, "essential", JS_NewBool(c, ((p ? p->flags : 0) | (inst ? inst->flags : 0)) & (IF_ESSENTIAL | IF_PROTECTED)));
    JS_SetPropertyStr(c, o, "auto", JS_NewBool(c, inst && is_auto(name)));
    JS_SetPropertyStr(c, o, "app", p && p->app != UINT32_MAX && p->app < X.h->napp ? app_obj(c, &X.app[p->app], l) : JS_NULL);
    return o;
}

/* ---- search ---- */

static const char *ci_find(const char *hay, const char *needle, size_t nl) {
    if (!nl) return hay;
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == needle[i]) i++;
        if (i == nl) return h;
    }
    return NULL;
}

typedef struct { long rec; int score; } Hit;

static JSValue f_search(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    refresh();
    const char *q = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    int limit = opt_int(c, opts, "limit", 50, 1, 500), apps_only = opt_bool(c, opts, "apps");
    char lang[16], section[64];
    opt_str(c, opts, "lang", lang, sizeof lang);
    opt_str(c, opts, "section", section, sizeof section);
    int l = lang_of(lang);
    char terms[8][64];
    int nt = 0;
    if (q) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", q);
        for (char *t = strtok(buf, " \t"); t && nt < 8; t = strtok(NULL, " \t")) {
            size_t k = 0;
            for (; t[k] && k < 63; k++) terms[nt][k] = (char)tolower((unsigned char)t[k]);
            terms[nt++][k] = 0;
        }
        JS_FreeCString(c, q);
    }
    Hit *hits = calloc((size_t)limit + 1, sizeof *hits);
    int nh = 0;
    for (long i = 0; hits && X.map && i < (long)X.h->npkg; i++) {
        if (i && X.pkg[i].name == X.pkg[i - 1].name) continue;        /* once per name */
        const IdxPkg *p = &X.pkg[i];
        const IdxApp *a = p->app != UINT32_MAX && p->app < X.h->napp ? &X.app[p->app] : NULL;
        if (apps_only && (!a || strcmp(idx_s(&X, a->type), "desktop-application"))) continue;
        if (section[0] && strcmp(idx_s(&X, p->section), section)) continue;
        const char *name = idx_s(&X, p->name);
        int score = a ? 300 : 0, all = 1;            /* applications first */
        for (int t = 0; t < nt && all; t++) {
            size_t tl = strlen(terms[t]);
            int s = 0;
            const char *m = ci_find(name, terms[t], tl);
            if (m) s = !strcasecmp(name, terms[t]) ? 1000 : m == name ? 500 : 200;
            if (a) {
                const char *an = app_name(a, l);
                const char *am = ci_find(an, terms[t], tl);
                int as = !am ? 0 : !strcasecmp(an, terms[t]) ? 900 : am == an ? 450 : 250;
                if (as > s) s = as;
                if (!s && ci_find(app_summary(a, l), terms[t], tl)) s = 80;
            }
            if (!s && ci_find(idx_s(&X, p->summary), terms[t], tl)) s = 50;
            if (!s) all = 0;
            score += s;
        }
        if (!all) continue;
        if (!nt && !a) continue;        /* no text: only the applications */
        /* top-k by insertion */
        int pos = nh;
        while (pos > 0 && (hits[pos - 1].score < score || (hits[pos - 1].score == score && strcmp(idx_s(&X, X.pkg[hits[pos - 1].rec].name), name) > 0))) pos--;
        if (pos >= limit) continue;
        int move = (nh < limit ? nh : limit - 1) - pos;
        if (move > 0) memmove(hits + pos + 1, hits + pos, sizeof *hits * (size_t)move);
        hits[pos] = (Hit){ i, score };
        if (nh < limit) nh++;
    }
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < nh; i++) {
        const char *name = idx_s(&X, X.pkg[hits[i].rec].name);
        JS_SetPropertyUint32(c, arr, (uint32_t)i, summary_obj(c, name, hits[i].rec, status_find(&ST, name, strlen(name)), l));
    }
    free(hits);
    return arr;
}

static int arg_name(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    int ok = strlen(s) < n && deb_valid_name(s, strlen(s));
    if (ok) snprintf(out, n, "%s", s);
    JS_FreeCString(c, s);
    return ok ? 0 : -1;
}

static JSValue f_info(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char name[129], lang[16];
    if (argc < 1 || arg_name(c, argv[0], name, sizeof name)) return JS_NULL;
    opt_str(c, argc > 1 ? argv[1] : JS_UNDEFINED, "lang", lang, sizeof lang);
    refresh();
    long rec = idx_find(&X, name, strlen(name));
    const StPkg *inst = status_find(&ST, name, strlen(name));
    if (rec < 0 && !inst) return JS_NULL;
    JSValue o = summary_obj(c, name, rec, inst, lang_of(lang));
    long cand = rec >= 0 ? pkg_candidate(&X, name, strlen(name), inst) : -1;
    const IdxPkg *p = cand >= 0 ? &X.pkg[cand] : rec >= 0 ? &X.pkg[rec] : NULL;
    if (p) {
        JS_SetPropertyStr(c, o, "description", p->desclen && p->desc + (uint64_t)p->desclen <= X.desc_len ? JS_NewStringLen(c, X.desc + p->desc, p->desclen) : JS_NULL);
        set_s(c, o, "homepage", idx_s(&X, p->homepage));
        set_s(c, o, "arch", idx_s(&X, p->arch));
        set_s(c, o, "source", idx_s(&X, p->source));
        set_s(c, o, "depends", idx_s(&X, p->depends));
        set_s(c, o, "preDepends", idx_s(&X, p->predepends));
        set_s(c, o, "recommends", idx_s(&X, p->recommends));
        set_s(c, o, "suggests", idx_s(&X, p->suggests));
        set_s(c, o, "provides", idx_s(&X, p->provides));
        set_s(c, o, "conflicts", idx_s(&X, p->conflicts));
        set_s(c, o, "breaks", idx_s(&X, p->breaks));
        static const char *const prios[] = { NULL, "required", "important", "standard", "optional", "extra" };
        set_s(c, o, "priority", p->prio <= 5 ? prios[p->prio] : NULL);
        const IdxSrc *s = &X.src[p->src < X.h->nsrc ? p->src : 0];
        set_s(c, o, "suite", idx_s(&X, s->suite));
        set_s(c, o, "origin", idx_s(&X, s->label));
        if (p->app != UINT32_MAX && p->app < X.h->napp) {
            const IdxApp *a = &X.app[p->app];
            JS_SetPropertyStr(c, o, "appDescription", a->desc < X.desc_len ? JS_NewString(c, X.desc + a->desc) : JS_NULL);
        }
    } else if (inst) {
        set_s(c, o, "depends", inst->depends);
        set_s(c, o, "arch", inst->arch);
    }
    JSValue vers = JS_NewArray(c);
    uint32_t nv = 0;
    for (long i = rec; i >= 0 && i < (long)X.h->npkg && X.pkg[i].name == X.pkg[rec].name; i++) {
        JSValue v = JS_NewObject(c);
        set_s(c, v, "version", idx_s(&X, X.pkg[i].version));
        set_s(c, v, "suite", idx_s(&X, X.src[X.pkg[i].src < X.h->nsrc ? X.pkg[i].src : 0].suite));
        JS_SetPropertyStr(c, v, "size", JS_NewFloat64(c, X.pkg[i].size));
        JS_SetPropertyUint32(c, vers, nv++, v);
    }
    JS_SetPropertyStr(c, o, "versions", vers);
    return o;
}

static JSValue f_installed(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char lang[16];
    opt_str(c, argc > 0 ? argv[0] : JS_UNDEFINED, "lang", lang, sizeof lang);
    refresh();
    JSValue arr = JS_NewArray(c);
    uint32_t n = 0;
    for (int i = 0; i < ST.n; i++) {
        const StPkg *p = &ST.p[i];
        if (!p->native) continue;
        long rec = idx_find(&X, p->name, strlen(p->name));
        JS_SetPropertyUint32(c, arr, n++, summary_obj(c, p->name, rec, p, lang_of(lang)));
    }
    return arr;
}

static JSValue f_upgrades(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    refresh();
    JSValue arr = JS_NewArray(c);
    uint32_t n = 0;
    for (int i = 0; X.map && i < ST.n; i++) {
        const StPkg *p = &ST.p[i];
        if (!p->native) continue;
        long cand = pkg_candidate(&X, p->name, strlen(p->name), p);
        if (cand < 0 || ver_cmp(idx_s(&X, X.pkg[cand].version), p->version) <= 0) continue;
        JSValue o = JS_NewObject(c);
        set_s(c, o, "name", p->name);
        set_s(c, o, "from", p->version);
        set_s(c, o, "to", idx_s(&X, X.pkg[cand].version));
        set_s(c, o, "summary", idx_s(&X, X.pkg[cand].summary));
        set_s(c, o, "suite", idx_s(&X, X.src[X.pkg[cand].src < X.h->nsrc ? X.pkg[cand].src : 0].suite));
        JS_SetPropertyStr(c, o, "size", JS_NewFloat64(c, X.pkg[cand].size));
        JS_SetPropertyUint32(c, arr, n++, o);
    }
    return arr;
}

static int names_of(JSContext *c, JSValueConst o, const char *k, char names[][129], int max) {
    if (!JS_IsObject(o)) return 0;
    JSValue a = JS_GetPropertyStr(c, o, k);
    int n = 0;
    if (JS_IsArray(c, a)) {
        JSValue lv = JS_GetPropertyStr(c, a, "length");
        int32_t len = 0;
        JS_ToInt32(c, &len, lv);
        JS_FreeValue(c, lv);
        for (int32_t i = 0; i < len && n < max; i++) {
            JSValue v = JS_GetPropertyUint32(c, a, (uint32_t)i);
            if (!arg_name(c, v, names[n], 129)) n++;
            else n = -1000;             /* invalid name: the whole request is invalid */
            JS_FreeValue(c, v);
            if (n < 0) break;
        }
    }
    JS_FreeValue(c, a);
    return n;
}

static JSValue f_plan(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    static char ins[64][129], rem[64][129];
    int ni = names_of(c, o, "install", ins, 64), nr = names_of(c, o, "remove", rem, 64);
    JSValue r = JS_NewObject(c);
    if (ni < 0 || nr < 0) {
        JS_SetPropertyStr(c, r, "ok", JS_FALSE);
        set_s(c, r, "error", "pkg.invalidName");
        return r;
    }
    refresh();
    const char *ip[64], *rp[64];
    for (int i = 0; i < ni; i++) ip[i] = ins[i];
    for (int i = 0; i < nr; i++) rp[i] = rem[i];
    PlanOpts po = { opt_bool(c, o, "recommends"), opt_bool(c, o, "allowRemove"), opt_bool(c, o, "cascade"), opt_bool(c, o, "upgrade"),
                    nr > 0 && !opt_bool(c, o, "keepOrphans"), (const char *const *)autos, nautos };
    Plan p;
    plan_make(&X, &ST, ip, ni, rp, nr, &po, &p);
    JS_SetPropertyStr(c, r, "ok", JS_NewBool(c, p.ok));
    set_s(c, r, "error", p.ok ? NULL : p.err);
    JSValue items = JS_NewArray(c);
    for (int i = 0; i < p.n; i++) {
        const PlanItem *it = &p.items[i];
        JSValue v = JS_NewObject(c);
        set_s(c, v, "action", it->action == PA_INSTALL ? "install" : it->action == PA_UPGRADE ? "upgrade" : "remove");
        set_s(c, v, "name", it->name);
        set_s(c, v, "version", it->version);
        set_s(c, v, "oldVersion", it->old_version);
        JS_SetPropertyStr(c, v, "size", JS_NewFloat64(c, it->rec >= 0 ? X.pkg[it->rec].size : 0));
        JS_SetPropertyStr(c, v, "requested", JS_NewBool(c, it->requested));
        set_s(c, v, "reason", it->reason);
        JS_SetPropertyUint32(c, items, (uint32_t)i, v);
    }
    JS_SetPropertyStr(c, r, "items", items);
    JSValue held = JS_NewArray(c);
    for (int i = 0; i < p.nheld; i++) JS_SetPropertyUint32(c, held, (uint32_t)i, JS_NewString(c, p.held[i]));
    JS_SetPropertyStr(c, r, "held", held);
    JS_SetPropertyStr(c, r, "download", JS_NewFloat64(c, (double)p.download));
    JS_SetPropertyStr(c, r, "installedDelta", JS_NewFloat64(c, (double)p.isize_delta * 1024));
    plan_free(&p);
    return r;
}

/* Desktop applications (AppStream), by category, sorted by name. */
static int app_lang;
static int app_cmp(const void *a, const void *b) {
    return strcasecmp(app_name(&X.app[*(const uint32_t *)a], app_lang), app_name(&X.app[*(const uint32_t *)b], app_lang));
}

static int has_category(const IdxApp *a, const char *cat) {
    const char *s = idx_s(&X, a->categories);
    size_t n = strlen(cat);
    while (*s) {
        const char *e = strchr(s, ';');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == n && !strncmp(s, cat, n)) return 1;
        s += l + (e ? 1 : 0);
    }
    return 0;
}

static JSValue f_apps(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    char cat[64], lang[16], type[48];
    opt_str(c, o, "category", cat, sizeof cat);
    opt_str(c, o, "lang", lang, sizeof lang);
    opt_str(c, o, "type", type, sizeof type);
    if (!type[0]) snprintf(type, sizeof type, "desktop-application");
    int limit = opt_int(c, o, "limit", 100, 1, 5000), offset = opt_int(c, o, "offset", 0, 0, 1000000);
    refresh();
    JSValue arr = JS_NewArray(c);
    if (!X.map) return arr;
    uint32_t *v = malloc(sizeof *v * (X.h->napp ? X.h->napp : 1));
    uint32_t n = 0;
    for (uint32_t i = 0; v && i < X.h->napp; i++) {
        const IdxApp *a = &X.app[i];
        if (strcmp(idx_s(&X, a->type), type) || (cat[0] && !has_category(a, cat))) continue;
        const char *pn = idx_s(&X, a->package);
        long rec = idx_find(&X, pn, strlen(pn));
        if (rec < 0 || X.pkg[rec].app != i) continue;         /* the package has another main app */
        v[n++] = i;
    }
    app_lang = lang_of(lang);
    if (v) qsort(v, n, sizeof *v, app_cmp);
    uint32_t out = 0;
    for (uint32_t k = (uint32_t)offset; v && k < n && out < (uint32_t)limit; k++) {
        const IdxApp *a = &X.app[v[k]];
        JSValue ao = app_obj(c, a, app_lang);
        const char *pn = idx_s(&X, a->package);
        const StPkg *inst = status_find(&ST, pn, strlen(pn));
        long rec = idx_find(&X, pn, strlen(pn));
        long cand = pkg_candidate(&X, pn, strlen(pn), inst);
        set_s(c, ao, "package", pn);
        set_s(c, ao, "installed", inst ? inst->version : NULL);
        set_s(c, ao, "version", idx_s(&X, X.pkg[cand >= 0 ? cand : rec].version));
        JS_SetPropertyStr(c, ao, "size", JS_NewFloat64(c, X.pkg[cand >= 0 ? cand : rec].size));
        JS_SetPropertyUint32(c, arr, out++, ao);
    }
    free(v);
    return arr;
}

/* Dependencies installed by LordOS that nobody uses anymore (what pkg-autoremove would remove). */
static JSValue f_orphans(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    refresh();
    PlanOpts po = { .orphans = 1, .autos = (const char *const *)autos, .nautos = nautos };
    Plan p;
    plan_make(&X, &ST, NULL, 0, NULL, 0, &po, &p);
    JSValue arr = JS_NewArray(c);
    uint32_t n = 0;
    for (int i = 0; i < p.n; i++) {
        if (!p.items[i].orphan) continue;
        const StPkg *inst = status_find(&ST, p.items[i].name, strlen(p.items[i].name));
        JSValue v = JS_NewObject(c);
        set_s(c, v, "name", p.items[i].name);
        set_s(c, v, "version", p.items[i].version);
        set_s(c, v, "summary", inst ? inst->summary : NULL);
        JS_SetPropertyStr(c, v, "installedSize", JS_NewFloat64(c, inst ? 1024.0 * inst->isize : 0));
        JS_SetPropertyUint32(c, arr, n++, v);
    }
    plan_free(&p);
    return arr;
}

static JSValue f_categories(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    refresh();
    JSValue o = JS_NewObject(c);
    for (uint32_t i = 0; X.map && i < X.h->napp; i++) {
        const IdxApp *a = &X.app[i];
        if (strcmp(idx_s(&X, a->type), "desktop-application")) continue;
        const char *s = idx_s(&X, a->categories);
        while (*s) {
            const char *e = strchr(s, ';');
            size_t l = e ? (size_t)(e - s) : strlen(s);
            char k[64];
            snprintf(k, sizeof k, "%.*s", (int)(l < 63 ? l : 63), s);
            JSValue cur = JS_GetPropertyStr(c, o, k);
            int32_t n = 0;
            if (JS_IsNumber(cur)) JS_ToInt32(c, &n, cur);
            JS_FreeValue(c, cur);
            JS_SetPropertyStr(c, o, k, JS_NewInt32(c, n + 1));
            s += l + (e ? 1 : 0);
        }
    }
    return o;
}

static JSValue f_sources(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    refresh();
    JSValue arr = JS_NewArray(c);
    for (uint32_t i = 0; X.map && i < X.h->nsrc; i++) {
        const IdxSrc *s = &X.src[i];
        JSValue o = JS_NewObject(c);
        set_s(c, o, "uri", idx_s(&X, s->uri));
        set_s(c, o, "suite", idx_s(&X, s->suite));
        set_s(c, o, "component", idx_s(&X, s->component));
        set_s(c, o, "codename", idx_s(&X, s->codename));
        set_s(c, o, "label", idx_s(&X, s->label));
        JS_SetPropertyStr(c, o, "date", JS_NewFloat64(c, (double)s->date));
        JS_SetPropertyStr(c, o, "validUntil", JS_NewFloat64(c, (double)s->valid_until));
        JS_SetPropertyStr(c, o, "priority", JS_NewInt32(c, (int32_t)s->prio));
        JS_SetPropertyStr(c, o, "packages", JS_NewInt32(c, (int32_t)s->npkgs));
        JS_SetPropertyUint32(c, arr, i, o);
    }
    return arr;
}

static JSValue f_last_update(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    refresh();
    return JS_NewFloat64(c, X.map ? (double)X.mtime : 0);      /* the last time it was checked */
}

static JSValue f_available(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    refresh();
    return JS_NewBool(c, X.map != NULL);
}

static JSValue f_icon(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char name[129];
    if (argc < 1 || arg_name(c, argv[0], name, sizeof name)) return JS_NULL;
    refresh();
    long rec = idx_find(&X, name, strlen(name));
    if (rec < 0 || X.pkg[rec].app == UINT32_MAX || X.pkg[rec].app >= X.h->napp) return JS_NULL;
    char path[600];
    const char *icon = idx_s(&X, X.app[X.pkg[rec].app].icon);
    snprintf(path, sizeof path, ICON_DIR "/%s", icon);
    return icon[0] && !access(path, R_OK) ? JS_NewString(c, path) : JS_NULL;
}

/* lord.pkg.releaseNotes(name, version) → Markdown text or null: the release notes a source publishes
 * for that version (ReleaseNotes in its signed Release, stored by pkg-update; docs/UPDATES.md). */
/* lord.pkg.featured(): the Store's banner the last update verified (pkg.c, featured_valid), as
 * { data: <the JSON object>, image: path or null }, or null without one. No arguments, read-only. */
static JSValue f_featured(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    int fd = open(PKG_DIR "/featured.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return JS_NULL;
    struct stat st;
    char *buf = NULL;
    size_t got = 0;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (64 << 10) && (buf = malloc((size_t)st.st_size + 1))) {
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        buf[got] = 0;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size) { free(buf); return JS_NULL; }
    JSValue data = JS_ParseJSON(c, buf, got, "featured.json");
    free(buf);
    if (JS_IsException(data)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    if (!JS_IsObject(data)) { JS_FreeValue(c, data); return JS_NULL; }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "data", data);
    struct stat is;
    int img = !lstat(PKG_DIR "/featured.png", &is) && S_ISREG(is.st_mode);
    JS_SetPropertyStr(c, o, "image", img ? JS_NewString(c, PKG_DIR "/featured.png") : JS_NULL);
    return o;
}

static JSValue f_release_notes(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char name[129], ver[129];
    if (argc < 2 || arg_name(c, argv[0], name, sizeof name)) return JS_NULL;
    const char *v = JS_ToCString(c, argv[1]);
    if (!v) return JS_NULL;
    int ok = strlen(v) < sizeof ver && deb_valid_version(v, strlen(v));
    if (ok) snprintf(ver, sizeof ver, "%s", v);
    JS_FreeCString(c, v);
    if (!ok) return JS_NULL;
    int fd = open(PKG_DIR "/notes", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return JS_NULL;
    struct stat st;
    char *buf = NULL;
    size_t got = 0;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (4 << 20) && (buf = malloc((size_t)st.st_size))) {
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
    }
    close(fd);
    size_t tl = 0;
    char *text = buf ? deb_notes_find(buf, got, name, ver, &tl) : NULL;
    free(buf);
    JSValue r = text ? JS_NewStringLen(c, text, tl) : JS_NULL;
    free(text);
    return r;
}

/* lord.pkg.rebootRequired() → null, or the packages that asked for a restart ([] if none named):
 * the Debian convention /run/reboot-required(.pkgs), written by LordOS's postinst and its kernel hook. */
static JSValue f_reboot_required(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (access("/run/reboot-required", F_OK)) return JS_NULL;
    JSValue arr = JS_NewArray(c);
    FILE *f = fopen("/run/reboot-required.pkgs", "re");
    char line[160];
    uint32_t n = 0;
    while (f && n < 64 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!deb_valid_name(line, strlen(line))) continue;
        int dup = 0;
        for (uint32_t i = 0; i < n && !dup; i++) {
            JSValue e = JS_GetPropertyUint32(c, arr, i);
            const char *es = JS_ToCString(c, e);
            dup = es && !strcmp(es, line);
            JS_FreeCString(c, es);
            JS_FreeValue(c, e);
        }
        if (!dup) JS_SetPropertyUint32(c, arr, n++, JS_NewString(c, line));
    }
    if (f) fclose(f);
    return arr;
}

static const JSCFunctionListEntry pkg_funcs[] = {
    JS_CFUNC_DEF("releaseNotes", 2, f_release_notes),
    JS_CFUNC_DEF("featured", 0, f_featured),
    JS_CFUNC_DEF("rebootRequired", 0, f_reboot_required),
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("lastUpdate", 0, f_last_update),
    JS_CFUNC_DEF("sources", 0, f_sources),
    JS_CFUNC_DEF("search", 2, f_search),
    JS_CFUNC_DEF("info", 2, f_info),
    JS_CFUNC_DEF("installed", 1, f_installed),
    JS_CFUNC_DEF("upgrades", 0, f_upgrades),
    JS_CFUNC_DEF("plan", 1, f_plan),
    JS_CFUNC_DEF("orphans", 0, f_orphans),
    JS_CFUNC_DEF("apps", 1, f_apps),
    JS_CFUNC_DEF("categories", 0, f_categories),
    JS_CFUNC_DEF("icon", 1, f_icon),
};

void js_pkg_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, pkg_funcs, sizeof pkg_funcs / sizeof pkg_funcs[0]);
    JS_SetPropertyStr(c, lord, "pkg", o);
}
