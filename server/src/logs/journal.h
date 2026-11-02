/* journal.h — our own reader for systemd journal files (see journal.c and docs/SYSTEM.md,
 * «Logs»). No libsystemd: files are read with pread and every offset is validated, so a
 * broken or truncated file can't bring axia-server down. */
#ifndef LORD_LOGS_JOURNAL_H
#define LORD_LOGS_JOURNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define JMSG_MAX 16384                  /* maximum message length (cut with «…») */

typedef struct {
    uint64_t realtime;                  /* µs since 1970 (from the entry header) */
    uint64_t seqnum;
    int priority;                       /* 0 (emerg) … 7 (debug); -1 if absent */
    int pid, uid;                       /* -1 if absent */
    int kernel;                         /* _TRANSPORT=kernel */
    char ident[64];                     /* SYSLOG_IDENTIFIER */
    char comm[32];                      /* _COMM */
    char unit[96];                      /* _SYSTEMD_UNIT or _SYSTEMD_USER_UNIT */
    char boot[33];                      /* _BOOT_ID (hex) */
    char *message;                      /* malloc; "" if absent */
} JEntry;

typedef struct JFile JFile;

/* Opens a journal file (read-only). NULL if it is not a journal or can't be read. */
JFile   *jf_open(const char *path);
void     jf_close(JFile *f);
size_t   jf_count(const JFile *f);                        /* entries found */
int      jf_compact(const JFile *f);
/* realtime of entry idx (0 = the oldest), without decoding its fields; 0 if broken */
uint64_t jf_realtime(JFile *f, size_t idx);
/* Decodes entry idx. 0 ok (free with jentry_free), -1 broken. */
int      jf_entry(JFile *f, size_t idx, JEntry *out);
void     jentry_free(JEntry *e);

/* ---- queries (several files, merged by time) ---- */

typedef struct {
    uint64_t since, until;              /* µs; 0 = no limit. since is exclusive, so is until */
    int prio;                           /* maximum priority (0..7), -1 = all */
    int kernel;                         /* kernel entries only */
    char match[96];                     /* SYSLOG_IDENTIFIER, _COMM or unit (empty = all) */
    char grep[128];                     /* message text, case-insensitive (empty = all) */
    size_t limit;                       /* maximum entries (the newest) */
} JQuery;

enum { JSCOPE_USER = 1, JSCOPE_SYSTEM = 2 };

/* Journal files of that scope (user uid's, or the system's) in /var/log/journal and
 * /run/log/journal that this process can read. Returns how many (malloc'd paths). */
int  jq_files(int scope, uid_t uid, char ***paths);
void jq_files_free(char **paths, int n);
/* Runs the query. Entries come out in chronological order (oldest first). Returns the count,
 * or -1. Free with jq_free. */
long jq_run(char *const *paths, int npaths, const JQuery *q, JEntry **out);
void jq_free(JEntry *e, long n);
/* One entry as JSON: {"t":µs,"p":n,"id":"","u":"","pid":n,"uid":n,"k":0|1,"b":"","m":""}.
 * The message is cut to msgmax bytes. Returns the length written (0 if it doesn't fit). */
size_t jentry_json(const JEntry *e, char *buf, size_t cap, size_t msgmax);
/* Validates a unit name or identifier: [A-Za-z0-9@._:-], up to 95. */
int  jq_valid_match(const char *s);
/* Parses a text query, one «key=value» per line (since, until, prio, match, grep, kernel,
 * limit). since/until in seconds or µs (> 1e12). Returns 0 if everything is valid. */
int  jq_parse(const char *text, JQuery *q, size_t max_limit);

/* LZ4 block decoding (the format journald uses, without the size prefix). Returns the length
 * written or -1. Exposed for the test. */
long lz4_block_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

#endif
