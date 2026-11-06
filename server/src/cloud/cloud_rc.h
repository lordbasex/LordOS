/* cloud_rc.h — the rc client (cloud_rc.c). */
#ifndef LORD_CLOUD_RC_H
#define LORD_CLOUD_RC_H

#include "cloud_pure.h"

typedef struct {
    char sock[108];         /* $XDG_RUNTIME_DIR/lordos/cloud/rc.sock */
    char auth[160];         /* base64 of user:password, random per start */
} RcConn;

/* One call: the HTTP status (200, or rclone's 4xx/5xx with {"error":…}) and its body in *out (malloc, NUL-terminated),
 * or -1 if the socket or the answer failed. m is an RcMethod; anything else is refused. */
int rc_call(const RcConn *c, int m, const char *body, char **out, size_t *olen, int timeout_ms);

#endif
