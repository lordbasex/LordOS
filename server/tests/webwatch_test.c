/* webwatch_test.c — when silence means a web app hangs (src/web/webwatch.h), on a made-up clock: the first ping a period
 * after the start, an answer clears the misses, two periodic misses in a row are a hang but one isn't, an urgent ping
 * (the person asked for the app) is a hang after one short wait, nothing is decided in the start's grace, a ping already
 * in flight becomes urgent, "nobody owns the name" counts at once after the grace and not during it, a late answer is
 * ignored, the watcher's own stall restarts the wait, and webwatch_next says when the next step matters. Both limit
 * sets (lord-server's of a process, lord-web's of a page).
 *
 *   make -C server build/webwatch-test        (also in `make test`) */
#include "../src/web/webwatch.h"

#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* runs the watch from t to until in 100 ms steps, answering every ping at once when answer is set; the first verdict
 * that isn't WAIT or PING (HUNG), or WAIT; *t ends where it stopped */
static int run(WebWatch *w, double *t, double until, int answer, int *pings) {
    static uint32_t serial = 100;
    for (; *t <= until; *t += 100) {
        int r = webwatch_step(w, *t);
        if (r == WEBWATCH_HUNG) return r;
        if (r == WEBWATCH_PING) {
            webwatch_sent(w, ++serial, *t);
            if (pings) (*pings)++;
            if (answer) CHECK(webwatch_answer(w, serial, *t + 50), "the answer to %u", serial);
        }
    }
    return WEBWATCH_WAIT;
}

static void test_limits(const char *name, WebWatchLimits L) {
    WebWatch w;
    double t = 1000;
    webwatch_init(&w, L, t);
    CHECK(webwatch_step(&w, t) == WEBWATCH_WAIT, "%s: no ping at the start", name);
    CHECK(webwatch_next(&w, t) == L.period, "%s: the first ping a period later: %g", name, webwatch_next(&w, t));

    /* a healthy one: pings every period, never hung */
    int pings = 0;
    CHECK(run(&w, &t, 1000 + 10 * L.period, 1, &pings) == WEBWATCH_WAIT, "%s: answered: never hung", name);
    CHECK(pings >= 9 && pings <= 11, "%s: a ping every period: %d", name, pings);

    /* silent from now on, past the grace: hung after `misses` periodic pings */
    double t0 = t;
    int r = run(&w, &t, t0 + 10 * L.period, 0, NULL);
    CHECK(r == WEBWATCH_HUNG, "%s: silent: hung", name);
    double took = t - t0;
    double most = L.misses * L.period + L.wait + 1000, least = (L.misses - 1) * L.period + L.wait - 1000;
    CHECK(took >= least && took <= most, "%s: after %g ms (between %g and %g)", name, took, least, most);
    /* the verdict starts a new watch: grace again, no second verdict at once */
    CHECK(run(&w, &t, t + L.grace - 500, 0, NULL) == WEBWATCH_WAIT, "%s: a restarted one gets its grace", name);

    /* one miss, then an answer: not hung, and the misses start over */
    webwatch_init(&w, L, 0);
    t = L.grace + 1;
    w.last_step = t;
    w.last_ping = t - L.period;
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "%s: due", name);
    webwatch_sent(&w, 7, t);
    int any_hung = 0;
    for (double end = t + L.wait + 1; t < end; ) { t += 100; any_hung |= webwatch_step(&w, t) == WEBWATCH_HUNG; }
    CHECK(!any_hung && w.misses == 1, "%s: one miss is not a hang", name);
    CHECK(!webwatch_answer(&w, 7, t), "%s: the late answer to the missed ping is ignored", name);
    pings = 0;
    while (webwatch_step(&w, t) != WEBWATCH_PING) { t += 100; w.last_step = t; }
    webwatch_sent(&w, 8, t);
    CHECK(webwatch_answer(&w, 8, t + 10) && w.misses == 0, "%s: an answer clears the misses", name);
    CHECK(!webwatch_answer(&w, 8, t + 20), "%s: answered once", name);

    /* urgent (the person asked for the app): a ping at once, and one short wait is enough */
    webwatch_urgent(&w);
    CHECK(webwatch_next(&w, t) == 0 && webwatch_step(&w, t) == WEBWATCH_PING, "%s: urgent: a ping now", name);
    webwatch_sent(&w, 9, t);
    CHECK(webwatch_next(&w, t) == L.urgent, "%s: the urgent wait: %g", name, webwatch_next(&w, t));
    double t1 = t;
    r = WEBWATCH_WAIT;
    for (; r != WEBWATCH_HUNG && t < t1 + L.period; t += 100) r = webwatch_step(&w, t);
    CHECK(r == WEBWATCH_HUNG && t - t1 >= L.urgent && t - t1 <= L.urgent + 200, "%s: hung after the urgent wait: %g", name, t - t1);

    /* urgent, answered: fine */
    webwatch_init(&w, L, 0);
    t = L.grace + 1;
    w.last_step = t;
    webwatch_urgent(&w);
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "%s: urgent ping", name);
    webwatch_sent(&w, 11, t);
    CHECK(webwatch_answer(&w, 11, t + L.urgent - 1), "%s: answered in time", name);
    CHECK(run(&w, &t, t + 3 * L.period, 1, NULL) == WEBWATCH_WAIT, "%s: and fine after", name);

    /* a periodic ping in flight becomes urgent: its wait counts from when it went */
    webwatch_init(&w, L, 0);
    t = L.grace + 1;
    w.last_step = t;
    w.last_ping = t - L.period;
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "%s: due again", name);
    webwatch_sent(&w, 12, t);
    t += 1000;
    w.last_step = t;
    webwatch_urgent(&w);
    t1 = t;
    r = WEBWATCH_WAIT;
    for (; r != WEBWATCH_HUNG && t < t1 + L.period; t += 100) r = webwatch_step(&w, t);
    CHECK(r == WEBWATCH_HUNG && t - t1 <= L.urgent, "%s: the ping in flight became urgent: %g", name, t - t1);

    /* the grace: silence during it decides nothing, even urgent */
    webwatch_init(&w, L, 0);
    t = 0;
    webwatch_urgent(&w);
    CHECK(run(&w, &t, L.grace - 1000, 0, NULL) == WEBWATCH_WAIT, "%s: nothing decided in the grace", name);
}

