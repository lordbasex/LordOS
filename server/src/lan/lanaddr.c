/* lanaddr.c — see lanaddr.h. */
#include "lanaddr.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int lan_private(uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8 || (a >> 16) == 0xa9fe;
}

int lan_sweep_range(uint32_t addr, uint32_t mask, uint32_t *first, uint32_t *last) {
    if (mask < 0xffffff00u) mask = 0xffffff00u;
    if ((uint32_t)~mask < 3) return 0;
    uint32_t net = addr & mask, bc = net | ~mask;
    *first = net + 1;
    *last = bc - 1;
    return (int)(*last - *first + 1);
}

/* one UTF-8 sequence at p; its length, or 0 if it is invalid */
static size_t utf8_len(const unsigned char *p) {
    size_t l = p[0] < 0x80 ? 1 : (p[0] >> 5) == 6 ? 2 : (p[0] >> 4) == 14 ? 3 : (p[0] >> 3) == 30 ? 4 : 0;
    for (size_t i = 1; i < l; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    return l;
}

int lan_clean_text(const char *in, char *out, size_t n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p == ' ') p++;
    while (*p) {
        size_t l = utf8_len(p);
        if (!l) { p++; continue; }
        if (l == 1 && (*p < 0x20 || *p == 0x7f)) { p++; continue; }
        if (o + l + 1 > n) break;
        memcpy(out + o, p, l);
        o += l;
        p += l;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return o != 0;
}

/* field `i` of a request: nothing below 0x20, nothing past the cap */
static int field_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n > max) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
    return 1;
}

static int share_name_ok(const char *s) {
    if (!*s || !field_ok(s, 80)) return 0;
    if (!strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (; *s; s++) if (strchr("/\\:*?\"<>|", *s)) return 0;
    return 1;
}

static int export_path_ok(const char *s) {
    if (s[0] != '/' || !field_ok(s, 200) || strstr(s, "//")) return 0;
    for (const char *p = s; *p; p++) if (strchr("\\:\"'`$;|&<>", *p)) return 0;
    for (const char *p = s; (p = strstr(p, "/..")); p += 3) if (p[3] == '/' || !p[3]) return 0;
    return 1;
}

static int user_ok(const char *s) {
    if (!field_ok(s, 60)) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || (unsigned char)*s >= 0x80 || strchr("._- ", *s))) return 0;
    return 1;
}

int share_req_parse(char *buf, ShareReq *r) {
    char *f[6];
    int n = 0;
    f[n++] = buf;
    for (char *p = buf; *p && n < 6; p++) if (*p == '\n') { *p = 0; f[n++] = p + 1; }
    if (n != 6) return -1;                              /* kind, addr, share, user, domain, password */
    memset(r, 0, sizeof *r);
    if (!strcmp(f[0], "nfs")) r->nfs = 1;
    else if (strcmp(f[0], "smb")) return -1;
    struct in_addr in;
    if (inet_pton(AF_INET, f[1], &in) != 1 || !lan_private(ntohl(in.s_addr))) return -1;
    snprintf(r->addr, sizeof r->addr, "%s", f[1]);
    if (r->nfs ? !export_path_ok(f[2]) : !share_name_ok(f[2])) return -1;
    snprintf(r->share, sizeof r->share, "%s", f[2]);
    if (!user_ok(f[3]) || !user_ok(f[4])) return -1;
    if (r->nfs && (f[3][0] || f[4][0] || f[5][0])) return -1;       /* NFS takes no credentials */
    if (strlen(f[5]) > 256 || !field_ok(f[5], 256)) return -1;
    snprintf(r->user, sizeof r->user, "%s", f[3]);
    snprintf(r->domain, sizeof r->domain, "%s", f[4]);
    r->password = f[5];
    return 0;
}

void share_leaf(const ShareReq *r, char *out, size_t n) {
    const char *s = r->share;
    if (r->nfs) { const char *b = strrchr(s, '/'); s = b && b[1] ? b + 1 : "export"; }
    char name[128];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 1 < sizeof name; p++) name[o++] = (*p < 0x20 || *p == '/' || *p == 0x7f) ? '_' : (char)*p;
    name[o] = 0;
    if (name[0] == '.') name[0] = '_';
    snprintf(out, n, "%s on %s", name, r->addr);
}

int share_path_ok(const char *path, const char *user) {
    char base[96];
    int w = snprintf(base, sizeof base, "/media/%s/", user);
    if (w < 0 || (size_t)w >= sizeof base || strncmp(path, base, (size_t)w)) return -1;
    const char *leaf = path + w;
    if (!*leaf || strlen(leaf) > 200 || strchr(leaf, '/') || leaf[0] == '.') return -1;
    for (const char *p = leaf; *p; p++) if ((unsigned char)*p < 0x20) return -1;
    return 0;
}

int share_fstype(const char *t) {
    return !strcmp(t, "cifs") || !strcmp(t, "smb3") || !strcmp(t, "nfs") || !strcmp(t, "nfs4");
}
