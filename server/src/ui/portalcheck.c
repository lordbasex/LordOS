/* portalcheck.c — see portalcheck.h. Kept apart from portal.c so that tests/portal_check_test.c can run
 * it without a bus. */
#include "portalcheck.h"
#include "../fsops.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_MAX 4096

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }

/* a plain dotted IPv4: four decimal parts 0..255, no leading zero */
static int ipv4(const char *s, size_t n) {
    int parts = 0;
    size_t i = 0;
    while (i <= n) {
        size_t j = i, v = 0;
        while (j < n && is_digit(s[j])) { v = v * 10 + (size_t)(s[j] - '0'); if (j - i >= 3) return 0; j++; }
        if (j == i || (s[i] == '0' && j - i > 1) || v > 255) return 0;
        parts++;
        if (j == n) break;
        if (s[j] != '.') return 0;
        i = j + 1;
        if (i == n) return 0;
    }
    return parts == 4;
}

/* an IPv6 address (lower case, no brackets): groups of 1..4 hex digits, at most one "::", 8 groups in
 * all (fewer with "::"), the last two may be a dotted IPv4 */
static int ipv6(const char *s, size_t n) {
    int groups = 0, gap = 0;
    size_t i = 0;
    if (n >= 2 && s[0] == ':' && s[1] == ':') { gap = 1; i = 2; }
    else if (n && s[0] == ':') return 0;
    while (i < n) {
        size_t j = i;
        while (j < n && is_hex(s[j])) j++;
        if (j < n && s[j] == '.') {                     /* the trailing IPv4 */
            if (!ipv4(s + i, n - i)) return 0;
            groups += 2;
            i = n;
            break;
        }
        if (j == i || j - i > 4) return 0;
        groups++;
        if (j == n) { i = n; break; }
        if (s[j] != ':') return 0;
        if (j + 1 < n && s[j + 1] == ':') {
            if (gap) return 0;
            gap = 1;
            i = j + 2;
        } else {
            if (j + 1 == n) return 0;                   /* a lone trailing ':' */
            i = j + 1;
        }
    }
    return gap ? groups <= 7 : groups == 8;
}

int portal_web_host(const char *u, char *host, size_t hostlen) {
    if (!u || !hostlen) return -1;
    host[0] = 0;
    size_t len = strlen(u);
    if (!len || len > WEB_MAX) return -1;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)u[i] <= 0x20 || (unsigned char)u[i] >= 0x7f || u[i] == '\\') return -1;
    const char *p;
    if (!strncasecmp(u, "https://", 8)) p = u + 8;
    else if (!strncasecmp(u, "http://", 7)) p = u + 7;
    else return -1;
    size_t an = strcspn(p, "/?#");
    if (!an || an >= 300) return -1;
    char a[300];
    for (size_t i = 0; i < an; i++) a[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] - 'A' + 'a') : p[i];
    a[an] = 0;
    if (memchr(a, '@', an) || memchr(a, '%', an)) return -1;
    size_t hn;
    const char *port = NULL;
    if (a[0] == '[') {
        char *c = strchr(a, ']');
        if (!c) return -1;
        hn = (size_t)(c - a) + 1;
        if (c[1]) { if (c[1] != ':' || !c[2]) return -1; port = c + 2; }
        if (!ipv6(a + 1, hn - 2)) return -1;
    } else {
        char *c = strchr(a, ':');
        hn = c ? (size_t)(c - a) : an;
        if (c) { if (!c[1]) return -1; port = c + 1; }
        if (!hn || hn > 253) return -1;
        for (size_t i = 0; i < hn; i++)
            if (!(is_digit(a[i]) || (a[i] >= 'a' && a[i] <= 'z') || a[i] == '.' || a[i] == '_' || a[i] == '-')) return -1;
        int dot = a[hn - 1] == '.';
        size_t n = hn - (size_t)dot, ls = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i < n && a[i] != '.') continue;
            if (i == ls || i - ls > 63) return -1;  /* an empty or too long label */
            if (i < n) ls = i + 1;
        }
        /* a last label that is a number makes the whole host an IPv4 to a browser */
        const char *last = a + ls;
        size_t ln = n - ls, k = 0;
        int numeric = 1;
        if (ln >= 2 && last[0] == '0' && last[1] == 'x') { for (k = 2; k < ln; k++) if (!is_hex(last[k])) numeric = 0; }
        else for (k = 0; k < ln; k++) if (!is_digit(last[k])) numeric = 0;
        if (numeric && (dot || !ipv4(a, hn))) return -1;
    }
    if (port) {
        size_t pn = strlen(port);
        if (!pn || pn > 5) return -1;
        long v = 0;
        for (size_t i = 0; i < pn; i++) { if (!is_digit(port[i])) return -1; v = v * 10 + (port[i] - '0'); }
        if (v > 65535) return -1;
    }
    if (hn >= hostlen) return -1;
    memcpy(host, a, hn);
    host[hn] = 0;
    return 0;
}

/* inside the home folder or the user's mounts and outside the folders lord.fs protects (launchers,
 * autostart, settings…). Opening, unlike showing, never takes an executable or a launcher (.desktop):
 * opening must never run something. */
static int local_kind(const char *real, const struct stat *st, int kind) {
    if (!fsops_allowed(real) || fsops_protected(real)) return -1;
    if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)) return -1;
    if (kind == PORTAL_REVEAL) return PORTAL_REVEAL;
    if (S_ISDIR(st->st_mode)) return PORTAL_FOLDER;
    size_t l = strlen(real);
    if ((st->st_mode & 0111) || (l >= 8 && !strcasecmp(real + l - 8, ".desktop"))) return -1;
    return PORTAL_FILE;
}

/* The real path itself, never a link at its end, as an O_PATH fd: what it is, without opening the file */
static int stat_real(const char *real, struct stat *st) {
    int fd = open(real, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = fstat(fd, st);
    close(fd);
    return r;
}

static int seen_kind(const char *real, int kind, PortalSeen *seen) {
    struct stat st;
    if (stat_real(real, &st)) return -1;
    int k = local_kind(real, &st, kind);
    if (k >= 0 && seen) { seen->dev = st.st_dev; seen->ino = st.st_ino; seen->type = st.st_mode & S_IFMT; }
    return k;
}

int portal_file_kind(const char *name, int kind, char *real, PortalSeen *seen) {
    if (!name || name[0] != '/' || !realpath(name, real)) return -1;
    return seen_kind(real, kind, seen);
}

int portal_fd_kind(int fd, int kind, char *real, PortalSeen *seen) {
    struct stat a;
    char link[64], name[PATH_MAX];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(link, name, sizeof name - 1);
    if (l <= 0 || fstat(fd, &a)) return -1;
    name[l] = 0;
    PortalSeen s;
    int k = portal_file_kind(name, kind, real, &s);
    if (k < 0 || s.dev != a.st_dev || s.ino != a.st_ino) return -1;
    if (seen) *seen = s;
    return k;
}

int portal_file_still(const char *real, int kind, const PortalSeen *seen) {
    char again[PATH_MAX];
    struct stat st;
    if (!real || !seen || !realpath(real, again) || strcmp(again, real)) return 0;   /* a folder on the way became a link */
    if (stat_real(real, &st)) return 0;
    if (st.st_dev != seen->dev || st.st_ino != seen->ino || (st.st_mode & S_IFMT) != seen->type) return 0;
    return local_kind(real, &st, kind == PORTAL_FOLDER ? PORTAL_FILE : kind) == kind;
}
