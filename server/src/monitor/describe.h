/* describe.h — what a program is, from what the computer already has (Monitor → Processes → Get Info's
 * "Description"; sdk/describe.js, docs/MONITOR.md "Descriptions"). Read-only, local, no program run, no network:
 *   "pkg"  an installed Debian package's description from dpkg's database (/var/lib/dpkg/status), in the person's
 *          language when apt already has Debian's translation (/var/lib/apt/lists/<source>_i18n_Translation-<code>, the
 *          entry whose Description-md5 is the MD5 of the installed English description);
 *   "man"  a program's one-line summary from its manual page's NAME section (/usr/share/man[/<code>]/man{1,8,6}/<name>.<n>[.gz]),
 *          as whatis shows it, the person's language first when a translated page is installed;
 *   "unit" a systemd unit's Description= (the system's and the user's unit folders).
 *
 * desc_lookup never blocks on the disk: it answers from a cache, and what isn't in it is queued for a worker thread.
 * Bounded: DESC_CACHE answers, files read up to fixed sizes, texts cut to DESC_TEXT_MAX and DESC_LONG_MAX bytes. */
#ifndef LORD_MONITOR_DESCRIBE_H
#define LORD_MONITOR_DESCRIBE_H

#include <stddef.h>

#define DESC_CACHE 512
#define DESC_TEXT_MAX 320
#define DESC_LONG_MAX 1600

typedef struct {
    char text[DESC_TEXT_MAX];       /* one line */
    char lng[DESC_LONG_MAX];        /* a package's long description (paragraphs split by "\n\n"), or "" */
    char lang[16];                  /* the language it is in: "en", or the person's (the code asked for) */
} DescOut;

/* 1: *json gets {"text":"…","long":"…","lang":"…"} (malloc'd); 0: nothing is known; -1: not known yet (queued), or a
 * request that can't be asked (kind unknown, arg or lang not valid). lang: the interface language ("es", "pt",
 * "zh-Hans"…). */
int desc_lookup(const char *kind, const char *arg, const char *lang, char **json);

/* The work itself, for the worker and the tests (0 found, -1 not). */
int desc_pkg(const char *status, const char *lists, const char *pkg, const char *lang, DescOut *o);
int desc_man(const char *root, const char *name, const char *lang, DescOut *o);
int desc_unit(const char *const *dirs, const char *unit, DescOut *o);
/* The parts, for the tests: the MD5 Debian's Description-md5 is (hex, 33 bytes), a manual page's NAME summary, and
 * the codes apt and man use for an interface language (up to 3, NUL-separated in out, "" for English). */
void desc_md5_hex(const void *data, size_t len, char out[33]);
int desc_man_summary(const char *page, size_t len, char *out, size_t n);
int desc_lang_codes(const char *lang, char out[3][16]);

/* The places read (tests point them elsewhere). */
extern const char *desc_status_path, *desc_lists_dir, *desc_man_root;
extern const char *const *desc_unit_dirs;
extern int desc_require_root;          /* manual pages: root's only (1); the tests' own files (0) */

#endif
