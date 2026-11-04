/* firstrun.c — strict parser of lordos-firstrun.txt (see firstrun.h and docs/IMAGER.md).
 *
 * Pure: no I/O, no locale, no allocation except the SSH key lines. Every byte is accounted for:
 * the file is UTF-8 text with LF or CRLF line ends, at most FR_MAX_FILE bytes, no NUL. A line is
 * empty, a comment ("#…") or "key=value" with a key from the table below, exactly once (ssh_key
 * may repeat). The first non-comment line must be "version=1". */
#include "firstrun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum_l(char c) { return is_lower(c) || is_digit(c); }

int fr_valid_hostname(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 63) return 0;
    if (!is_alnum_l(s[0]) || !is_alnum_l(s[n - 1])) return 0;
    for (size_t i = 0; i < n; i++) if (!is_alnum_l(s[i]) && s[i] != '-') return 0;
    /* all digits looks like a number (RFC 1123 allows it, but resolvers and people get confused) */
    size_t d = 0;
    for (size_t i = 0; i < n; i++) d += is_digit(s[i]);
    if (d == n) return 0;
    if (!strcmp(s, "localhost")) return 0;
    return 1;
}

static const char *const RESERVED[] = {
    "root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail", "news", "uucp", "proxy",
    "www-data", "backup", "list", "irc", "nobody", "lordos", "lordos-greeter", "messagebus",
    "polkitd", "sshd", "pipewire", "sudo", "admin", "wheel", "users", "staff", "adm", "disk",
    "audio", "video", "input", "render", "plugdev", "netdev", "bluetooth", "avahi", "_apt",
    "lordos-remote", "operator", "shadow", "utmp", "tty", "kmem", "dialout", "cdrom",
};

int fr_valid_user(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 32) return 0;
    if (!is_lower(s[0]) && s[0] != '_') return 0;
    for (size_t i = 1; i < n; i++) if (!is_alnum_l(s[i]) && s[i] != '_' && s[i] != '-') return 0;
    if (!strncmp(s, "systemd-", 8)) return 0;
    for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++) if (!strcmp(s, RESERVED[i])) return 0;
    return 1;
}

static int is_b64crypt(char c) { return is_alnum_l(c) || (c >= 'A' && c <= 'Z') || c == '.' || c == '/'; }

/* $6$[rounds=N$]<salt 1..16>$<86 chars>   or   $y$<params>$<salt>$<43 chars> */
int fr_valid_hash(const char *s) {
    size_t n = strlen(s);
    if (n > 250) return 0;
    if (!strncmp(s, "$6$", 3)) {
        const char *p = s + 3;
        if (!strncmp(p, "rounds=", 7)) {
            p += 7;
            unsigned long r = 0;
            int nd = 0;
            while (is_digit(*p) && nd < 10) { r = r * 10 + (unsigned long)(*p - '0'); p++; nd++; }
            if (!nd || *p != '$' || r < 1000 || r > 999999999UL) return 0;
            p++;
        }
        size_t sl = 0;
        while (p[sl] && p[sl] != '$') { if (!is_b64crypt(p[sl])) return 0; sl++; }
        if (sl < 1 || sl > 16 || p[sl] != '$') return 0;
        p += sl + 1;
        if (strlen(p) != 86) return 0;
        for (; *p; p++) if (!is_b64crypt(*p)) return 0;
        return 1;
    }
    if (!strncmp(s, "$y$", 3)) {
        const char *p = s + 3;
        int parts = 0;
        size_t len[3] = { 0, 0, 0 };
        for (; *p; p++) {
            if (*p == '$') { if (++parts > 2) return 0; continue; }
            if (!is_b64crypt(*p)) return 0;
            len[parts]++;
        }
        return parts == 2 && len[0] >= 1 && len[0] <= 32 && len[1] >= 1 && len[1] <= 86 && len[2] == 43;
    }
    return 0;
}

