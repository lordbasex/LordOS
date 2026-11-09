/* cloud_int.h — shared between cloud.c (accounts, rclone, the thread) and cloud_files.c (status, eviction, keeping). */
#ifndef LORD_CLOUD_INT_H
#define LORD_CLOUD_INT_H

#include "cloud.h"
#include "cloud_pure.h"
#include "cloud_rc.h"

#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#define ACC_MAX 32
#define KV_MAX 48
#define Q_MAX 256
#define T_MAX 64
#define KEEP_MAX 128
#define ERRN_MAX 16

typedef struct { char key[65]; char *val; } KV;
typedef struct { KV kv[KV_MAX]; int n; } Params;

typedef struct { char *name; long long size; double expiry; int uploading, tries, qid; } QItem;
typedef struct { char *name; long long bytes, size; double pct; int up; } TItem;

typedef struct {
    int used;
    char id[16], name[CLOUD_NAME_MAX + 1];
    const CloudKind *k;
    long long created;
    int paused, own_client;
    Params p;                       /* non-secret options (what ~/.config/lordos/cloud/rclone.conf has) */
    /* this run of rclone */
    int configured, mounted, held;  /* held: Pause is holding its uploads back */
    int locked;                     /* 1 keyring locked, 2 no keyring */
    int fresh_tried;
    char sess[400];                 /* the last session summary logged (cloud_session_summary) */                /* a refused saved session was dropped once this run (mount_acc) */
    char err[96];                   /* a cloud.* code, or "" */
    char message[256];              /* rclone's own words, for Get Info */
    int have_about;
    double ab[5];                   /* total used free trashed other; < 0 not given */
    long long cache_bytes, kept_bytes;
    QItem q[Q_MAX];
    int nq;
    TItem t[T_MAX];
    int nt;
    int in_use;
    int links_supported;            /* -1 not known yet */
    int trash_supported;            /* operations/cleanup (rclone's CleanUp feature); -1 not known yet */
    long long measured_bytes, measured_count, measured_at;   /* Measure (17.2 item 5); at in ms, 0 never */
    char rcjob_kind;                /* 'm' measure, 't' empty trash, 0 none: one async rclone job at a time */
    long long rcjob_id;
    int rcjob_req;
    int pending_req;                /* add()/answer() waiting for config/create's next step */
    int new_acc;                    /* added in this session and not finished: removed if its setup fails */
    char trust_kind;                /* 'h' host keys, 'H' to replace the kept ones, 'c' a certificate, shown and waiting (cloud_trust.c) */
    int trust_from_other;           /* this add took another account's known key for the same server (N1) */
    int reconnect;                  /* its remote is made again (a new key kept for it, N4) */
    char fp_old[256], fp_new[256];  /* a key or certificate that changed: both, for the state (U10) */
    time_t pending_since;
    char *trust_data;
    char *pend_sec;                 /* add's secrets while that question waits (wiped) */
    char *pend_tsec;                /* the same over the kept ones, for the checks (B15) */
    int pend_upd;
    time_t check_at;                /* the next listing that checks a mounted account still answers */
    char *checked[Q_MAX];           /* queued files the conflict rule already looked at (7.2) */
#define SYNCED_MAX 64
    struct { char rel[CLOUD_REL_MAX + 1]; char fp[256]; } synced[SYNCED_MAX];   /* our last uploads, as the cloud has them (N5) */
    int nsynced;
    char srv_host[256];             /* the server it talks to, for the state: N4's "other accounts on this server" */
    int srv_port;
    int precision_s;                /* the backend's modtime precision, seconds (fsinfo) */
    int polls;                      /* the backend has change notifications (fsinfo's ChangeNotify): rclone polls them */
    unsigned mount_gen;             /* +1 at each mount: what rclone listed before is gone */
    unsigned listed;                /* +1 when a folder Files shows was listed again in the background (7.3) */
    unsigned tree_gen;              /* the keeper listed the kept folders whole in this mount… */
    time_t tree_at;                 /* …at this time */
    int nchecked;
    unsigned long long tok_hash;    /* of the secrets last saved: a refreshed token changes it */
    char *pending_secret;           /* not saved yet (keyring locked): retried */
    time_t about_at, tok_at, retry_at;
    char *keep[KEEP_MAX];
    unsigned char keep_busy[KEEP_MAX];   /* #71: keep[i]'s files aren't all here yet (set when kept, or when the keeper
                                          * finds one missing; cleared when a pass of the keeper got them all) */
    int nkeep;
    char *errn[ERRN_MAX];           /* files whose last transfer failed */
    int nerrn;
} Acc;

extern pthread_mutex_t cl_mu;
extern Acc cl_acc[ACC_MAX];
extern char cl_home[PATH_MAX], cl_root[PATH_MAX], cl_cache[PATH_MAX], cl_cfg[PATH_MAX];
extern RcConn cl_rc;
extern int cl_running;

/* snprintf for paths and JSON pieces: the same result, through a function (gcc's truncation warnings judge only direct calls;
 * every caller here checks lengths where it matters) */
int   cl_pf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

Acc  *cl_find(const char *id);              /* with cl_mu held */
Acc  *cl_find_name(const char *name);
void  cl_changed(void);                     /* push the state soon */
int   cl_paused(const Acc *a);              /* Pause, Pause All or on battery */
void  cl_save_keep(void);                   /* with cl_mu held */
int   cl_rc_simple(int m, const char *body, int timeout);   /* an rc call whose answer doesn't matter: its status */
void  cl_reply(int req, char *json);        /* an asynchronous answer for JS (takes json) */
int   cl_next_req(void);

/* cloud_trust.c */
int   ct_needed(const Acc *a, const char *sec);
int   ct_check(Acc *a, const char *sec, char **question, char *code, size_t cn, char *msg, size_t mn);   /* 0 ok, 1 a question, -1 refused */
int   ct_accept(Acc *a, int apply_others);     /* apply_others: a new key also for the accounts that kept the same old one (N4) */
void  ct_fingerprints(Acc *a, const char *sec, char *old, size_t on, char *now, size_t nn);   /* the kept and (one probe) the server's */
int   ct_key_mismatch(Acc *a, const char *sec, char **question, char *code, size_t cn, char *msg, size_t mn);
int   ct_probed_lately(const Acc *a);
void  ct_refuse(Acc *a);
void  ct_forget(const Acc *a);

/* cloud_files.c */
void  cf_start(void);                                   /* the Keep on This Computer thread */
void  cf_kick(void);                                    /* a kept folder changed, or 5 minutes went by */
void  cf_browse(const char *path);                      /* a folder Files shows (7.3): the browser thread takes the last one */
char *cf_status(const char *const *paths, int n);
char *cf_free(const char *const *paths, int n);
void  cf_measure(void);                                 /* cache and kept bytes of every account (thread) */
void  cf_auto_free(int below_gb);                       /* the automatic pass (thread) */
long long cf_rm_tree(const char *path);                 /* removes a cache folder: bytes */
char *cf_delete(const char *const *paths, int n, int *req);   /* Delete in the cloud (17.2 item 6), through the mount */

#endif
