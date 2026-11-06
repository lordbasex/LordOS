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
    char trust_kind;                /* 'h' host keys, 'c' a certificate, shown and waiting for the person (cloud_trust.c) */
    char *trust_data;
    char *pend_sec;                 /* add's secrets while that question waits (wiped) */
    int pend_upd;
    time_t check_at;                /* the next listing that checks a mounted account still answers */
    unsigned long long tok_hash;    /* of the secrets last saved: a refreshed token changes it */
    char *pending_secret;           /* not saved yet (keyring locked): retried */
    time_t about_at, tok_at, retry_at;
    char *keep[KEEP_MAX];
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
int   ct_needed(const Acc *a);
int   ct_check(Acc *a, char **question, char *code, size_t cn, char *msg, size_t mn);   /* 0 ok, 1 a question, -1 refused */
int   ct_accept(Acc *a);
void  ct_refuse(Acc *a);
void  ct_forget(const Acc *a);

/* cloud_files.c */
void  cf_start(void);                                   /* the Keep on This Computer thread */
void  cf_kick(void);                                    /* a kept folder changed, or 5 minutes went by */
char *cf_status(const char *const *paths, int n);
char *cf_free(const char *const *paths, int n);
void  cf_measure(void);                                 /* cache and kept bytes of every account (thread) */
void  cf_auto_free(int below_gb);                       /* the automatic pass (thread) */
long long cf_rm_tree(const char *path);                 /* removes a cache folder: bytes */
char *cf_delete(const char *const *paths, int n, int *req);   /* Delete in the cloud (17.2 item 6), through the mount */

#endif
