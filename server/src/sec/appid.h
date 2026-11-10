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

#endif
