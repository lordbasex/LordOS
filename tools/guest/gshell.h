/* gshell.h — lordos-guest 8 (docs/VIRTUALIZATION.md 6.12, 6.18):
 *
 * The agent's shell. On AGK_SHELL_OPEN from the host the helper (which runs as the person signed in at the guest's
 * display, never root: as root it answers AG_SHELL_NOSTART) starts that person's own login shell in a new pty: the
 * shell of their /etc/passwd entry when it is an executable listed in /etc/shells, else /bin/sh, as "-name", in their
 * home, with TERM=xterm-256color. Nothing the host sends names a program, an argument, a path or an environment
 * variable: the host only types into it (AGK_SHELL_DATA), sizes it (AGK_SHELL_SIZE) and ends it (AGK_SHELL_CLOSE, a
 * SIGHUP to its process group). Flow control by AGK_SHELL_ACK both ways (AG_SHELL_WINDOW).
 *
 * Links and notifications for the host. The helper listens on $XDG_RUNTIME_DIR/lordos-guest.sock (0600; peers of this
 * uid only, SO_PEERCRED; four at a time, 2 s each, one frame each): a program of the person's sends one AGK_LINK or
 * AGK_NOTIFY frame, the helper parses it as lord-vm will and passes it on only when the host's hello said "links" or
 * "notify" (the machine allows them), answering one GSA_* byte. `lordos-guest --open-on-host URL` and
 * `lordos-guest --notify-host TITLE [BODY]` are that program (the link percent-encoded, ag_link_encode). The helper
 * sends nothing of its own: only what a program of this person asked for (and, tools 10, the person's desktop notifications
 * while the host takes them, gnotes.h). Tools 9: a frame of kind AGK_SEND_OFFER whose payload is a file's absolute path
 * queues that file for the host (gsend.h, `lordos-guest --send-to-host FILE…`). */
#ifndef LORDOS_GUEST_GSHELL_H
#define LORDOS_GUEST_GSHELL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { GSA_SENT = 0, GSA_REFUSED = 1, GSA_MALFORMED = 2, GSA_NOHOST = 3, GSA_FULL = 4 };

/* the shell for uid: 1 theirs (out), 0 /bin/sh (out). passwd, shells: other files for tests (NULL: /etc's) */
int gshell_pick(uid_t uid, const char *passwd, const char *shells, char *out, size_t cap);
/* an AGK_SHELL_* frame from the host: 1 it was one (handled), 0 another kind */
int gshell_frame(int port, uint16_t kind, const uint8_t *p, uint32_t n);
int gshell_fd(void);            /* the pty's master, -1 none */
int gshell_can_read(void);      /* 1 while the host's window has room for more of what the shell writes */
int gshell_wants_out(void);     /* 1 while typing waits for the pty */
void gshell_pump(int port, short revents);
void gshell_end(void);

int gshell_sock_path(char *out, size_t cap);
int gshell_listen(void);        /* the socket, -1 none */
int gshell_listen_fd(void);
int gshell_client_fd(int i);    /* 0..3 */
void gshell_accept(void);
void gshell_client(int i, int port, unsigned host_caps);
void gshell_expire(void);
/* the command line (--open-on-host URL, --notify-host TITLE [BODY]): the exit status */
int gshell_cli(const char *what, const char *a, const char *b);

#endif
