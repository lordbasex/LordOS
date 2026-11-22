/* fhget.c — lord-fetch's FQ_FLATHUB exchange (see fhget.h). */
#include "fhget.h"
#include "../mail/http.h"

#include <stdio.h>
#include <string.h>

int fh_exchange(Conn *c, int list, long long now, FhIds *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    char path[128];
    if (fh_path(list, now, path, sizeof path)) { snprintf(err, errlen, "bad-request"); return -1; }
    HttpResp r;
    memset(&r, 0, sizeof r);
    int rc = http_request(c, "GET", FH_HOST, path, "Accept: application/json\r\n",
                          NULL, 0, FH_ANSWER_MAX, FH_TIMEOUT_MS, &r);
    if (rc == -2) { snprintf(err, errlen, "big"); return -1; }
    if (rc) { snprintf(err, errlen, "http"); http_resp_free(&r); return -1; }
    if (r.status >= 300 && r.status < 400) { snprintf(err, errlen, "redirect"); http_resp_free(&r); return -1; }
    if (r.status != 200) { snprintf(err, errlen, "status"); http_resp_free(&r); return -1; }
    int bad = fh_parse(list, r.body ? r.body : "", r.blen, out);
    http_resp_free(&r);
    if (bad) { snprintf(err, errlen, "format"); return -1; }
    return 0;
}
