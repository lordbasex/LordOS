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
    RCM_CLEANUP,        /* operations/cleanup: Empty Trash (17.2) */
    RCM_SIZE,           /* operations/size, as an _async job: Measure (17.2) */
    RCM_JOB_STATUS,     /* job/status of those two */
    RCM_JOB_STOP,       /* job/stop: cancel a Measure */
    RCM_LIST,           /* operations/list of an account's top folder only: a new account is checked before it counts */
    RCM_STAT,           /* operations/stat of one file about to go up: the conflict rule (7.2) */
    RCM_COPYFILE,       /* operations/copyfile, inside one account only: the cloud's version kept before ours replaces it */
    RCM_FSCACHE_CLEAR,  /* fscache/clear: FTP and SMB keep a dead connection after the server restarts (B18) */
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
int  cloud_value_ok(const char *v);
void cloud_session_summary(const char *trust, const char *cookies, char *out, size_t n);   /* lengths and names only */          /* a field's value: no control characters, at most CLOUD_VALUE_MAX */
int  cloud_kept(const char *const *kept, int n, const char *rel);   /* rel is a kept folder or file, or inside one */
/* #71: a folder still coming down as a whole: rel is inside a kept entry whose files aren't all here yet (busy[i]), or
 * holds one. Its glyph stays "coming down" between one file and the next instead of flipping to kept. */
int  cloud_kept_busy(const char *const *kept, const unsigned char *busy, int n, const char *rel);

/* ---- providers (6.1) ---- */
typedef struct {
    const char *kind;       /* ours: what JS names */
    const char *type;       /* rclone's backend */
    const char *label;      /* rclone's name for it (or ours for a variant), plain text in the provider list only */
    const char *group;      /* oauth | server | object | crypt */
    const char *hint;
    const char *preset;     /* fixed parameters, JSON object members ("\"vendor\":\"nextcloud\""), or NULL */
    const char *hidden;     /* fields the preset sets, comma separated, not shown */
    const char *website;    /* the provider's page, or NULL */
    const char *webfield;   /* or the account's own address in this field, only if https */
    int variant;            /* ours: a preset on rclone's backend `type` (Nextcloud on WebDAV), not one of rclone's own */
} CloudKind;
int              cloud_kind_count(void);
const CloudKind *cloud_kind_at(int i);
const CloudKind *cloud_kind(const char *kind);
int              cloud_field_hidden(const CloudKind *k, const char *field);
int              cloud_excluded(const char *name);      /* a backend that isn't a storage of its own (alias, union…) */
/* oauth (rclone's token, client_id and client_secret options), object (keys, endpoint, bucket), crypt, else server */
const char      *cloud_derive_group(const char *name, const char *const *opts, int n);
int              cloud_internal_question(const char *opt);
/* an rclone option JS may never set (4.2): it runs a program, reads or writes a local file, folder or socket, reaches the
 * environment or another program's credentials, or is one of rclone's config_ internals; and every type that isn't plain
 * data (a list given to a shell, …) is refused unless known. type: rclone's Type ("string", "bool", …), NULL unknown. */
int              cloud_option_refused(const char *name, const char *type);
/* rclone's words when an account didn't work → cloud.authFailed | cloud.unreachable | cloud.badAddress | NULL (other) */
const char      *cloud_failure_code(const char *msg);
/* #85: an account whose provider doesn't answer now (err is the account's cloud.* code): nothing walks its folder */
int              cloud_err_offline(const char *err);
/* #85: what a call through the mount failed with (errno) means the provider didn't answer (rclone's "IO error": EIO, or a
 * time out), not that the file is gone: the keeper stops its pass there instead of waiting on every folder */
int              cloud_mount_gave_up(int err);
/* a message fit for Get Info: HTML tags dropped, white space collapsed, cut at a character boundary to fit n (≤ n-1 bytes) */
void             cloud_clean_text(const char *in, char *out, size_t n);
/* the conflict rule (7.2): did the object in the cloud change since this copy came down? fingerprint as kept in vfsMeta,
 * the object now (size, modtime in seconds or < 0 unknown, its hashes). 1 changed, 0 the same, -1 can't tell */
int              cloud_remote_changed(const char *fingerprint, long long size, double mtime, const char *const *hashes, int nh, int precision_s);
/* "dir/name (cloud copy).ext", n > 1 adds " n" before the extension: 0, -1 too long */
/* the server an account talks to, for the state (not a secret there): host into h, the port returned, 0 when none
 * (an OAuth provider, or S3 on the provider's own endpoint). host/port/url/endpoint/tls as the account's fields */
int              cloud_server_of(const char *type, const char *host, const char *port, const char *url, const char *endpoint,
                                 const char *tls, char *h, size_t hn);
int              cloud_conflict_name(const char *rel, int n, char *out, size_t cap);   /* config/create's questions LordOS answers itself */
/* a question's state as JS sees it, "<account id>:<rclone's state>": 0 and both parts */
int              cloud_answer_state(const char *s, char *id, size_t n, const char **state);
/* the page of an account: 1 and out, 0 none. field: the value of k->webfield (NULL if none). */
int  cloud_site_url(const CloudKind *k, const char *field, char *out, size_t n);
int  cloud_url_ok(const char *url);          /* https://, printable ASCII without quotes, spaces or backslashes, at most 4096 (presigned links) */

