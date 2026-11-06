/* cloud_pure.c — see cloud_pure.h. */
#include "cloud_pure.h"
#include "../sec/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- rc methods ---- */

static const char *const methods[RCM_COUNT] = {
    [RCM_VERSION] = "core/version",
    [RCM_PROVIDERS] = "config/providers",
    [RCM_CREATE] = "config/create",
    [RCM_UPDATE] = "config/update",
    [RCM_DELETE] = "config/delete",
    [RCM_GET] = "config/get",
    [RCM_MOUNT] = "mount/mount",
    [RCM_UNMOUNT] = "mount/unmount",
    [RCM_LISTMOUNTS] = "mount/listmounts",
    [RCM_ABOUT] = "operations/about",
    [RCM_PUBLICLINK] = "operations/publiclink",
    [RCM_FSINFO] = "operations/fsinfo",
    [RCM_STATS] = "core/stats",
    [RCM_TRANSFERRED] = "core/transferred",
    [RCM_BWLIMIT] = "core/bwlimit",
    [RCM_VFS_STATS] = "vfs/stats",
    [RCM_VFS_QUEUE] = "vfs/queue",
    [RCM_VFS_EXPIRY] = "vfs/queue-set-expiry",
    [RCM_VFS_REFRESH] = "vfs/refresh",
    [RCM_VFS_FORGET] = "vfs/forget",
    [RCM_QUIT] = "core/quit",
    [RCM_CLEANUP] = "operations/cleanup",
    [RCM_SIZE] = "operations/size",
    [RCM_JOB_STATUS] = "job/status",
    [RCM_JOB_STOP] = "job/stop",
};

const char *rc_method_path(int m) { return m >= 0 && m < RCM_COUNT ? methods[m] : NULL; }

void cloud_b64(const unsigned char *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

int rc_build_header(char *out, size_t cap, int m, const char *auth, size_t blen) {
    const char *p = rc_method_path(m);
    if (!p || !auth || !auth[0]) return -1;
    for (const char *s = auth; *s; s++) if (!isalnum((unsigned char)*s) && *s != '+' && *s != '/' && *s != '=') return -1;
    int n = snprintf(out, cap, "POST /%s HTTP/1.0\r\nHost: rc\r\nAuthorization: Basic %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\n\r\n", p, auth, blen);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* chunked body, decoded in place: its length, or -1 */
static long dechunk(char *b, size_t len) {
    size_t r = 0, w = 0;
    for (;;) {
        char *end = NULL;
        if (r >= len) return -1;
        unsigned long n = strtoul(b + r, &end, 16);
        if (end == b + r) return -1;
        char *nl = memchr(end, '\n', len - (size_t)(end - b));
        if (!nl) return -1;
        r = (size_t)(nl - b) + 1;
        if (!n) return (long)w;
        if (n > len - r) return -1;
        memmove(b + w, b + r, n);
        w += n;
        r += n;
        if (r < len && b[r] == '\r') r++;
        if (r < len && b[r] == '\n') r++;
    }
}

int rc_parse_response(char *buf, size_t len, int *status, char **body, size_t *blen) {
    if (len < 12 || strncmp(buf, "HTTP/1.", 7) || buf[8] != ' ') return -1;
    int st = atoi(buf + 9);
    if (st < 100 || st > 599) return -1;
    char *h = NULL;
    for (size_t i = 0; i + 3 < len; i++) if (!memcmp(buf + i, "\r\n\r\n", 4)) { h = buf + i + 4; break; }
    if (!h) return -1;
    size_t hl = (size_t)(h - buf), bl = len - hl;
    long clen = -1;
    int chunked = 0;
    for (char *l = buf; l < h - 2; ) {
        char *e = memchr(l, '\n', (size_t)(h - l));
        if (!e) break;
        if (!strncasecmp(l, "Content-Length:", 15)) clen = strtol(l + 15, NULL, 10);
        if (!strncasecmp(l, "Transfer-Encoding:", 18)) {
            char v[32] = "";
            size_t k = 0;
            for (char *p = l + 18; p < e && k + 1 < sizeof v; p++) if (*p != ' ' && *p != '\r') v[k++] = (char)tolower((unsigned char)*p);
            v[k] = 0;
            chunked = !strcmp(v, "chunked");
        }
        l = e + 1;
    }
    if (chunked) {
        long n = dechunk(h, bl);
        if (n < 0) return -1;
        bl = (size_t)n;
    } else if (clen >= 0) {
        if ((size_t)clen > bl) return -1;          /* cut short */
        bl = (size_t)clen;
    }
    h[bl] = 0;                                      /* the caller's buffer has a byte to spare */
    *status = st;
    *body = h;
    *blen = bl;
    return 0;
}

/* ---- names and paths ---- */

/* valid UTF-8, no control characters (C0, DEL, C1) */
static int text_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n > max) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x20 || *p == 0x7f) return 0;
        if (*p < 0x80) { p++; continue; }
        int k = (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || (*p == 0xc0 || *p == 0xc1)) return 0;
        unsigned cp = *p & (0x3f >> k);
        for (int i = 1; i <= k; i++) { if ((p[i] & 0xc0) != 0x80) return 0; cp = cp << 6 | (p[i] & 0x3f); }
        if (cp >= 0x80 && cp < 0xa0) return 0;
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        p += k + 1;
    }
    return 1;
}

