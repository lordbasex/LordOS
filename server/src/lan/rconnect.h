/* rconnect.h — file connections Files opens through rclone (docs/FILES.md, "Connect to Server"). lord-server, as the session user, starts a closed list of programs at fixed paths, each in a scope of its
 * own (the firewall sees desktop:<program>), with no shell. Passwords reach them through a pipe or a memfd, never argv
 * or the environment. */
#ifndef LORD_LAN_RCONNECT_H
#define LORD_LAN_RCONNECT_H

#include <stddef.h>

/* kind: sftp ftp ftps webdav davs, a folder under ~/.local/share/lordos/remote. 1 started, 0 busy or refused. */
int   rconnect_start(const char *kind, const char *host, int port, const char *user, const char *domain, const char *password, const char *path);
/* {"state":"idle|running|done|failed|refused|missing","package":"…","path":"…"} (malloc) */
char *rconnect_poll(void);
int   rconnect_unmount(const char *path);   /* 0 ok, a folder under that root only */

/* pure, tests/lan_test.c */
int   rconnect_host_ok(const char *s);
int   rconnect_user_ok(const char *s);
int   rconnect_path_ok(const char *s);
int   rconnect_config(char *out, size_t cap, const char *kind, const char *host, int port, const char *user, const char *obscured, const char *path);

#endif
