/* batthist_test.c — Settings → Battery's history (src/hw/batthist.c): levels every 10 minutes, the discharge per day
 * and in total (LordOS's cycles), screen time on battery, each app's CPU on battery, the last charge, and a saved
 * file read back strictly. */
#include "../src/hw/batthist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static PowerSupply bat(int p, const char *status) {
    PowerSupply s;
    memset(&s, 0, sizeof s);
    s.battery = 1; s.present = 1; s.percent = p;
    snprintf(s.status, sizeof s.status, "%s", status);
    return s;
}

int main(void) {
    static BattHist h;
    memset(&h, 0, sizeof h);
    long long t = 1763380000;
    BhScope sc[3] = { { "app-lordos-google-chrome.scope", 1000000 }, { "server", 500000 }, { "../evil", 1 } };
    PowerSupply b = bat(80, "Discharging");
    bh_tick(&h, t, 20261117, &b, 0, 1, sc, 3);
    CHECK(h.first_seen == t && h.ns == 1 && h.nd == 1);
    /* 30 s later, seen, 1 % down */
    b = bat(79, "Discharging");
    bh_tick(&h, t + 30, 20261117, &b, 0, 1, NULL, 0);
    CHECK(h.d[0].on_s == 30 && h.d[0].off_s == 0 && h.d[0].drain == 10 && h.drain_total == 10);
    CHECK(h.ns == 1);                                   /* the next level only after 10 minutes */
    /* screen off for 60 s */
    bh_tick(&h, t + 90, 20261117, &b, 0, 0, NULL, 0);
    CHECK(h.d[0].off_s == 60);
    /* a suspend (gap of an hour): no screen time counted */
    b = bat(70, "Discharging");
    bh_tick(&h, t + 3690, 20261117, &b, 0, 1, NULL, 0);
    CHECK(h.d[0].on_s == 30 && h.d[0].drain == 100 && h.ns == 2);
    /* each app's CPU on battery, 10 minutes after the first reading */
    sc[0].usec = 61000000; sc[1].usec = 2500000;
    bh_tick(&h, t + 600 + 3600, 20261117, &b, 0, 1, sc, 3);
    /* the gap since the first app reading is more than 3 readings: no difference taken, a new start */
    CHECK(h.d[0].napps == 0);
    sc[0].usec = 121000000;
    bh_tick(&h, t + 1200 + 3600, 20261117, &b, 0, 1, sc, 3);
    CHECK(h.d[0].napps == 1 && !strcmp(h.d[0].apps[0].scope, "app-lordos-google-chrome.scope") && h.d[0].apps[0].usec == 60000000);
    /* charging, then unplugged at 95 %: the last charge */
    b = bat(90, "Charging");
    bh_tick(&h, t + 5000, 20261117, &b, 1, 1, NULL, 0);
    b = bat(95, "Discharging");
    bh_tick(&h, t + 5030, 20261117, &b, 0, 1, NULL, 0);
    CHECK(h.last_charge_t == t + 5030 && h.last_charge_p == 95);
    CHECK(h.drain_total == 100);                        /* charging never counts as drain */
    /* a new day */
    b = bat(94, "Discharging");
    bh_tick(&h, t + 90000, 20261118, &b, 0, 1, NULL, 0);
    CHECK(h.nd == 2 && h.d[1].drain == 10 && h.drain_total == 110);

    /* saved and read back */
    static char buf[400 * 1024];
    size_t n = bh_save(&h, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf);
    static BattHist g;
    bh_load(&g, buf);
    CHECK(g.first_seen == h.first_seen && g.drain_total == 110 && g.last_charge_p == 95 && g.ns == h.ns && g.nd == 2);
    CHECK(g.d[0].napps == 1 && g.d[0].apps[0].usec == 60000000 && g.d[0].on_s == h.d[0].on_s);
    /* malformed lines are skipped, nothing else */
    bh_load(&g, "v 1\nfirst -5\ns 100 150 0\ns 100 50 9\na 20261117 ../x.scope 5\nd 1 2 3 4\ns 200 50 1\nlast 300 101\ndrain 7 8\n");
    CHECK(g.first_seen == 0 && g.ns == 1 && g.s[0].p == 50 && g.nd == 0 && g.last_charge_t == 0 && g.drain_total == 0);
    /* scope names */
    CHECK(bh_scope_ok("app-lordos-lpk-passwords.scope") && bh_scope_ok("server") && !bh_scope_ok("../x.scope") && !bh_scope_ok("cgroup.procs") && !bh_scope_ok(""));

    /* the JSON */
    static char js[200 * 1024];
    size_t jn = bh_json(&h, t + 90000, js, sizeof js);
    CHECK(jn > 0 && js[0] == '{' && js[jn - 1] == '}');
    CHECK(strstr(js, "\"cycles\":0.1") && strstr(js, "\"ymd\":20261118") && strstr(js, "\"app-lordos-google-chrome.scope\":60000000"));
    /* no battery, or one without a level: nothing changes */
    static BattHist e;
    memset(&e, 0, sizeof e);
    bh_tick(&e, t, 20261117, NULL, 0, 1, NULL, 0);
    b = bat(-1, "Unknown");
    bh_tick(&e, t, 20261117, &b, 0, 1, NULL, 0);
    CHECK(e.first_seen == 0 && e.ns == 0 && e.nd == 0);

    printf("batthist-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