int cloud_name_ok(const char *name) {
    if (!name || !name[0] || name[0] == '.' || name[0] == ' ' || strchr(name, '/') || !text_ok(name, CLOUD_NAME_MAX)) return 0;
    size_t n = strlen(name);
    return name[n - 1] != ' ';
}

int cloud_rel_ok(const char *rel) {
    if (!rel) return 0;
    if (!rel[0]) return 1;
    if (rel[0] == '/' || !text_ok(rel, CLOUD_REL_MAX) || strstr(rel, "//")) return 0;
    size_t n = strlen(rel);
    if (rel[n - 1] == '/') return 0;
    for (const char *p = rel; *p; ) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if ((l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.')) return 0;
        p += l + (e ? 1 : 0);
    }
    return 1;
}

int cloud_split(const char *root, const char *path, char *name, size_t nn, char *rel, size_t rn) {
    size_t r = strlen(root);
    if (!path || strncmp(path, root, r) || path[r] != '/') return -1;
    const char *a = path + r + 1, *s = strchr(a, '/');
    size_t l = s ? (size_t)(s - a) : strlen(a);
    if (!l || l >= nn) return -1;
    memcpy(name, a, l);
    name[l] = 0;
    const char *rr = s ? s + 1 : "";
    if (s && !*rr) return -1;
    if (strlen(rr) >= rn || !cloud_name_ok(name) || !cloud_rel_ok(rr)) return -1;
    snprintf(rel, rn, "%s", rr);
    return 0;
}

int cloud_key_ok(const char *k) {
    size_t n = k ? strlen(k) : 0;
    if (!n || n > 64) return 0;
    for (; *k; k++) if (!((*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_')) return 0;
    return 1;
}

int cloud_value_ok(const char *v) { return v && text_ok(v, CLOUD_VALUE_MAX); }

int cloud_kept(const char *const *kept, int n, const char *rel) {
    for (int i = 0; i < n; i++) {
        size_t l = strlen(kept[i]);
        if (!l) return 1;                                       /* the whole account */
        if (!strncmp(rel, kept[i], l) && (rel[l] == 0 || rel[l] == '/')) return 1;
    }
    return 0;
}

/* ---- providers ---- */

/* The overlay on rclone's own list (docs/CLOUD.md 17.2): what rclone can't say. Labels are rclone's names (its
 * providers' Description) and are used only until rclone has listed them, except for the variants (a preset on a
 * backend: Nextcloud and ownCloud on WebDAV), which are ours. group: only where rclone's options mislead the rule in
 * cloud_derive_group. */
static const CloudKind kinds[] = {
    { "drive", "drive", "Google Drive", "oauth", NULL, NULL, NULL, "https://drive.google.com/", NULL, 0 },
    { "onedrive", "onedrive", "Microsoft OneDrive", "oauth", NULL, NULL, NULL, "https://onedrive.live.com/", NULL, 0 },
    { "dropbox", "dropbox", "Dropbox", "oauth", NULL, NULL, NULL, "https://www.dropbox.com/home", NULL, 0 },
    { "box", "box", "Box", "oauth", NULL, NULL, NULL, "https://app.box.com/", NULL, 0 },
    { "pcloud", "pcloud", "Pcloud", "oauth", NULL, NULL, NULL, "https://my.pcloud.com/", NULL, 0 },
    { "yandex", "yandex", "Yandex Disk", "oauth", NULL, NULL, NULL, "https://disk.yandex.com/", NULL, 0 },
    { "zoho", "zoho", "Zoho", "oauth", NULL, NULL, NULL, "https://workdrive.zoho.com/", NULL, 0 },
    { "hidrive", "hidrive", "HiDrive", "oauth", NULL, NULL, NULL, "https://hidrive.ionos.com/", NULL, 0 },
    { "putio", "putio", "Put.io", "oauth", NULL, NULL, NULL, "https://app.put.io/", NULL, 0 },
    { "gcs", "google cloud storage", "Google Cloud Storage (this is not Google Drive)", "oauth", NULL, NULL, NULL, "https://console.cloud.google.com/storage/browser", NULL, 0 },
    { "jottacloud", "jottacloud", "Jottacloud", "server", NULL, NULL, NULL, "https://www.jottacloud.com/web", NULL, 0 },
    { "mailru", "mailru", "Mail.ru Cloud", "server", NULL, NULL, NULL, "https://cloud.mail.ru/", NULL, 0 },
    { "webdav", "webdav", "WebDAV", "server", NULL, NULL, NULL, NULL, "url", 0 },
    { "nextcloud", "webdav", "Nextcloud", "server", "Your Nextcloud's address, ending in /remote.php/dav/files/<user>", "\"vendor\":\"nextcloud\"", "vendor", NULL, "url", 1 },
    { "owncloud", "webdav", "ownCloud", "server", "Your ownCloud's address, ending in /remote.php/webdav", "\"vendor\":\"owncloud\"", "vendor", NULL, "url", 1 },
    { "sftp", "sftp", "SSH/SFTP", "server", NULL, "\"shell_type\":\"none\"", "shell_type", NULL, NULL, 0 },
    { "mega", "mega", "Mega", "server", NULL, NULL, NULL, "https://mega.nz/fm", NULL, 0 },
    { "protondrive", "protondrive", "Proton Drive", "server", NULL, NULL, NULL, "https://drive.proton.me/", NULL, 0 },
    { "koofr", "koofr", "Koofr, Digi Storage and other Koofr-compatible storage providers", "server", NULL, NULL, NULL, NULL, NULL, 0 },
    { "seafile", "seafile", "seafile", "server", NULL, NULL, NULL, NULL, "url", 0 },
    { "s3", "s3", "Amazon S3 Compliant Storage Providers", "object", NULL, NULL, NULL, NULL, "endpoint", 0 },
    { "b2", "b2", "Backblaze B2", "object", NULL, NULL, NULL, "https://secure.backblaze.com/b2_buckets.htm", NULL, 0 },
    { "azureblob", "azureblob", "Microsoft Azure Blob Storage", "object", NULL, NULL, NULL, "https://portal.azure.com/", NULL, 0 },
    { "azurefiles", "azurefiles", "Microsoft Azure Files", "object", NULL, NULL, NULL, "https://portal.azure.com/", NULL, 0 },
    { "swift", "swift", "OpenStack Swift", "object", NULL, NULL, NULL, NULL, NULL, 0 },
    { "oos", "oracleobjectstorage", "Oracle Cloud Infrastructure Object Storage", "object", NULL, NULL, NULL, NULL, NULL, 0 },
    { "crypt", "crypt", "Encrypt/Decrypt a remote", "crypt", "Names and contents encrypted before they leave this computer", NULL, NULL, NULL, NULL, 0 },
};

int cloud_kind_count(void) { return (int)(sizeof kinds / sizeof kinds[0]); }
const CloudKind *cloud_kind_at(int i) { return i >= 0 && i < cloud_kind_count() ? &kinds[i] : NULL; }
const CloudKind *cloud_kind(const char *kind) {
    for (int i = 0; kind && i < cloud_kind_count(); i++) if (!strcmp(kinds[i].kind, kind)) return &kinds[i];
    return NULL;
}

/* not a storage of its own (docs/CLOUD.md 17.2): wrappers, the local disk, memory, plain HTTP */
int cloud_excluded(const char *name) {
    static const char *const no[] = { "alias", "archive", "cache", "chunker", "combine", "compress", "hasher", "http", "local",
                                      "memory", "union", "tardigrade", NULL };
    for (int i = 0; name && no[i]; i++) if (!strcmp(no[i], name)) return 1;
    return !name || !name[0];
}

const char *cloud_derive_group(const char *name, const char *const *opts, int n) {
    int tok = 0, cid = 0, csec = 0, obj = 0;
    if (name && !strcmp(name, "crypt")) return "crypt";
    for (int i = 0; i < n; i++) {
        const char *o = opts[i];
        tok |= !strcmp(o, "token");
        cid |= !strcmp(o, "client_id");
        csec |= !strcmp(o, "client_secret");
        obj |= !strcmp(o, "access_key_id") || !strcmp(o, "secret_access_key") || !strcmp(o, "endpoint") || !strcmp(o, "sas_url") ||
               !strcmp(o, "storage_url") || !strcmp(o, "bucket") || !strcmp(o, "bucket_name") || !strcmp(o, "application_key");
    }
    if (tok && cid && csec) return "oauth";         /* a browser sign-in: rclone's OAuth options */
    return obj ? "object" : "server";
}

int cloud_internal_question(const char *opt) {
    return opt && (!strcmp(opt, "config_refresh_token") || !strcmp(opt, "config_is_local"));
}

int cloud_answer_state(const char *s, char *id, size_t n, const char **state) {
    const char *c = s ? strchr(s, ':') : NULL;
    if (!c || c == s || (size_t)(c - s) >= n || !c[1] || strlen(c + 1) > 512) return -1;
    memcpy(id, s, (size_t)(c - s));
    id[c - s] = 0;
    if (!cloud_key_ok(id) || id[0] != 'c') return -1;
    for (const char *p = c + 1; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f) return -1;
    *state = c + 1;
    return 0;
}

int cloud_field_hidden(const CloudKind *k, const char *f) {
    if (!k->hidden) return 0;
    size_t l = strlen(f);
    for (const char *p = k->hidden; *p; ) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !strncmp(p, f, n)) return 1;
        p += n + (e ? 1 : 0);
    }
    return 0;
}

int cloud_url_ok(const char *u) {
    size_t n = u ? strlen(u) : 0;
    if (n < 9 || n > 300 || strncmp(u, "https://", 8) || u[8] == '/' || u[8] == '-') return 0;
    for (; *u; u++) if (*u <= ' ' || *u >= 0x7f || *u == '"' || *u == '\'' || *u == '\\' || *u == '<' || *u == '>' || *u == '`') return 0;
    return 1;
}

int cloud_site_url(const CloudKind *k, const char *field, char *out, size_t n) {
    if (!k) return 0;
    const char *u = k->webfield ? field : k->website;
    if (!u || !cloud_url_ok(u) || strlen(u) >= n) return 0;
    snprintf(out, n, "%s", u);
    return 1;
}

/* ---- signing in ---- */

int cloud_auth_url(const char *line, char *url, size_t n) {
    static const char pre[] = "http://127.0.0.1:53682/auth?state=";
    const char *p = line ? strstr(line, "Please go to the following link: ") : NULL;
    if (!p) return 0;
    p += strlen("Please go to the following link: ");
    if (strncmp(p, pre, sizeof pre - 1)) return 0;
    const char *s = p + sizeof pre - 1, *e = s;
    while (isalnum((unsigned char)*e) || *e == '-' || *e == '_') e++;
    if (e - s < 8 || e - s > 128 || (*e && *e != '\n' && *e != '\r' && *e != ' ')) return 0;
    size_t l = (size_t)(e - p);
    if (l >= n) return 0;
    memcpy(url, p, l);
    url[l] = 0;
    return 1;
}

int cloud_auth_token(const char *out, char *tok, size_t n) {
    const char *a = out ? strstr(out, "--->") : NULL, *b;
    a = a ? a + 4 : out;
    if (!a) return 0;
    b = strstr(a, "<---End paste");
    const char *s = strchr(a, '{');
    if (!s || (b && s > b)) return 0;
    const char *e = b ? b : s + strlen(s);
    while (e > s && e[-1] != '}') e--;
    if (e <= s) return 0;
    size_t l = (size_t)(e - s);
    if (l >= n || l > 16384) return 0;
    char *t = malloc(l + 1);
    if (!t) return 0;
    memcpy(t, s, l);
    t[l] = 0;
    JDoc d;
    int ok = !json_parse(t, l, &d, NULL, 0);
    if (ok) {
        ok = d.root->type == J_OBJ && (json_str(d.root, "access_token") || json_str(d.root, "refresh_token"));
        json_free(&d);
    }
    for (size_t i = 0; ok && i < l; i++) if ((unsigned char)t[i] < 0x20) ok = 0;   /* one line in rclone.conf */
    if (ok) memcpy(tok, t, l + 1);
    explicit_bzero(t, l);
    free(t);
    return ok;
}

const char *cloud_default_answer(const char *opt, const char *def) {
    if (!opt) return def ? def : "";
    if (!strcmp(opt, "config_refresh_token")) return "false";          /* the token we just gave it */
    if (!strcmp(opt, "config_shared_client_id")) return "true";        /* 6.2: rclone's own IDs in 0.8.8.5 */
    if (!strcmp(opt, "config_change_team_drive")) return "false";
    if (!strcmp(opt, "config_is_local")) return "false";
    return def ? def : "";
}

/* ---- options ---- */

int cloud_bw_rate(long up, long down, char *out, size_t n) {
    if (up < 0 || down < 0 || up > CLOUD_BW_MAX || down > CLOUD_BW_MAX) return -1;
    char a[24] = "off", b[24] = "off";
    if (up) snprintf(a, sizeof a, "%ldKi", up);
    if (down) snprintf(b, sizeof b, "%ldKi", down);
    int r = up || down ? snprintf(out, n, "%s:%s", a, b) : snprintf(out, n, "off");
    return r < 0 || (size_t)r >= n ? -1 : 0;
}

int cloud_expire(const char *e, char *arg, size_t n, long long *secs) {
    static const struct { const char *k, *v; long long s; } t[] = { { "1d", "1d", 86400 }, { "7d", "7d", 7 * 86400 }, { "30d", "30d", 30 * 86400 }, { "never", "", 0 } };
    for (size_t i = 0; e && i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(e, t[i].k)) { snprintf(arg, n, "%s", t[i].v); *secs = t[i].s; return 0; }
    return -1;
}

/* ---- files on demand ---- */

static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

long long cloud_rfc3339(const char *s) {
    int Y, M, D, h, m, sec, n = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &Y, &M, &D, &h, &m, &sec, &n) != 6 || n != 19) return -1;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return -1;
    const char *p = s + 19;
    if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++; }
    long long off = 0;
    if (*p == 'Z') p++;
    else if (*p == '+' || *p == '-') {
        int oh, om;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) != 2) return -1;
        off = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
        p += 6;
    } else return -1;
    if (*p) return -1;
    return days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + sec - off;
}

