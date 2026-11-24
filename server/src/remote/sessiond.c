/* sessiond.c — lord-sessiond: the LordOS session manager (docs/SESSIONS.md).
 *
 * lordos.service starts it as root at boot, replacing the old autostart. It acts as
 * our own "display manager":
 *   - the monitor (monitor.c): lends the video card and the input to the session being shown
 *     (the session draws directly, with GPU if available);
 *   - the login screen: a lord-server --greeter (theme themes/greeter) running as the
 *     system user lordos-greeter; its requests (log in, restart, shut down) arrive here and
 *     passwords are verified here, against /etc/shadow;
 *   - the sessions: one lord-server --headless per user, which stays alive in the background
 *     when another user is using the monitor (fast user switching) and looks the same from the
 *     monitor and from the browser (sess.c);
 *   - the LAN remote desktop (conn.c), only if enabled in Settings → Sharing:
 *     one child per connection that drops root as soon as it knows whose session it is.
 *
 *   /etc/lordos/session.conf         AUTOLOGIN=user (empty or missing line: login screen)
 *   /etc/lordos/remote/config        enabled=0|1, port=5673, allow=all|group, net=lan|list|any, insecure_dev=0,
 *                                    stun=, turn=, turn_user=, turn_key=, turn_transport=udp|tcp|tls, turn_fp= (0644)
 *   /etc/lordos/remote/access        manual "allow <cidr>" / "block <cidr>" lists (root 0600, admin actions)
 *   /var/lib/lordos/remote/blocklist sources blocked after failed logins (root 0600, survives restarts)
 *   /var/lib/lordos/remote/requests  "unblock <cidr>" lines from the admin helper (read on SIGHUP)
 *   /etc/lordos/remote/cert.pem      self-signed certificate (created when remote is turned on)
 *   /etc/lordos/remote/key.pem       its key (0600)
 *   /etc/lordos/remote/fingerprint   SHA-256 fingerprint of the certificate (shown by Settings)
 *   /run/lordos/status.json          sessions, who is on the monitor and who is connected (Settings)
 *
 * SIGHUP rereads the configuration (turns remote on or off without touching the sessions).
 * It's the only LordOS program that links OpenSSL (TLS): lord-server doesn't. */
#include "sessiond.h"
#include "rtcwire.h"
#include "../sec/cgroup.h"
#include "proto.h"
#include "rsec.h"
#include "lord_version.h"           /* build/gen: version, channel and build (Makefile) */
#include "../bench/boottest.h"
#include "../lordlog.h"
#include "../users/admingroups.h"
#include "startlogin.h"
#include "greeterscale.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/kd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/vt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

RdShared *rd_shared;
RdConfig rd_conf;

double rd_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
void rd_lock(void) {
    if (pthread_mutex_lock(&rd_shared->lock) == EOWNERDEAD) pthread_mutex_consistent(&rd_shared->lock);
}
void rd_unlock(void) { pthread_mutex_unlock(&rd_shared->lock); }

static int path_ok(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* snprintf that, if it doesn't fit, leaves the destination empty (truncated paths are useless) */
#define SP(dst, ...) do { int r_ = snprintf(dst, sizeof dst, __VA_ARGS__); if (r_ < 0 || (size_t)r_ >= sizeof dst) dst[0] = 0; } while (0)

/* Paths of lord-server and of the data: next to this binary (installed: /usr/lib/lordos/bin,
 * data in /usr/lib/lordos; development: server/build, data at the repository root). */
static void find_paths(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    SP(rd_conf.server, "%s/lord-server", exe);
    char root[PATH_MAX], probe[PATH_MAX + 32];
    SP(root, "%s/..", exe);
    SP(probe, "%s/themes", root);
    if (!path_ok(probe)) SP(root, "%s/../..", exe);
    char real[PATH_MAX];
    if (realpath(root, real)) SP(root, "%s", real);
    SP(rd_conf.theme, "%s/themes/default/theme.bundle.js", root);
    SP(rd_conf.greeter_theme, "%s/themes/greeter/theme.bundle.js", root);
    SP(rd_conf.fonts, "%s/assets/fonts", root);
    SP(rd_conf.shell, "%s/assets/shell", root);
    SP(rd_conf.workdir, "%s", exe);
}

/* A small root-owned file that nobody else can write (else it's ignored): its text, or NULL. */
static char *read_root_file(const char *path, size_t max) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    char *t = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && (size_t)st.st_size <= max &&
        (t = malloc((size_t)st.st_size + 1))) {
        ssize_t n = read(fd, t, (size_t)st.st_size);
        if (n < 0) n = 0;
        t[n] = 0;
    } else if (!fstat(fd, &st) && (st.st_uid != 0 || (st.st_mode & 022))) sd_log("%s ignored: not owned by root or writable by others", path);
    close(fd);
    return t;
}

/* Text mode (docs/SESSIONS.md, "Text mode"): lordos.text=1 on the kernel's command line (the boot menu's "LordOS
 * (text mode)") or /etc/lordos/text-mode (Settings → Users & Login, the boot-text action; root's, else ignored). Then
 * lord-sessiond leaves the monitor alone: lordos-text-login.service puts a login prompt on tty1, and startlordos there
 * starts the desktop as startx does. The network, remote desktop and the rest go on as usual. */
static int text_mode(void) {
    char cmd[2048] = "";
    FILE *f = fopen("/proc/cmdline", "r");
    if (f) { if (!fgets(cmd, sizeof cmd, f)) cmd[0] = 0; fclose(f); }
    for (const char *p = cmd; (p = strstr(p, "lordos.text=1")) != NULL; p++)
        if ((p == cmd || p[-1] == ' ') && (p[13] == 0 || p[13] == ' ' || p[13] == '\n')) return 1;
    char *t = read_root_file("/etc/lordos/text-mode", 4096);
    if (t) { free(t); return 2; }
    return 0;
}

static void load_access(void) {
    char *t = read_root_file(RD_ACCESS, 16384);
    int bad = 0;
    rs_access_parse(t, &rd_conf.access, &bad);
    if (bad) sd_log("%s: %d invalid line(s) skipped", RD_ACCESS, bad);
    free(t);
}

static void load_config(void) {
    rd_conf.port = RD_DEFAULT_PORT;
    rd_conf.enabled = rd_conf.group_only = rd_conf.insecure_dev = 0;
    rd_conf.net_mode = RS_NET_LAN;
    rd_conf.nstun = 0;
    rd_conf.turn[0] = rd_conf.turn_user[0] = 0;
    rd_conf.turn_transport = RW_TURN_UDP;
    rd_conf.turn_has_fp = 0;
    FILE *f = fopen(RD_CONF_DIR "/config", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        int v;
        char s[32], h[160];
        if (sscanf(line, "turn_transport=%31s", s) == 1) { if (rw_turn_transport(s) >= 0) rd_conf.turn_transport = rw_turn_transport(s); continue; }
        if (sscanf(line, "turn_fp=%159s", h) == 1) { rd_conf.turn_has_fp = !rw_fp_parse(h, rd_conf.turn_fp); continue; }
        if (sscanf(line, "stun=%159s", h) == 1) {
            if (rw_ice_server_valid(h) && rd_conf.nstun < 4) snprintf(rd_conf.stun[rd_conf.nstun++], 96, "%.95s", h);
            continue;
        }
        if (sscanf(line, "turn=%159s", h) == 1) { if (rw_ice_server_valid(h)) snprintf(rd_conf.turn, sizeof rd_conf.turn, "%.95s", h); continue; }
        if (sscanf(line, "turn_user=%159s", h) == 1) { if (rw_turn_user_valid(h)) snprintf(rd_conf.turn_user, sizeof rd_conf.turn_user, "%.128s", h); continue; }
        if (sscanf(line, "enabled=%d", &v) == 1) rd_conf.enabled = v == 1;
        else if (sscanf(line, "port=%d", &v) == 1 && v > 0 && v < 65536) rd_conf.port = v;
        else if (sscanf(line, "allow=%31s", s) == 1) rd_conf.group_only = !strcmp(s, "group");
        else if (sscanf(line, "net=%31s", s) == 1 && rs_net_mode(s) >= 0) rd_conf.net_mode = rs_net_mode(s);
        else if (sscanf(line, "insecure_dev=%d", &v) == 1) rd_conf.insecure_dev = v == 1;
    }
    if (f) fclose(f);
    load_access();
}

/* Self-signed P-256 certificate for 10 years (CN = host name). */
static int ensure_cert(void) {
    if (path_ok(RD_CONF_DIR "/cert.pem") && path_ok(RD_CONF_DIR "/key.pem")) return 0;
    mkdir("/etc/lordos", 0755);
    mkdir(RD_CONF_DIR, 0755);
    EVP_PKEY *key = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    if (!key || !x) return -1;
    X509_set_version(x, 2);
    unsigned char serial[16];
    FILE *ur = fopen("/dev/urandom", "r");
    if (!ur || fread(serial, 1, sizeof serial, ur) != sizeof serial) { if (ur) fclose(ur); return -1; }
    fclose(ur);
    serial[0] &= 0x7f;
    BIGNUM *bn = BN_bin2bn(serial, sizeof serial, NULL);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 10L * 365 * 24 * 3600);
    X509_set_pubkey(x, key);
    char host[256] = "lordos";
    gethostname(host, sizeof host - 1);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"LordOS", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)host, -1, -1, 0);
    X509_set_issuer_name(x, name);
    char san[300];
    snprintf(san, sizeof san, "DNS:%s,DNS:%s.local", host, host);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, san);
    if (ext) { X509_add_ext(x, ext, -1); X509_EXTENSION_free(ext); }
    if (!X509_sign(x, key, EVP_sha256())) return -1;
    mode_t old = umask(077);
    FILE *kf = fopen(RD_CONF_DIR "/key.pem.new", "w");
    umask(old);
    FILE *cf = fopen(RD_CONF_DIR "/cert.pem.new", "w");
    int ok = kf && cf && PEM_write_PrivateKey(kf, key, NULL, NULL, 0, NULL, NULL) && PEM_write_X509(cf, x);
    if (kf) fclose(kf);
    if (cf) fclose(cf);
    chmod(RD_CONF_DIR "/cert.pem.new", 0644);
    if (ok) ok = !rename(RD_CONF_DIR "/key.pem.new", RD_CONF_DIR "/key.pem") && !rename(RD_CONF_DIR "/cert.pem.new", RD_CONF_DIR "/cert.pem");
    X509_free(x);
    EVP_PKEY_free(key);
    fprintf(stderr, "lord-sessiond: new certificate for %s\n", host);
    return ok ? 0 : -1;
}

