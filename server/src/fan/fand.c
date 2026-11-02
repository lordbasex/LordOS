/* fand.c — axia-fand: the root service that drives the fans by temperature (docs/FANS.md).
 * Closed and minimal, like axia-netperm:
 *
 *   - executes nothing and opens no network sockets (the unit forbids every family but AF_UNIX);
 *   - reads the CPU temperature every FAND_TICK_MS and writes each fan's duty (fan/fanhw.c);
 *   - its configuration is /etc/lordos/fan.json, written only here, after validating;
 *   - talks to sessions over /run/lordos/fand.sock (0666) with one line per connection:
 *       STATUS                     anyone: a JSON line with temperature, fans, profile, step and
 *                                  whether this peer may change them ("canChange")
 *       PROFILE <name> / CURVE <t:p,...> / FORCE off|on|<minutes>
 *                                  administrators only (SO_PEERCRED: root, or a member of
 *                                  sudo, admin or wheel): "OK" or "ERR <code>"
 *     Clients never name paths or devices: the daemon drives every fan it found.
 *   - on SIGTERM/SIGINT, and in `axia-fand --restore` (the unit's ExecStopPost, which also runs
 *     after a crash), the fans go back to the kernel or the firmware.
 *
 * Other modes, for diagnosis: `axia-fand --detect` prints what it found and exits.
 * For tests only (never in the unit): --root DIR (sysfs root), --config FILE, --run DIR. */
#include "fan/fancurve.h"
#include "fan/fanhw.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FAND_TICK_MS   2000
#define FAND_CLIENTS   8
#define FAND_CLIENT_MS 1000
#define FAND_STATUS_MAX 4096

typedef struct {
    int fd;
    long long deadline;
    size_t len;
    char buf[FAN_REQ_MAX + 2];
} Client;

static struct {
    const char *root, *config, *run;
    char sock[256], state[256];
    FanHw hw;
    FanConfig cfg;
    FanCurve curve;
    int step;
    int temp;
    int out_pct;          /* what the fans were asked for */
    int controlling;      /* 1 while we drive the fans (profile != auto or forced) */
    Client cl[FAND_CLIENTS];
} S = { .root = "", .config = "/etc/lordos/fan.json", .run = "/run/lordos", .step = -1 };

static volatile sig_atomic_t quit;
static void on_signal(int s) { (void)s; quit = 1; }

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static long long wall_s(void) { return (long long)time(NULL); }

/* ---- configuration ---- */

static void load_config(void) {
    fan_config_default(&S.cfg, S.hw.n > 0 && S.hw.t[0].kind == FK_BOARD);
    int fd = open(S.config, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    char buf[4096];
    ssize_t r = -1;
    /* only a root-owned regular file that nobody else can write */
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && st.st_size < (off_t)sizeof buf)
        r = read(fd, buf, sizeof buf - 1);
    close(fd);
    char err[96] = "";
    if (r < 0 || fan_config_parse(buf, (size_t)r, &S.cfg, err, sizeof err))
        fprintf(stderr, "axia-fand: %s ignored (%s), using defaults\n", S.config, r < 0 ? "not a root-owned file" : err);
}

static int save_config(void) {
    char buf[1024], tmp[300];
    size_t n = fan_config_write(&S.cfg, buf, sizeof buf);
    snprintf(tmp, sizeof tmp, "%s.tmp", S.config);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    int ok = n > 0 && write(fd, buf, n) == (ssize_t)n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, S.config)) { unlink(tmp); return -1; }
    return 0;
}

/* ---- control ---- */

static int forced(void) {
    if (S.cfg.force == FF_ON) return 1;
    if (S.cfg.force == FF_TIMED) {
        if (wall_s() < S.cfg.force_until) return 1;
        S.cfg.force = FF_OFF;
        S.cfg.force_until = 0;
        save_config();
    }
    return 0;
}

static void apply_profile(void) {
    fan_profile_curve(&S.cfg, &S.curve);
    S.step = -1;
}