int cloud_meta_parse(const char *json, size_t len, CloudMeta *m) {
    JDoc d;
    memset(m, 0, sizeof *m);
    if (json_parse(json, len, &d, NULL, 0)) return -1;
    int r = -1;
    const JVal *o = d.root;
    const JVal *sz = json_get(o, "Size");
    if (o->type == J_OBJ && sz && sz->type == J_NUM && sz->num >= 0) {
        m->size = (long long)sz->num;
        m->dirty = json_bool(o, "Dirty", 0);
        const char *at = json_str(o, "ATime");
        m->atime = at ? cloud_rfc3339(at) : -1;
        const JVal *rs = json_get(o, "Rs");
        r = 0;
        if (rs && rs->type == J_ARR)
            for (const JVal *x = rs->kid; x; x = x->next) {
                double s = json_num(x, "Size", -1);
                if (s < 0) { r = -1; break; }
                m->cached += (long long)s;
            }
        else if (rs && rs->type != J_NULL) r = -1;
        if (m->cached > m->size) m->cached = m->size;
    }
    json_free(&d);
    return r;
}

const char *cloud_file_status(const CloudMeta *m, const CloudLive *l, double *pct) {
    *pct = -1;
    if (l->error) return "error";
    if (l->uploading) { *pct = l->up_pct; return "up"; }
    if (l->queued || (m && m->dirty)) { *pct = 0; return "up"; }
    if (l->downloading) { *pct = l->down_pct; return "down"; }
    int whole = m && m->cached >= m->size;
    if (whole) return l->kept ? "kept" : "here";
    if (l->kept) { *pct = m && m->size ? 100.0 * (double)m->cached / (double)m->size : 0; return "down"; }   /* still coming */
    return "cloud";
}

int cloud_evict_why(const CloudCand *c) {
    if (c->kept) return CE_KEPT;
    if (c->dirty || c->queued) return CE_PENDING;
    if (c->open) return CE_OPEN;
    return CE_OK;
}

const char *cloud_skip_name(int why) { return why == CE_OPEN ? "open" : why == CE_PENDING ? "pending" : why == CE_KEPT ? "kept" : ""; }

int cloud_evict_lru(const CloudCand *c, int n, long long need, int *pick) {
    int k = 0;
    long long got = 0;
    for (int i = 0; i < n; i++) if (cloud_evict_why(&c[i]) == CE_OK && c[i].bytes > 0) pick[k++] = i;
    /* insertion sort by last use, oldest first (a few thousand at most per pass) */
    for (int i = 1; i < k; i++) {
        int v = pick[i], j = i - 1;
        while (j >= 0 && c[pick[j]].atime > c[v].atime) { pick[j + 1] = pick[j]; j--; }
        pick[j + 1] = v;
    }
    int used = 0;
    while (used < k && got < need) got += c[pick[used++]].bytes;
    return used;
}