int fr_valid_timezone(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 63) return 0;
    if (s[0] == '/' || s[n - 1] == '/') return 0;
    if (strstr(s, "..") || strstr(s, "//")) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z'))) return 0;      /* Area names start with a capital (and UTC) */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(is_alnum_l(c) || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '+' || c == '/')) return 0;
    }
    if (!strncmp(s, "posix/", 6) || !strncmp(s, "right/", 6)) return 0;
    return 1;
}

/* Valid UTF-8 (shortest form, no surrogates, <= U+10FFFF); returns 0 on a bad sequence. */
static int utf8_ok(const unsigned char *p, size_t n, int allow_ctl) {
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        if (c < 0x80) {
            if (!allow_ctl && (c < 0x20 || c == 0x7f)) return 0;
            if (!c) return 0;
            i++;
            continue;
        }
        size_t len;
        unsigned cp;
        if ((c & 0xe0) == 0xc0) { len = 2; cp = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { len = 3; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { len = 4; cp = c & 0x07; }
        else return 0;
        if (i + len > n) return 0;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i + k] & 0x3f);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return 0;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if (cp >= 0x80 && cp < 0xa0) return 0;           /* C1 controls */
        if (cp == 0x2028 || cp == 0x2029 || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069)) return 0;   /* line separators, bidi overrides */
        i += len;
    }
    return 1;
}

int fr_valid_fullname(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 128) return 0;
    if (strpbrk(s, ":,=\\")) return 0;                   /* passwd GECOS separators; no escapes */
    if (s[0] == ' ' || s[n - 1] == ' ' || s[0] == '-') return 0;
    return utf8_ok((const unsigned char *)s, n, 0);
}

int fr_valid_wallpaper(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 48) return 0;
    if (!is_alnum_l(s[0]) || !is_alnum_l(s[n - 1])) return 0;
    for (size_t i = 0; i < n; i++) if (!is_alnum_l(s[i]) && s[i] != '-') return 0;
    return 1;
}

static const char *const ACCENTS[] = { "azul", "violeta", "rosa", "naranja", "verde", "grafito", "rojo", "amarillo", "turquesa", "celeste" };

int fr_valid_accent(const char *s) {
    for (size_t i = 0; i < sizeof ACCENTS / sizeof *ACCENTS; i++) if (!strcmp(s, ACCENTS[i])) return 1;
    return 0;
}

/* ---- SSH public keys ---- */

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Strict base64 (padding required, no whitespace); returns the decoded length or -1. */
static long b64_decode(const char *s, size_t n, unsigned char *out, size_t cap) {
    if (n == 0 || n % 4) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4], pad = 0;
        for (int k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)s[i + k];
            if (c == '=') {
                if (i + 4 != n || k < 2) return -1;
                v[k] = 0;
                pad++;
            } else {
                if (pad) return -1;
                v[k] = b64val(c);
                if (v[k] < 0) return -1;
            }
        }
        unsigned trip = (unsigned)v[0] << 18 | (unsigned)v[1] << 12 | (unsigned)v[2] << 6 | (unsigned)v[3];
        unsigned char b[3] = { (unsigned char)(trip >> 16), (unsigned char)(trip >> 8), (unsigned char)trip };
        for (int k = 0; k < 3 - pad; k++) {
            if (o >= cap) return -1;
            out[o++] = b[k];
        }
    }
    return (long)o;
}

static const char *const KEY_TYPES[] = {
    "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521", "sk-ecdsa-sha2-nistp256@openssh.com", "ssh-rsa",
};

