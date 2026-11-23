/* fhsvc_test.c — the Store's rankings in lord-server (src/fetch/fhsvc.c, E10), no network: each list at most once a
 * UTC day, a failure waits (15 min, doubled in a row, at most a day), the cache in ~/.cache/lordos/flathub read back
 * with every id checked again (a file with bad ids, too many ids, a link or garbage). lord-fetch is missing here on
 * purpose: every fetch fails at once, before any socket is made. */
#include "../src/fetch/fhsvc.h"
#include "../src/sec/netconn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* never reached: the helper is missing, so no fetch gets as far as a socket */
int netconn_socket(NcComponent comp, int fam, int type, const struct sockaddr *to, socklen_t tolen) {
    (void)comp; (void)fam; (void)type; (void)to; (void)tolen;
    abort();
}

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

#define DAY 86400LL
#define NOV22 1795355100LL       /* 2026-11-22 13:45:00 UTC */
static long long now_v = NOV22;
static long long fake_now(void) { return now_v; }

static char cache[512];

static void put(const char *name, const char *text) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s.json", cache, name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static char *get_file(const char *name) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s.json", cache, name);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    static char b[65536];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static void wait_idle(void) {
    for (int i = 0; i < 500 && fhs_busy(); i++) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK(!fhs_busy());
}

static void test_due(void) {
    CHECK(fhs_due(NOV22, 0, 0, 0));                              /* never fetched */
    CHECK(!fhs_due(NOV22, NOV22 - 3600, NOV22 - 3600, 0));       /* fetched today */
    CHECK(!fhs_due(NOV22, NOV22 - 13 * 3600, 0, 0));             /* today, just after midnight UTC */
    CHECK(fhs_due(NOV22, NOV22 - 14 * 3600, 0, 0));              /* yesterday, though less than a day ago */
    CHECK(fhs_due(NOV22, NOV22 + DAY, 0, 0));                    /* in the future: the clock went back */
    CHECK(!fhs_due(NOV22, 0, NOV22 - 899, 1));                   /* a failure waits 15 minutes */
    CHECK(fhs_due(NOV22, 0, NOV22 - 900, 1));
    CHECK(!fhs_due(NOV22, 0, NOV22 - 1799, 2));                  /* doubled */
    CHECK(fhs_due(NOV22, 0, NOV22 - 1800, 2));
    CHECK(!fhs_due(NOV22, 0, NOV22 - 7 * 3600, 6));              /* 8 h after six */
    CHECK(!fhs_due(NOV22, 0, NOV22 - DAY + 1, 30));              /* never more than a day */
    CHECK(fhs_due(NOV22, 0, NOV22 - DAY, 30));
    CHECK(fhs_due(NOV22, 0, NOV22 + 3 * DAY, 4));                /* a try in the future doesn't block for ever */
    CHECK(!fhs_due(NOV22, NOV22 - 60, NOV22 - 60, 3));           /* a good copy today wins */
}

