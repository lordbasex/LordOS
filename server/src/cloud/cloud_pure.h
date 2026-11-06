/* cloud_pure.h — the parts of Cloud (docs/CLOUD.md) that touch no socket, process or file: the closed table of rc
 * methods and the HTTP request built for one, argument and path checks, the provider table, the website table,
 * reading `rclone authorize`'s output, the VFS cache metadata, the per-file status and the eviction choice.
 * Everything here is tested by tests/cloud_test.c. */
#ifndef LORD_CLOUD_PURE_H
#define LORD_CLOUD_PURE_H

#include <stddef.h>

/* the binary of the lordos-rclone package; Connect to Server uses it too (lan/rconnect.c) */
#define CLOUD_RCLONE "/usr/lib/lordos/rclone/rclone"
/* firewall app desktop:lordos-cloud, scope app-lordos-lordos-cloud.scope (reserved: sec/cgroup.h app_scope_reserved) */
#define CLOUD_SCOPE "lordos-cloud"

/* ---- the closed rc method table (4.2) ----
 * A method is named by this enum only; its path comes from a fixed table. Nothing takes a method name as text, so
 * core/command, config/dump, config/show, options/set, every rc/ method and every operations/ method outside this list can't be sent. */
typedef enum {
    RCM_VERSION,        /* core/version */
    RCM_PROVIDERS,      /* config/providers */
    RCM_CREATE,         /* config/create */
    RCM_UPDATE,         /* config/update */
    RCM_DELETE,         /* config/delete */
    RCM_GET,            /* config/get: one remote, to write a refreshed token back to the keyring */
    RCM_MOUNT,          /* mount/mount */
    RCM_UNMOUNT,        /* mount/unmount */
    RCM_LISTMOUNTS,     /* mount/listmounts */
    RCM_ABOUT,          /* operations/about */
    RCM_PUBLICLINK,     /* operations/publiclink */
    RCM_FSINFO,         /* operations/fsinfo: whether the provider has public links */
    RCM_STATS,          /* core/stats */
    RCM_TRANSFERRED,    /* core/transferred: the last changes */
    RCM_BWLIMIT,        /* core/bwlimit */
    RCM_VFS_STATS,      /* vfs/stats */
    RCM_VFS_QUEUE,      /* vfs/queue */
    RCM_VFS_EXPIRY,     /* vfs/queue-set-expiry: Pause holds uploads back */
    RCM_VFS_REFRESH,    /* vfs/refresh */
    RCM_VFS_FORGET,     /* vfs/forget */
    RCM_QUIT,           /* core/quit */
    RCM_COUNT
} RcMethod;

const char *rc_method_path(int m);   /* NULL outside the table */

/* "POST /<path> HTTP/1.0" with Basic auth and a JSON body of blen bytes: the header only, into out. Bytes, or -1 (no
 * such method, the auth isn't base64, or it doesn't fit). */
int  rc_build_header(char *out, size_t cap, int m, const char *auth_b64, size_t blen);
/* One whole answer (HTTP/1.0 or 1.1, Content-Length, chunked or to the end): status and body, decoded in place.
 * 0, or -1 if it isn't HTTP. */
int  rc_parse_response(char *buf, size_t len, int *status, char **body, size_t *blen);
void cloud_b64(const unsigned char *in, size_t n, char *out);   /* out: 4 * ((n + 2) / 3) + 1 */

/* ---- names and paths ---- */
#define CLOUD_NAME_MAX 64           /* bytes of an account's name: its folder in ~/Cloud */
#define CLOUD_REL_MAX 1024
#define CLOUD_VALUE_MAX 2048
int  cloud_name_ok(const char *name);        /* a folder name: no '/', no leading '.', no control characters, UTF-8 */
int  cloud_rel_ok(const char *rel);          /* inside an account: "" (its top), no '/' at either end, no "." or ".." part, no "//" */
/* path = <root>/<name>[/<rel>], root being ~/Cloud: 0 and the two parts, -1 otherwise */
int  cloud_split(const char *root, const char *path, char *name, size_t nn, char *rel, size_t rn);
int  cloud_key_ok(const char *key);          /* an rclone option name: [a-z0-9_], 1 to 64 */
int  cloud_value_ok(const char *v);          /* a field's value: no control characters, at most CLOUD_VALUE_MAX */
int  cloud_kept(const char *const *kept, int n, const char *rel);   /* rel is a kept folder or file, or inside one */