int fr_valid_ssh_key(const char *s) {
    size_t n = strlen(s);
    if (n < 20 || n >= FR_KEY_MAX) return 0;
    const char *sp = strchr(s, ' ');
    if (!sp) return 0;
    size_t tl = (size_t)(sp - s);
    int known = -1;
    for (size_t i = 0; i < sizeof KEY_TYPES / sizeof *KEY_TYPES; i++)
        if (strlen(KEY_TYPES[i]) == tl && !memcmp(s, KEY_TYPES[i], tl)) known = (int)i;
    if (known < 0) return 0;
    const char *b = sp + 1;
    const char *e = strchr(b, ' ');
    size_t bl = e ? (size_t)(e - b) : strlen(b);
    static unsigned char blob[FR_KEY_MAX];
    long dl = b64_decode(b, bl, blob, sizeof blob);
    if (dl < 4 + (long)tl) return 0;
    /* the blob starts with the same type as a length-prefixed string */
    unsigned long sl = (unsigned long)blob[0] << 24 | (unsigned long)blob[1] << 16 | (unsigned long)blob[2] << 8 | blob[3];
    if (sl != tl || memcmp(blob + 4, s, tl)) return 0;
    if (known == 0 && dl != 4 + 11 + 4 + 32) return 0;          /* ssh-ed25519: exactly one 32-byte key */
    if (known == 6 && dl < 260) return 0;                        /* ssh-rsa: at least 2048 bits */
    if (e) {
        const char *c = e + 1;
        size_t cl = strlen(c);
        if (cl > 256 || !utf8_ok((const unsigned char *)c, cl, 0)) return 0;
    }
    return 1;
}

int fr_unhex(const char *hex, unsigned char *out, size_t cap) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i + (size_t)k];
            int d = is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i / 2] = (unsigned char)v;
    }
    return (int)(n / 2);
}

/* ---- the parser ---- */

enum {
    K_VERSION, K_HOSTNAME, K_LANG, K_TIMEZONE, K_KEYBOARD, K_VARIANT, K_WIFI_COUNTRY, K_USER, K_FULLNAME,
    K_HASH, K_AUTOLOGIN, K_SSID, K_PSK, K_HIDDEN, K_SSH, K_SSH_KEY, K_REMOTE, K_BLUETOOTH, K_THEME, K_MODE,
    K_ACCENT, K_WALLPAPER, K_COOLER, K_PERF, K_OVERCLOCK, K_UPDATES, K_COUNT
};
static const char *const KEYS[K_COUNT] = {
    "version", "hostname", "lang", "timezone", "keyboard", "keyboard_variant", "wifi_country", "user", "fullname",
    "password_hash", "autologin", "wifi_ssid_hex", "wifi_psk", "wifi_hidden", "ssh", "ssh_key", "remote_desktop",
    "bluetooth", "theme", "mode", "accent", "wallpaper", "cooler", "performance", "overclock", "auto_updates",
};

static int set_err(char *err, size_t errn, int line, const char *what) {
    if (err && errn) snprintf(err, errn, "line %d: %s", line, what);
    return -1;
}

static int bool01(const char *v, int *out) {
    if (!strcmp(v, "1")) { *out = 1; return 0; }
    if (!strcmp(v, "0")) { *out = 0; return 0; }
    return -1;
}

static int copy_to(char *dst, size_t cap, const char *v) {
    size_t n = strlen(v);
    if (n >= cap) return -1;
    memcpy(dst, v, n + 1);
    return 0;
}

static int pick(const char *v, const char *const *names, int n) {
    for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i;
    return -1;
}

