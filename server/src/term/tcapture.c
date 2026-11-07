/* tcapture.c — Save to Passwords from the Terminal (see tcapture.h). The typed value lives only in one page
 * of locked memory (mlock, MADV_DONTDUMP; lord-server is not dumpable), is wiped with explicit_bzero on every
 * way out, and leaves only through tcap_value() to termfill.c, which sends it to lord-vault. */
#include "tcapture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

enum { ST_FREE, ST_TYPING, ST_SENT, ST_OFFERED };

typedef struct {
    int sid, st, asking, overflow;
    size_t n;
    double t;                       /* when the state began (ST_SENT: Enter; ST_OFFERED: the offer) */
    double quiet;                   /* ST_SENT: when the program stopped asking (0 = still asking) */
    char tail[192];                 /* ST_SENT: the program's output since Enter, lowercase, for a rejection */
    size_t tail_n;
    unsigned token;
    TcapTarget target;
} Slot;

static Slot slots[CAP_SLOTS];
static unsigned char *secure;       /* CAP_SLOTS × CAP_MAX bytes, locked; NULL: capturing is off */
static int secure_tried;

static unsigned char *buf_of(const Slot *s) { return secure + (size_t)(s - slots) * CAP_MAX; }

static int secure_init(void) {
    if (secure) return 0;
    if (secure_tried) return -1;
    secure_tried = 1;
    long pg = sysconf(_SC_PAGESIZE);
    size_t len = (size_t)CAP_SLOTS * CAP_MAX;
    len = (len + (size_t)pg - 1) / (size_t)pg * (size_t)pg;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    /* never swapped out, never in a core dump; without the lock, nothing is captured */
    if (mlock(p, len)) { munmap(p, len); return -1; }
    madvise(p, len, MADV_DONTDUMP);
    secure = p;
    return 0;
}

static void wipe(Slot *s) {
    if (secure) explicit_bzero(buf_of(s), CAP_MAX);
    explicit_bzero(s, sizeof *s);
    s->st = ST_FREE;
}

static Slot *slot_of(int sid) {
    for (int i = 0; i < CAP_SLOTS; i++) if (slots[i].st != ST_FREE && slots[i].sid == sid) return &slots[i];
    return NULL;
}

/* ---- what asked ---- */

int tcap_excluded(const char *p) {
    /* the user's own login password: never kept by the Terminal */
    static const char *const own[] = { "sudo", "su", "doas", "passwd", "login", "pkexec", "chsh", "chfn", "gpasswd",
                                       "systemd-ask-password", "systemd-tty-ask-password-agent", "lord-pkexec", NULL };
    for (int i = 0; own[i]; i++) if (!strcmp(p, own[i])) return 1;
    return 0;
}

static void copy(char *dst, size_t n, const char *src, size_t len) {
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* host names and user names as they may be stored: letters, digits and . _ - : (IPv6) [ ] */
static int clean(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("._-:[]", *s))) return 0;
    return 1;
}

static int numeric(const char *s) {
    if (!*s || strlen(s) > 5) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* "[user@]host[:rest]" (scp, sftp, rsync's remote side: the part before ':' only when split_colon) */
static void destination(const char *d, int split_colon, TcapTarget *t) {
    if (!strncmp(d, "ssh://", 6)) {          /* ssh://user@host:port */
        d += 6;
        split_colon = 2;
    }
    const char *at = strrchr(d, '@');
    const char *h = at ? at + 1 : d;
    if (at) copy(t->user, sizeof t->user, d, (size_t)(at - d));
    size_t hl = strlen(h);
    const char *colon = split_colon ? strchr(h, ':') : NULL;
    if (colon && *h != '[') hl = (size_t)(colon - h);
    const char *slash = strchr(h, '/');
    if (slash && (size_t)(slash - h) < hl) hl = (size_t)(slash - h);
    copy(t->host, sizeof t->host, h, hl);
    if (split_colon == 2 && colon) {
        char port[8];
        copy(port, sizeof port, colon + 1, strcspn(colon + 1, "/"));
        if (numeric(port)) copy(t->port, sizeof t->port, port, strlen(port));
    }
    if (!clean(t->user)) t->user[0] = 0;
    if (!clean(t->host)) t->host[0] = 0;
}

/* argv[i] is an option of this program; *val gets its value (attached "-hhost" or the next argument); returns
 * the arguments it took (1 or 2), 0 if it isn't one of opts. */
static int opt(const char *const *argv, int argc, int i, const char *opts, char letter, const char **val) {
    const char *a = argv[i];
    if (a[0] != '-' || a[1] != letter || !strchr(opts, letter)) return 0;
    if (a[2]) { *val = a + 2; return 1; }
    if (i + 1 < argc) { *val = argv[i + 1]; return 2; }
    *val = "";
    return 1;
}

/* remote_colon: the destination is "[user@]host:path" (the part before ':' is kept); need_colon: only an argument
 * with a ':' is the remote one (scp and rsync, whose other arguments are local paths) */
static void ssh_like(const char *const *argv, int argc, const char *withval, char portopt, int remote_colon, int need_colon, TcapTarget *t) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = NULL;
        if (!strcmp(a, "--")) { if (i + 1 < argc) destination(argv[i + 1], remote_colon, t); return; }
        if (a[0] == '-' && a[1]) {
            int took = 0;
            for (const char *o = withval; *o && !took; o++) took = opt(argv, argc, i, withval, *o, &v);
            if (took) {
                if (a[1] == portopt && numeric(v)) copy(t->port, sizeof t->port, v, strlen(v));
                if (a[1] == 'l' && clean(v)) copy(t->user, sizeof t->user, v, strlen(v));
                i += took - 1;
            }
            continue;
        }
        /* scp and rsync: the remote side is the argument with a ':' (local paths come first or last) */
        if (need_colon && !strchr(a, ':')) continue;
        char user[64];
        copy(user, sizeof user, t->user, strlen(t->user));
        destination(a, remote_colon, t);
        if (!t->user[0] && user[0]) copy(t->user, sizeof t->user, user, strlen(user));
        return;
    }
}

