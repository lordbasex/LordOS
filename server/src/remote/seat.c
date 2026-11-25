/* seat.c — is this user the one on the monitor (see seat.h)? */
#include "seat.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEAT_MAX (256 * 1024)           /* status.json is a few KB; a longer one is not read (the answer is "no") */

/* a user name as lord-sessiond takes one (rsec.c rs_valid_username): [a-z_][a-z0-9_-]{0,31} */
static int plain_name(const char *s, size_t n) {
    if (!n || n > 32 || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int seat_monitor_user(const char *json, char *out, size_t n) {
    static const char key[] = "],\"monitor\":\"";
    const char *last = NULL;
    for (const char *p = json; (p = strstr(p, key)); p++) last = p;
    if (!last || !n) return -1;
    const char *v = last + sizeof key - 1;
    const char *e = strchr(v, '"');
    if (!e || strncmp(e, "\",\"sessions\":[", 14)) return -1;
    size_t len = (size_t)(e - v);
    if (len && !plain_name(v, len)) return -1;
    if (len >= n) return -1;
    memcpy(out, v, len);
    out[len] = 0;
    return 0;
}

/* "pid":N, the first one: lord-sessiond's own (write_status puts it before any list) */
static long status_pid(const char *json) {
    static const char key[] = "{\"running\":";
    if (strncmp(json, key, sizeof key - 1)) return -1;
    const char *p = strstr(json, ",\"pid\":");
    if (!p) return -1;
    char *end;
    long pid = strtol(p + 7, &end, 10);
    return pid > 1 && pid < INT_MAX && *end == ',' ? pid : -1;
}

static int is_sessiond(const char *proc, long pid) {
    char link[PATH_MAX], exe[PATH_MAX];
    snprintf(link, sizeof link, "%s/%ld/exe", proc, pid);
    ssize_t l = readlink(link, exe, sizeof exe - 1);
    if (l <= 0) return 0;
    exe[l] = 0;
    /* after a package upgrade the running binary was replaced: the link ends in " (deleted)" */
    static const char deleted[] = " (deleted)";
    if ((size_t)l > sizeof deleted - 1 && !strcmp(exe + l - (sizeof deleted - 1), deleted)) exe[l - (sizeof deleted - 1)] = 0;
    const char *base = strrchr(exe, '/');
    return base && !strcmp(base + 1, "lord-sessiond");
}

int seat_user_is_active(const char *status, const char *proc, uid_t owner, const char *user) {
    if (!user || !plain_name(user, strlen(user))) return 0;
    int fd = open(status, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner || (st.st_mode & 022) || st.st_size <= 0 || st.st_size > SEAT_MAX) {
        close(fd);
        return 0;
    }
    char *j = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (j && got < (size_t)st.st_size) {
        ssize_t r = read(fd, j + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!j) return 0;
    j[got] = 0;
    char who[40];
    long pid = status_pid(j);
    int ok = got == (size_t)st.st_size && pid > 0 && !seat_monitor_user(j, who, sizeof who) && who[0] && !strcmp(who, user) &&
             is_sessiond(proc, pid);
    free(j);
    return ok;
}
