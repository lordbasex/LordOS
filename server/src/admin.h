/* admin.h — LordOS privileged helper (see admin.c). */
#ifndef LORD_ADMIN_H
#define LORD_ADMIN_H

#include <stddef.h>
#include <sys/types.h>

#define ADMIN_ARG_MAX 65536             /* argument of an action (may have several lines) */

/* ---- server side (lord-server, without root) ---- */

/* Called as root, before dropping privileges. user = the session user. */
int  admin_start(const char *user);
int  admin_fd(void);                                  /* -1 if there is no helper */
int  admin_known(const char *action);                 /* is it in the closed list? */
/* Sends an action; returns its id (> 0) or -1. Replies arrive with that id: LOG (progress),
 * and at the end OK or ERR. Notices that answer nothing (network state, VPN) arrive with
 * id 0 and kind EVENT. Several actions can run at once. */
int  admin_send(const char *action, const char *arg, size_t len);
void admin_dispatch(void (*fn)(int id, const char *kind, const char *text));
/* The one descriptor an action answered with (admin_ok_fd: only vm-usb-allow, its "OK vm.usbReady" line), for the code that
 * sent that request, from inside fn: taken once (the caller closes it), -1 when there is none. One nobody takes while fn runs
 * is closed after it: JS never gets one */
int  admin_take_fd(int id);

/* ---- lord-netd (root, started by lord-sessiond): the system network; see admin.c, docs/NETWORK.md ---- */
int  admin_netd_serve(void);                          /* returns only on error */

/* ---- helper side (root): for modules that add actions ---- */

typedef struct {
    int id;                             /* of the running action */
    const char *user, *home;            /* the session user */
    uid_t uid;
    gid_t gid;
    int admin;                          /* the session user is an administrator */
} AdminReq;

/* Replies of the running action: "LOG", "OK" or "ERR" (one line; \n become spaces). */
void admin_say(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Unsolicited notice (id 0, EVENT): "net link eth0 up", "vpn wg0 connected"... */
void admin_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* What reaches the UI is a code line, never prose (see errcode.h): the UI translates it with
 * t('errors.<code>') for ERR and t('status.<code>') for OK/LOG. Pairs (key, value) end in NULL:
 *   admin_err("pkg.removeEssential", "name", n, NULL);   →  "<id> ERR pkg.removeEssential name=dpkg" */
#include "errcode.h"
__attribute__((sentinel))
static inline void admin_code(const char *kind, const char *code, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, code);
    lerr_vset(line, sizeof line, code, ap);
    va_end(ap);
    admin_say(kind, "%s", line);
}
/* The same with one descriptor passed along (SCM_RIGHTS on the same message as the line); the helper keeps its own copy,
 * the caller closes it. Only from an action that runs in the session's helper itself (not A_FORK, not lord-netd's) */
void admin_say_fd(int fd, const char *kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
__attribute__((sentinel))
static inline void admin_code_fd(int fd, const char *kind, const char *code, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, code);
    lerr_vset(line, sizeof line, code, ap);
    va_end(ap);
    admin_say_fd(fd, kind, "%s", line);
}
#define admin_ok_fd(fd, ...) admin_code_fd((fd), "OK", __VA_ARGS__)
#define admin_err(...) admin_code("ERR", __VA_ARGS__)
#define admin_ok(...)  admin_code("OK", __VA_ARGS__)
#define admin_log(...) admin_code("LOG", __VA_ARGS__)

/* Fixed-path program with a clean environment; its output goes as LOG. Returns the exit code. */
int  admin_run(const char *const argv[]);
/* Same, with `input` on standard input (e.g. a password for chpasswd: never in argv). */
int  admin_run_input(const char *const argv[], const char *input);
/* Same, but stores the output (stdout) in out (0-terminated). */
int  admin_run_capture(const char *const argv[], char *out, size_t cap);

/* Resident processes (wpa_supplicant, openvpn, the DHCP client...), with a unique name. They live
 * until stopped or until the session ends. A program's output arrives as EVENT
 * "daemon <name> <line>"; a process with our own code (fn) uses admin_event. When it ends,
 * EVENT "daemon <name> exit <code>". */
int  admin_daemon_exec(const char *name, const char *const argv[], const char *const env[]);
int  admin_daemon_fork(const char *name, void (*fn)(void *), void *arg);
int  admin_daemon_stop(const char *name);             /* SIGTERM; 0 if it was running */
int  admin_daemon_running(const char *name);
int  admin_daemon_signal(const char *name, int sig);  /* 0 if it was running */

#endif
