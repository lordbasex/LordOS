/* xdgact.h — xdg-activation tokens: who may bring a window to the front (see xdgact.c). */
#ifndef LORD_XDGACT_H
#define LORD_XDGACT_H

#include <stddef.h>
#include <stdint.h>

#define XACT_TOKEN_LEN 32               /* hex characters, without the NUL */
#define XACT_MAX 32                     /* tokens kept at once; a new one replaces the oldest */
#define XACT_LAUNCH_MS 60000            /* issued by LordOS when it launches something (a cold start can be slow) */
#define XACT_CLIENT_MS 10000            /* asked for by an app (xdg_activation_token_v1.commit); the most it gets */
#define XACT_SERIAL_MS 10000            /* an input serial counts this long after it was sent */

enum { XACT_OK = 0, XACT_DENIED, XACT_STALE, XACT_UNKNOWN };

/* Monotonic milliseconds, the clock every call below expects. */
double xact_now(void);

/* A new token, valid for ttl_ms from now_ms. granted = 0: an app asked without a reason to (no
 * keyboard, no recent input): activating with it only asks for attention. NULL if there is no
 * randomness; the returned text stays valid until the next xact_issue. */
const char *xact_issue(double now_ms, int ttl_ms, int granted, const char *app_id);

/* The same for a token an app asked for: client is who asked (only compared, never used), and the
 * life is at most XACT_CLIENT_MS. Its grant is revoked when the keyboard moves away from that client
 * (xact_focus_moved) or the client leaves (xact_client_gone); a token LordOS issued (xact_issue) is
 * never revoked this way. A revoked token is still used up by xact_take and returns XACT_DENIED. */
const char *xact_issue_for(double now_ms, int ttl_ms, int granted, const char *app_id, const void *client);

/* The keyboard went to client (NULL: to the desktop itself or nowhere): the tokens every other app
 * asked for stop granting. Returns how many were revoked. */
int xact_focus_moved(const void *client);

/* Uses a token up (each one works once): XACT_OK, XACT_DENIED (issued without a grant), XACT_STALE
 * (expired) or XACT_UNKNOWN (never issued, already used, or not a token). app_id gets what it was
 * issued for ("" if nothing), when not NULL. */
int xact_take(const char *token, double now_ms, char *app_id, size_t n);

/* Input serials (wl_pointer.button, wl_keyboard.key and enter): which client got each one, for the
 * set_serial an app sends with its request. client is only compared, never used. */
void xact_note_serial(uint32_t serial, const void *client, double now_ms);
int  xact_serial_recent(uint32_t serial, const void *client, double now_ms);
void xact_client_gone(const void *client);     /* its serials and tokens no longer count (the pointer may be reused) */

void xact_reset(void);                         /* tests */

#endif
