/* geocities_test.c — the weather widget's offline city list (src/fetch/geocities.c): search by folded name (starts
 * with first, then contains, largest first), lookup by number, and malformed lines and queries refused. */
#include "../src/fetch/geocities.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

int main(void) {
    char path[] = "/tmp/geocities-XXXXXX";
    int fd = mkstemp(path);
    const char *tsv =
        "# GeoNames cities15000 — header\n"
        "3435910\tBuenos Aires\t\tAR\t07\t-34.61\t-58.38\n"
        "3432043\tLa Plata\t\tAR\t01\t-34.92\t-57.95\n"
        "3433955\tCiudad de La Plata bad\t\tAR\t01\t-34.92\n"                 /* a field missing: skipped */
        "3678097\tLa Plata\t\tCO\t16\t2.39\t-75.89\n"
        "2988507\tParís \"x\"\tParis \"x\"\tFR\t11\t48.85\t2.35\n"
        "3108008\tTolosa\t\tES\t59\t43.13\t-2.08\n"
        "9999999\tSão Paulo\tSao Paulo\tBR\t27\t-23.55\t-46.64\n";
    if (write(fd, tsv, strlen(tsv)) != (ssize_t)strlen(tsv)) return 1;
    close(fd);

    double lat, lon;
    char name[64];
    CHECK(geo_lookup(path, 3432043, &lat, &lon, name, sizeof name) == 0 && fabs(lat + 34.92) < 1e-9 && !strcmp(name, "La Plata"));
    CHECK(geo_lookup(path, 3433955, &lat, &lon, NULL, 0) == -1);
    CHECK(geo_lookup(path, 1, &lat, &lon, NULL, 0) == -1);

    char out[4096];
    geo_search_json(path, "la plata", 20, out, sizeof out);
    CHECK(strstr(out, "3432043") && strstr(out, "3678097") && !strstr(out, "3433955"));
    CHECK(strstr(out, "3432043") < strstr(out, "3678097"));                    /* in the list's order */
    geo_search_json(path, "plata", 20, out, sizeof out);
    CHECK(strstr(out, "3432043") != NULL);                                     /* contains */
    geo_search_json(path, "sao", 20, out, sizeof out);
    CHECK(strstr(out, "\"São Paulo\"") != NULL);                               /* by its ASCII name, shown with accents */
    geo_search_json(path, "paris", 20, out, sizeof out);
    CHECK(strstr(out, "\\\"x\\\"") != NULL);                                   /* quotes escaped */
    geo_search_json(path, "a", 20, out, sizeof out);
    CHECK(!strcmp(out, "[]"));                                                 /* too short */
    const char *bad[] = { "La Plata", "x/../y", "a\"b", "ñandu", "", NULL };
    for (int i = 0; bad[i]; i++) { CHECK(!geo_query_ok(bad[i])); geo_search_json(path, bad[i], 20, out, sizeof out); CHECK(!strcmp(out, "[]")); }
    geo_search_json(path, "la plata", 1, out, sizeof out);
    CHECK(strstr(out, "3432043") && !strstr(out, "3678097"));                  /* at most max */
    char tiny[40];
    geo_search_json(path, "la plata", 20, tiny, sizeof tiny);
    CHECK(tiny[0] == '[' && tiny[strlen(tiny) - 1] == ']');                    /* cut short, still a list */
    unlink(path);
    printf("geocities-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
