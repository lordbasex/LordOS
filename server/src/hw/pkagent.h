/* pkagent.h — the session's polkit authentication agent (docs/PRIVACY.md, "Authentication for Linux
 * apps"). Part of lord-server, which runs as the session user: it holds no privilege, and the password
 * goes only to polkit's setuid helper. JavaScript draws the prompt and can only cancel it. */
#ifndef LORD_HW_PKAGENT_H
#define LORD_HW_PKAGENT_H

#include <poll.h>

typedef void (*PkEmit)(const char *kind, const char *json);

void pkagent_start(PkEmit emit);        /* after sysbus_start */
int  pkagent_pollfds(struct pollfd *p, int max);
void pkagent_dispatch(const struct pollfd *p, int n);
void pkagent_tick(void);
void pkagent_cancel(int id);            /* the prompt's Cancel */

/* A key from a device of this computer, the only way in. 1: it was the prompt's, don't pass it on. */
int  pkagent_key(int code, int value);
/* A key from anywhere else (remote desktop, screen sharing, on-screen keyboard): 1 drop it. */
int  pkagent_blocks_injected(int value);
/* Does this session hold this computer's input devices (is it on the monitor)? */
void pkagent_set_local(int present);

#endif