static int value(FirstRun *fr, int k, const char *v, char *why, size_t whyn) {
#define BAD(msg) do { snprintf(why, whyn, "%s: %s", KEYS[k], msg); return -1; } while (0)
    switch (k) {
    case K_VERSION:
        if (strcmp(v, "1")) BAD("only version 1 is understood");
        fr->version = 1;
        return 0;
    case K_HOSTNAME:
        if (!fr_valid_hostname(v) || copy_to(fr->hostname, sizeof fr->hostname, v)) BAD("not a host name (a-z, 0-9 and -, up to 63)");
        fr->has_hostname = 1;
        return 0;
    case K_LANG: {
        static const char *const L[] = { "en", "es", "pt", "fr", "it" };
        if (pick(v, L, 5) < 0) BAD("not a LordOS language");
        copy_to(fr->lang, sizeof fr->lang, v);
        fr->has_lang = 1;
        return 0;
    }
    case K_TIMEZONE:
        if (!fr_valid_timezone(v) || copy_to(fr->timezone, sizeof fr->timezone, v)) BAD("not a time zone name");
        fr->has_timezone = 1;
        return 0;
    case K_KEYBOARD: {
        size_t n = strlen(v);
        if (n < 2 || n > 10) BAD("not a keyboard layout");
        for (size_t i = 0; i < n; i++) if (!is_lower(v[i])) BAD("not a keyboard layout");
        copy_to(fr->keyboard, sizeof fr->keyboard, v);
        fr->has_keyboard = 1;
        return 0;
    }
    case K_VARIANT: {
        size_t n = strlen(v);
        if (n > 32) BAD("too long");
        for (size_t i = 0; i < n; i++) if (!is_alnum_l(v[i]) && !(v[i] >= 'A' && v[i] <= 'Z') && v[i] != '_' && v[i] != '-') BAD("not a keyboard variant");
        copy_to(fr->variant, sizeof fr->variant, v);
        fr->has_variant = 1;
        return 0;
    }
    case K_WIFI_COUNTRY:
        if (strlen(v) != 2 || v[0] < 'A' || v[0] > 'Z' || v[1] < 'A' || v[1] > 'Z') BAD("two capital letters");
        copy_to(fr->wifi_country, sizeof fr->wifi_country, v);
        fr->has_wifi_country = 1;
        return 0;
    case K_USER:
        if (!fr_valid_user(v) || copy_to(fr->user, sizeof fr->user, v)) BAD("not a valid user name, or a reserved one");
        fr->has_user = 1;
        return 0;
    case K_FULLNAME:
        if (!fr_valid_fullname(v) || copy_to(fr->fullname, sizeof fr->fullname, v)) BAD("not a valid name");
        fr->has_fullname = 1;
        return 0;
    case K_HASH:
        if (!fr_valid_hash(v) || copy_to(fr->hash, sizeof fr->hash, v)) BAD("not a SHA-512 crypt or yescrypt hash");
        fr->has_hash = 1;
        return 0;
    case K_AUTOLOGIN:
        if (bool01(v, &fr->autologin)) BAD("0 or 1");
        fr->has_autologin = 1;
        return 0;
    case K_SSID: {
        if (!*v) BAD("empty");
        int n = fr_unhex(v, fr->ssid, sizeof fr->ssid);
        if (n < 1) BAD("1 to 32 bytes in lower-case hex");
        fr->ssid_len = (size_t)n;
        for (int i = 0; i < n; i++) if (!fr->ssid[i]) BAD("contains a NUL byte");
        fr->has_wifi = 1;
        return 0;
    }
    case K_PSK: {
        unsigned char tmp[32];
        if (strlen(v) != 64 || fr_unhex(v, tmp, sizeof tmp) != 32) BAD("64 lower-case hex digits (the derived key, never the password)");
        memset(tmp, 0, sizeof tmp);
        copy_to(fr->psk, sizeof fr->psk, v);
        fr->has_psk = 1;
        return 0;
    }
    case K_HIDDEN:
        if (bool01(v, &fr->hidden)) BAD("0 or 1");
        fr->has_hidden = 1;
        return 0;
    case K_SSH: {
        static const char *const S[] = { "off", "password", "keys" };
        int i = pick(v, S, 3);
        if (i < 0) BAD("off, password or keys");
        fr->ssh = i;
        fr->has_ssh = 1;
        return 0;
    }
    case K_SSH_KEY:
        if (fr->nkeys >= FR_MAX_KEYS) BAD("too many keys (16 at most)");
        if (!fr_valid_ssh_key(v)) BAD("not an OpenSSH public key of an accepted type");
        if (!(fr->keys[fr->nkeys] = strdup(v))) BAD("out of memory");
        fr->nkeys++;
        return 0;
    case K_REMOTE:
        if (bool01(v, &fr->remote)) BAD("0 or 1");
        fr->has_remote = 1;
        return 0;
    case K_BLUETOOTH:
        if (bool01(v, &fr->bluetooth)) BAD("0 or 1");
        fr->has_bluetooth = 1;
        return 0;
    case K_THEME: {
        static const char *const T[] = { "default", "classic" };
        if (pick(v, T, 2) < 0) BAD("default or classic");
        copy_to(fr->theme, sizeof fr->theme, v);
        fr->has_theme = 1;
        return 0;
    }
    case K_MODE: {
        static const char *const M[] = { "light", "dark" };
        if (pick(v, M, 2) < 0) BAD("light or dark");
        copy_to(fr->mode, sizeof fr->mode, v);
        fr->has_mode = 1;
        return 0;
    }
    case K_ACCENT:
        if (!fr_valid_accent(v)) BAD("not a LordOS accent");
        copy_to(fr->accent, sizeof fr->accent, v);
        fr->has_accent = 1;
        return 0;
    case K_WALLPAPER:
        if (!fr_valid_wallpaper(v)) BAD("not a wallpaper id");
        copy_to(fr->wallpaper, sizeof fr->wallpaper, v);
        fr->has_wallpaper = 1;
        return 0;
    case K_COOLER: {
        static const char *const C[] = { "case-fan", "active-cooler", "passive", "none" };
        int i = pick(v, C, 4);
        if (i < 0) BAD("case-fan, active-cooler, passive or none");
        fr->cooler = FR_COOL_CASE_FAN + i;
        fr->has_cooler = 1;
        return 0;
    }
    case K_PERF: {
        static const char *const P[] = { "balanced", "performance", "quiet", "conservative" };
        int i = pick(v, P, 4);
        if (i < 0) BAD("balanced, performance, quiet or conservative");
        fr->perf = FR_PERF_BALANCED + i;
        fr->has_perf = 1;
        return 0;
    }
    case K_OVERCLOCK:
        if (bool01(v, &fr->overclock)) BAD("0 or 1");
        fr->has_overclock = 1;
        return 0;
    case K_UPDATES:
        if (bool01(v, &fr->updates)) BAD("0 or 1");
        fr->has_updates = 1;
        return 0;
    }
    snprintf(why, whyn, "unknown key");
    return -1;
#undef BAD
}

