/* fetchspawn.h — starting and stopping lord-fetch for one of lord-server's workers (the weather widget's, wxsvc.c, and
 * the Store's rankings, fhsvc.c): a fixed path, owned by root or lord-server's user and not writable by others, no
 * arguments, LANG only (and LORDOS_SESSIOND when lord-server has it), the channel on fd 3. */
#ifndef LORD_FETCH_SPAWN_H
#define LORD_FETCH_SPAWN_H

#include <sys/types.h>

typedef struct { int fd; pid_t pid; } FetchHelper;    /* fd -1: not running */

/* 0 with h running, -1 if bin isn't trusted or couldn't start. */
int fetch_helper_start(const char *bin, FetchHelper *h);
/* FQ_END, then waits a second for it to exit before killing it. */
void fetch_helper_stop(FetchHelper *h);
/* A blocking TCP socket to host:443, made by comp's connector (sec/netconn.h: its own scope and firewall rules), for
 * lord-fetch to speak TLS on. The fd, -2 if the name didn't resolve, -1 if no address connected (15 s each). */
int fetch_connect(int comp, const char *host);

#endif