static void test_absent(void) {
    WebWatchLimits L = WEBWATCH_PROCESS;
    WebWatch w;
    /* not on the bus yet, during the grace: nothing */
    webwatch_init(&w, L, 0);
    webwatch_urgent(&w);
    CHECK(webwatch_step(&w, 100) == WEBWATCH_PING, "a ping");
    webwatch_sent(&w, 5, 100);
    CHECK(webwatch_absent(&w, 5, 150) && w.sent == 0 && w.misses == 0, "absent in the grace: forgotten");
    CHECK(!webwatch_absent(&w, 5, 160), "once");
    /* after it: a miss at once, and an urgent one is a hang at once */
    double t = L.grace + 10;
    w.last_step = t;
    webwatch_urgent(&w);
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "urgent after the grace");
    webwatch_sent(&w, 6, t);
    CHECK(!webwatch_absent(&w, 99, t) && webwatch_absent(&w, 6, t + 5), "absent: only its own serial");
    CHECK(webwatch_next(&w, t + 5) == 0, "decided at the next step");
    CHECK(webwatch_step(&w, t + 10) == WEBWATCH_HUNG, "absent after the grace, urgent: hung at once");
    /* periodic and absent: a miss, the second one a hang */
    webwatch_init(&w, L, 0);
    t = L.grace + 10;
    w.last_step = t;
    w.last_ping = t - L.period;
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "periodic");
    webwatch_sent(&w, 20, t);
    webwatch_absent(&w, 20, t);
    CHECK(webwatch_step(&w, t + 100) == WEBWATCH_WAIT && w.misses == 1, "one miss");
    t += L.period + 100;
    w.last_step = t - 100;
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "the next one");
    webwatch_sent(&w, 21, t);
    webwatch_absent(&w, 21, t);
    CHECK(webwatch_step(&w, t + 100) == WEBWATCH_HUNG, "two: hung");
}

static void test_stall(void) {
    WebWatchLimits L = WEBWATCH_PROCESS;
    WebWatch w;
    webwatch_init(&w, L, 0);
    double t = L.grace + 10;
    w.last_step = t;
    webwatch_urgent(&w);
    CHECK(webwatch_step(&w, t) == WEBWATCH_PING, "a ping");
    webwatch_sent(&w, 30, t);
    /* the watcher itself didn't run for a minute (lord-server busy, the computer asleep): not their silence */
    t += 60000;
    CHECK(webwatch_step(&w, t) == WEBWATCH_WAIT && w.sent > 0, "after a stall the wait starts again");
    CHECK(webwatch_answer(&w, 30, t + 100), "and the answer that was waiting still counts");
    /* a stall inside the grace extends it */
    webwatch_init(&w, L, 0);
    CHECK(webwatch_step(&w, 10000) == WEBWATCH_WAIT, "grace, after a stall");
    CHECK(w.started >= 10000 - WEBWATCH_STALL_MS - 1, "the grace moved: %g", w.started);
}

int main(void) {
    test_limits("process", WEBWATCH_PROCESS);
    test_limits("page", WEBWATCH_PAGE);
    test_absent();
    test_stall();
    printf("webwatch-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