/* Rules between keys, once everything is read. */
static int cross_check(const FirstRun *fr, char *why, size_t whyn) {
#define BAD(msg) do { snprintf(why, whyn, "%s", msg); return -1; } while (0)
    int user_keys = fr->has_fullname || fr->has_hash || fr->has_autologin;
    if (user_keys && !fr->has_user) BAD("fullname, password_hash or autologin without user");
    if (fr->has_user && !fr->has_hash) BAD("user without password_hash");
    if (fr->has_variant && !fr->has_keyboard) BAD("keyboard_variant without keyboard");
    if ((fr->has_psk || fr->has_hidden) && !fr->has_wifi) BAD("wifi_psk or wifi_hidden without wifi_ssid_hex");
    if (fr->has_ssh && fr->ssh == FR_SSH_KEYS && fr->nkeys == 0) BAD("ssh=keys without any ssh_key");
    if (fr->nkeys && !(fr->has_ssh && fr->ssh != FR_SSH_OFF)) BAD("ssh_key with ssh off");
    if (fr->nkeys && !fr->has_user) BAD("ssh_key without user");
    if (fr->has_ssh && fr->ssh != FR_SSH_OFF && !fr->has_user) BAD("ssh on without a user (root never logs in)");
    if (fr->has_remote && fr->remote && !fr->has_user) BAD("remote_desktop=1 without a user");
    if (fr->has_overclock && fr->overclock && !(fr->has_cooler && (fr->cooler == FR_COOL_CASE_FAN || fr->cooler == FR_COOL_ACTIVE)))
        BAD("overclock=1 needs cooler=case-fan or cooler=active-cooler");
    if (fr->has_perf && fr->perf == FR_PERF_PERFORMANCE && !(fr->has_cooler && (fr->cooler == FR_COOL_CASE_FAN || fr->cooler == FR_COOL_ACTIVE)))
        BAD("performance=performance needs a fan (cooler=case-fan or active-cooler)");
    if (fr->has_perf && fr->perf == FR_PERF_QUIET && !(fr->has_cooler && (fr->cooler == FR_COOL_CASE_FAN || fr->cooler == FR_COOL_ACTIVE)))
        BAD("performance=quiet is a fan profile (cooler=case-fan or active-cooler)");
    if (fr->has_perf && !fr->has_cooler) BAD("performance without cooler");
    if (fr->has_overclock && !fr->has_cooler) BAD("overclock without cooler");
    return 0;
#undef BAD
}

