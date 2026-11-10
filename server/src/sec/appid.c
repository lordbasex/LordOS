/* appid.c — app of a process by its cgroup (see appid.h). */
#include "appid.h"
#include "../fw/rules.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Only the root-owned session subtree of lordos.service counts: a cgroup the user creates under
 * user@<uid>.service with the same names must never pose as an app. */
int appid_of_cgroup(const char *path, char *out, size_t n) {
    static const char base[] = "/system.slice/lordos.service/lordos-session-";
    if (strncmp(path, base, sizeof base - 1)) return -1;
    const char *p = path + sizeof base - 1;
    char *end;
    unsigned long uid = strtoul(p, &end, 10);
    if (end == p || *end != '/' || uid != (unsigned long)getuid()) return -1;
    p = end + 1;
    char scope[FW_APP_MAX + 32];
    size_t l = strcspn(p, "/\n");
    if (l < 18 || l >= sizeof scope || strncmp(p, "app-lordos-", 11) || strncmp(p + l - 6, ".scope", 6)) return -1;
    memcpy(scope, p, l);
    scope[l] = 0;
    return fw_app_canon(scope, out, n);
}

int appid_of_pid(unsigned pid, char *out, size_t n) {
    char f[64], line[1024];
    snprintf(f, sizeof f, "/proc/%u/cgroup", pid);
    FILE *fp = fopen(f, "re");
    if (!fp) return -1;
    int r = -1;
    while (fgets(line, sizeof line, fp))
        if (!strncmp(line, "0::", 3)) { r = appid_of_cgroup(line + 3, out, n) ? 1 : 0; break; }
    fclose(fp);
    return r;
}

int appid_running(int (*match)(const char *app, void *u), void *u) {
    char dir[96];
    snprintf(dir, sizeof dir, "/sys/fs/cgroup/system.slice/lordos.service/lordos-session-%u", (unsigned)getuid());
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        char path[512], app[FW_APP_MAX];
        if (strncmp(e->d_name, "app-lordos-", 11)) continue;
        snprintf(path, sizeof path, "/system.slice/lordos.service/lordos-session-%u/%s", (unsigned)getuid(), e->d_name);
        if (!appid_of_cgroup(path, app, sizeof app)) found = match(app, u);
    }
    closedir(d);
    return found;
}
