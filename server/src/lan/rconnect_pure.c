/* rconnect_pure.c — what rconnect.c checks and writes, with no process around it (tests/lan_test.c). */
#include "rconnect.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int rconnect_host_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 253 || s[0] == '-' || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '-') return 0;
    return 1;
}
int rconnect_user_ok(const char *s) {
    size_t n = strlen(s);
    if (n > 64 || s[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && !strchr("._-", s[i])) return 0;
    return 1;
}
int rconnect_path_ok(const char *s) {
    if (!s[0]) return 1;
    if (s[0] != '/' || strlen(s) > 200 || strstr(s, "/../") || strstr(s, "//")) return 0;
    size_t n = strlen(s);
    if (n >= 3 && !strcmp(s + n - 3, "/..")) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("/._-", *p)) return 0;
    return 1;
}

/* rclone's configuration for one remote. Values were checked above, so a line can't start another key. */
int rconnect_config(char *o, size_t cap, const char *kind, const char *host, int port, const char *user, const char *ob, const char *path) {
    int n;
    if (!strcmp(kind, "sftp")) n = snprintf(o, cap, "[r]\ntype = sftp\nhost = %s\nuser = %s\nport = %d\npass = %s\n", host, user, port ? port : 22, ob);
    else if (!strcmp(kind, "ftp") || !strcmp(kind, "ftps"))
        n = snprintf(o, cap, "[r]\ntype = ftp\nhost = %s\nuser = %s\nport = %d\npass = %s\nexplicit_tls = %s\n", host, user[0] ? user : "anonymous", port ? port : 21, ob, kind[3] ? "true" : "false");
    else if (!strcmp(kind, "webdav") || !strcmp(kind, "davs")) {
        char pt[16] = "";
        if (port) snprintf(pt, sizeof pt, ":%d", port);
        n = snprintf(o, cap, "[r]\ntype = webdav\nurl = %s://%s%s%s\nvendor = other\nuser = %s\npass = %s\n", kind[0] == 'w' ? "http" : "https", host, pt, path, user, ob);
    } else return -1;
    return n > 0 && (size_t)n < cap ? n : -1;
}