static void write_fingerprint(void) {
    FILE *f = fopen(RD_CONF_DIR "/cert.pem", "r");
    X509 *x = f ? PEM_read_X509(f, NULL, NULL, NULL) : NULL;
    if (f) fclose(f);
    if (!x) return;
    unsigned char md[32];
    unsigned int n = 0;
    X509_digest(x, EVP_sha256(), md, &n);
    X509_free(x);
    FILE *o = fopen(RD_CONF_DIR "/fingerprint.new", "w");
    if (!o) return;
    for (unsigned i = 0; i < n; i++) fprintf(o, "%02X%s", md[i], i + 1 < n ? ":" : "\n");
    fclose(o);
    chmod(RD_CONF_DIR "/fingerprint.new", 0644);
    rename(RD_CONF_DIR "/fingerprint.new", RD_CONF_DIR "/fingerprint");
}

/* ---- JSON ---- */

typedef struct { char *p; size_t n, cap; } Str;

static void sput(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sput(Str *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? s->cap - s->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (s->p && s->n + (size_t)k < s->cap) { s->n += (size_t)k; return; }
        size_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->n + (size_t)k + 1) cap *= 2;
        char *nb = realloc(s->p, cap);
        if (!nb) return;
        s->p = nb;
        s->cap = cap;
    }
}

/* text as a JSON string (without surrounding quotes; control chars and quotes escaped) */
static void sput_esc(Str *s, const char *t, size_t max) {
    for (size_t i = 0; t[i] && i < max; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '"' || c == '\\') sput(s, "\\%c", c);
        else if (c < 0x20) sput(s, "\\u%04x", c);
        else sput(s, "%c", c);
    }
}

/* Value of "key":"…" in a flat JSON from the login screen (no escapes except \" and \\). */
static int jstr(const char *j, const char *key, char *out, size_t n) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\":\"", key);
    const char *p = strstr(j, k);
    if (!p) return -1;
    p += strlen(k);
    size_t o = 0;
    for (; *p && *p != '"' && o + 1 < n; p++) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p;
    }
    out[o] = 0;
    return 0;
}

/* Value of "key":<whole number> in a flat JSON, between 0 and max (0 when missing or out of range). */
static int jint(const char *j, const char *key, int max) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(j, k);
    if (!p) return 0;
    p += strlen(k);
    long v = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 9; p++, digits++) v = v * 10 + (*p - '0');
    return digits && v <= max ? (int)v : 0;
}

/* ---- state (Settings reads it) ---- */

static int listening, lfd = -1;

static void write_status(void) {
    mkdir(SD_RUN_DIR, 0755);
    Str s = { 0 };
    sput(&s, "{\"running\":%s,\"pid\":%d,\"port\":%d,\"allow\":\"%s\",\"clients\":[", listening ? "true" : "false",
         (int)getpid(), rd_conf.port, rd_conf.group_only ? "group" : "all");
    rd_lock();
    int first = 1;
    for (int i = 0; i < RD_MAX_CONNS; i++) {
        RdConn *c = &rd_shared->conns[i];
        if (!c->pid) continue;
        sput(&s, "%s{\"pid\":%d,\"ip\":\"%s\",\"user\":\"%s\",\"since\":%.0f,\"rx\":%llu,\"tx\":%llu}", first ? "" : ",", (int)c->pid, c->ip,
             c->user, c->since, c->rx, c->tx);
        first = 0;
    }
    sput(&s, "],\"net\":\"%s\",\"blocked\":[", rs_net_name(rd_conf.net_mode));
    first = 1;
    double now = rd_now();
    for (int i = 0; i < RL_MAX; i++) {
        RlEntry *e = &rd_shared->lockout.e[i];
        if (!e->key[0] || e->until <= now) continue;
        sput(&s, "%s{\"ip\":\"%s\",\"until\":%.0f,\"strikes\":%d,\"manual\":false}", first ? "" : ",", e->key, e->until, e->strikes);
        first = 0;
    }
    for (int i = 0; i < rd_conf.access.nblock; i++) {
        char c[64];
        rs_cidr_format(&rd_conf.access.block[i], c, sizeof c);
        sput(&s, "%s{\"ip\":\"%s\",\"until\":0,\"strikes\":0,\"manual\":true}", first ? "" : ",", c);
        first = 0;
    }
    sput(&s, "],\"allowed\":[");
    for (int i = 0; i < rd_conf.access.nallow; i++) {
        char c[64];
        rs_cidr_format(&rd_conf.access.allow[i], c, sizeof c);
        sput(&s, "%s\"%s\"", i ? "," : "", c);
    }
    sput(&s, "],\"monitor\":\"%s\",\"sessions\":[", rd_shared->monitor);
    first = 1;
    for (int i = 0; i < SD_MAX_SESSIONS; i++) {
        SdSession *x = &rd_shared->sessions[i];
        if (!x->pid) continue;
        if (kill(x->pid, 0)) { memset(x, 0, sizeof *x); continue; }
        sput(&s, "%s{\"pid\":%d,\"user\":\"%s\",\"since\":%.0f}", first ? "" : ",", (int)x->pid, x->user, x->since);
        first = 0;
    }
    rd_unlock();
    /* failed sign-ins: only for the admin action remote-failures (root 0600), never in status.json */
    Str fl = { 0 };
    sput(&fl, "# lord-sessiond: recent failed remote sign-ins (key count first last user)\n");
    rd_lock();
    for (int i = 0; i < RD_FAILLOG; i++) {
        RdFailLog *x = &rd_shared->faillog[i];
        if (x->count && now - x->last < 24 * 3600) sput(&fl, "%s %d %.0f %.0f %s\n", x->key, x->count, x->first, x->last, x->user);
    }
    rd_unlock();
    sput(&s, "]}\n");
    if (fl.p) {
        unlink(SD_FAILURES ".new");
        int ff = open(SD_FAILURES ".new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (ff >= 0) {
            int ok = write(ff, fl.p, fl.n) == (ssize_t)fl.n;
            close(ff);
            if (!ok || rename(SD_FAILURES ".new", SD_FAILURES)) unlink(SD_FAILURES ".new");
        }
        free(fl.p);
    }
    FILE *f = fopen(SD_STATUS ".new", "w");
    if (f && s.p) { fputs(s.p, f); }
    if (f) {
        fclose(f);
        chmod(SD_STATUS ".new", 0644);
        rename(SD_STATUS ".new", SD_STATUS);
    }
    free(s.p);
}

static void bump(void) { rd_lock(); rd_shared->version++; rd_unlock(); }

/* ---- login screen: users and host ---- */

/* an administrator by the shared list (src/users/admingroups.c); an unreadable list is not one */
static int is_admin(const struct passwd *pw) {
    AdminMembership m;
    admin_membership_read(pw->pw_name, pw->pw_uid, pw->pw_gid, &m);
    return admin_membership_is_admin(&m, 0);
}

/* "#rrggbb" of the avatar (/var/lib/lordos/users/<u>.json), validated */
static void avatar_color(const char *user, char out[8]) {
    out[0] = 0;
    char p[128], buf[512];
    snprintf(p, sizeof p, SD_USERS_DIR "/%s.json", user);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char c[16];
    if (jstr(buf, "color", c, sizeof c) || strlen(c) != 7 || c[0] != '#') return;
    for (int i = 1; i < 7; i++) if (!strchr("0123456789abcdefABCDEF", c[i])) return;
    memcpy(out, c, 8);
}

static void file_value(const char *path, const char *key, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    char line[256];
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1;
        if (*v == '"') v++;
        size_t k = 0;
        while (v[k] && v[k] != '"' && v[k] != '\n' && (isalnum((unsigned char)v[k]) || strchr(",._@-", v[k])) && k + 1 < n) { out[k] = v[k]; k++; }
        out[k] = 0;
    }
    if (f) fclose(f);
}

static char greeter_mode[8] = "login", greeter_user[33];
/* A lock's idle state (docs/SETTINGS.md, "Display off, lock, sleep"): the locked session's screen was
 * already off (the login screen stays dark until input), its display-off and sleep timeouts and the seconds
 * left until sleep. The login screen keeps those deadlines, since the session no longer has the monitor.
 * Only for "lock". */
static int greeter_dark, greeter_off_s, greeter_sleep_s, greeter_sleep_left_s;
/* The login screen costs a lord-server (about 50 MB of its own memory, measured in a VM) and isn't
 * needed while a session is on the monitor: it is stopped this long after a session takes the monitor,
 * and started again for a lock, a user switch or a log out (sd_greeter_get starts it on demand). */
#define GREETER_LINGER_S 10
static double greeter_stop_at;
static double greeter_asked_at;
static int left_on_purpose;             /* the monitor was just taken from the session for a lock or a switch */         /* when the login screen was asked for (lock, switch), to log the wait */

/* The last user whose session was shown on the monitor: the login screen preselects it. Kept in
 * SD_LAST_USER (root's, 0644) so it survives a reboot; only a valid user name is read back. */
#define SD_LAST_USER "/var/lib/lordos/last-user"
static char last_user[33];

static void load_last_user(void) {
    last_user[0] = 0;
    int fd = open(SD_LAST_USER, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    char buf[40];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    if (sd_valid_username(buf)) snprintf(last_user, sizeof last_user, "%.32s", buf);
}

static void save_last_user(const char *user) {
    if (!sd_valid_username(user) || !strcmp(user, last_user)) return;
    snprintf(last_user, sizeof last_user, "%.32s", user);
    mkdir("/var/lib/lordos", 0755);
    unlink(SD_LAST_USER ".new");
    int fd = open(SD_LAST_USER ".new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    size_t n = strlen(last_user);
    int ok = write(fd, last_user, n) == (ssize_t)n && write(fd, "\n", 1) == 1;
    close(fd);
    if (!ok || rename(SD_LAST_USER ".new", SD_LAST_USER)) unlink(SD_LAST_USER ".new");
}

/* The main display last chosen in the session shown on the monitor, by connector ("HDMI-A-1", "DP-2"):
 * the login screen opens there (sd_main_display), so a dark first connector doesn't leave it blind.
 * Kept in SD_MAIN_DISPLAY (root's, 0644); only [A-Za-z0-9-], up to 31 characters, is accepted or read back. */
#define SD_MAIN_DISPLAY "/var/lib/lordos/main-display"

static int valid_connector(const char *c) {
    size_t n = strlen(c);
    if (!n || n > 31) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)c[i]) && c[i] != '-') return 0;
    return 1;
}

