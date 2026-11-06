/* maild.h — internals of lord-maild, the sandboxed Mail/Calendar/Contacts helper (docs/MAIL.md).
 *
 * Threads:
 *   - main: reads the channel with lord-server, answers local requests (lists, search, opening a
 *     cached message, calendars and contacts) and queues network work;
 *   - one worker per mail account: IMAP sync, IDLE on the inbox, flag changes, moves, appends;
 *   - one sender: SMTP (and the "send later" outbox);
 *   - one worker per CalDAV/CardDAV account.
 * One coarse lock (G) protects the in-memory cache; network I/O never holds it.
 *
 * Files: everything lives under the cache folder lord-server opened for us (cache_fd):
 *   <acct>/folders            folder list
 *   <acct>/f/<dir>/state      uidvalidity uidnext highestmodseq
 *   <acct>/f/<dir>/index      one line per message (headers, flags, preview)
 *   <acct>/f/<dir>/m/<uid>    the raw message, once downloaded (Maildir-like: one file each)
 *   pim/…                     calendars (.ics) and address books (.vcf), see pim.c
 *   att/…                     attachments saved for opening
 *   img/…                     remote images the user loaded
 *   outbox/…                  messages scheduled with "send later" */
#ifndef LORD_MAIL_MAILD_H
#define LORD_MAIL_MAILD_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "../doc/wire.h"
#include "../sec/json.h"
#include "net.h"
#include "snpf.h"

extern pthread_mutex_t G;
extern int cache_fd;

/* ---- channel ---- */
void reply_done(uint32_t id, JBuf *b);              /* frees b */
void reply_err(uint32_t id, const char *code, const char *detail);
void send_event(JBuf *b);                           /* frees b */
void reply_image(uint32_t id, const uint32_t *argb, int w, int h);
/* Asks lord-server for a TCP connection; returns a connected fd or -1 (why: denied/dns/connect). */
int  net_connect(const char *host, int port, char *why, size_t wcap);
/* Connects and starts TLS (implicit or STARTTLS is the caller's job when starttls=1).
 * On a certificate problem emits a "cert" event for that account. */
/* smtp: the connection is the account's SMTP server (its certificate event says so, so Mail pins it as "smtp") */
Conn *open_conn(const char *acct, int smtp, const char *host, int port, int tls_now, const char *pin, TlsInfo *ti, char *why, size_t wcap);
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- files under cache_fd ---- */
int  fs_mkdirs(const char *rel);
char *fs_read(const char *rel, size_t *len, size_t max);
int  fs_write(const char *rel, const void *data, size_t len);      /* atomic (tmp + rename) */
int  fs_exists(const char *rel);
int  fs_unlink(const char *rel);
int  fs_rmtree(const char *rel);
/* safe file/dir name component from any text (no '/', no leading '.') */
void fs_safe_name(const char *in, char *out, size_t cap);
/* 16 hex chars from a string (for folder dirs) */
void fs_hash(const char *s, char *out17);

/* ---- accounts ---- */
enum { SEC_SSL, SEC_STARTTLS, SEC_NONE };
typedef struct {
    char host[256];
    int port, sec;
} Server;

typedef struct Account Account;
struct Account {
    char id[40];
    char kind[8];                   /* "mail" | "dav" */
    char name[128], email[256], full_name[160], user[256];
    char auth[16];                  /* "password" | "oauth2" (prepared, disabled) */
    Server imap, smtp;
    int sent_saved;                 /* the server files sent mail itself (some large providers) */
    char pin_imap[96], pin_smtp[96], pin_dav[96];
    char url[512];                  /* DAV: base URL */
    int cal, card;                  /* DAV: sync calendars / contacts */
    char *password;                 /* in memory only (from the keyring); never written */
    int need_auth_sent;
    /* worker */
    pthread_t thread;
    int started, stop;
    int wake;                       /* eventfd */
    pthread_mutex_t qmu;
    struct Job *jobs;
    /* state */
    char status[24];                /* "idle" | "sync" | "offline" | "auth" | "cert" | "error" */
    char last_error[200];
    int64_t last_sync;
    int initial_done;
};

extern Account *accounts[];
extern int naccounts;
Account *acct_get(const char *id);
void accounts_apply_config(const JVal *cfg);        /* (re)creates workers */
void acct_set_password(Account *a, const char *pw);
void acct_status(Account *a, const char *status, const char *err);   /* + event */

/* ---- jobs ---- */
typedef struct Job {
    int kind;
    uint32_t req;                   /* request id to answer (0 = none) */
    int prio;
    char folder[512];
    uint32_t uid;
    uint32_t *uids; int nuids;
    char *arg;                      /* JSON or text */
    size_t arglen;
    int fds[15]; int nfds;
    struct Job *next;
} Job;
enum { J_SYNC_ALL = 1, J_SYNC_FOLDER, J_FETCH_BODY, J_FLAGS, J_MOVE, J_DELETE, J_APPEND_DRAFT, J_EXPUNGE_DRAFT,
       J_SEND, J_DAV_SYNC, J_DAV_PUT, J_DAV_DELETE, J_DAV_COLLECTION, J_TEST, J_IMAGE, J_CREATE_FOLDER };
void job_push(Account *a, Job *j);                 /* takes ownership */
Job *job_new(int kind, uint32_t req, int prio);
void job_free(Job *j);
Job *job_pop(Account *a);

