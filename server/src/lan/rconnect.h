/* rconnect.h — file connections Files opens through rclone (docs/FILES.md, "Connect to Server"). lord-server, as the session user, starts a closed list of programs at fixed paths, each in a scope of its
 * own (the firewall sees desktop:<program>), with no shell. Passwords reach them through a pipe or a memfd, never argv
 * or the environment. */
#ifndef LORD_LAN_RCONNECT_H
#define LORD_LAN_RCONNECT_H

#include <stddef.h>

/* kind: sftp ftp ftps webdav davs, a folder under ~/.local/share/lordos/remote. 1 started, 0 busy or refused. */
int   rconnect_start(const char *kind, const char *host, int port, const char *user, const char *domain, const char *password, const char *path);
/* {"state":"idle|running|confirm|changed|done|failed|refused|missing","package":"…","path":"…","print":"…"} (malloc) */
char *rconnect_poll(void);
void  rconnect_trust(const char *print, int yes);   /* the answer to "confirm", for the fingerprint that was shown */
int   rconnect_unmount(const char *path);   /* 0 ok, a folder under that root only */

/* pure, tests/lan_test.c */
int   rconnect_host_ok(const char *s);
int   rconnect_user_ok(const char *s);
int   rconnect_path_ok(const char *s);
int   rconnect_config(char *out, size_t cap, const char *kind, const char *host, int port, const char *user, const char *obscured, const char *path, const char *known);
int   rconnect_keyline_ok(const char *line, const char *tag);
void  rconnect_host_tag(const char *host, int port, char *out, size_t n);
int   rconnect_known(const char *file, const char *tag, const char *line);
int   rconnect_tls_parse(const char *out, char *pem, size_t cap);
/* ssh-keygen -lf's lines as "ED25519 SHA256:… · RSA SHA256:…": every key that would be trusted */
int   rconnect_prints(const char *keygen, char *out, size_t cap);
int   rconnect_tls_decide(const char *pin, const char *pem, int verified);   /* RC_* */
enum { RC_TLS_CHANGED = -1, RC_TLS_SYSTEM = 0, RC_TLS_PIN = 1, RC_TLS_ASK = 2 };
int   rconnect_name_ok(const char *checkhost);                 /* openssl x509 -checkhost / -checkip says it matches */
int   rconnect_answer(const char *shown, const char *print, int yes);   /* 1 yes, -1 no, 0 not about what's on screen */

/* rconnect_run.c. RCONNECT_SCOPE: desktop:lordos-rclone for the firewall. */
#define RCONNECT_SCOPE "lordos-rclone"
typedef struct {
    char *const *argv;
    const char *input;      /* stdin: well under a pipe's size */
    size_t ilen;
    char *out;              /* its stdout */
    size_t cap;
    int timeout_ms;         /* then SIGKILL, and -1 */
    int offline;            /* no TCP at all */
    int any_status;         /* its exit status doesn't count */
} RconnectRun;
int   rconnect_run(const RconnectRun *job);                    /* bytes of output, -1 failed or out of time */

#endif
