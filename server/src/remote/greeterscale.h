/* greeterscale.h — the login screen's display scale (#242, docs/DISPLAYS.md "The login screen's scale").
 *
 * An administrator's session asks lord-sessiond, on the monitor channel, to use one display's scale on the login
 * screen (and so the lock screen, which is the same login screen): {"a":"display","scaleOn":"<connector>","scale":<pct>},
 * pct 0 to go back to the recommended scale. lord-sessiond checks, as root, that the user of the session being shown
 * is an administrator (never a field of the message), keeps it in GSCALE_FILE (root's, 0644) as "<connector> <pct>\n"
 * and starts the login screen with GSCALE_ENV="<connector>:<pct>". lord-server uses it only with --greeter, only for
 * the display on that connector, and only when the display has no saved scale of its own.
 *
 * Pure: no I/O. Every parser here is strict (exact shapes, no spaces or extra fields) and takes only a scale that
 * out_scale_valid accepts. */
#ifndef LORD_REMOTE_GREETERSCALE_H
#define LORD_REMOTE_GREETERSCALE_H

#include <stddef.h>

#define GSCALE_FILE "/var/lib/lordos/greeter-scale"
#define GSCALE_ENV  "LORD_MAIN_SCALE"
#define GSCALE_CONN_MAX 31              /* a connector's characters ("HDMI-A-1", "eDP-1", "DP-10") */

/* [A-Za-z0-9-], 1 to GSCALE_CONN_MAX characters */
int gscale_conn_ok(const char *c);

/* The session's request, the whole message exactly as lord-server writes it:
 *   {"a":"display","scaleOn":"<connector>","scale":<pct>}
 * 1 and conn/pct set (pct 0 means "back to the recommended scale"); -1 anything else (unknown shape, a connector or
 * a scale that isn't valid, a number with a sign, leading zeros or more than 3 digits). cn must be > GSCALE_CONN_MAX. */
int gscale_from_msg(const char *json, char *conn, size_t cn, int *pct);

/* The file: "<connector> <pct>" with one optional '\n' and nothing after it. 0 and conn/pct set, -1 otherwise. */
int gscale_parse_file(const char *s, char *conn, size_t cn, int *pct);

/* The environment: "<connector>:<pct>". 0 and conn/pct set, -1 otherwise. */
int gscale_parse_env(const char *s, char *conn, size_t cn, int *pct);

/* lord-server's request to send: the exact message gscale_from_msg takes; -1 if conn or pct is not valid */
int gscale_msg(char *out, size_t n, const char *conn, int pct);

#endif
