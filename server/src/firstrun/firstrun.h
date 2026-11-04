/* firstrun.h — the LordOS first-run file (lordos-firstrun.txt on the boot partition).
 *
 * The LordOS Imager (apps/imager, desktop/imager) writes it next to the image; the Raspberry Pi's
 * lordos-rpi-firstboot.service runs `lordos-firstrun apply` on the first boot, which parses it with
 * fr_parse() (pure, no I/O: tests/firstrun_test.c and an ASan/UBSan fuzz run), applies it once and
 * shreds it. The format is specified in docs/IMAGER.md, "The first-run file".
 *
 * Strict on purpose: one bad line, an unknown key, a repeated key or a value out of range makes the
 * whole file invalid, and nothing is applied (the board then starts in setup mode). Secrets never
 * travel in clear: the password is a crypt(3) hash, the Wi-Fi key is the derived PSK. */
#ifndef LORD_FIRSTRUN_H
#define LORD_FIRSTRUN_H

#include <stddef.h>

#define FR_MAX_FILE   65536             /* bytes */
#define FR_MAX_KEYS   16                /* ssh_key lines */
#define FR_KEY_MAX    4096              /* one OpenSSH public key line */

enum { FR_SSH_OFF = 0, FR_SSH_PASSWORD, FR_SSH_KEYS };
enum { FR_COOL_UNSET = 0, FR_COOL_CASE_FAN, FR_COOL_ACTIVE, FR_COOL_PASSIVE, FR_COOL_NONE };
enum { FR_PERF_UNSET = 0, FR_PERF_BALANCED, FR_PERF_PERFORMANCE, FR_PERF_QUIET, FR_PERF_CONSERVATIVE };

typedef struct {
    int version;
    /* each field: has_x says the key was present */
    int has_hostname;        char hostname[64];
    int has_lang;            char lang[8];             /* en | es | pt | fr | it */
    int has_timezone;        char timezone[64];        /* Area/City, checked against zoneinfo on apply */
    int has_keyboard;        char keyboard[16];        /* xkb layout [a-z]{2,10}, checked on apply */
    int has_variant;         char variant[40];
    int has_wifi_country;    char wifi_country[3];     /* two upper-case letters */
    int has_user;            char user[33];
    int has_fullname;        char fullname[129];       /* UTF-8, no control characters, no : , */
    int has_hash;            char hash[256];           /* $6$… (SHA-512 crypt) or $y$… (yescrypt) */
    int has_autologin;       int autologin;
    int has_wifi;            unsigned char ssid[32];   size_t ssid_len;    /* from wifi_ssid_hex */
    int has_psk;             char psk[65];             /* 64 lower-case hex digits */
    int has_hidden;          int hidden;
    int has_ssh;             int ssh;                  /* FR_SSH_* */
    int nkeys;               char *keys[FR_MAX_KEYS];  /* malloc'd; fr_free() */
    int has_remote;          int remote;
    int has_bluetooth;       int bluetooth;
    int has_theme;           char theme[16];           /* default | classic */
    int has_mode;            char mode[8];             /* light | dark */
    int has_accent;          char accent[16];
    int has_wallpaper;       char wallpaper[49];
    int has_cooler;          int cooler;               /* FR_COOL_* */
    int has_perf;            int perf;                 /* FR_PERF_* */
    int has_overclock;       int overclock;
    int has_updates;         int updates;
} FirstRun;

/* Parses buf[0..n). 0 = valid (out filled); -1 = invalid, with a short English reason and the
 * line number in err (for the journal). Always call fr_free(), also after an error. */
int  fr_parse(const char *buf, size_t n, FirstRun *out, char *err, size_t errn);
void fr_free(FirstRun *fr);

/* The validators, exported for the tests and for the imager's own checks. */
int fr_valid_hostname(const char *s);         /* RFC 1123 label: [a-z0-9]([a-z0-9-]{0,61}[a-z0-9])? */
int fr_valid_user(const char *s);             /* ^[a-z_][a-z0-9_-]{0,31}$, not a reserved name */
int fr_valid_hash(const char *s);             /* $6$[rounds=N$]salt$hash or $y$params$salt$hash */
int fr_valid_timezone(const char *s);         /* shape only: Area[/City[/More]], [A-Za-z0-9_+-] */
int fr_valid_fullname(const char *s);
int fr_valid_ssh_key(const char *s);          /* "<type> <base64> [comment]" for the accepted types */
int fr_valid_wallpaper(const char *s);
int fr_valid_accent(const char *s);

/* Lower-case hex → bytes; returns the byte count or -1. */
int fr_unhex(const char *hex, unsigned char *out, size_t cap);

#endif
