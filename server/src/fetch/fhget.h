/* fhget.h — lord-fetch's FQ_FLATHUB exchange on a connection that is already up (E10; flathub.h). Split from fetch.c
 * so tests/flathub_fetch_test.c can run it over a plain socket with recorded answers. */
#ifndef LORD_FETCH_FHGET_H
#define LORD_FETCH_FHGET_H

#include "flathub.h"
#include "../mail/net.h"

#define FH_TIMEOUT_MS 30000             /* the whole answer */

/* One GET of list's path (built here from the number and now) to FH_HOST on c, at most FH_ANSWER_MAX bytes, no
 * redirect followed: a 3xx is a failure like any other status. 0 with out filled, or -1 with err set to one of
 * "bad-request", "big", "http", "redirect", "status", "format". */
int fh_exchange(Conn *c, int list, long long now, FhIds *out, char *err, size_t errlen);

#endif