/* ---- providers (6.1) ---- */
typedef struct {
    const char *kind;       /* ours: what JS names */
    const char *type;       /* rclone's backend */
    const char *label;      /* shown as text in the provider list: plain names, no other operating system's names */
    const char *group;      /* oauth | server | object | crypt */
    const char *hint;
    const char *preset;     /* fixed parameters, JSON object members ("\"vendor\":\"nextcloud\""), or NULL */
    const char *hidden;     /* fields the preset sets, comma separated, not shown */
    const char *website;    /* the provider's page, or NULL */
    const char *webfield;   /* or the account's own address in this field, only if https */
} CloudKind;
int              cloud_kind_count(void);
const CloudKind *cloud_kind_at(int i);
const CloudKind *cloud_kind(const char *kind);
int              cloud_field_hidden(const CloudKind *k, const char *field);
/* the page of an account: 1 and out, 0 none. field: the value of k->webfield (NULL if none). */
int  cloud_site_url(const CloudKind *k, const char *field, char *out, size_t n);
int  cloud_url_ok(const char *url);          /* https://, printable ASCII without quotes, spaces or backslashes, at most 300 */

/* ---- signing in (6.2): rclone authorize <type> --auth-no-open-browser ---- */
/* a line of its log: 1 and the address (http://127.0.0.1:53682/auth?state=…, nothing else accepted) */
int  cloud_auth_url(const char *line, char *url, size_t n);
/* its stdout: 1 and the token (one JSON object) */
int  cloud_auth_token(const char *out, char *tok, size_t n);
/* config/create's next question asked non-interactively: what to answer (the default, except these) */
const char *cloud_answer(const char *option, const char *def);

/* ---- options ---- */
#define CLOUD_BW_MAX (10L * 1024 * 1024)        /* KiB/s */
int  cloud_bw_rate(long up_kib, long down_kib, char *out, size_t n);   /* core/bwlimit's rate ("off", "512Ki:off"): 0, -1 out of range */
/* '1d' | '7d' | '30d' | 'never' → publiclink's expire ("" for never) and its seconds (0); -1 anything else */
int  cloud_expire(const char *e, char *arg, size_t n, long long *secs);

/* ---- files on demand (7.2) ---- */
typedef struct {
    long long size;         /* of the file */
    long long cached;       /* bytes present in the cache (the ranges) */
    long long atime;        /* last use, seconds */
    int dirty;              /* changed here, not uploaded */
} CloudMeta;
long long cloud_rfc3339(const char *s);       /* seconds since the epoch, -1 */
int  cloud_meta_parse(const char *json, size_t len, CloudMeta *m);   /* a vfsMeta file: 0 */

typedef struct {
    int queued;             /* in vfs/queue */
    int uploading;          /* being sent: core/stats with dstFs */
    double up_pct;
    int downloading;        /* coming down: core/stats with srcFs */
    double down_pct;
    int kept;               /* Keep on This Computer */
    int error;              /* the last transfer of it failed */
} CloudLive;
/* "cloud" | "down" | "here" | "kept" | "up" | "error"; pct (when it's a transfer or a kept file still coming) or -1 */
const char *cloud_file_status(const CloudMeta *m, const CloudLive *l, double *pct);

enum { CE_OK = 0, CE_OPEN, CE_PENDING, CE_KEPT };
typedef struct {
    long long bytes, atime;
    int open, dirty, queued, kept;
} CloudCand;
int  cloud_evict_why(const CloudCand *c);                   /* CE_*: only CE_OK may go */
const char *cloud_skip_name(int why);                      /* "open" | "pending" | "kept" */
/* the automatic pass: the least recently used that may go, oldest first, until need bytes; their indexes in pick */
int  cloud_evict_lru(const CloudCand *c, int n, long long need, int *pick);

#endif
