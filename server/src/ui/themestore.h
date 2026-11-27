/* themestore.h — the per-user folder of community themes (docs/THEMES-GUIDE.md, "Community themes"; AGENTS.md
 * "Community themes"). Pure C over a folder descriptor: no JavaScript, testable alone (build/themestore-test).
 *
 *   ~/.local/share/lordos/themes/            0700, on lord.fs's deny list (fsops.c)
 *     <id>/theme.json                        the manifest, as the theme compiler checked it
 *     <id>/theme.js                          the code LordOS built from the theme's source (lord-themec)
 *     <id>/assets/…, <id>/locales/<lang>.json
 *     <id>/install.json                      written only by lord-server: theme.js's SHA-256, the author, the
 *                                            capabilities, the person's confirmation, a failure and the crash count
 *
 * Only lord-server writes there, and only through these functions: the helper's answer (a stream of files) is read
 * strictly, every name checked again, every file created with O_EXCL | O_NOFOLLOW in a fresh staging folder that is
 * then renamed into place. */
#ifndef LORD_THEMESTORE_H
#define LORD_THEMESTORE_H

#include <stddef.h>
#include <stdint.h>

#define TS_ID_MAX 26                    /* a community theme's id: [A-Za-z0-9_-], the kit's THEME_ID */
#define TS_FILES_MAX 400
#define TS_FILE_MAX (5u << 20)          /* one file (an image) */
#define TS_CODE_MAX (4u << 20)          /* theme.js */
#define TS_TOTAL_MAX (24u << 20)        /* everything the helper may answer */
#define TS_PATH_MAX 160

/* capabilities a theme.json may declare (read access to a system model, or showing notification history) */
enum {
    TS_CAP_WIFI = 1 << 0, TS_CAP_BLUETOOTH = 1 << 1, TS_CAP_SOUND = 1 << 2, TS_CAP_BATTERY = 1 << 3,
    TS_CAP_BRIGHTNESS = 1 << 4, TS_CAP_MEDIA = 1 << 5, TS_CAP_NOTIFICATIONS = 1 << 6,
};
unsigned ts_cap_bit(const char *name);          /* 0 if unknown */
const char *ts_cap_name(unsigned bit);

typedef struct {
    char id[TS_ID_MAX + 1];
    char author[96];
    char version[32];
    char sha256[65];                    /* of theme.js */
    unsigned caps;
    int trusted;                        /* the person confirmed "This theme isn't from LordOS…" */
    int crashes;                        /* sessions that ended while it ran, in a row (reset after a healthy minute) */
    char failed[256];                   /* why the watchdog took it off (empty: not failed) */
    long long installed;                /* unix time */
} TsRecord;

int ts_valid_id(const char *id);
/* a relative name inside a theme: [A-Za-z0-9_][A-Za-z0-9._-]* segments, no "..", no leading '/', at most 6 levels and
 * TS_PATH_MAX bytes; only theme.json, theme.js, assets/<…>.png|.jpg|.jpeg and locales/<lang>.json */
int ts_valid_name(const char *name);

/* the record: parse (0 ok) and write (text into out, its length; 0 when it doesn't fit) */
int ts_record_parse(const char *text, size_t len, TsRecord *r);
size_t ts_record_format(const TsRecord *r, char *out, size_t n);

/* The helper's answer, a stream:
 *   "LORDTHEME 1\n"
 *   "R <n>\n" <n bytes of JSON report> "\n"
 *   "F <name> <n>\n" <n bytes>        repeated, only after an "ok":true report
 *   "END\n"
 * ts_stream_parse checks it whole and calls file() for each file (names checked, sizes bounded); report gets the
 * report's bytes (NUL-terminated, at most report_n - 1). Returns 0, or -1 with why. */
typedef int (*TsFileFn)(void *ud, const char *name, const uint8_t *data, size_t len);
int ts_stream_parse(const uint8_t *buf, size_t len, char *report, size_t report_n, TsFileFn file, void *ud,
                    char *why, size_t why_n);

/* Writes the stream's files into root/.staging-<random>/, then renames it to root/<id> (an older <id> is moved aside
 * and removed). root_fd is the per-user themes folder (O_DIRECTORY). rec is written as install.json; its sha256 is
 * filled from theme.js. 0, or -1 with why. */
int ts_install(int root_fd, const uint8_t *stream, size_t len, TsRecord *rec, char *why, size_t why_n);

/* reads <id>/install.json (O_NOFOLLOW, a regular file of at most 8 KB) — 0 ok */
int ts_record_read(int root_fd, const char *id, TsRecord *r);
/* replaces <id>/install.json (written aside, renamed) — 0 ok */
int ts_record_write(int root_fd, const TsRecord *r);
/* reads <id>/theme.js whole (O_NOFOLLOW, regular, at most TS_CODE_MAX) and checks it against the record's SHA-256:
 * the bytes (malloc'd, NUL-terminated) or NULL */
char *ts_code_read(int root_fd, const TsRecord *r, size_t *len);
/* removes <id> and everything in it (no link followed) — 0 ok */
int ts_remove(int root_fd, const char *id);

/* the person's wallpaper file a community theme's own Image may show (js_theme.c src_ok; QA of 0.8.9.6.2, BUG-H):
 * <home>/.local/share/lordos/wallpapers/<16 hex>[.thumb].jpg or /var/lib/lordos/wallpapers/<64 hex>.jpg; 1 yes */
int jst_wallpaper_file(const char *s, const char *home);

#endif
