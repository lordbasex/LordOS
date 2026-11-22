/* appid.h — which app a process belongs to, by its cgroup (the launcher's root-owned scopes,
 * sec/cgroup.h): ".../lordos-session-<uid>/app-lordos-<id>.scope[/...]" → "desktop:<id>",
 * "app-lordos-lpk-<id>.scope" → "lpk:<id>" (fw_app_canon). Used by the notification server and the
 * PipeWire guard to apply the firewall's restrictions to Linux apps. */
#ifndef LORD_SEC_APPID_H
#define LORD_SEC_APPID_H

#include <stddef.h>

/* From a cgroup path (the "0::" line of /proc/<pid>/cgroup, without "0::"). 0 ok, -1 not an app's scope. */
int appid_of_cgroup(const char *path, char *out, size_t n);
/* From a PID. 0 ok, 1 the process is in no app's scope, -1 it can't be read (gone). */
int appid_of_pid(unsigned pid, char *out, size_t n);
/* Whether an app running in this session (a scope of the launcher's, as appid_of_cgroup reads it) is one match says
 * yes to: 1 or 0 (no app runs, or there is no such session). */
int appid_running(int (*match)(const char *app, void *u), void *u);

/* ---- who a process belongs to, only to show it (the capture indicator), never for a decision ----
 * Some programs leave the scope the launcher put them in: Chrome and Chromium move their browser process (and what it
 * starts afterwards, such as its audio service) into a unit of the user's own service manager
 * (user@<uid>.service/app.slice/app-com.google.Chrome-<pid>.scope). That cgroup says nothing about the app, and the
 * user controls that subtree, so no name there is ever trusted. What the kernel still says: a process is created in its
 * parent's cgroup, and only root puts one into the session's root-owned scopes. So a process in the user's manager that
 * has a child in an app's scope, or whose parent is in one, was in that scope itself: it is that app's. The walk goes up
 * through the user's manager only (never its init.scope, never a process of another user, at most 16 steps), and
 * answers only when every such child says the same app. The restrictions, the portals' gates and restore tokens keep
 * using appid_of_pid alone.
 *
 * The session's owner of a cgroup path: 0 an app (as appid_of_cgroup) or a LordOS component in its own scope
 * ("lordos-net-<name>.scope" → "lordos:<name>", fw_component_scope); 2 LordOS itself (lord-server's own cgroup, its
 * helpers' scopes: "lordos"); -1 not this session's (or an app-lordos-* name that is no app). */
int appid_session_owner(const char *cg, char *out, size_t n);
/* A view of the processes, so tests can give fixtures (appid_of_lineage reads /proc and /sys/fs/cgroup). */
typedef struct {
    int (*cgroup)(void *u, unsigned pid, char *out, size_t n);      /* the "0::" path, without "0::" or newline; 0 ok */
    unsigned (*parent)(void *u, unsigned pid);                      /* PPid; 0 when it can't be read */
    int (*mine)(void *u, unsigned pid);                             /* 1: a process of this user */
    /* every process in this session's subtree: each(ctx, its cgroup path, pid) */
    void (*session_procs)(void *u, void (*each)(void *ctx, const char *cg, unsigned pid), void *ctx);
    void *u;
} AppidView;
/* 0 and the owner in out (as appid_session_owner; "lordos" only for the process itself); -1 none can be told. */
int appid_lineage_in(const AppidView *v, unsigned pid, char *out, size_t n);
int appid_of_lineage(unsigned pid, char *out, size_t n);
/* Every process of this session's root-owned subtree (folders root owns only, two levels down). */
void appid_session_procs(void (*each)(void *ctx, const char *cg, unsigned pid), void *ctx);
/* The program a process runs: the base name of /proc/<pid>/exe (the kernel's, not the comm a process sets itself), only
 * [A-Za-z0-9._+-] kept, at most n - 1 bytes. 0 ok, -1 none. Only to say what is known of a program LordOS can't name. */
int appid_exe_name(unsigned pid, char *out, size_t n);
int appid_exe_base(const char *link, char *out, size_t n);         /* the same from the link's text (tests) */

#endif