int main(void) {
    test_due();
    char home[] = "/tmp/fhsvc-home-XXXXXX", bin[] = "/tmp/fhsvc-bin-XXXXXX";      /* no lord-fetch in bin */
    if (!mkdtemp(home) || !mkdtemp(bin)) return 1;
    snprintf(cache, sizeof cache, "%s/.cache", home);
    mkdir(cache, 0700);
    snprintf(cache, sizeof cache, "%s/.cache/lordos", home);
    mkdir(cache, 0700);
    snprintf(cache, sizeof cache, "%s/" FHS_DIR, home);
    mkdir(cache, 0700);
    char line[2048];
    /* popular: fetched today; trending: yesterday; added: bad ids among good ones; updated: more than 250; day: garbage;
     * week: a link to another file */
    snprintf(line, sizeof line, "{\"at\":%lld,\"tried\":%lld,\"fails\":0,\"ids\":[\"org.a.One\",\"org.b.Two\"]}", NOV22 - 3600, NOV22 - 3600);
    put("popular", line);
    snprintf(line, sizeof line, "{\"at\":%lld,\"tried\":%lld,\"fails\":0,\"ids\":[\"org.c.Three\"]}", NOV22 - DAY, NOV22 - DAY);
    put("trending", line);
    snprintf(line, sizeof line, "{\"at\":%lld,\"tried\":0,\"fails\":0,\"ids\":[\"org.d.Four\",\"../../x\",\"org.d.Four\",7,\"org.e.Five\",\"a b.c.d\"]}", NOV22 - DAY);
    put("added", line);
    {
        static char big[32768];
        size_t w = (size_t)snprintf(big, sizeof big, "{\"at\":%lld,\"tried\":0,\"fails\":0,\"ids\":[", NOV22 - 60);
        for (int i = 0; i < 251; i++) w += (size_t)snprintf(big + w, sizeof big - w, "%s\"org.app.N%d\"", i ? "," : "", i);
        snprintf(big + w, sizeof big - w, "]}");
        put("updated", big);
    }
    put("day", "{\"at\":1,\"ids\":[\"org.x.Y\"],\"at\":2}");
    snprintf(line, sizeof line, "%s/elsewhere.json", home);
    {
        FILE *f = fopen(line, "w");
        if (f) { fprintf(f, "{\"at\":%lld,\"tried\":0,\"fails\":0,\"ids\":[\"org.evil.Link\"]}", NOV22 - 60); fclose(f); }
        char lp[600];
        snprintf(lp, sizeof lp, "%s/week.json", cache);
        CHECK(!symlink(line, lp));
    }
    fhs_set_clock(fake_now);
    CHECK(fhs_request() == -1);                                   /* not initialized */
    CHECK(fhs_init(bin, home) == 0);

    FhIds ids;
    long long at = 0;
    CHECK(fhs_get(FH_POPULAR, &ids, &at) == 1 && ids.n == 2 && !strcmp(ids.id[1], "org.b.Two") && at == NOV22 - 3600);
    CHECK(fhs_get(FH_TRENDING, &ids, &at) == 1 && ids.n == 1);    /* yesterday's copy is still shown */
    CHECK(fhs_get(FH_ADDED, &ids, &at) == 1 && ids.n == 2 && !strcmp(ids.id[0], "org.d.Four") && !strcmp(ids.id[1], "org.e.Five"));
    CHECK(fhs_get(FH_UPDATED, &ids, &at) == 0);                   /* more than 250: refused */
    CHECK(fhs_get(FH_DAY, &ids, &at) == 0);                       /* not strict JSON */
    CHECK(fhs_get(FH_WEEK, &ids, &at) == 0);                      /* a link is never followed */
    CHECK(fhs_get(-1, &ids, &at) == 0 && fhs_get(FH_LISTS, &ids, &at) == 0);

    /* the lists that are due are fetched (and fail here); popular, fetched today, isn't */
    CHECK(fhs_request() == 1);
    wait_idle();
    char *f = get_file("popular");
    CHECK(f && strstr(f, "\"fails\":0") && strstr(f, "org.b.Two"));
    f = get_file("trending");
    CHECK(f && strstr(f, "\"fails\":1") && strstr(f, "org.c.Three"));        /* the failure is kept, so is the copy */
    CHECK(fhs_get(FH_TRENDING, &ids, &at) == 1 && ids.n == 1);
    f = get_file("added");
    CHECK(f && strstr(f, "\"fails\":1") && !strstr(f, "../") && strstr(f, "\"org.d.Four\",\"org.e.Five\""));
    f = get_file("updated");
    CHECK(f && strstr(f, "\"fails\":1") && strstr(f, "\"ids\":[]"));
    struct stat st;
    char lp[600];
    snprintf(lp, sizeof lp, "%s/week.json", cache);
    CHECK(!lstat(lp, &st) && S_ISREG(st.st_mode));                           /* replaced, the link's target untouched */
    f = NULL;
    {
        FILE *e = fopen(line, "r");
        char b[256] = "";
        if (e) { size_t n = fread(b, 1, sizeof b - 1, e); b[n] = 0; fclose(e); }
        CHECK(strstr(b, "org.evil.Link") && !strstr(b, "fails\":1"));
    }
    /* right after: everything waits */
    CHECK(fhs_request() == 0);
    now_v += 899;
    CHECK(fhs_request() == 0);
    now_v += 1;                                                   /* 15 minutes: a second try */
    CHECK(fhs_request() == 1);
    wait_idle();
    CHECK(strstr(get_file("trending"), "\"fails\":2"));
    now_v += 1799;
    CHECK(fhs_request() == 0);                                    /* now 30 minutes */
    /* the next UTC day: popular is due too */
    now_v = NOV22 + 11 * 3600;
    CHECK(fhs_request() == 1);
    wait_idle();
    f = get_file("popular");
    CHECK(f && strstr(f, "\"fails\":1") && strstr(f, "org.b.Two"));
    CHECK(fhs_get(FH_POPULAR, &ids, &at) == 1 && ids.n == 2);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", home, bin);
    if (system(cmd)) { /* best effort */ }
    printf("fhsvc-test: %d/%d ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
