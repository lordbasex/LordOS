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
static int clean(const char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 0; return 1; }

int rconnect_config(char *o, size_t cap, const char *kind, const char *host, int port, const char *user, const char *ob, const char *path, const char *known) {
    int n;
    /* every value is one line: nothing can start another key */
    if (!clean(host) || !clean(user) || !clean(ob) || !clean(path) || !clean(known)) return -1;
    if (!strcmp(kind, "sftp")) {
        if (!known[0]) return -1;       /* never without a host key to check against */
        n = snprintf(o, cap, "[r]\ntype = sftp\nhost = %s\nuser = %s\nport = %d\npass = %s\nknown_hosts_file = %s\n", host, user, port ? port : 22, ob, known);
    }
    else if (!strcmp(kind, "ftp") || !strcmp(kind, "ftps"))
        n = snprintf(o, cap, "[r]\ntype = ftp\nhost = %s\nuser = %s\nport = %d\npass = %s\n%s\n", host, user[0] ? user : "anonymous", port ? port : 21, ob,
                     !kind[3] ? "explicit_tls = false" : port == 990 ? "tls = true" : "explicit_tls = true");     /* 990: TLS from the first byte */
    else if (!strcmp(kind, "webdav") || !strcmp(kind, "davs")) {
        char pt[16] = "";
        if (port) snprintf(pt, sizeof pt, ":%d", port);
        n = snprintf(o, cap, "[r]\ntype = webdav\nurl = %s://%s%s%s\nvendor = other\nuser = %s\npass = %s\n", kind[0] == 'w' ? "http" : "https", host, pt, path, user, ob);
    } else return -1;
    return n > 0 && (size_t)n < cap ? n : -1;
}


/* one "host type base64" line of ssh-keyscan's, for the host asked about; what a server sent us */
int rconnect_keyline_ok(const char *l, const char *tag) {
    size_t t = strlen(tag);
    if (strncmp(l, tag, t) || l[t] != ' ') return 0;
    const char *p = l + t + 1, *sp = strchr(p, ' ');
    if (!sp || sp == p || sp - p > 40) return 0;
    for (const char *q = p; q < sp; q++) if (!isalnum((unsigned char)*q) && !strchr("-@.", *q)) return 0;
    size_t n = strlen(sp + 1);
    if (n < 40 || n > 8192) return 0;
    for (const char *q = sp + 1; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("+/=", *q)) return 0;
    return 1;
}
void rconnect_host_tag(const char *host, int port, char *out, size_t n) {
    if (!port || port == 22) snprintf(out, n, "%s", host); else snprintf(out, n, "[%s]:%d", host, port);
}
/* 1 the file holds this exact line, 0 it has other lines for the tag (a changed key), -1 nothing for the tag */
int rconnect_known(const char *file, const char *tag, const char *line) {
    size_t t = strlen(tag), l = strlen(line);
    int other = 0;
    for (const char *p = file; p && *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !memcmp(p, line, l)) return 1;
        if (n > t && !memcmp(p, tag, t) && p[t] == ' ') other = 1;
        p = e ? e + 1 : NULL;
    }
    return other ? 0 : -1;
}
/* s_client's output: 1 the system trusts it, 0 it doesn't, -1 no answer. pem gets the server's certificate. */
int rconnect_tls_parse(const char *out, char *pem, size_t cap) {
    const char *a = strstr(out, "-----BEGIN CERTIFICATE-----"), *b = a ? strstr(a, "-----END CERTIFICATE-----") : NULL;
    if (!b) return -1;
    size_t n = (size_t)(b - a) + 25;
    if (n + 2 > cap) return -1;
    memcpy(pem, a, n);
    pem[n] = '\n';
    pem[n + 1] = 0;
    const char *v = strstr(out, "Verify return code: ");
    return v && !strncmp(v + 20, "0 (ok)", 6) ? 1 : 0;
}