static void tick(void) {
    S.temp = fanhw_temp(&S.hw);
    int want_control = S.hw.n > 0 && (S.cfg.profile != FP_AUTO || forced());
    if (!want_control) {
        if (S.controlling) { fanhw_release(&S.hw); unlink(S.state); S.controlling = 0; }
        S.out_pct = -1;
        S.step = -1;
        return;
    }
    if (!S.controlling) { fanhw_take(&S.hw, S.state); S.controlling = 1; }
    int pct;
    if (forced()) pct = 100;
    else if (S.temp <= -1000000) pct = 100;            /* no temperature: be safe */
    else {
        S.step = fan_curve_step(&S.curve, S.cfg.hyst, S.step, S.temp);
        pct = fan_curve_pct(&S.curve, S.step);
    }
    S.out_pct = fan_output_pct(pct);
    for (int i = 0; i < S.hw.n; i++) fanhw_set(&S.hw, i, S.out_pct);
}

/* ---- status ---- */

static size_t status_json(char *o, size_t n, int can_change) {
    size_t k = 0;
#define PUT(...) do { if (k < n) { int r_ = snprintf(o + k, n - k, __VA_ARGS__); if (r_ > 0) k += (size_t)r_; } } while (0)
    PUT("{\"ok\":true,\"canChange\":%s,\"board\":\"%s\",\"tempSource\":\"%s\",", can_change ? "true" : "false", S.hw.board ? "pi5" : "", S.hw.temp_src);
    if (S.temp > -1000000) PUT("\"temp\":%d.%d,", S.temp / 1000, (S.temp < 0 ? -S.temp : S.temp) % 1000 / 100);
    else PUT("\"temp\":null,");
    PUT("\"profile\":\"%s\",\"hyst\":%d,\"controlling\":%s,\"step\":%d,\"target\":%d,",
        fan_profile_name(S.cfg.profile), S.cfg.hyst, S.controlling ? "true" : "false", S.step, S.out_pct);
    FanCurve cur;
    fan_profile_curve(&S.cfg, &cur);
    PUT("\"floor\":%d,\"curve\":[", cur.floor_pct);
    for (int i = 0; i < cur.n; i++) PUT("%s[%d,%d]", i ? "," : "", cur.p[i].temp, cur.p[i].pct);
    PUT("],\"custom\":[");
    for (int i = 0; i < S.cfg.custom.n; i++) PUT("%s[%d,%d]", i ? "," : "", S.cfg.custom.p[i].temp, S.cfg.custom.p[i].pct);
    long long left = S.cfg.force == FF_TIMED ? S.cfg.force_until - wall_s() : 0;
    PUT("],\"force\":\"%s\",\"forceLeft\":%lld,\"fans\":[", S.cfg.force == FF_ON ? "on" : S.cfg.force == FF_TIMED ? "timed" : "off", left > 0 ? left : 0);
    for (int i = 0; i < S.hw.n; i++)
        PUT("%s{\"id\":%d,\"kind\":\"%s\",\"chip\":\"%s\",\"ch\":%d,\"pct\":%d,\"rpm\":%d}", i ? "," : "", i,
            fanhw_kind_name(S.hw.t[i].kind), S.hw.t[i].chip, S.hw.t[i].ch, fanhw_pct(&S.hw, i), fanhw_rpm(&S.hw, i));
    PUT("],\"firmwareFans\":[");
    for (int i = 0; i < S.hw.nro; i++) PUT("%s{\"chip\":\"%s\",\"rpm\":%d}", i ? "," : "", S.hw.ro[i].chip, fanhw_ro_rpm(&S.hw, i));
    PUT("]}\n");
#undef PUT
    return k < n ? k : 0;
}

/* ---- socket ---- */

