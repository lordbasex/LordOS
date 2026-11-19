/* wxsvc_test.c — the weather widget's forecast table (src/fetch/wxsvc.c), no network: each weather widget has its own
 * city since 0.8.9.1 (#193), so the table of places must not fill for good. Two widgets on one city share one place;
 * when the table is full, the place asked least recently gives up its slot. lord-fetch is missing here on purpose:
 * every fetch fails at once ("helper"), before any socket is made. */
#define _POSIX_C_SOURCE 200809L
#include "../src/fetch/wxsvc.h"
#include "../src/sec/netconn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* never reached: the helper is missing, so no fetch gets as far as a socket */
int netconn_socket(NcComponent comp, int fam, int type, const struct sockaddr *to, socklen_t tolen) {
    (void)comp; (void)fam; (void)type; (void)to; (void)tolen;
    abort();
}

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

/* zone has a place in the table */
static int known(const char *zone) {
    WxData d;
    long long at;
    char err[24];
    return wxs_get(zone, &d, &at, err, sizeof err) != -1;
}
/* waits until the worker has tried zone (a failure recorded) */
static int tried(const char *zone) {
    for (int i = 0; i < 300; i++) {
        WxData d;
        long long at;
        char err[24] = "";
        if (wxs_get(zone, &d, &at, err, sizeof err) == 0 && err[0]) return 1;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int main(void) {
    char path[] = "/tmp/wxsvc-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    FILE *f = fdopen(fd, "w");
    fprintf(f, "# test list\n");
    for (int i = 1; i <= WXS_ZONES + 4; i++) fprintf(f, "%d\tCity %d\t\tFR\t93\t43.%02d\t7.%02d\n", 1000 + i, i, i, i);
    fclose(f);
    char dir[] = "/tmp/wxsvc-bin-XXXXXX";                   /* no lord-fetch in it */
    if (!mkdtemp(dir)) return 1;
    wxs_set_cities(path);
    CHECK(wxs_init(dir) == 0);

    char z[64];
    CHECK(wxs_request("geonames/999") == -1);              /* not in the list */
    CHECK(wxs_request("geonames/12x") == -1);
    CHECK(!known("geonames/1001"));                       /* never asked */
    /* fill the table, one place per city; asking again for one already there takes no slot */
    for (int i = 1; i <= WXS_ZONES; i++) {
        snprintf(z, sizeof z, "geonames/%d", 1000 + i);
        CHECK(wxs_request(z) == 0);
        CHECK(tried(z));
    }
    CHECK(wxs_request("geonames/1001") == 0);              /* two widgets on one city: the same place */
    /* the table is full; 1001 was just asked, so 1002 is the least recent and makes room for a new city */
    snprintf(z, sizeof z, "geonames/%d", 1000 + WXS_ZONES + 1);
    CHECK(wxs_request(z) == 0);
    CHECK(tried(z));
    CHECK(known("geonames/1001"));                        /* asked again just before: still there */
    int gone = 0;
    for (int i = 2; i <= WXS_ZONES; i++) {
        snprintf(z, sizeof z, "geonames/%d", 1000 + i);
        if (!known(z)) gone++;
    }
    CHECK(gone == 1);
    CHECK(!known("geonames/1002"));                       /* the least recent went */
    /* the place that went can be asked again (it takes another slot) */
    CHECK(wxs_request("geonames/1002") == 0);
    CHECK(tried("geonames/1002"));

    unlink(path);
    rmdir(dir);
    printf("wxsvc-test: %d/%d ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