/* mysql/mariadb: -h -u -P (or --host= --user= --port=); psql: -h -U -p or a postgresql:// URI */
static void db_args(const char *const *argv, int argc, char hostc, char userc, char portc, TcapTarget *t) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = NULL;
        if (!strncmp(a, "--host=", 7)) v = a + 7, copy(t->host, sizeof t->host, v, strlen(v));
        else if (!strncmp(a, "--user=", 7) || !strncmp(a, "--username=", 11)) v = strchr(a, '=') + 1, copy(t->user, sizeof t->user, v, strlen(v));
        else if (!strncmp(a, "--port=", 7)) { v = a + 7; if (numeric(v)) copy(t->port, sizeof t->port, v, strlen(v)); }
        else if (!strncmp(a, "postgresql://", 13) || !strncmp(a, "postgres://", 11)) destination(strstr(a, "//") + 2, 2, t);
        else if (a[0] == '-' && a[1] && a[1] != '-') {
            char opts[4] = { 0 };                   /* the letters this program has (0: none) */
            size_t no = 0;
            if (hostc) opts[no++] = hostc;
            if (userc) opts[no++] = userc;
            if (portc) opts[no++] = portc;
            int took = opt(argv, argc, i, opts, a[1], &v);
            if (!took) continue;
            if (a[1] == hostc) copy(t->host, sizeof t->host, v, strlen(v));
            else if (a[1] == userc) copy(t->user, sizeof t->user, v, strlen(v));
            else if (a[1] == portc && numeric(v)) copy(t->port, sizeof t->port, v, strlen(v));
            i += took - 1;
        }
    }
    if (!clean(t->host)) t->host[0] = 0;
    if (!clean(t->user)) t->user[0] = 0;
}

int tcap_target(const char *const *argv, int argc, const char *prompt, TcapTarget *t) {
    memset(t, 0, sizeof *t);
    if (argc < 1 || !argv[0]) return -1;
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    copy(t->program, sizeof t->program, base, strlen(base));
    const char *p = t->program;
    if (!strcmp(p, "ssh")) { t->kind = TK_SSH; ssh_like(argv, argc, "BbcDEeFIiJLlmOopQRSWw", 'p', 0, 0, t); }
    else if (!strcmp(p, "sftp")) { t->kind = TK_SSH; ssh_like(argv, argc, "BbcDFiJlOoPRSs", 'P', 1, 0, t); }
    else if (!strcmp(p, "scp")) { t->kind = TK_SSH; ssh_like(argv, argc, "cFiJlOoPS", 'P', 1, 1, t); }
    else if (!strcmp(p, "rsync")) { t->kind = TK_SSH; ssh_like(argv, argc, "e", 0, 1, 1, t); }
    else if (!strcmp(p, "mysql") || !strcmp(p, "mariadb")) { t->kind = TK_DB; copy(t->engine, sizeof t->engine, "MySQL", 5); db_args(argv, argc, 'h', 'u', 'P', t); }
    else if (!strcmp(p, "psql")) { t->kind = TK_DB; copy(t->engine, sizeof t->engine, "PostgreSQL", 10); db_args(argv, argc, 'h', 'U', 'p', t); }
    else if (!strcmp(p, "redis-cli")) { t->kind = TK_DB; copy(t->engine, sizeof t->engine, "Redis", 5); db_args(argv, argc, 'h', 0, 'p', t); }
    else if (!strcmp(p, "mongosh") || !strcmp(p, "mongo")) { t->kind = TK_DB; copy(t->engine, sizeof t->engine, "MongoDB", 7); db_args(argv, argc, 0, 'u', 0, t); }
    else if (!strcmp(p, "git") || !strncmp(p, "git-remote-http", 15)) {
        t->kind = TK_GIT;
        /* git asks "Password for 'https://user@host': " */
        const char *u = prompt ? strstr(prompt, "://") : NULL;
        if (u) {
            char d[200];
            copy(d, sizeof d, u + 3, strcspn(u + 3, "'\" "));
            destination(d, 0, t);
        }
    } else t->kind = TK_OTHER;
    if ((t->kind == TK_DB || t->kind == TK_OTHER) && !t->host[0]) copy(t->host, sizeof t->host, "localhost", 9);
    return 0;
}