static int peer_is_admin(int fd) {
    struct ucred uc;
    socklen_t l = sizeof uc;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &l)) return 0;
    if (uc.uid == 0) return 1;
    struct passwd pw, *pp = NULL;
    char pbuf[2048];
    if (getpwuid_r(uc.uid, &pw, pbuf, sizeof pbuf, &pp) || !pp) return 0;
    gid_t groups[64];
    int n = 64;
    if (getgrouplist(pp->pw_name, pp->pw_gid, groups, &n) < 0) return 0;
    for (int i = 0; i < n; i++) {
        struct group gr, *gp = NULL;
        char gbuf[4096];
        if (getgrgid_r(groups[i], &gr, gbuf, sizeof gbuf, &gp) || !gp) continue;
        if (!strcmp(gp->gr_name, "sudo") || !strcmp(gp->gr_name, "admin") || !strcmp(gp->gr_name, "wheel")) return 1;
    }
    return 0;
}

static void reply(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void handle(Client *c) {
    char *nl = memchr(c->buf, '\n', c->len);
    if (!nl) { reply(c->fd, "ERR fan.badRequest\n", 19); return; }
    *nl = 0;
    FanReq r;
    if (fan_req_parse(c->buf, (size_t)(nl - c->buf), &r)) { reply(c->fd, "ERR fan.badRequest\n", 19); return; }
    if (r.type == FR_STATUS) {
        char o[FAND_STATUS_MAX];
        size_t n = status_json(o, sizeof o, peer_is_admin(c->fd));
        reply(c->fd, o, n);
        return;
    }
    if (!peer_is_admin(c->fd)) { reply(c->fd, "ERR fan.notAdmin\n", 17); return; }
    FanConfig old = S.cfg;
    if (r.type == FR_PROFILE) S.cfg.profile = r.profile;
    else if (r.type == FR_CURVE) { S.cfg.custom = r.curve; S.cfg.profile = FP_CUSTOM; }
    else if (r.type == FR_FORCE) {
        S.cfg.force = r.force;
        S.cfg.force_until = r.force == FF_TIMED ? wall_s() + (long long)r.minutes * 60 : 0;
    }
    if (save_config()) { S.cfg = old; reply(c->fd, "ERR fan.saveFailed\n", 19); return; }
    fprintf(stderr, "axia-fand: %s\n", c->buf);
    apply_profile();
    tick();
    reply(c->fd, "OK\n", 3);
}

static void drop(Client *c) { close(c->fd); c->fd = -1; c->len = 0; }

static int listen_sock(void) {
    mkdir(S.run, 0755);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(S.sock) >= sizeof a.sun_path) { close(fd); return -1; }
    memcpy(a.sun_path, S.sock, strlen(S.sock) + 1);
    unlink(S.sock);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || chmod(S.sock, 0666) || listen(fd, 8)) { close(fd); return -1; }
    return fd;
}

/* ---- modes ---- */

static int detect(void) {
    FanHw *h = &S.hw;
    printf("board: %s\ntemperature: %s (%s)", h->board ? "Raspberry Pi 5" : "other", h->temp_src[0] ? h->temp_src : "none", h->temp_path);
    int t = fanhw_temp(h);
    if (t > -1000000) printf(" %d.%d C", t / 1000, (t < 0 ? -t : t) % 1000 / 100);
    printf("\n");
    for (int i = 0; i < h->n; i++)
        printf("fan %d: %s %s pwm%d at %d %%, %d rpm (%s)\n", i, fanhw_kind_name(h->t[i].kind), h->t[i].chip, h->t[i].ch,
               fanhw_pct(h, i), fanhw_rpm(h, i), h->t[i].dir);
    for (int i = 0; i < h->ntrips; i++) printf("kernel trip %d: %d mC (%s)\n", h->trip[i], h->trip_orig[i], h->zone);
    for (int i = 0; i < h->nro; i++) printf("firmware fan: %s %d rpm\n", h->ro[i].chip, fanhw_ro_rpm(h, i));
    if (!h->n) printf("no controllable fans\n");
    return 0;
}

