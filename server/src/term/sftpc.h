/* sftpc.h — lord-server's side of the Terminal's Files mode (docs/TERMINAL.md "Files mode"): SFTP,
 * remote file information and port forwards over the tab's own ssh connection.
 *
 * The connection is never named by JS: `open` names a Terminal session, and the ssh master is found
 * by its pid being that session's foreground process group (sshmux_find in the user's own
 * $XDG_RUNTIME_DIR/lordos-term/ssh). A thread per connection opens an `sftp` subsystem on it (or, as
 * root, the server's sftp-server through `sudo`, a fixed command) and starts lord-sftp (fixed path
 * next to this binary, no arguments, LANG only; its channel on fd 3, the ssh channel on fds 4 and 5).
 * A transfer runs on a channel and a lord-sftp of its own, so the listing stays usable and Cancel
 * just kills it; its local file is opened here with lord.fs's rules (fsops). The compositor never
 * waits: replies and events queue up and JS reads them with sftp_poll() (lord.sftp.poll).
 *
 * Requests ({"op":…}):
 *   open     {"sid","root"?,"password"?}          → {"conn"}
 *   list     {"conn","path"}  stat {"conn","path"}  mkdir {"conn","path","perm"?}
 *   remove   {"conn","path","dir"?}  rename {"conn","from","to"}  chmod {"conn","path","perm"}
 *   chown    {"conn","path","uid","gid"}           → lord-sftp's answer (sftpproto.h)
 *   get      {"conn","path","dest","replace"?}     → {"job"}; events progress / done / error
 *   put      {"conn","src","path"}                 → {"job"}
 *   cancel   {"job"}
 *   info     {"conn","path","what":"file|sha256"}  → {"text"} (the remote command's first 4 KB, printable)
 *   forward  {"sid","host","port","local"?,"close"?} → {"local"} (127.0.0.1 only; local 0 = a free port)
 *   close    {"conn"}
 */
#ifndef LORD_TERM_SFTPC_H
#define LORD_TERM_SFTPC_H

#include <stddef.h>
#include <sys/types.h>

#define SFTP_MAX_CONNS  16
#define SFTP_MAX_JOBS   32

/* $XDG_RUNTIME_DIR/lordos-term/ssh, made 0700 when missing; NULL when there is no runtime dir */
const char *sftp_mux_dir(void);
int  sftp_available(void);                      /* 0 if lord-sftp is there and safe to run */

/* the Terminal session's foreground process group, set by term (js_term.c) for the lookup */
typedef pid_t (*SftpFgFn)(int sid);
void sftp_set_fg(SftpFgFn fn);

int  sftp_request(const char *json, size_t len);  /* request id, or -1 when lord-sftp is missing */

typedef struct { int id; int type; char *data; size_t len; } SftpItem;   /* type: SFTP_OK, SFTP_ERR, SFTP_EVT */
enum { SFTP_OK = 0, SFTP_ERR = 1, SFTP_EVT = 2 };
int  sftp_poll(SftpItem **items);
void sftp_items_free(SftpItem *items, int n);

#endif