/* ---- signing in (6.2): rclone authorize <type> --auth-no-open-browser ---- */
/* a line of its log: 1 and the address (http://127.0.0.1:53682/auth?state=…, nothing else accepted) */
int  cloud_auth_url(const char *line, char *url, size_t n);
/* its stdout: 1 and the token (one JSON object). Started with a config blob ("{}" in base64) as its second argument,
 * rclone prints every option the sign-in set (configmap.Simple.Encode: base64 of a JSON object), not the token alone:
 * pCloud's API host for the account's region (eapi.pcloud.com in Europe), which only comes back with the redirect.
 * Those options go into opts as JSON members after the token (",\"hostname\":\"…\"", "" when none); a client's id
 * or secret and any option refused by name (cloud_option_refused) are left out. */
int  cloud_auth_token(const char *out, char *tok, size_t n, char *opts, size_t on);
/* config/create's next question asked non-interactively: what to answer (the default, except these) */
const char *cloud_default_answer(const char *option, const char *def);
/* #87: a choice rclone offers that may not work, the drive of a browser sign-in (OneDrive's config_driveid lists every
 * drive /me/drives gives, a stale one sometimes first, and the account's own drive last). When the next step fails and
 * rclone's state machine goes back (its ConfigError), the choice is made again with one not tried yet. One run of
 * answer_states keeps one CloudChoices (zeroed). */
#define CLOUD_CHOICES_MAX 8
#define CLOUD_CHOICE_RETRIES 4
typedef struct {
    char name[64];                  /* the question whose choices are tried */
    int n;                          /* how many it offers (at most CLOUD_CHOICES_MAX count) */
    char tried[CLOUD_CHOICES_MAX][256];
    int ntried;
    int last;                       /* the last answer given was this question's */
    int retries;
} CloudChoices;
/* the answer to give: ans (the default, cloud_default_answer's) unless the question was asked again after a failure, then
 * the first example not tried yet. ex: the question's example values, nex of them. */
const char *cloud_choose(CloudChoices *c, const char *name, const char *ans, const char *const *ex, int nex);
/* rclone answered an error that goes back to a state: 1 to go there again (another choice remains), 0 to give up */
int         cloud_choice_retry(CloudChoices *c);
/* rclone's words when a sign-in worked but the account has no drive LordOS can use (OneDrive: none listed, or none that
 * opens): cloud.signinNoDrive instead of cloud.signinFailed */
int         cloud_no_drive_msg(const char *msg);

/* ---- options ---- */
#define CLOUD_BW_MAX (10L * 1024 * 1024)        /* KiB/s */
int  cloud_bw_rate(long up_kib, long down_kib, char *out, size_t n);   /* core/bwlimit's rate ("off", "512Ki:off"): 0, -1 out of range */
/* '1d' | '7d' | '30d' | 'never' → publiclink's expire ("" for never) and its seconds (0); -1 anything else */
int  cloud_expire(const char *e, char *arg, size_t n, long long *secs);

/* ---- browsing (7.3) ----
 * rclone's VFS lists a folder from the provider when it is first read and again on the first read after its directory
 * cache expires; that listing holds the reader (Files) for the whole round trip. The mounts keep listings an hour
 * (CLOUD_DIR_CACHE_S) and stay current otherwise: the provider's change notifications polled every minute where rclone
 * has them (ChangeNotify), elsewhere a folder Files shows listed again in the background once it is 5 minutes old, kept
 * folders by the keeper. */
#define CLOUD_DIR_CACHE_S 3600
#define CLOUD_POLL_S 60
#define CLOUD_STALE_S 300             /* a folder shown, on a provider without change notifications, listed again after this */
#define CLOUD_TREE_POLL_S 1800        /* kept folders listed whole again, on a provider with change notifications */
#define CLOUD_AHEAD_MAX 24            /* subfolders of a folder shown listed ahead, at most */
/* mount/mount's "vfsOpt" and "mountOpt" members (a leading comma, no braces around them): 0, -1 doesn't fit */
int  cloud_mount_opts(char *out, size_t n);
/* the folders listed in this run of an account's mount: when, by a 64-bit key of account, mount and folder */
#define CLOUD_SEEN_MAX 2048
typedef struct { unsigned long long key; long long at; } CloudSeenItem;
typedef struct { CloudSeenItem it[CLOUD_SEEN_MAX]; int n; } CloudSeen;
unsigned long long cloud_seen_key(const char *id, unsigned gen, const char *rel);
long long cloud_seen_at(const CloudSeen *s, unsigned long long key);      /* ms, -1 never */
void cloud_seen_put(CloudSeen *s, unsigned long long key, long long now); /* full: the oldest goes */
/* a folder Files shows: 1 list it again in the background now. seen: when it was last listed (ms, -1 never: it was just
 * listed by Files itself), polls: the provider sends changes, kept: the keeper lists it */
int  cloud_browse_due(long long seen, long long now, int polls, int kept);

/* ---- files on demand (7.2) ---- */
typedef struct {
    long long size;         /* of the file */
    long long cached;       /* bytes present in the cache (the ranges) */
    long long atime;        /* last use, seconds */
    int dirty;              /* changed here, not uploaded */
    char fingerprint[256];  /* rclone's of the object as it was when it came down: "size[,modtime][,hash]" */
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