int fr_parse(const char *buf, size_t n, FirstRun *fr, char *err, size_t errn) {
    memset(fr, 0, sizeof *fr);
    if (err && errn) err[0] = 0;
    if (!buf || n == 0) return set_err(err, errn, 0, "empty file");
    if (n > FR_MAX_FILE) return set_err(err, errn, 0, "file too large");
    if (memchr(buf, 0, n)) return set_err(err, errn, 0, "NUL byte");
    if (!utf8_ok((const unsigned char *)buf, n, 1)) return set_err(err, errn, 0, "not valid UTF-8");
    size_t start = 0;
    if (n >= 3 && (unsigned char)buf[0] == 0xef && (unsigned char)buf[1] == 0xbb && (unsigned char)buf[2] == 0xbf) start = 3;   /* BOM */
    unsigned seen[K_COUNT] = { 0 };
    int line = 0, got_version = 0;
    char lb[FR_KEY_MAX + 64], why[160];
    for (size_t i = start; i < n;) {
        size_t e = i;
        while (e < n && buf[e] != '\n') e++;
        size_t len = e - i;
        line++;
        if (len && buf[i + len - 1] == '\r') len--;
        if (len >= sizeof lb) return set_err(err, errn, line, "line too long");
        memcpy(lb, buf + i, len);
        lb[len] = 0;
        i = e < n ? e + 1 : e;
        for (size_t k = 0; k < len; k++)
            if ((unsigned char)lb[k] < 0x20 && lb[k] != '\t') return set_err(err, errn, line, "control character");
        if (strchr(lb, '\t')) return set_err(err, errn, line, "tab character");
        if (!len || lb[0] == '#') continue;
        char *eq = strchr(lb, '=');
        if (!eq) return set_err(err, errn, line, "expected key=value");
        *eq = 0;
        int k = -1;
        for (int j = 0; j < K_COUNT; j++) if (!strcmp(lb, KEYS[j])) k = j;
        if (k < 0) return set_err(err, errn, line, "unknown key");
        if (!got_version && k != K_VERSION) return set_err(err, errn, line, "the first setting must be version=1");
        if (seen[k] && k != K_SSH_KEY) return set_err(err, errn, line, "repeated key");
        seen[k]++;
        const char *v = eq + 1;
        /* the full name and the key comment may hold spaces inside; nothing may start or end with one */
        size_t vl = strlen(v);
        if (vl && (v[0] == ' ' || v[vl - 1] == ' ')) return set_err(err, errn, line, "space around the value");
        if (k != K_FULLNAME && k != K_SSH_KEY && strchr(v, ' ')) return set_err(err, errn, line, "space in the value");
        if (k != K_VARIANT && !vl) return set_err(err, errn, line, "empty value");
        if (value(fr, k, v, why, sizeof why)) return set_err(err, errn, line, why);
        if (k == K_VERSION) got_version = 1;
    }
    if (!got_version) return set_err(err, errn, line, "no version=1");
    if (cross_check(fr, why, sizeof why)) return set_err(err, errn, line, why);
    return 0;
}

void fr_free(FirstRun *fr) {
    for (int i = 0; i < fr->nkeys; i++) {
        if (fr->keys[i]) { memset(fr->keys[i], 0, strlen(fr->keys[i])); free(fr->keys[i]); }
        fr->keys[i] = NULL;
    }
    fr->nkeys = 0;
    memset(fr->hash, 0, sizeof fr->hash);
    memset(fr->psk, 0, sizeof fr->psk);
}