int main(int argc, char **argv) {
    int mode_detect = 0, mode_restore = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--detect")) mode_detect = 1;
        else if (!strcmp(argv[i], "--restore")) mode_restore = 1;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) S.root = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) S.config = argv[++i];
        else if (!strcmp(argv[i], "--run") && i + 1 < argc) S.run = argv[++i];
        else { fprintf(stderr, "usage: axia-fand [--detect | --restore]\n"); return 2; }
    }
    if (strlen(S.run) > 100 || strlen(S.config) > 200) return 2;
    snprintf(S.sock, sizeof S.sock, "%s/fand.sock", S.run);
    snprintf(S.state, sizeof S.state, "%s/fand.restore", S.run);
    umask(022);

    if (mode_restore) {
        if (access(S.state, F_OK)) return 0;          /* nothing taken */
        int r = fanhw_restore_file(S.root, S.state);
        fprintf(stderr, "axia-fand: fans given back to the kernel/firmware%s\n", r ? " (partially)" : "");
        return 0;
    }
    /* a previous run that died without cleaning up: undo it before reading the "original" values */
    if (!mode_detect && !access(S.state, F_OK)) fanhw_restore_file(S.root, S.state);
    fanhw_scan(&S.hw, S.root);
    if (mode_detect) return detect();

    load_config();
    apply_profile();
    fprintf(stderr, "axia-fand: %d fan(s), %d firmware fan(s), temperature from %s, profile %s\n",
            S.hw.n, S.hw.nro, S.hw.temp_src[0] ? S.hw.temp_src : "nothing", fan_profile_name(S.cfg.profile));

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = listen_sock();
    if (lfd < 0) fprintf(stderr, "axia-fand: %s: %s (fans still controlled, no settings)\n", S.sock, strerror(errno));
    for (int i = 0; i < FAND_CLIENTS; i++) S.cl[i].fd = -1;

    long long next = now_ms();
    while (!quit) {
        long long now = now_ms();
        if (now >= next) { tick(); next = now + FAND_TICK_MS; }
        struct pollfd p[1 + FAND_CLIENTS];
        int np = 0;
        if (lfd >= 0) p[np++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
        long long wake = next;
        for (int i = 0; i < FAND_CLIENTS; i++)
            if (S.cl[i].fd >= 0) {
                p[np++] = (struct pollfd){ .fd = S.cl[i].fd, .events = POLLIN };
                if (S.cl[i].deadline < wake) wake = S.cl[i].deadline;
            }
        int to = (int)(wake - now_ms());
        int r = poll(p, (nfds_t)np, to < 0 ? 0 : to);
        if (r < 0 && errno != EINTR) break;
        now = now_ms();
        for (int k = 0; r > 0 && k < np; k++) {
            if (!p[k].revents) continue;
            if (p[k].fd == lfd) {
                int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd < 0) continue;
                int slot = -1;
                for (int i = 0; i < FAND_CLIENTS && slot < 0; i++) if (S.cl[i].fd < 0) slot = i;
                if (slot < 0) { close(fd); continue; }
                S.cl[slot] = (Client){ .fd = fd, .deadline = now + FAND_CLIENT_MS };
                continue;
            }
            for (int i = 0; i < FAND_CLIENTS; i++) {
                Client *c = &S.cl[i];
                if (c->fd != p[k].fd) continue;
                ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len, 0);
                if (n <= 0) { drop(c); break; }
                c->len += (size_t)n;
                if (memchr(c->buf, '\n', c->len) || c->len >= sizeof c->buf - 1) { handle(c); drop(c); }
                break;
            }
        }
        for (int i = 0; i < FAND_CLIENTS; i++) if (S.cl[i].fd >= 0 && now >= S.cl[i].deadline) drop(&S.cl[i]);
    }
    if (S.controlling) { fanhw_release(&S.hw); unlink(S.state); }
    if (lfd >= 0) { close(lfd); unlink(S.sock); }
    fprintf(stderr, "axia-fand: stopped, fans given back to the kernel/firmware\n");
    return 0;
}