static void load_main_display(void) {
    sd_main_display[0] = 0;
    int fd = open(SD_MAIN_DISPLAY, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    char buf[40];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    if (valid_connector(buf)) snprintf(sd_main_display, sizeof sd_main_display, "LORD_MAIN_DISPLAY=%s", buf);
}

static void save_main_display(const char *conn) {
    char env[sizeof sd_main_display];
    if (!valid_connector(conn)) return;
    snprintf(env, sizeof env, "LORD_MAIN_DISPLAY=%s", conn);
    if (!strcmp(env, sd_main_display)) return;
    memcpy(sd_main_display, env, sizeof env);
    mkdir("/var/lib/lordos", 0755);
    unlink(SD_MAIN_DISPLAY ".new");
    int fd = open(SD_MAIN_DISPLAY ".new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    size_t n = strlen(conn);
    int ok = write(fd, conn, n) == (ssize_t)n && write(fd, "\n", 1) == 1;
    close(fd);
    if (!ok || rename(SD_MAIN_DISPLAY ".new", SD_MAIN_DISPLAY)) unlink(SD_MAIN_DISPLAY ".new");
}

/* LordBench's boot time test (bench/boottest.h): after a test boot's first frame, when to restart (rd_now()
 * seconds, 0 = not pending) and which boot of how many this one was */
static double bt_reboot_at;
static int bt_k, bt_n;

static void greeter_reply_hello(void) {
    Str s = { 0 };
    sput(&s, "{\"t\":\"hello\",\"mode\":\"%s\",\"user\":\"%s\",\"last\":\"%s\",\"host\":\"", greeter_mode, greeter_user,
         last_user);
    char host[128] = "lordos";
    gethostname(host, sizeof host - 1);
    sput_esc(&s, host, 64);
    char layouts[96], variants[96], lang[48];
    file_value("/etc/default/keyboard", "XKBLAYOUT", layouts, sizeof layouts);
    file_value("/etc/default/keyboard", "XKBVARIANT", variants, sizeof variants);
    file_value("/etc/default/locale", "LANG", lang, sizeof lang);
    sput(&s, "\",\"layouts\":\"%s\",\"variants\":\"%s\",\"lang\":\"%s\",", layouts, variants, lang);
    if (bt_reboot_at > 0) {
        double left = bt_reboot_at - rd_now();
        sput(&s, "\"bootTest\":{\"k\":%d,\"n\":%d,\"in\":%d},", bt_k, bt_n, left > 0 ? (int)(left + 0.5) : 0);
    }
    sput(&s, "\"users\":[");
    setpwent();
    int first = 1, count = 0;
    struct passwd *pw;
    while ((pw = getpwent()) && count < 64) {
        if (!sd_user_allowed(pw, 0)) continue;
        char full[128];
        snprintf(full, sizeof full, "%s", pw->pw_gecos ? pw->pw_gecos : "");
        char *comma = strchr(full, ',');
        if (comma) *comma = 0;
        char color[8], img[160];
        avatar_color(pw->pw_name, color);
        snprintf(img, sizeof img, SD_USERS_DIR "/%s.png", pw->pw_name);
        struct stat st;
        int has_img = !lstat(img, &st) && S_ISREG(st.st_mode) && st.st_size < (4 << 20);
        sput(&s, "%s{\"name\":\"%s\",\"full\":\"", first ? "" : ",", pw->pw_name);
        sput_esc(&s, full, 100);
        sput(&s, "\",\"color\":\"%s\",\"image\":%s,\"admin\":%s,\"open\":%s}", color, has_img ? "true" : "false",
             is_admin(pw) ? "true" : "false", sd_session_running(pw->pw_uid) ? "true" : "false");
        first = 0;
        count++;
    }
    endpwent();
    sput(&s, "]}");
    if (s.p && s.n < 60000) sd_mon_greeter(s.p);
    free(s.p);
}

/* ---- the monitor ---- */

static pid_t worker_pid = -1;
static int wk_r = -1, wk_w = -1;        /* workers → main (SOCK_SEQPACKET, with fds) */
static int has_monitor;
static char shown = '-', shown_user[33];

/* Pending action while another child works (creating a session can take a few seconds). */
typedef enum { W_NONE, W_GREETER, W_LOGIN, W_AUTOLOGIN } WorkKind;
static WorkKind pending;
static char pending_user[33];

static void set_mode(const char *mode, const char *user) {
    /* through copies: callers may pass greeter_mode and greeter_user themselves (a retry), and an
     * snprintf onto its own source is undefined (it left the mode empty) */
    char m[sizeof greeter_mode], u[sizeof greeter_user];
    snprintf(m, sizeof m, "%s", mode ? mode : "login");
    snprintf(u, sizeof u, "%s", user ? user : "");
    memcpy(greeter_mode, m, sizeof m);
    memcpy(greeter_user, u, sizeof u);
    if (strcmp(m, "lock")) greeter_dark = greeter_off_s = greeter_sleep_s = greeter_sleep_left_s = 0;
}

/* In a child: the login screen (creates it if needed) to the monitor. */
static void work_greeter(void) {
    int fd = sd_greeter_get(1280, 800);         /* takes the monitor's size when shown */
    if (fd < 0) _exit(1);
    char msg[256];
    snprintf(msg, sizeof msg, "greeter-in {\"t\":\"mode\",\"mode\":\"%s\",\"user\":\"%s\",\"dark\":%s,\"off\":%d,\"sleep\":%d,"
             "\"sleepLeft\":%d}", greeter_mode, greeter_user, greeter_dark ? "true" : "false", greeter_off_s, greeter_sleep_s,
             greeter_sleep_left_s);
    sd_ctl_send(wk_w, "show g", fd);
    sd_ctl_send(wk_w, msg, -1);
    _exit(0);
}

/* In a child: a user's (already verified) session to the monitor. */
/* The password just verified opens the session's keyring (docs/PASSWORDS.md): it's sent
 * only once over the session socket (RP_KEYRING, root only) and wiped. */
static void send_keyring(int fd, char *pass) {
    size_t n = pass ? strlen(pass) : 0;
    if (!n || n > 1024) return;
    unsigned char buf[1 + 4 + 1024];
    uint32_t len = (uint32_t)(1 + n);
    memcpy(buf, &len, 4);
    buf[4] = RP_KEYRING;
    memcpy(buf + 5, pass, n);
    ssize_t w = send(fd, buf, 5 + n, MSG_NOSIGNAL);
    (void)w;
    explicit_bzero(buf, sizeof buf);
}

static void work_user_pw(const char *user, char *pass) {
    struct passwd *pw = getpwnam(user);
    int fd = pw ? sd_session_get(pw, 1280, 800) : -1;
    if (fd < 0) { if (pass) explicit_bzero(pass, strlen(pass)); sd_ctl_send(wk_w, "greeter-in {\"t\":\"error\"}", -1); _exit(1); }
    send_keyring(fd, pass);
    if (pass) explicit_bzero(pass, strlen(pass));
    char msg[80];
    snprintf(msg, sizeof msg, "show u %s", user);
    sd_ctl_send(wk_w, "greeter-in {\"t\":\"ok\"}", -1);
    sd_ctl_send(wk_w, msg, fd);
    _exit(0);
}

static void work_user(const char *user) { work_user_pw(user, NULL); }

/* In a child: verifies the password typed on the monitor and logs in. */
static void work_login(const char *user, char *pass) {
    char key[56];
    snprintf(key, sizeof key, "monitor:%.32s", user);
    double wait = sd_fail_wait(key);
    if (wait > 0) {
        explicit_bzero(pass, strlen(pass));
        char msg[96];
        snprintf(msg, sizeof msg, "greeter-in {\"t\":\"fail\",\"wait\":%.0f}", wait + 0.5);
        sd_ctl_send(wk_w, msg, -1);
        _exit(1);
    }
    int ok = sd_check_password(user, pass, 0);
    if (!ok) {
        explicit_bzero(pass, strlen(pass));
        sd_fail_note(key, 1);
        sd_log("failed login on the monitor for '%s'", sd_valid_username(user) ? user : "?");
        sleep(1);
        double w2 = sd_fail_wait(key);
        char msg[96];
        snprintf(msg, sizeof msg, "greeter-in {\"t\":\"fail\",\"wait\":%.0f}", w2 > 0 ? w2 + 0.5 : 0);
        sd_ctl_send(wk_w, msg, -1);
        _exit(1);
    }
    sd_fail_note(key, 0);
    sd_log("%s logged in on the monitor", user);
    work_user_pw(user, pass);          /* also opens the session's keyring */
}

static int start_worker(WorkKind kind, const char *user, char *pass) {
    if (worker_pid > 0 || wk_w < 0 || !has_monitor) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (lfd >= 0) close(lfd);
        sd_mon_child();                 /* without the monitor's card or input */
        if (wk_r >= 0) close(wk_r);
        if (kind == W_GREETER) work_greeter();
        else if (kind == W_LOGIN) work_login(user, pass);
        else work_user(user);
        _exit(0);
    }
    if (pid > 0) worker_pid = pid;
    return pid > 0 ? 0 : -1;
}

static void show_greeter(const char *mode, const char *user) {
    set_mode(mode, user);
    if (start_worker(W_GREETER, NULL, NULL)) { pending = W_GREETER; sd_log("login screen (%s) queued behind a running worker", greeter_mode); }
}

/* Is idle sleep held off? A machine running in some session holds logind's "idle" block inhibitor (Machines, "Keep
 * this computer awake", docs/VIRTUALIZATION.md 6.14), which the login screen's sleep deadline, an idle sleep, obeys.
 * Read from logind's BlockInhibited ("shutdown:idle") through busctl, two seconds at most. */
static int idle_inhibited(void) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(p[1], 1); dup2(null, 2);
        const char *const a[] = { "/usr/bin/busctl", "--system", "get-property", "org.freedesktop.login1", "/org/freedesktop/login1",
                                  "org.freedesktop.login1.Manager", "BlockInhibited", NULL };
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        execve(a[0], (char *const *)a, env);
        _exit(127);
    }
    close(p[1]);
    char buf[512];
    size_t n = 0;
    while (pid > 0 && n + 1 < sizeof buf) {
        struct pollfd pf = { p[0], POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0) break;
        ssize_t r = read(p[0], buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(p[0]);
    buf[n] = 0;
    if (pid > 0) { kill(pid, SIGKILL); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
    /* s "shutdown:idle:handle-lid-switch" */
    for (char *t = strchr(buf, '"'), *end; t && (end = strchr(++t, '"')); t = NULL) {
        *end = 0;
        for (char *w = strtok(t, ":"); w; w = strtok(NULL, ":")) if (!strcmp(w, "idle")) return 1;
    }
    return 0;
}

/* "suspend" only comes from the login screen: the sleep deadline of a session locked on the monitor, which
 * the login screen keeps while it has the monitor (a session's own sleep goes through its admin helper). */
static void power(const char *what, const char *who) {
    const char *verb = !strcmp(what, "reboot") ? "reboot" : !strcmp(what, "poweroff") ? "poweroff"
                     : !strcmp(what, "suspend") ? "suspend" : NULL;
    if (!verb) return;
    if (!strcmp(verb, "suspend") && idle_inhibited()) { sd_log("suspend requested %s: not now, idle sleep is held off (a running machine)", who); return; }
    sd_log("%s requested %s", verb, who);
    pid_t pid = fork();
    if (pid == 0) {
        const char *const a[] = { "/usr/bin/systemctl", verb, NULL };
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        execve(a[0], (char *const *)a, env);
        _exit(127);
    }
}

static void autologin_user(char *out, size_t n) {
    out[0] = 0;
    char u[64];
    file_value(SD_SESSION_CONF, "AUTOLOGIN", u, sizeof u);
    struct passwd *pw = u[0] && sd_valid_username(u) ? getpwnam(u) : NULL;
    if (pw && sd_user_allowed(pw, 0)) snprintf(out, n, "%.32s", u);
}

/* At startup: autologin (if chosen) or the login screen. Only once
 * per machine boot (/run is emptied on reboot): if the service restarts, it goes back
 * through the login screen even if the session had been locked. */
static void boot_monitor(void) {
    char au[40];
    autologin_user(au, sizeof au);
    struct stat st;
    int first = stat(SD_RUN_DIR "/autologin-done", &st) != 0;
    if (first) close(open(SD_RUN_DIR "/autologin-done", O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
    if (first && au[0]) {
        sd_log("autologin: %s", au);
        snprintf(pending_user, sizeof pending_user, "%s", au);
        if (start_worker(W_AUTOLOGIN, au, NULL)) pending = W_AUTOLOGIN;
    } else show_greeter("login", NULL);
}

static void set_shown(char k, const char *user) {
    shown = k;
    snprintf(shown_user, sizeof shown_user, "%.32s", k == 'u' ? user : "");
    rd_lock();
    snprintf(rd_shared->monitor, sizeof rd_shared->monitor, "%s", shown_user);
    rd_shared->version++;
    rd_unlock();
}

/* Boot splash hand-off (docs/SESSIONS.md, "Boot splash"). Until the first session frame of this boot
 * is on the monitor, the screen shows the boot splash (or the console): the session keeps it there
 * instead of a black first frame (RP_DISPLAY_KEEP) and says when its frame is up (RP_SHOWN). Then
 * SD_FIRST_FRAME is created and lordos-splash-quit.service ends Plymouth. Only before that first
 * frame: later the screen may hold another user's session, which must never stay visible. */
static int splash_up(void) {
    struct stat st;
    return stat(SD_FIRST_FRAME, &st) != 0;
}

/* Every boot's time to the first frame, kept across reboots to compare boots and versions:
 * /var/lib/lordos/boot-times.jsonl (root's, 0644), one JSON object a line, the last BOOT_TIMES_MAX.
 * Only values LordOS produces itself: no user or host names. About LordOS shows this boot's line
 * (js_sys.c, matched by bootId). */
#define BOOT_TIMES SD_STATE_DIR "/boot-times.jsonl"
#define BOOT_TIMES_MAX 200
static void record_boot_time(double secs, const char *what, int test_k, int test_n) {
    char id[40] = "";
    FILE *b = fopen("/proc/sys/kernel/random/boot_id", "re");
    if (b) { if (!fgets(id, sizeof id, b)) id[0] = 0; fclose(b); }
    id[strspn(id, "0123456789abcdef-")] = 0;
    struct utsname u;
    if (uname(&u)) u.release[0] = 0;
    char rel[65];
    snprintf(rel, sizeof rel, "%s", u.release);
    for (char *q = rel; *q; q++) if (!isalnum((unsigned char)*q) && !strchr(".+~-_", *q)) *q = '_';
    char line[400], test[48] = "";
    if (test_k > 0) snprintf(test, sizeof test, ",\"test\":%d,\"testOf\":%d", test_k, test_n);     /* a boot time test's boot */
    snprintf(line, sizeof line, "{\"bootId\":\"%s\",\"at\":%lld,\"seconds\":%.3f,\"shown\":\"%s\",\"version\":\"%s%s%s\",\"build\":\"%s\",\"kernel\":\"%s\"%s}\n",
             id, (long long)time(NULL), secs, what, LORD_VERSION, LORD_CHANNEL[0] ? "-" : "", LORD_CHANNEL, LORD_BUILD, rel, test);
    /* the older lines, at most BOOT_TIMES_MAX - 1, then this one; written whole and renamed */
    static char old[BOOT_TIMES_MAX * 400];
    size_t n = 0;
    int fd = open(BOOT_TIMES, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        ssize_t r;
        while (n < sizeof old - 1 && (r = read(fd, old + n, sizeof old - 1 - n)) > 0) n += (size_t)r;
        close(fd);
    }
    old[n] = 0;
    int lines = 0;
    for (size_t i = 0; i < n; i++) if (old[i] == '\n') lines++;
    char *keep = old;
    while (lines >= BOOT_TIMES_MAX && (keep = strchr(keep, '\n'))) { keep++; lines--; }
    if (!keep) keep = old + n;
    mkdir(SD_STATE_DIR, 0755);
    char tmp[] = SD_STATE_DIR "/.boot-times.XXXXXX";
    int t = mkostemp(tmp, O_CLOEXEC);
    if (t < 0) { sd_log("warning: %s: %s", BOOT_TIMES, strerror(errno)); return; }
    size_t kl = strlen(keep);
    int ok = (kl == 0 || write(t, keep, kl) == (ssize_t)kl) && write(t, line, strlen(line)) == (ssize_t)strlen(line);
    ok = fchmod(t, 0644) == 0 && ok;
    ok = close(t) == 0 && ok;
    if (!ok || rename(tmp, BOOT_TIMES)) { unlink(tmp); sd_log("warning: could not write %s", BOOT_TIMES); }
}

static void first_frame(void) {
    if (!splash_up()) return;
    int f = open(SD_FIRST_FRAME, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (f < 0) { sd_log("warning: %s: %s", SD_FIRST_FRAME, strerror(errno)); return; }
    close(f);
    /* the boot time to the screen, from the kernel's start (CLOCK_BOOTTIME), for the log, About and
     * the results (docs/RESULTS.md) */
    struct timespec bt;
    clock_gettime(CLOCK_BOOTTIME, &bt);
    double secs = bt.tv_sec + bt.tv_nsec / 1e9;
    const char *what = !has_monitor ? "none" : shown == 'u' ? "session" : "login";   /* autologin, login screen, no monitor */
    sd_log("first frame on the monitor %.3f s after the kernel started (LordOS %s%s%s, build %s, %s): the boot splash can go",
           secs, LORD_VERSION, LORD_CHANNEL[0] ? "-" : "", LORD_CHANNEL, LORD_BUILD, what);
    /* LordBench's boot time test: this boot's number, and another restart if any are left */
    BootTest b, after;
    struct stat bst;
    int have = bt_read(BT_PATH, &b) == 0, k = 0;
    int act = bt_next(have, &b, (long long)time(NULL), &k, &after);
    if (!have && !lstat(BT_PATH, &bst)) { unlink(BT_PATH); sd_log("boot time test: %s is not valid; removed", BT_PATH); }
    if (act == BT_STALE) { unlink(BT_PATH); sd_log("boot time test: the state is too old; removed"); }
    else if (act == BT_LAST) { unlink(BT_PATH); sd_log("boot time test: boot %d of %d, the last one", k, b.total); }
    else if (act == BT_AGAIN) {
        if (bt_write(BT_PATH, &after)) { unlink(BT_PATH); sd_log("warning: boot time test: could not write %s; stopped", BT_PATH); }
        else {
            bt_reboot_at = rd_now() + BT_SETTLE_S;
            bt_k = k;
            bt_n = b.total;
            sd_log("boot time test: boot %d of %d; restarting in %d s (a login at the login screen cancels it)", k, b.total, BT_SETTLE_S);
        }
    }
    record_boot_time(secs, what, act == BT_LAST || act == BT_AGAIN ? k : 0, b.total);
}

/* the boot time test stops: no restart, the state file removed */
static void boot_test_cancel(const char *why) {
    if (bt_reboot_at <= 0) return;
    bt_reboot_at = 0;
    unlink(BT_PATH);
    sd_log("boot time test cancelled (%s) after boot %d of %d", why, bt_k, bt_n);
}

/* the restart, if nobody cancelled meanwhile and the file still says what this boot wrote */
static void boot_test_tick(void) {
    if (bt_reboot_at <= 0 || rd_now() < bt_reboot_at) return;
    bt_reboot_at = 0;
    BootTest c;
    if (bt_read(BT_PATH, &c) || c.total != bt_n || c.left != bt_n - bt_k) { sd_log("boot time test: cancelled meanwhile"); return; }
    char who[64];
    snprintf(who, sizeof who, "by the boot time test (boot %d of %d done)", bt_k, bt_n);
    power("reboot", who);
}

/* No card with KMS (0.8.1): before this, the boot ended on a silent black screen. The message goes
 * to the text console (tty1, drawn by the firmware framebuffer if there is one) a few seconds
 * later, once the boot splash has quit; 80 columns at most, the size of a text console. A grandchild
 * does it, so nothing waits and nothing has to be
 * reaped here. */
static void tell_no_display(void) {
    pid_t p = fork();
    if (p < 0) return;
    if (p > 0) { while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {} return; }
    if (fork() != 0) _exit(0);
    syscall(SYS_close_range, 3, ~0u, 0);
    sleep(3);
    int t = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (t < 0) _exit(0);
    ioctl(t, KDSETMODE, KD_TEXT);
    static const char msg[] =
        "\033[2J\033[H\r\n"
        "  LordOS could not show its desktop: no display device with kernel\r\n"
        "  modesetting (KMS) was found.\r\n\r\n"
        "  In a virtual machine, choose another display device: VirtIO GPU,\r\n"
        "  standard VGA or SVGA. On a PC, check that the graphics card has a\r\n"
        "  kernel driver.\r\n\r\n"
        "  Press Ctrl+Alt+F2 for a text console.\r\n";
    ssize_t w = write(t, msg, sizeof msg - 1);
    (void)w;
    _exit(0);
}

/* RP_DISPLAY_KEEP only for the first lend of this process: whatever happens to RP_SHOWN (lost on a
 * switch, the file could not be created), a later lend (lock, user switch) must start black, never
 * on the picture left by the previous session. */
static int keep_lend(void) {
    static int lent;
    int keep = !lent && splash_up();
    if (lent) first_frame();
    lent = 1;
    return keep;
}

/* What the workers send: which session to show and messages for the login screen. */
static int held_fd = -1;                /* a "show" that came while a text console was in front, and its line */
static char held[48];
static void show_from(const char *buf, int fd);
static void from_worker(void) {
    char buf[4200];
    int fd;
    int n = sd_ctl_recv(wk_r, buf, sizeof buf, &fd);
    if (n > 0) { show_from(buf, fd); return; }
    if (fd >= 0) close(fd);
}
static void show_from(const char *buf, int fd) {
    char user[40];
    if (sd_mon_away() && fd >= 0 && (!strcmp(buf, "show g") || !strncmp(buf, "show u ", 7))) {
        /* a text console is in front: nothing is lent until ours is back (vt_return) */
        if (held_fd >= 0) close(held_fd);
        held_fd = fd;
        snprintf(held, sizeof held, "%.47s", buf);
        return;
    }
    if (!strcmp(buf, "show g") && fd >= 0) {
        greeter_stop_at = 0;
        if (greeter_asked_at) sd_log("monitor: the login screen (%s), %.0f ms after the screen went dark", greeter_mode, (rd_now() - greeter_asked_at) * 1000);
        greeter_asked_at = 0;
        set_shown('g', NULL);
        if (sd_mon_show(fd, "-", keep_lend())) { sd_mon_lost(); set_shown('-', NULL); }
        return;
    }
    if (sscanf(buf, "show u %32s", user) == 1 && fd >= 0 && sd_valid_username(user)) {
        sd_log("monitor: %s's session", user);
        set_shown('u', user);
        if (sd_mon_show(fd, user, keep_lend())) { sd_mon_lost(); set_shown('-', NULL); show_greeter("login", NULL); }
        else { save_last_user(user); greeter_stop_at = rd_now() + GREETER_LINGER_S; }
        return;
    }
    if (fd >= 0) close(fd);
    if (!strncmp(buf, "greeter-in ", 11) && shown == 'g') sd_mon_greeter(buf + 11);
}

/* #242: the login screen's scale, chosen by an administrator in Settings → Displays for one display (greeterscale.h).
 * The request comes only from the session being shown (from_shown); whose it is, and whether that user is an
 * administrator, is decided here as root from shown_user (the session lord-sessiond started), never from the message.
 * Kept in GSCALE_FILE (root's, 0644, in root's /var/lib/lordos); written at most once every 2 s; read back only
 * when it parses. Two administrators who disagree overwrite each other: the last one wins. */
static void load_greeter_scale(void) {
    sd_main_scale[0] = 0;
    int fd = open(GSCALE_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return;
    struct stat st;
    char buf[48];
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && st.st_size < (off_t)sizeof buf)
        n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char conn[GSCALE_CONN_MAX + 1];
    int pct;
    if (!gscale_parse_file(buf, conn, sizeof conn, &pct)) snprintf(sd_main_scale, sizeof sd_main_scale, GSCALE_ENV "=%s:%d", conn, pct);
}

/* /var/lib/lordos itself: a directory of root's that no one else can write (mkdir'ed if missing) */
static int lib_dir_ok(void) {
    mkdir("/var/lib/lordos", 0755);
    struct stat st;
    return !lstat("/var/lib/lordos", &st) && S_ISDIR(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
}

static void save_greeter_scale(const char *j) {
    static double last;
    /* one request every 2 s at most, before anything else: a flood (or a refused user asking again) costs nothing and
     * logs nothing */
    double now = rd_now();
    if (last && now - last < 2) return;
    last = now;
    char conn[GSCALE_CONN_MAX + 1];
    int pct;
    if (gscale_from_msg(j, conn, sizeof conn, &pct) != 1) { sd_log("login screen scale: a request that isn't valid, ignored"); return; }
    struct passwd *pw = sd_valid_username(shown_user) ? getpwnam(shown_user) : NULL;
    if (!pw || !is_admin(pw)) { sd_log("%s asked to change the login screen's scale: not an administrator, refused", shown_user); return; }
    char env[sizeof sd_main_scale] = "";
    if (pct) snprintf(env, sizeof env, GSCALE_ENV "=%s:%d", conn, pct);
    if (!strcmp(env, sd_main_scale)) return;
    if (!lib_dir_ok()) { sd_log("login screen scale: /var/lib/lordos isn't root's, not saved"); return; }
    if (!pct) {
        if (unlink(GSCALE_FILE) && errno != ENOENT) return;
        sd_main_scale[0] = 0;
        sd_log("%s set the login screen's scale back to the recommended one", shown_user);
        return;
    }
    char line[48];
    int len = snprintf(line, sizeof line, "%s %d\n", conn, pct);
    unlink(GSCALE_FILE ".new");
    int fd = open(GSCALE_FILE ".new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    int ok = !fchmod(fd, 0644) && len > 0 && write(fd, line, (size_t)len) == len && !fsync(fd);
    close(fd);
    if (!ok || rename(GSCALE_FILE ".new", GSCALE_FILE)) { unlink(GSCALE_FILE ".new"); return; }
    memcpy(sd_main_scale, env, sizeof env);
    sd_log("%s set the login screen's scale to %d %% on %s", shown_user, pct, conn);
}

/* Restart or shut down from the session on the monitor (0.8.1: the crown menu, the Classic start
 * menu; docs/SESSIONS.md, "Restart and Shut Down from a session"). Only "reboot" or "poweroff",
 * through the same closed path as the login screen. Policy, decided here as root: the user on the
 * monitor may do it alone; while other users have sessions open, only an administrator (sudo,
 * admin or wheel, as for the admin actions). A refusal goes back to that session. */
static int other_sessions(uid_t me) {
    int n = 0;
    rd_lock();
    for (int i = 0; i < SD_MAX_SESSIONS; i++) {
        SdSession *x = &rd_shared->sessions[i];
        if (x->pid && x->uid != me && kill(x->pid, 0) == 0) n++;
    }
    rd_unlock();
    return n;
}

static void session_power(const char *j) {
    char what[16] = "";
    jstr(j, "p", what, sizeof what);
    if (strcmp(what, "reboot") && strcmp(what, "poweroff")) return;
    struct passwd *pw = sd_valid_username(shown_user) ? getpwnam(shown_user) : NULL;
    if (!pw) return;
    int others = other_sessions(pw->pw_uid);
    if (others && !is_admin(pw)) {
        sd_log("%s asked to %s with %d other session(s) open: not an administrator, refused", shown_user, what, others);
        char r[96];
        snprintf(r, sizeof r, "{\"t\":\"power\",\"ok\":false,\"why\":\"admin\",\"others\":%d}", others);
        sd_mon_greeter(r);
        return;
    }
    char who[96];
    snprintf(who, sizeof who, "by %s (%d other session(s) open)", shown_user, others);
    power(what, who);
}

/* Lock and switch user: the login screen isn't kept running behind a session (it is stopped a few
 * seconds after a session takes the monitor, greeter_stop_at), and starting it takes a moment. So the
 * monitor is taken from the session first: input revoked, its framebuffers removed (the screen goes
 * dark), and only then is the login screen asked for. An unlocked desktop is never left on the screen
 * while it starts. */
static void leave_for_greeter(const char *mode) {
    char user[40];
    snprintf(user, sizeof user, "%s", shown_user);
    greeter_stop_at = 0;
    greeter_asked_at = rd_now();
    left_on_purpose = 1;                /* the closed connection isn't the session ending (monitor_event) */
    double t0 = rd_now();
    sd_mon_lost();
    sd_log("monitor: taken from %s in %.0f ms", user, (rd_now() - t0) * 1000);
    set_shown('-', NULL);
    show_greeter(mode, user);
}

/* Requests from the session being shown: lock, switch user, restart or shut down; from the login screen:
 * log in, restart or shut down. */
static void from_shown(int type, const char *j) {
    if (type == RP_SHOWN) { first_frame(); return; }
    /* Ctrl+Alt+F1…F12, from the session or the login screen: the switch itself happens in vt_away */
    if (type == RP_SESSION && (shown == 'u' || shown == 'g') && strstr(j, "\"vt\"")) {
        char a[16] = "";
        jstr(j, "a", a, sizeof a);
        if (!strcmp(a, "vt")) sd_mon_vt_switch(jint(j, "n", 12));   /* jint's third: the most it takes */
        return;
    }
    if (type == RP_SESSION && shown == 'u') {
        char a[16] = "";
        jstr(j, "a", a, sizeof a);
        if (!strcmp(a, "power")) { session_power(j); return; }
        if (!strcmp(a, "display")) {
            if (strstr(j, "\"scaleOn\"")) { save_greeter_scale(j); return; }   /* #242: an administrator only */
            char m[40] = "";
            jstr(j, "main", m, sizeof m);
            save_main_display(m);
            return;
        }
        if (strstr(j, "\"lock\"")) {
            sd_log("%s locked the screen", shown_user);
            struct passwd *lp = sd_valid_username(shown_user) ? getpwnam(shown_user) : NULL;
            if (lp) rd_tokens_drop_uid(lp->pw_uid);        /* locking also signs that user's browsers out */
            /* before the worker that sends the mode is forked (set_mode keeps them for "lock") */
            greeter_dark = strstr(j, "\"dark\":true") != NULL;
            greeter_off_s = jint(j, "off", 86400);
            greeter_sleep_s = jint(j, "sleep", 86400);
            greeter_sleep_left_s = jint(j, "sleepLeft", 86400);
            leave_for_greeter("lock");
        }
        else if (strstr(j, "\"switch\"")) { sd_log("%s is switching user", shown_user); leave_for_greeter("switch"); }
        /* a logout: the login screen at once; the session closes its apps and unmounts its cloud folders afterwards,
         * off the screen, instead of leaving a frozen desktop up meanwhile */
        else if (!strcmp(a, "logout")) { sd_log("%s is logging out", shown_user); leave_for_greeter("login"); }
        return;
    }
    if (type != RP_GREETER || shown != 'g') return;
    char t[16] = "";
    jstr(j, "t", t, sizeof t);
    if (!strcmp(t, "hello")) greeter_reply_hello();
    else if (!strcmp(t, "login")) {
        char u[40] = "", pass[300] = "";
        jstr(j, "user", u, sizeof u);
        jstr(j, "pass", pass, sizeof pass);
        boot_test_cancel("a login at the login screen");
        if (!sd_valid_username(u) || start_worker(W_LOGIN, u, pass)) sd_mon_greeter("{\"t\":\"busy\"}");
        explicit_bzero(pass, sizeof pass);
    } else if (!strcmp(t, "bootTestCancel")) {
        boot_test_cancel("from the login screen");
        greeter_reply_hello();
    } else if (!strcmp(t, "power")) {
        char what[16] = "";
        jstr(j, "a", what, sizeof what);
        power(what, "from the login screen");
    }
}

/* ---- text mode: startlordos asks for the login screen (docs/SESSIONS.md, "Text mode") ---- */
#define START_SOCK SD_RUN_DIR "/start.sock"
static int text_now;                    /* in text mode, the monitor not taken yet */
static int start_fd = -1;               /* START_SOCK, listening while text_now */

static void start_listen(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", START_SOCK);
    unlink(START_SOCK);
    mkdir(SD_RUN_DIR, 0755);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || chmod(START_SOCK, 0666) || listen(fd, 4)) {
        sd_log("text mode: %s: %s; startlordos will run the desktop on its own", START_SOCK, strerror(errno));
        close(fd);
        unlink(START_SOCK);
        return;
    }
    start_fd = fd;
}
static void start_stop(void) {
    if (start_fd >= 0) { close(start_fd); unlink(START_SOCK); }
    start_fd = -1;
}

/* tty1's text login (lordos-text-login.service) stops before the console goes to graphics: its agetty, restarting,
 * would reset tty1 to text under the login screen. Waited for (a second or two at most), once. */
static void text_login_stop(void) {
    pid_t pid = fork();
    if (pid == 0) {
        const char *const a[] = { "/usr/bin/systemctl", "stop", "lordos-text-login.service", NULL };
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        execve(a[0], (char *const *)a, env);
        _exit(127);
    }
    if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static void boot_monitor(void);
/* One request: the caller from the socket (SO_PEERCRED), its terminal from /proc, the console in front and that
 * console's owner from the system; never anything the caller says (startlogin.h). Yes: the login screen comes up on
 * tty1 (brought to the front), with that user chosen; nothing is unlocked or started for them. */
static void start_request(void) {
    int c = accept4(start_fd, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) return;
    struct ucred cr;
    socklen_t cl = sizeof cr;
    const char *answer = "refused\n";
    if (!getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl)) {
        char path[48], st[1024] = "";
        snprintf(path, sizeof path, "/proc/%d/stat", (int)cr.pid);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = f >= 0 ? read(f, st, sizeof st - 1) : -1;
        if (f >= 0) close(f);
        unsigned long tty_nr = 0;
        const char *rp = n > 0 ? (st[n] = 0, strrchr(st, ')')) : NULL;   /* the name may hold spaces: after its ')' */
        if (rp) { char state; int ppid, pgrp, sess; if (sscanf(rp + 1, " %c %d %d %d %lu", &state, &ppid, &pgrp, &sess, &tty_nr) != 5) tty_nr = 0; }
        int active = 0;
        int t0 = open("/dev/tty0", O_RDONLY | O_CLOEXEC | O_NOCTTY);
        struct vt_stat vs;
        if (t0 >= 0 && !ioctl(t0, VT_GETSTATE, &vs)) active = vs.v_active;
        if (t0 >= 0) close(t0);
        struct stat ts;
        snprintf(path, sizeof path, "/dev/tty%d", active);
        uid_t owner = active > 0 && !stat(path, &ts) ? ts.st_uid : (uid_t)-1;
        int vt = startlogin_check(cr.uid, tty_nr, active, owner);
        /* tty1 is about to be taken (its text login stopped, a console session there ended): only if it is the caller's
         * or nobody's, never someone else's console session */
        struct stat t1;
        if (vt > 0 && (stat("/dev/tty1", &t1) || (t1.st_uid != 0 && t1.st_uid != cr.uid))) vt = SL_NOT_ITS_OWNER;
        struct passwd *pw = vt > 0 ? getpwuid(cr.uid) : NULL;
        if (vt > 0 && pw && sd_user_allowed(pw, 0) && text_now) {
            sd_log("text mode: %s asked for the login screen from tty%d", pw->pw_name, vt);
            text_now = 0;
            /* answered first: tty1's text login goes now, with whatever ran there (startlordos itself, typed on tty1) */
            if (write(c, "ok\n", 3) < 0) { /* the caller left */ }
            close(c);
            c = -1;
            text_login_stop();
            has_monitor = sd_mon_open() == 0;
            if (has_monitor) {
                char u[40];
                snprintf(u, sizeof u, "%.32s", pw->pw_name);
                set_mode("login", u);
                show_greeter("login", u);
                sd_mon_vt_front();
            } else sd_log("text mode: no usable display for the login screen");
        } else sd_log("text mode: a login screen request refused (uid %u, reason %d)", (unsigned)cr.uid, vt);
    }
    if (c >= 0) {
        if (write(c, answer, strlen(answer)) < 0) { /* the caller left */ }
        close(c);
    }
    if (!text_now) start_stop();
}

/* The kernel wants to show another console (Ctrl+Alt+Fn, a chvt): a session on the monitor is locked as with Lock
 * Screen (the login screen comes up locked on our console when it is back), the login screen just leaves; the monitor
 * is taken back, then the switch goes on. */
static void vt_away(void) {
    if (shown == 'u') {
        sd_log("%s left for a text console: the session is locked", shown_user);
        struct passwd *lp = sd_valid_username(shown_user) ? getpwnam(shown_user) : NULL;
        if (lp) rd_tokens_drop_uid(lp->pw_uid);
        greeter_dark = 0;
        greeter_off_s = greeter_sleep_s = greeter_sleep_left_s = 86400;
        leave_for_greeter("lock");      /* the login screen it asks for is held until we are back */
    } else if (shown == 'g') {
        left_on_purpose = 1;
        sd_mon_lost();
        set_shown('-', NULL);
        sd_log("the login screen left for a text console");
    }
    sd_mon_vt_release();
}
/* Our console is in front again: what was held is shown, or the login screen is asked for. */
static void vt_return(void) {
    sd_mon_vt_acquire();
    sd_log("back from a text console");
    if (held_fd >= 0) {
        char line[48];
        snprintf(line, sizeof line, "%s", held);
        int fd = held_fd;
        held_fd = -1;
        show_from(line, fd);
    } else if (shown == '-' && worker_pid < 0 && pending == W_NONE) show_greeter(greeter_mode, greeter_user);
}

static void monitor_event(void) {
    left_on_purpose = 0;
    if (sd_mon_read(from_shown) == 0 || left_on_purpose) { left_on_purpose = 0; return; }
    /* the session being shown ended: it logged out, or the login screen crashed */
    char was = shown;
    sd_mon_lost();
    set_shown('-', NULL);
    if (was == 'u') { sd_log("the monitor's session ended"); show_greeter("login", NULL); }
    else show_greeter(greeter_mode, greeter_user);
}

/* ---- remote desktop ---- */

static SSL_CTX *tls;
/* this process's connection children (the only pids it ever signals) */
static struct { pid_t pid; char key[48]; } kids[RD_MAX_CHILDREN];

static void stop_remote(void) {
    if (lfd >= 0) close(lfd);
    lfd = -1;
    listening = 0;
    /* only our own children (never a pid read back from shared memory) */
    for (int i = 0; i < RD_MAX_CHILDREN; i++) if (kids[i].pid > 1) kill(kids[i].pid, SIGTERM);
    rd_tokens_clear();                  /* turning remote off signs every browser out */
    if (tls) SSL_CTX_free(tls);
    tls = NULL;
}

/* ICE-TCP handoff sockets left by connections that ended without cleaning up (killed): only
 * sockets, only in root's own folder (conn.c ice_listen) */
static void clean_rtc_dir(void) {
    struct stat st;
    if (lstat(RD_RTC_DIR, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0) return;
    DIR *d = opendir(RD_RTC_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 8) continue;
        if (!fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISSOCK(st.st_mode)) unlinkat(dirfd(d), e->d_name, 0);
    }
    closedir(d);
}

static void start_remote(void) {
    clean_rtc_dir();
    if (ensure_cert()) { sd_log("could not create the certificate"); return; }
    write_fingerprint();
    /* the private key: root's, 0600 (fixed if it isn't) */
    struct stat ks;
    if (!stat(RD_CONF_DIR "/key.pem", &ks) && (ks.st_uid != 0 || (ks.st_mode & 077))) {
        sd_log("%s had owner %u and mode %o: set to root 0600", RD_CONF_DIR "/key.pem", (unsigned)ks.st_uid, (unsigned)(ks.st_mode & 0777));
        if (chown(RD_CONF_DIR "/key.pem", 0, 0) || chmod(RD_CONF_DIR "/key.pem", 0600)) { sd_log("could not fix the key's permissions"); return; }
    }
    tls = SSL_CTX_new(TLS_server_method());
    /* TLS 1.2 and 1.3, AEAD suites with forward secrecy only (the key is P-256: ECDSA), no
     * compression, no renegotiation, the server's order */
    if (!tls || SSL_CTX_set_min_proto_version(tls, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(tls, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305") != 1 ||
        SSL_CTX_set_ciphersuites(tls, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256") != 1 ||
        SSL_CTX_set1_groups_list(tls, "X25519:P-256:P-384") != 1 ||
        SSL_CTX_use_certificate_file(tls, RD_CONF_DIR "/cert.pem", SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(tls, RD_CONF_DIR "/key.pem", SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr);
        if (tls) SSL_CTX_free(tls);
        tls = NULL;
        return;
    }
    SSL_CTX_set_options(tls, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    lfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1, zero = 0;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)rd_conf.port), .sin6_addr = in6addr_any };
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 64)) {
        sd_log("could not listen on port %d: %s", rd_conf.port, strerror(errno));
        if (lfd >= 0) close(lfd);
        lfd = -1;
        return;
    }
    listening = 1;
    sd_log("remote desktop on https://*:%d", rd_conf.port);
}

/* sockaddr (AF_INET or AF_INET6) → RsAddr; 0 on success */
static int sa_addr(const struct sockaddr *sa, RsAddr *x) {
    static const unsigned char pre[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    memset(x, 0, sizeof *x);
    if (!sa) return -1;
    if (sa->sa_family == AF_INET) {
        memcpy(x->a, pre, 12);
        memcpy(x->a + 12, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        x->v4 = 1;
        return 0;
    }
    if (sa->sa_family == AF_INET6) {
        memcpy(x->a, &((const struct sockaddr_in6 *)sa)->sin6_addr, 16);
        x->v4 = !memcmp(x->a, pre, 12);
        return 0;
    }
    return -1;
}

static int mask_bits(const RsAddr *m) {
    int n = 0;
    for (int i = m->v4 ? 12 : 0; i < 16; i++) {
        unsigned char b = m->a[i];
        while (b & 0x80) { n++; b = (unsigned char)(b << 1); }
        if (m->a[i] != 0xff) break;
    }
    return m->v4 ? n + 96 : n;
}

/* Does this connection count as "over a LordOS VPN" for net=lan? Only on an interface LordOS's VPN
 * manager created (wg-, ovpn-, ips-, ppp-, oc-, fvpn- + name), to that interface's own address, and
 * from inside its subnet or its point-to-point peer (ipnet.c rs_vpn_accepts). A bare ppp0 (a PPPoE
 * uplink), tun0 or wg0 never counts. */
static int via_vpn(int fd, const RsAddr *peer) {
    struct sockaddr_in6 me;
    socklen_t ml = sizeof me;
    if (getsockname(fd, (struct sockaddr *)&me, &ml)) return 0;
    RsAddr local;
    if (sa_addr((struct sockaddr *)&me, &local)) return 0;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs)) return 0;
    int vpn = 0;
    for (struct ifaddrs *i = ifs; i && !vpn; i = i->ifa_next) {
        RsAddr x, m, d;
        if (!i->ifa_name || !rs_vpn_ifname(i->ifa_name) || sa_addr(i->ifa_addr, &x) || sa_addr(i->ifa_netmask, &m)) continue;
        int ptp = (i->ifa_flags & IFF_POINTOPOINT) && !sa_addr(i->ifa_dstaddr, &d);
        vpn = rs_vpn_accepts(i->ifa_name, &x, mask_bits(&m), ptp ? &d : NULL, &local, peer);
    }
    freeifaddrs(ifs);
    return vpn;
}

/* Refusals are logged at most once a minute per source (a flood must not flood the journal). */
static void refuse_log(const char *key, const char *why) {
    static struct { char key[48]; double at; } seen[32];
    double now = rd_now();
    int slot = 0;
    for (int i = 0; i < 32; i++) {
        if (!strcmp(seen[i].key, key)) { if (now - seen[i].at < 60) return; slot = i; break; }
        if (seen[i].at < seen[slot].at) slot = i;
    }
    snprintf(seen[slot].key, sizeof seen[slot].key, "%s", key);
    seen[slot].at = now;
    sd_log("remote connection from %s refused: %s", key, why);
}

static RcTable rate;

static void accept_remote(void) {
    struct sockaddr_in6 peer;
    socklen_t pl = sizeof peer;
    int fd = accept4(lfd, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
    if (fd < 0) return;
    char ip[64];
    inet_ntop(AF_INET6, &peer.sin6_addr, ip, sizeof ip);
    const char *shown_ip = !strncmp(ip, "::ffff:", 7) ? ip + 7 : ip;
    RsAddr a = { 0 };
    memcpy(a.a, &peer.sin6_addr, 16);
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    a.v4 = !memcmp(a.a, mapped, 12);
    char key[48];
    rs_addr_key(&a, key, sizeof key);
    /* everything here is cheap and happens before any TLS work: refused sources are just closed */
    int vpn = rd_conf.net_mode == RS_NET_LAN && !rs_addr_is_local(&a) ? via_vpn(fd, &a) : 0;
    int acc = rs_access_decide(&rd_conf.access, rd_conf.net_mode, &a, vpn);
    if (acc != RS_ACC_OK) {
        refuse_log(key, acc == RS_ACC_BLOCKED ? "blocked in Settings" :
                   rd_conf.net_mode == RS_NET_LIST ? "not in the allowed list" : "not a local network address (Settings -> Sharing)");
        close(fd);
        return;
    }
    rd_lock();
    double left = rl_blocked(&rd_shared->lockout, key, rd_now());
    rd_unlock();
    if (left > 0) {
        char why[96];
        snprintf(why, sizeof why, "blocked after failed logins, %.0f min left", left / 60 + 0.5);
        refuse_log(key, why);
        close(fd);
        return;
    }
    struct timespec mt;
    clock_gettime(CLOCK_MONOTONIC, &mt);
    if (!rc_take(&rate, key, (double)mt.tv_sec + mt.tv_nsec / 1e9)) { refuse_log(key, "too many new connections"); close(fd); return; }
    int total = 0, mine = 0, free_slot = -1;
    for (int i = 0; i < RD_MAX_CHILDREN; i++) {
        if (!kids[i].pid) { if (free_slot < 0) free_slot = i; continue; }
        total++;
        if (!strcmp(kids[i].key, key)) mine++;
    }
    if (free_slot < 0 || total >= RD_MAX_CHILDREN || mine >= RD_MAX_PER_SOURCE) {
        refuse_log(key, mine >= RD_MAX_PER_SOURCE ? "too many open connections from it" : "too many open connections");
        close(fd);
        return;
    }
    pid_t c = fork();
    if (c == 0) {
        close(lfd);
        sd_mon_child();                 /* without the monitor's card or input */
        if (wk_r >= 0) close(wk_r);
        if (wk_w >= 0) close(wk_w);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        rd_serve(tls, fd, shown_ip, key);
        _exit(0);
    }
    if (c > 0) {
        kids[free_slot].pid = c;
        snprintf(kids[free_slot].key, sizeof kids[free_slot].key, "%s", key);
    }
    close(fd);
}

/* ---- lockout: persistence, unblock requests from Settings, notices ---- */

static void save_lockout(void) {
    rd_lock();
    rd_shared->lockout_dirty = 0;
    RlTable copy = rd_shared->lockout;
    rd_unlock();
    char *t = malloc(RL_MAX * 128 + 256);
    if (!t) return;
    int n = rl_format(&copy, rd_now(), t, RL_MAX * 128 + 256);
    mkdir("/var/lib/lordos", 0755);
    mkdir(RL_FILE_DIR, 0700);
    struct stat st;
    if (n < 0 || lstat(RL_FILE_DIR, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0) { free(t); return; }
    if (st.st_mode & 077) chmod(RL_FILE_DIR, 0700);
    unlink(RL_FILE ".new");
    int fd = open(RL_FILE ".new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { free(t); return; }
    int ok = write(fd, t, (size_t)n) == n && !fsync(fd);
    close(fd);
    free(t);
    if (!ok || rename(RL_FILE ".new", RL_FILE)) { unlink(RL_FILE ".new"); sd_log("could not save %s", RL_FILE); }
}

static void load_lockout(void) {
    char *t = read_root_file(RL_FILE, RL_MAX * 160);
    RlTable tab;
    rl_parse(&tab, t, rd_now());
    free(t);
    int n = 0;
    for (int i = 0; i < RL_MAX; i++) if (tab.e[i].key[0] && tab.e[i].until > rd_now()) n++;
    rd_lock();
    rd_shared->lockout = tab;
    rd_unlock();
    if (n) sd_log("remote desktop: %d source(s) still blocked after failed logins (%s)", n, RL_FILE);
}

/* "unblock <cidr>" lines the admin helper appended (remote-unblock), under flock */
static void unblock_requests(void) {
    int fd = open(RL_REQUESTS, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 077)) { close(fd); unlink(RL_REQUESTS); return; }
    flock(fd, LOCK_EX);
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (ftruncate(fd, 0)) { /* read again next time */ }
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char net[64], extra;
        if (sscanf(line, "unblock %63s %c", net, &extra) != 1) continue;
        rd_lock();
        int k = rl_unblock(&rd_shared->lockout, net);
        if (k) { rd_shared->lockout_dirty = 1; rd_shared->version++; }
        /* and the growing per-source waits of those sources start over */
        RsCidr req, fk;
        if (!rs_cidr_parse(net, &req))
            for (int i = 0; i < 128; i++)
                if (rd_shared->fails[i].key[0] && !rs_key_parse(rd_shared->fails[i].key, &fk) &&
                    (rs_cidr_covers(&req, &fk) || rs_cidr_covers(&fk, &req))) memset(&rd_shared->fails[i], 0, sizeof rd_shared->fails[i]);
        rd_unlock();
        char clean[64];
        rs_log_clean(net, strlen(net), clean, sizeof clean);
        sd_log("remote desktop: %s unblocked from Settings (%d entr%s)", clean, k, k == 1 ? "y" : "ies");
    }
}

/* Blocks the connection processes reported: to the session on the monitor (a notification). They
 * wait (up to an hour) while the monitor shows the login screen. */
static unsigned notices_done;

static void deliver_notices(void) {
    unsigned seq;
    RdNotice ring[RD_NOTICES];
    rd_lock();
    seq = rd_shared->notice_seq;
    memcpy(ring, rd_shared->notices, sizeof ring);
    rd_unlock();
    if (seq == notices_done) return;
    if (shown != 'u' || sd_mon_fd() < 0) return;
    if (seq - notices_done > RD_NOTICES) notices_done = seq - RD_NOTICES;
    for (; notices_done != seq; notices_done++) {
        RdNotice *n = &ring[notices_done % RD_NOTICES];
        if (rd_now() - n->at > 3600 || !n->key[0]) continue;
        RsCidr c;
        if (rs_key_parse(n->key, &c)) continue;             /* only a valid address goes out */
        char json[160];
        snprintf(json, sizeof json, "{\"t\":\"blocked\",\"ip\":\"%s\",\"min\":%.0f,\"n\":%d}", n->key,
                 (n->until - n->at) / 60 + 0.5, n->strikes);
        sd_mon_notice(json);
    }
}

/* ---- login screen processes ----
 * The login screen keeps no state of its own. lordos.service uses KillMode=process so that the
 * sessions survive a restart, which also left the greeter running after its lord-sessiond had
 * stopped (systemd: "remains running after unit stopped"): after a package upgrade it kept the
 * old binary, and one the new lord-sessiond couldn't reach stayed for good. Every process of the
 * greeter's system user is stopped when lord-sessiond stops, and any left over (a crash) when it
 * starts; the next login screen is started on demand. */
static int proc_real_uid(long pid, uid_t *uid) {
    char p[64], line[128];
    snprintf(p, sizeof p, "/proc/%ld/status", pid);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    int found = -1;
    char state = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long u;
        if (sscanf(line, "State: %c", &state) == 1) continue;
        if (sscanf(line, "Uid: %lu", &u) == 1) { *uid = (uid_t)u; found = 0; break; }
    }
    fclose(f);
    return found == 0 && state != 'Z' ? 0 : -1;     /* a zombie is gone already */
}

static void stop_greeters(const char *why) {
    struct passwd *pw = getpwnam(SD_GREETER_USER);
    if (!pw || pw->pw_uid == 0) return;
    uid_t gu = pw->pw_uid;
    int stopped = 0;
    for (int round = 0; round < 30; round++) {         /* SIGTERM, then SIGKILL after 1 s */
        int left = 0;
        DIR *d = opendir("/proc");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            long pid = strtol(e->d_name, &end, 10);
            uid_t u;
            if (*end || pid <= 1 || pid == (long)getpid() || proc_real_uid(pid, &u) || u != gu) continue;
            if (round == 0) { kill((pid_t)pid, SIGTERM); stopped++; }
            else if (round == 20) kill((pid_t)pid, SIGKILL);
            left++;
        }
        closedir(d);
        if (!left) break;
        usleep(50000);
    }
    if (stopped) sd_log("login screen: %d process(es) of %s stopped (%s)", stopped, SD_GREETER_USER, why);
}

/* ---- main ---- */

static volatile sig_atomic_t stop, reload, vt_leave, vt_back;
static void on_term(int s) { (void)s; stop = 1; }
static void on_hup(int s) { (void)s; reload = 1; }
static void on_vt(int s) { if (s == SIGUSR1) vt_leave = 1; else vt_back = 1; }

int main(int argc, char **argv) {
    int no_monitor = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-monitor")) no_monitor = 1;   /* remote desktop only (testing) */
        else if (!strcmp(argv[i], "--start-login") || !strcmp(argv[i], "--release-cgroups")) { /* handled below */ }
        else { fprintf(stderr, "usage: lord-sessiond [--no-monitor] | --start-login | --release-cgroups\n"); return 2; }
    }
    /* lordos.service's ExecStartPre (GitHub #140, docs/SESSIONS.md "cgroups"): the controllers off in the service's
     * tree, so systemd 258 and later can start the new lord-sessiond in it while sessions live on. Always exit 0:
     * on an older systemd, or without delegation, the start goes on as before. */
    if (argc == 2 && !strcmp(argv[1], "--release-cgroups")) {
        if (geteuid() != 0) return 0;
        if (cgroup_service_release()) fprintf(stderr, "lord-sessiond: the cgroup controllers stayed on (a restart may fail on systemd 258 or later)\n");
        return 0;
    }
    /* startlordos (a user, on a text console): ask the running lord-sessiond for the login screen. Prints its answer:
     * ok, refused, no-display; exit 0 only for ok. */
    if (argc == 2 && !strcmp(argv[1], "--start-login")) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", START_SOCK);
        if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { puts("unavailable"); return 3; }
        char buf[32] = "";
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) buf[n] = 0;
        fputs(n > 0 ? buf : "refused\n", stdout);
        return !strncmp(buf, "ok", 2) ? 0 : 2;
    }
    if (geteuid() != 0) { fprintf(stderr, "lord-sessiond: must run as root (lordos.service)\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    /* its lines also go, stamped, to /var/log/lordos/lord-sessiond.log (lordlog.h) */
    if (lordlog_system_start("lord-sessiond")) fprintf(stderr, "lord-sessiond: no " LORDLOG_ROOT "/lord-sessiond.log: the journal only\n");
    char prefix[64];
    snprintf(prefix, sizeof prefix, "lord-sessiond[%d]", (int)getpid());
    sd_log_prefix(prefix);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    struct sigaction sh = { .sa_handler = on_hup };
    sigaction(SIGHUP, &sh, NULL);
    /* text consoles: the kernel's questions for our tty1 (monitor.c); no SA_RESTART, so poll wakes at once */
    struct sigaction sv = { .sa_handler = on_vt };
    sigaction(SIGUSR1, &sv, NULL);
    sigaction(SIGUSR2, &sv, NULL);
    find_paths();
    load_config();
    load_last_user();
    load_main_display();
    load_greeter_scale();
    stop_greeters("left over from before this start");
    mkdir("/run/lordos", 0755);
    /* delegated cgroup (Delegate=yes): sessiond moves into the "supervisor" leaf and each session gets
     * its own subtree (cgroup_delegate_session in sess.c). Without delegation it works as before. */
    if (!cgroup_service_init()) {
        sd_log("cgroup: delegated service; each session will get its own subtree");
        sd_cgroup_start();          /* app scopes are root's: placed on request (cgsup.c) */
    }

    rd_shared = mmap(NULL, sizeof *rd_shared, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (rd_shared == MAP_FAILED) return 1;
    memset(rd_shared, 0, sizeof *rd_shared);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&rd_shared->lock, &ma);
    sd_dummy_init();
    load_lockout();
    unblock_requests();
    /* sessions that stayed alive from before a service restart */
    for (uid_t u = 1000; u < 1100; u++) {
        int fd = sd_session_connect(u);
        if (fd < 0) continue;
        struct ucred cr;
        socklen_t cl = sizeof cr;
        struct passwd *pw = getpwuid(u);
        if (pw && !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) && cr.uid == u) {
            for (int i = 0; i < SD_MAX_SESSIONS; i++) {
                if (rd_shared->sessions[i].pid) continue;
                rd_shared->sessions[i] = (SdSession){ .pid = cr.pid, .uid = u, .since = rd_now() };
                snprintf(rd_shared->sessions[i].user, sizeof rd_shared->sessions[i].user, "%s", pw->pw_name);
                break;
            }
            /* it keeps the environment it started with: LORD_GPU or LORD_SW_CURSOR set since then reach it only
             * after logging out (KillMode=process keeps sessions across a restart of the service; #57) */
            sd_log("%s's session is still open (it keeps the GPU settings it started with until it logs out)", pw->pw_name);
        }
        close(fd);
    }

    /* session GPU: lordos.gpu=0 on the kernel command line (safe mode) or the service's LORD_GPU;
     * otherwise each session uses it while it's on the monitor */
    {
        char cmd[2048] = "";
        FILE *f = fopen("/proc/cmdline", "r");
        if (f) { if (!fgets(cmd, sizeof cmd, f)) cmd[0] = 0; fclose(f); }
        const char *g = getenv("LORD_GPU");
        if (strstr(cmd, "lordos.gpu=0")) snprintf(sd_gpu_env, sizeof sd_gpu_env, "LORD_GPU=0");
        else if (g && (!strcmp(g, "0") || !strcmp(g, "1"))) snprintf(sd_gpu_env, sizeof sd_gpu_env, "LORD_GPU=%s", g);
        /* the GPU without its cursor plane, for a driver that draws that plane wrong (#57) */
        const char *swc = getenv("LORD_SW_CURSOR");
        if (strstr(cmd, "lordos.swcursor=1") || (swc && !strcmp(swc, "1"))) snprintf(sd_cursor_env, sizeof sd_cursor_env, "LORD_SW_CURSOR=1");
        /* key latency trace for this boot (docs/TROUBLESHOOTING.md): only root can ask for it, from the
         * boot menu or the service; timings only, see keylag.h */
        const char *k = getenv("LORD_INPUT_DEBUG");
        for (const char *p = cmd; (p = strstr(p, "lordos.inputdebug=1")) != NULL; p++)
            if ((p == cmd || p[-1] == ' ') && (p[19] == 0 || p[19] == ' ' || p[19] == '\n')) sd_input_debug = 1;
        if (k && !strcmp(k, "1")) sd_input_debug = 1;
        if (sd_input_debug) sd_log("key latency trace on for user sessions (lordos.inputdebug=1)");
        /* GPU switches that are off by default, for a measurement: only root sets the service's environment,
         * and the login screen never gets them (docs/GPU.md) */
        static const char *const tune[SD_TUNE_N] = { "LORD_GPU_SCISSOR", "LORD_GPU_DRAG", "LORD_PIPELINE", "LORD_GPU_VERIFY" };
        for (int i = 0; i < SD_TUNE_N; i++) {
            const char *v = getenv(tune[i]);
            if (v && strlen(v) < 10) {
                snprintf(sd_tune_env[i], sizeof sd_tune_env[i], "%s=%s", tune[i], v);
                sd_log("user sessions get %s", sd_tune_env[i]);
            }
        }
    }
    sd_log("sessions with %s%s%s%s%s", rd_conf.server, sd_gpu_env[0] ? ", " : "", sd_gpu_env, sd_cursor_env[0] ? ", " : "", sd_cursor_env);
    int wk[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, wk) == 0) { wk_r = wk[0]; wk_w = wk[1]; }
    /* the network belongs to the system: lord-netd brings it up before the greeter (netsup.c) */
    sd_netd_start();
    int text = no_monitor ? 0 : text_mode();
    if (text) sd_log("text mode (%s): no login screen; log in on tty1 and type startlordos", text == 1 ? "lordos.text=1" : "/etc/lordos/text-mode");
    if (text) { text_now = 1; start_listen(); }
    has_monitor = !no_monitor && !text && sd_mon_open() == 0;
    if (has_monitor) boot_monitor();
    else {
        first_frame();                  /* nothing will be shown: the boot splash can go now */
        if (!no_monitor && !text) tell_no_display();
    }
    if (rd_conf.enabled) start_remote();
    write_status();

    unsigned seen = 0;
    while (!stop) {
        if (reload) {
            reload = 0;
            int was = rd_conf.enabled, port = rd_conf.port;
            load_config();
            unblock_requests();
            sd_log("configuration: remote %s, port %d, from %s; %d STUN server(s), TURN %s%s%s", rd_conf.enabled ? "on" : "off", rd_conf.port,
                   rs_net_name(rd_conf.net_mode), rd_conf.nstun, rd_conf.turn[0] ? "over " : "none", rd_conf.turn[0] ? rw_turn_transport_name(rd_conf.turn_transport) : "",
                   rd_conf.turn[0] && rd_conf.turn_has_fp ? " (pinned certificate)" : "");
            if (listening && (!rd_conf.enabled || port != rd_conf.port)) stop_remote();
            if (rd_conf.enabled && (!listening || !was)) start_remote();
            bump();
        }
        struct pollfd p[6] = { { wk_r, POLLIN, 0 }, { lfd, POLLIN, 0 }, { sd_mon_fd(), POLLIN, 0 }, { sd_mon_hotplug_fd(), POLLIN, 0 },
                               { sd_netd_fd(), POLLIN, 0 }, { start_fd, POLLIN, 0 } };
        int r = poll(p, 6, 1000);
        pid_t pid;
        int st;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            if (sd_netd_reaped(pid, st)) continue;     /* lord-netd (netsup.c) */
            if (sd_cgroup_reaped(pid, st)) continue;   /* cgroup placement service (cgsup.c) */
            if (pid == worker_pid) {
                worker_pid = -1;
                WorkKind k = pending;
                pending = W_NONE;
                if (k == W_GREETER) show_greeter(greeter_mode, greeter_user);
                else if (k == W_AUTOLOGIN && start_worker(W_AUTOLOGIN, pending_user, NULL)) pending = k;
                else if (WIFEXITED(st) && WEXITSTATUS(st) && shown != 'u' && shown != 'g') show_greeter("login", NULL);
            } else {
                for (int i = 0; i < RD_MAX_CHILDREN; i++) if (kids[i].pid == pid) memset(&kids[i], 0, sizeof kids[i]);
                rd_lock();
                for (int i = 0; i < RD_MAX_CONNS; i++)
                    if (rd_shared->conns[i].pid == pid) { memset(&rd_shared->conns[i], 0, sizeof rd_shared->conns[i]); rd_shared->version++; }
                rd_unlock();
            }
        }
        if (r > 0 && p[5].fd >= 0 && (p[5].revents & POLLIN)) start_request();
        if (vt_leave) { vt_leave = 0; vt_away(); }
        if (vt_back) { vt_back = 0; vt_return(); }
        if (r > 0 && p[4].fd >= 0 && p[4].revents) sd_netd_event();
        sd_netd_tick();
        sd_cgroup_tick();
        if (r > 0 && (p[0].revents & POLLIN)) from_worker();
        if (r > 0 && lfd >= 0 && (p[1].revents & POLLIN)) accept_remote();
        if (r > 0 && p[2].fd >= 0 && p[2].fd == sd_mon_fd() && p[2].revents) monitor_event();
        if (r > 0 && p[3].fd >= 0 && (p[3].revents & POLLIN)) sd_mon_hotplug();     /* hotplug */
        if (greeter_stop_at && rd_now() >= greeter_stop_at) {
            greeter_stop_at = 0;
            if (shown == 'u' && worker_pid < 0 && pending == W_NONE) stop_greeters("a session is on the monitor");
        }
        if (rd_shared->lockout_dirty) save_lockout();
        boot_test_tick();
        deliver_notices();
        if (rd_shared->version != seen) { seen = rd_shared->version; write_status(); }
        else {
            static double last;
            if (rd_now() - last > 5) { last = rd_now(); write_status(); }   /* sessions that ended */
        }
    }
    /* on shutdown: cuts remote and releases the monitor; sessions stay alive (on return
     * you log in again through the login screen) */
    stop_remote();
    save_lockout();
    start_stop();
    if (has_monitor) sd_mon_close();
    if (worker_pid > 0) kill(worker_pid, SIGTERM);
    stop_greeters("lord-sessiond is stopping");
    listening = 0;
    rd_lock();
    rd_shared->monitor[0] = 0;
    rd_unlock();
    write_status();
    return 0;
}
