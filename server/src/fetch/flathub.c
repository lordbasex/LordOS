/* flathub.c — the rankings' request path and strict parsing (see flathub.h). */
#include "flathub.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const NAMES[FH_LISTS] = { "popular", "trending", "added", "updated", "day", "week" };
/* the collections' path names on Flathub's side, in the order of the list */
static const char *const COLL[FH_DAY] = { "popular", "trending", "recently-added", "recently-updated" };

const char *fh_list_name(int list) { return list >= 0 && list < FH_LISTS ? NAMES[list] : NULL; }

int fh_list_of(const char *name) {
    for (int i = 0; name && i < FH_LISTS; i++) if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

/* days since 1970-01-01 → y-m-d (Howard Hinnant's civil_from_days) */
int fh_date(long long now, char *out, size_t n) {
    if (now < 0 || now > 253402300799LL) return -1;           /* 1970 … 9999 */
    long long z = now / FH_DAY_SECONDS + 719468;
    long long era = z / 146097, doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    int w = snprintf(out, n, "%04lld-%02lld-%02lld", y, m, d);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int fh_path(int list, long long now, char *out, size_t n) {
    int w;
    if (list >= 0 && list < FH_DAY) w = snprintf(out, n, "/api/v2/collection/%s", COLL[list]);
    else if (list == FH_DAY || list == FH_WEEK) {
        char day[16];
        if (fh_date(now, day, sizeof day)) return -1;
        w = snprintf(out, n, "/api/v2/app-picks/%s/%s", list == FH_DAY ? "app-of-the-day" : "apps-of-the-week", day);
    } else return -1;
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

static int has(const FhIds *o, const char *id) {
    for (int i = 0; i < o->n; i++) if (!strcmp(o->id[i], id)) return 1;
    return 0;
}

/* adds id when it passes fpak_id_ok and isn't there yet; 1 when the list is full */
static int add(FhIds *o, const JVal *v) {
    if (o->n >= FH_IDS_MAX) return 1;
    if (!v || v->type != J_STR || v->len != strlen(v->str) || !fpak_id_ok(v->str) || has(o, v->str)) return 0;
    snprintf(o->id[o->n++], sizeof o->id[0], "%s", v->str);
    return o->n >= FH_IDS_MAX;
}

int fh_parse(int list, const char *json, size_t len, FhIds *out) {
    memset(out, 0, sizeof *out);
    if (list < 0 || list >= FH_LISTS || !json || !len || len > FH_ANSWER_MAX) return -1;
    JDoc doc;
    if (json_parse(json, len, &doc, NULL, 0)) return -1;
    int rc = -1;
    const JVal *root = doc.root;
    if (!root || root->type != J_OBJ) goto out;
    if (list < FH_DAY) {
        const JVal *hits = json_get(root, "hits");
        if (!hits || hits->type != J_ARR) goto out;
        for (const JVal *h = hits->kid; h; h = h->next)
            if (h->type == J_OBJ && add(out, json_get(h, "app_id"))) break;
        rc = 0;
    } else if (list == FH_DAY) {
        const JVal *id = json_get(root, "app_id");
        if (!id || id->type != J_STR) goto out;
        add(out, id);
        rc = 0;
    } else {
        const JVal *apps = json_get(root, "apps");
        if (!apps || apps->type != J_ARR) goto out;
        /* in position order: positions are whole numbers 1..1000; equal ones keep Flathub's order. Only the first 1000
         * entries are looked at (a week has a handful), so a hostile answer costs at most 1000 × 1000 steps. */
        for (int pos = 1, left = 1; left && pos <= 1000; pos++) {
            left = 0;
            int seen = 0;
            for (const JVal *a = apps->kid; a && seen < 1000; a = a->next, seen++) {
                const JVal *p = a->type == J_OBJ ? json_get(a, "position") : NULL;
                if (!p || p->type != J_NUM || !isfinite(p->num) || p->num != floor(p->num) || p->num < pos || p->num > 1000) continue;
                if (p->num == pos) { if (add(out, json_get(a, "app_id"))) break; }
                else left = 1;
            }
            if (out->n >= FH_IDS_MAX) break;
        }
        rc = 0;
    }
out:
    json_free(&doc);
    if (rc) memset(out, 0, sizeof *out);
    return rc;
}

size_t fh_compact(const FhIds *ids, char *buf, size_t n) {
    size_t w = 0;
    int k = snprintf(buf, n, "{\"ids\":[");
    if (k < 0 || (size_t)k >= n) return 0;
    w = (size_t)k;
    for (int i = 0; i < ids->n; i++) {
        if (!fpak_id_ok(ids->id[i])) continue;                 /* only ids: never anything to escape */
        k = snprintf(buf + w, n - w, "%s\"%s\"", i ? "," : "", ids->id[i]);
        if (k < 0 || (size_t)k >= n - w) return 0;
        w += (size_t)k;
    }
    k = snprintf(buf + w, n - w, "]}");
    if (k < 0 || (size_t)k >= n - w) return 0;
    return w + (size_t)k;
}

int fh_ids_from(const JVal *arr, FhIds *out) {
    memset(out, 0, sizeof *out);
    if (!arr || arr->type != J_ARR || arr->len > FH_IDS_MAX) return -1;
    for (const JVal *v = arr->kid; v; v = v->next) add(out, v);
    return 0;
}

int fh_from_compact(const char *json, size_t len, FhIds *out) {
    memset(out, 0, sizeof *out);
    if (!json || !len || len > 64 * 1024) return -1;
    JDoc doc;
    if (json_parse(json, len, &doc, NULL, 0)) return -1;
    const JVal *root = doc.root;
    int rc = root && root->type == J_OBJ && root->len == 1 ? fh_ids_from(json_get(root, "ids"), out) : -1;
    json_free(&doc);
    return rc;
}
