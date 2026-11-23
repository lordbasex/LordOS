/* flathub_rank_test.c — the Store's rankings from Flathub's API, the pure part (src/fetch/flathub.c, E10): the closed
 * list of paths, the date, and the strict parsing of Flathub's answers into app ids only. The recorded answers are in
 * tests/data/flathub/api-2026-11-22/ (fetched once from flathub.org's public API on 2026-11-22; the collections other
 * than popular cut to 40 hits); the hostile ones are built here. No network.
 *   build/flathub-rank-test (run from server/) */
#define _POSIX_C_SOURCE 200809L
#include "../src/fetch/flathub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

#define DIR "tests/data/flathub/api-2026-11-22/"
#define NOV22 1795355100LL       /* 2026-11-22 13:45:00 UTC */

static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = malloc(4 << 20);
    *n = b ? fread(b, 1, 4 << 20, f) : 0;
    fclose(f);
    return b;
}

static FhIds ids;

/* parses s (a C string) for list */
static int parse(int list, const char *s) { return fh_parse(list, s, strlen(s), &ids); }

static void test_paths(void) {
    char p[128], d[16];
    CHECK(!fh_path(FH_POPULAR, NOV22, p, sizeof p) && !strcmp(p, "/api/v2/collection/popular"));
    CHECK(!fh_path(FH_TRENDING, NOV22, p, sizeof p) && !strcmp(p, "/api/v2/collection/trending"));
    CHECK(!fh_path(FH_ADDED, NOV22, p, sizeof p) && !strcmp(p, "/api/v2/collection/recently-added"));
    CHECK(!fh_path(FH_UPDATED, NOV22, p, sizeof p) && !strcmp(p, "/api/v2/collection/recently-updated"));
    CHECK(!fh_path(FH_DAY, NOV22, p, sizeof p) && !strcmp(p, "/api/v2/app-picks/app-of-the-day/2026-11-22"));
    CHECK(!fh_path(FH_WEEK, NOV22, p, sizeof p) && !strcmp(p, "/api/v2/app-picks/apps-of-the-week/2026-11-22"));
    /* any number outside the closed list is refused */
    int bad[] = { -1, FH_LISTS, FH_LISTS + 1, 100, -2147483647 - 1, 2147483647 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(fh_path(bad[i], NOV22, p, sizeof p) == -1 && !fh_list_name(bad[i]));
    CHECK(fh_path(FH_POPULAR, NOV22, p, 10) == -1);                   /* doesn't fit */
    CHECK(fh_path(FH_DAY, -5, p, sizeof p) == -1);                    /* a clock before 1970 */
    for (int l = 0; l < FH_LISTS; l++) CHECK(fh_list_of(fh_list_name(l)) == l);
    CHECK(fh_list_of("verified") == -1 && fh_list_of("") == -1 && fh_list_of(NULL) == -1);
    /* the date: UTC, by arithmetic */
    CHECK(!fh_date(0, d, sizeof d) && !strcmp(d, "1970-01-01"));
    CHECK(!fh_date(951782400, d, sizeof d) && !strcmp(d, "2000-02-29"));
    CHECK(!fh_date(1735689599, d, sizeof d) && !strcmp(d, "2024-12-31"));
    CHECK(!fh_date(1735689600, d, sizeof d) && !strcmp(d, "2025-01-01"));
    CHECK(!fh_date(NOV22 - 13 * 3600 - 45 * 60, d, sizeof d) && !strcmp(d, "2026-11-22"));
    CHECK(!fh_date(NOV22 + 10 * 3600 + 14 * 60 + 59, d, sizeof d) && !strcmp(d, "2026-11-22"));
    CHECK(!fh_date(NOV22 + 10 * 3600 + 15 * 60, d, sizeof d) && !strcmp(d, "2026-11-23"));
    CHECK(fh_date(253402300800LL, d, sizeof d) == -1);
}

static void test_recorded(void) {
    size_t n;
    char *b;
    b = slurp(DIR "popular.json", &n);
    CHECK(b && n > 400000);
    CHECK(b && !fh_parse(FH_POPULAR, b, n, &ids));
    CHECK(ids.n == 250 && !strcmp(ids.id[0], "org.vinegarhq.Sober"));
    printf("flathub-rank-test: popular: %d ids, first %s, last %s\n", ids.n, ids.id[0], ids.id[ids.n - 1]);
    CHECK(fh_parse(FH_DAY, b, n, &ids) == -1 && ids.n == 0);         /* a collection isn't the app of the day */
    CHECK(fh_parse(FH_WEEK, b, n, &ids) == -1);
    free(b);
    const char *colls[] = { DIR "trending.json", DIR "recently-added.json", DIR "recently-updated.json" };
    const char *first[] = { "io.github.Micro856.KittiesGtk", "app.rufin.rufin", "io.github.mrvladus.List" };
    for (int i = 0; i < 3; i++) {
        b = slurp(colls[i], &n);
        CHECK(b && !fh_parse(FH_TRENDING + i, b, n, &ids) && ids.n >= 38 && ids.n <= 40 && !strcmp(ids.id[0], first[i]));
        free(b);
    }
    b = slurp(DIR "app-of-the-day.json", &n);
    CHECK(b && !fh_parse(FH_DAY, b, n, &ids) && ids.n == 1 && !strcmp(ids.id[0], "nl.andreasknoben.Laser"));
    CHECK(b && fh_parse(FH_POPULAR, b, n, &ids) == -1);
    free(b);
    b = slurp(DIR "apps-of-the-week.json", &n);
    CHECK(b && !fh_parse(FH_WEEK, b, n, &ids) && ids.n == 5);
    CHECK(!strcmp(ids.id[0], "io.github.sitraorg.sitra") && !strcmp(ids.id[2], "com.endlessnetwork.fablemaker") &&
          !strcmp(ids.id[4], "io.gitlab.news_flash.NewsFlash"));
    free(b);
}

static void test_hostile(void) {
    /* not JSON, or not strict JSON */
    CHECK(parse(FH_POPULAR, "") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":[]") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":[]} x") == -1);                              /* trailing garbage */
    CHECK(parse(FH_POPULAR, "{\"hits\":[],\"hits\":[]}") == -1);                    /* a key twice */
    CHECK(parse(FH_POPULAR, "{\"hits\":[{\"app_id\":\"org.a.B\",\"app_id\":\"org.c.D\"}]}") == -1);
    CHECK(parse(FH_POPULAR, "{'hits':[]}") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":[{\"app_id\":\"org.a.\\u0000B\"}]}") == -1);
    /* wrong shapes */
    CHECK(parse(FH_POPULAR, "[]") == -1);
    CHECK(parse(FH_POPULAR, "{}") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":{}}") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":\"org.a.B\"}") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":null}") == -1);
    CHECK(parse(FH_DAY, "{\"app_id\":7}") == -1);
    CHECK(parse(FH_DAY, "{\"apps\":[]}") == -1);
    CHECK(parse(FH_WEEK, "{\"apps\":{\"app_id\":\"org.a.B\"}}") == -1);
    CHECK(parse(FH_WEEK, "{\"app_id\":\"org.a.B\"}") == -1);
    CHECK(parse(FH_POPULAR, "{\"hits\":[]}") == 0 && ids.n == 0);
    /* members of the wrong type are skipped one by one; bad ids and duplicates are dropped; order is kept */
    CHECK(parse(FH_POPULAR, "{\"hits\":[1,\"org.x.Y\",null,[],{\"app_id\":5},{\"app_id\":[\"org.a.B\"]},{\"app_id\":{\"x\":1}},"
                            "{\"app_id\":\"org.good.One\"},{\"name\":\"org.good.NoId\"},{\"app_id\":\"org.good.One\"},"
                            "{\"app_id\":\"org.ex ample.App\"},{\"app_id\":\"../../etc/passwd\"},{\"app_id\":\"org.example.App\\n\"},"
                            "{\"app_id\":\"a.b\"},{\"app_id\":\"1org.x.y\"},{\"app_id\":\"org.ex-ample.App\"},{\"app_id\":\"org.ex\\u00e4mple.App\"},"
                            "{\"app_id\":\"org.x.Y/../Z\"},{\"app_id\":\"org.x.\\\"Y\"},{\"app_id\":\"\"},{\"app_id\":\"org..App\"},"
                            "{\"app_id\":\"org.x.Y;rm\"},{\"app_id\":\"org.good.Two-beta\"},{\"app_id\":\"org.good.Three\"}]}") == 0);
    CHECK(ids.n == 3 && !strcmp(ids.id[0], "org.good.One") && !strcmp(ids.id[1], "org.good.Two-beta") && !strcmp(ids.id[2], "org.good.Three"));
    /* an id longer than FPAK_ID_MAX */
    char big[512], fit[64], over[64];
    memset(fit, 'a', sizeof fit);
    memset(over, 'a', sizeof over);
    fit[FPAK_ID_MAX - 12] = 0;                                   /* "org.example." + 47 = 59 bytes */
    over[FPAK_ID_MAX - 11] = 0;                                  /* 60 */
    snprintf(big, sizeof big, "{\"hits\":[{\"app_id\":\"org.example.%s\"},{\"app_id\":\"org.example.%s\"}]}", over, fit);
    CHECK(parse(FH_POPULAR, big) == 0 && ids.n == 1 && strlen(ids.id[0]) == FPAK_ID_MAX);
    /* deep: more than 32 levels is refused */
    char deep[256] = "{\"hits\":[";
    for (int i = 0; i < 40; i++) strcat(deep, "[");
    for (int i = 0; i < 40; i++) strcat(deep, "]");
    strcat(deep, "]}");
    CHECK(parse(FH_POPULAR, deep) == -1);
    /* more than 250 hits: the first 250 are kept, in order; with duplicates, they don't count */
    size_t cap = 2 << 20, w = 0;
    char *s = malloc(cap);
    w += (size_t)snprintf(s + w, cap - w, "{\"hits\":[");
    for (int i = 0; i < 400; i++) w += (size_t)snprintf(s + w, cap - w, "%s{\"app_id\":\"org.app.N%d\"},{\"app_id\":\"org.app.N%d\"}", i ? "," : "", i, i);
    w += (size_t)snprintf(s + w, cap - w, "]}");
    CHECK(fh_parse(FH_POPULAR, s, w, &ids) == 0 && ids.n == FH_IDS_MAX && !strcmp(ids.id[0], "org.app.N0") && !strcmp(ids.id[249], "org.app.N249"));
    /* huge: more than FH_ANSWER_MAX bytes is refused even when it is good JSON */
    w = 0;
    w += (size_t)snprintf(s + w, cap - w, "{\"hits\":[{\"app_id\":\"org.a.B\",\"pad\":\"");
    while (w < FH_ANSWER_MAX + 10) s[w++] = 'x';
    w += (size_t)snprintf(s + w, cap - w, "\"}]}");
    CHECK(fh_parse(FH_POPULAR, s, w, &ids) == -1 && ids.n == 0);
    free(s);
    /* the app of the day: one id, or none when it's bad */
    CHECK(parse(FH_DAY, "{\"app_id\":\"org.a.B\",\"day\":\"2026-11-22\"}") == 0 && ids.n == 1 && !strcmp(ids.id[0], "org.a.B"));
    CHECK(parse(FH_DAY, "{\"app_id\":\"org.a.B; rm -rf /\"}") == 0 && ids.n == 0);
    /* apps of the week: position order; a position that isn't a whole number from 1 to 1000 is skipped */
    CHECK(parse(FH_WEEK, "{\"apps\":[{\"app_id\":\"org.c.C\",\"position\":3},{\"app_id\":\"org.a.A\",\"position\":1},"
                         "{\"app_id\":\"org.x.Half\",\"position\":1.5},{\"app_id\":\"org.x.Neg\",\"position\":-1},"
                         "{\"app_id\":\"org.x.Str\",\"position\":\"2\"},{\"app_id\":\"org.x.Big\",\"position\":1e9},"
                         "{\"app_id\":\"org.x.None\"},{\"app_id\":\"org.b.B\",\"position\":2},{\"app_id\":\"org.a.A\",\"position\":4},"
                         "{\"app_id\":\"bad id\",\"position\":5},7,{\"app_id\":\"org.d.D\",\"position\":1000}]}") == 0);
    CHECK(ids.n == 4 && !strcmp(ids.id[0], "org.a.A") && !strcmp(ids.id[1], "org.b.B") && !strcmp(ids.id[2], "org.c.C") && !strcmp(ids.id[3], "org.d.D"));
    /* the compact answer, both ways */
    char out[FH_IDS_MAX * (FPAK_ID_MAX + 3) + 32];
    size_t n = fh_compact(&ids, out, sizeof out);
    CHECK(n && !strcmp(out, "{\"ids\":[\"org.a.A\",\"org.b.B\",\"org.c.C\",\"org.d.D\"]}"));
    CHECK(fh_compact(&ids, out, 12) == 0);
    FhIds back;
    CHECK(!fh_from_compact("{\"ids\":[\"org.a.A\",\"org.b.B\"]}", strlen("{\"ids\":[\"org.a.A\",\"org.b.B\"]}"), &back) && back.n == 2);
    CHECK(!fh_from_compact("{\"ids\":[\"org.a.A\",\"x\",\"org.a.A\",3]}", strlen("{\"ids\":[\"org.a.A\",\"x\",\"org.a.A\",3]}"), &back) && back.n == 1);
    CHECK(fh_from_compact("{\"ids\":[],\"name\":\"x\"}", strlen("{\"ids\":[],\"name\":\"x\"}"), &back) == -1);          /* nothing but ids */
    CHECK(fh_from_compact("{\"ids\":\"org.a.A\"}", strlen("{\"ids\":\"org.a.A\"}"), &back) == -1);
    CHECK(fh_from_compact("[]", strlen("[]"), &back) == -1);
    s = malloc(cap);
    w = 0;
    w += (size_t)snprintf(s + w, cap - w, "{\"ids\":[");
    for (int i = 0; i < 251; i++) w += (size_t)snprintf(s + w, cap - w, "%s\"org.app.N%d\"", i ? "," : "", i);
    w += (size_t)snprintf(s + w, cap - w, "]}");
    CHECK(fh_from_compact(s, w, &back) == -1);                                       /* more than 250 */
    free(s);
}

int main(void) {
    test_paths();
    test_recorded();
    test_hostile();
    printf("flathub-rank-test: %d/%d ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