/* ---- mail cache (store.c) ---- */
enum { F_SEEN = 1, F_FLAGGED = 2, F_ANSWERED = 4, F_DELETED = 8, F_DRAFT = 16, F_FORWARDED = 32, F_JUNK = 64,
       F_LABEL1 = 256 };            /* $label1..$label6 → F_LABEL1 << 0..5 */
#define F_LABELS (63u << 8)

typedef struct {
    uint32_t uid;
    uint32_t flags;
    int64_t date;
    uint32_t size;
    uint16_t att;                   /* has attachments */
    uint16_t body;                  /* the raw message is cached */
    char *from_name, *from_email, *to, *cc, *subject, *msgid, *inreplyto, *refs, *preview;
    char *text;                     /* folded words for search (subject, people, preview, body) */
    int doc;                        /* id in the search index */
} Msg;

typedef struct Folder {
    char name[512];                 /* IMAP name (modified UTF-7) */
    char display[512];              /* UTF-8 */
    char special[12];               /* inbox sent drafts trash junk archive all flagged "" */
    char delim;
    int noselect;
    char dir[20];
    uint32_t uidvalidity, uidnext;
    uint64_t modseq;
    Msg *msgs;
    int n, cap;
    int unread;
    uint32_t version;
    int dirty;
    int sorted;
    int synced;                     /* synced at least once in this run */
} Folder;

typedef struct {
    Folder **f;
    int n;
} FolderList;

FolderList *store_folders(Account *a);              /* G held */
Folder *store_folder(Account *a, const char *name, int create);
void store_load(Account *a);
void store_save(Account *a, Folder *f);            /* writes index/state if dirty */
void store_save_folders(Account *a);
Msg *store_msg(Folder *f, uint32_t uid);
Msg *store_add(Account *a, Folder *f, const Msg *m); /* copies strings */
void store_remove(Account *a, Folder *f, uint32_t uid);
void store_clear(Account *a, Folder *f);
void store_msg_text(Msg *m, const char *body_text);
void store_reindex(Account *a, Folder *f, Msg *m);        /* after store_msg_text */  /* (re)builds m->text and the search index */
char *store_body_path(Account *a, Folder *f, uint32_t uid, char *out, size_t cap);
void store_drop_account(const char *id);

/* JSON for the UI */
void store_folders_json(JBuf *b);
/* list a folder or the unified inbox ("*:inbox"): threads, filter, window */
void store_list_json(JBuf *b, const JVal *args);
void store_search_json(JBuf *b, const JVal *args);
void store_thread_json(JBuf *b, const JVal *args);           /* one conversation, oldest first */
void store_counts_json(JBuf *b);
void store_autocomplete(const char *q, JBuf *b, int *count, int max);

/* search index (search.c) */
int  search_add(const char *text, int acct_idx, Folder *f, uint32_t uid);
void search_remove(int doc);
typedef struct { int acct; Folder *f; uint32_t uid; } Hit;
int  search_query(const char *q, Hit *out, int max);
void search_fold(const char *in, char *out, size_t cap);   /* lower case, no accents */

/* ---- IMAP (imap.c) ---- */
void *mail_worker(void *arg);
int  imap_append_sent(Account *a, const char *msg, size_t len);

/* ---- SMTP + composing (smtp.c, compose.c) ---- */
void sender_start(void);
void sender_queue(Job *j);
char *compose_message(Account *a, const JVal *m, int *fds, int nfds, size_t *olen, char *msgid, size_t mcap, char *err, size_t ecap);
int  smtp_send(Account *a, const char *from, const char **rcpt, int nrcpt, const char *msg, size_t len, char *err, size_t ecap);

/* ---- opening a message (msgview.c) ---- */
void msg_open_json(uint32_t id, const JVal *args);           /* may queue a body fetch */
void msg_open_cached(uint32_t id, Account *a, Folder *f, uint32_t uid);
void msg_attachment(uint32_t id, const JVal *args);
void msg_image(uint32_t id, const JVal *args);
void msg_invite(uint32_t id, const JVal *args);
void msg_invite_respond(uint32_t id, const JVal *args);
char *msg_body_text(const char *raw, size_t len);            /* plain text of a message (search, quoting) */
void msg_preview(const char *hdr, size_t hlen, const char *part, size_t plen, char *out, size_t cap, int *att);
int  https_get(const char *url, char **body, size_t *blen, size_t max, char *err, size_t ecap);

/* ---- images (mimg.c, stb_image) ---- */
uint32_t *img_decode(const unsigned char *data, size_t len, int maxw, int maxh, int *w, int *h);
unsigned char *img_thumb_jpeg(const unsigned char *data, size_t len, int max, size_t *olen);

/* ---- CalDAV/CardDAV (dav.c) and the calendars/contacts store (pim.c) ---- */
void *dav_worker(void *arg);
void pim_init(void);
void pim_handle(uint32_t id, const char *op, const JVal *args, int *fds, int nfds);   /* cal-*, ev-*, todo-*, book-*, contact-* */
void pim_account_removed(const char *id);
/* DAV sync callbacks */
void pim_dav_sync(Account *a);
void pim_alarms_json(JBuf *b, int64_t from, int64_t to);
int  pim_add_invite(const char *cal, const char *ics, size_t len, const char *partstat, const char *me, char *err, size_t ecap);
void pim_autocomplete(const char *q, JBuf *b, int *count, int max);   /* contacts first */
int  pim_has_event(const char *uid, char *cal, size_t cap, char *partstat, size_t pcap, const char *me);

#endif