/* ---- the state machine ---- */

static unsigned new_token(void) {
    unsigned v = 0;
    while (!v) if (getrandom(&v, sizeof v, 0) != sizeof v) v = 0;
    return v;
}

void tcap_prompt(int sid, int on, const TcapTarget *target, double now) {
    Slot *s = slot_of(sid);
    if (on) {
        /* asked again after Enter: the password was rejected (or another one is asked: it starts over) */
        if (s && s->st != ST_TYPING) wipe(s), s = NULL;
        if (s) { s->asking = 1; return; }
        if (!target || target->kind == TK_NONE || tcap_excluded(target->program) || secure_init()) return;
        for (int i = 0; i < CAP_SLOTS && !s; i++) if (slots[i].st == ST_FREE) s = &slots[i];
        if (!s) return;
        wipe(s);
        s->sid = sid;
        s->st = ST_TYPING;
        s->asking = 1;
        s->t = now;
        s->target = *target;
        return;
    }
    if (!s) return;
    s->asking = 0;
    if (s->st == ST_TYPING) wipe(s);                /* it stopped asking without an Enter: abandoned or filled */
    else if (s->st == ST_SENT && !s->quiet) s->quiet = now;
}

static void tail_add(Slot *s, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n && s->tail_n + 1 < sizeof s->tail; i++) {
        unsigned char c = d[i];
        if (c == 0x1b) continue;
        s->tail[s->tail_n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c < 0x20 ? ' ' : c);
    }
    s->tail[s->tail_n] = 0;
}

void tcap_keys(int sid, const char *data, size_t n, double now) {
    Slot *s = slot_of(sid);
    if (!s || s->st != ST_TYPING) return;
    unsigned char *b = buf_of(s);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\r' || c == '\n') {               /* Enter: wait for the program's verdict */
            if (!s->n || s->overflow) { wipe(s); return; }
            s->st = ST_SENT;
            s->t = now;
            s->quiet = 0;
            s->tail_n = 0;
            s->tail[0] = 0;
            return;
        }
        if (c == 0x7f || c == 0x08) {               /* Backspace: the last character (UTF-8) */
            while (s->n && (b[s->n - 1] & 0xc0) == 0x80) b[--s->n] = 0;
            if (s->n) b[--s->n] = 0;
            continue;
        }
        if (c == 0x15) { explicit_bzero(b, CAP_MAX); s->n = 0; s->overflow = 0; continue; }     /* Ctrl+U */
        if (c < 0x20) { wipe(s); return; }          /* Ctrl+C, Escape, a key sequence: not a plain password */
        if (s->n + 1 >= CAP_MAX) { s->overflow = 1; continue; }
        b[s->n++] = c;
    }
}

void tcap_output(int sid, const uint8_t *data, size_t n) {
    Slot *s = slot_of(sid);
    if (s && s->st == ST_SENT) tail_add(s, data, n);
}

/* what programs print when the password was wrong (their own words, lowercased) */
static int rejected(const char *tail) {
    static const char *const words[] = { "permission denied", "authentication failed", "authentication failure", "access denied",
        "sorry, try again", "incorrect password", "login incorrect", "invalid password", "wrongpass", "bad password",
        "password authentication failed", "contraseña incorrecta", "senha incorreta", NULL };   /* i18n-ok: matched output */
    for (int i = 0; words[i]; i++) if (strstr(tail, words[i])) return 1;
    return 0;
}

void tcap_tick(double now, void (*offer)(int sid, unsigned token, const TcapTarget *t)) {
    for (int i = 0; i < CAP_SLOTS; i++) {
        Slot *s = &slots[i];
        if (s->st == ST_SENT) {
            if (rejected(s->tail) || now - s->t > CAP_SENT_MS) { wipe(s); continue; }
            if (s->quiet && now - s->quiet >= CAP_VERDICT_MS) {
                s->st = ST_OFFERED;
                s->t = now;
                s->token = new_token();
                if (offer) offer(s->sid, s->token, &s->target);
            }
        } else if (s->st == ST_OFFERED && now - s->t > SAVE_OFFER_MS) wipe(s);
        else if (s->st == ST_TYPING && now - s->t > SAVE_OFFER_MS && !s->asking) wipe(s);
    }
}

const char *tcap_value(int sid, unsigned token, size_t *len, TcapTarget *t) {
    Slot *s = slot_of(sid);
    if (!s || s->st != ST_OFFERED || !token || s->token != token) return NULL;
    if (len) *len = s->n;
    if (t) *t = s->target;
    return (const char *)buf_of(s);
}

void tcap_drop(int sid) {
    Slot *s = slot_of(sid);
    if (s) wipe(s);
}

int tcap_pending(void) {
    for (int i = 0; i < CAP_SLOTS; i++) if (slots[i].st != ST_FREE) return 1;
    return 0;
}
