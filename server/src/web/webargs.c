/* webargs.c — see webargs.h. */
#include "webargs.h"
#include "weburl.h"

#include <string.h>

int webargs_parse(int argc, char *const *argv, WebArgs *a) {
    memset(a, 0, sizeof *a);
    a->mode = WEBARGS_BAD;
    if (argc == 2 && !strcmp(argv[1], "--probe")) a->mode = WEBARGS_PROBE;
    else if (argc == 2 && !strcmp(argv[1], "--probe-icon")) a->mode = WEBARGS_PROBE_ICON;
    else if (argc == 5 && !strcmp(argv[1], "--app") && !strcmp(argv[3], "--profile") && webreg_id_ok(argv[2]) && webreg_id_ok(argv[4])) {
        memcpy(a->app, argv[2], strlen(argv[2]) + 1);
        memcpy(a->profile, argv[4], strlen(argv[4]) + 1);
        a->mode = WEBARGS_APP;
    }
    return a->mode;
}

int webargs_probe_line(const char *buf, size_t len, char *url, size_t n) {
    if (!buf || !url || len < 2 || len > WEBARGS_LINE_MAX + 1 || buf[len - 1] != '\n') return -1;
    size_t l = len - 1;
    if (memchr(buf, '\n', l) || memchr(buf, 0, l) || l + 1 > n) return -1;
    memcpy(url, buf, l);
    url[l] = 0;
    if (!weburl_app_ok(url)) { url[0] = 0; return -1; }
    return 0;
}
