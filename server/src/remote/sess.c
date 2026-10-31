/* sess.c — what the monitor and the remote desktop share in lord-sessiond: accounts,
 * passwords, waits after failed attempts and the session list (one lord-server --headless
 * per user; see docs/SESSIONS.md).
 *
 * A session belongs to a single user and can be viewed from anywhere: the monitor and the browsers that
 * log in with that user connect to the same socket ($XDG_RUNTIME_DIR/lordos/remote.sock).
 * So that two places at once don't create two sessions of the same user, creation goes through a
 * per-user lock (/run/lordos/spawn-<uid>.lock). */
#include "sessiond.h"
#include "../sec/cgroup.h"
#include "rsec.h"
#include "../par.h"

#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <shadow.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char log_prefix[96] = "lord-sessiond";
char sd_gpu_env[24];
int sd_input_debug;
char sd_tune_env[SD_TUNE_N][32];

void sd_log_prefix(const char *prefix) { snprintf(log_prefix, sizeof log_prefix, "%s", prefix); }

/* A whole line in one go (several processes share the journal). */
void sd_log(const char *fmt, ...) {
    char line[1200];
    int n = snprintf(line, sizeof line, "%s: ", log_prefix);
    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && (size_t)n < sizeof line) n += vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line - 1) n = sizeof line - 2;
    line[n++] = '\n';
    if (write(2, line, (size_t)n) < 0) { /* no logging */ }
}

/* ---- accounts and passwords ---- */

static int ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la != lb);
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

int sd_valid_username(const char *u) { return rs_valid_username(u); }

/* is the shell a real login shell? (listed in /etc/shells, and not nologin/false) */
static int login_shell(const char *sh) {
    if (!sh || !sh[0] || strstr(sh, "nologin") || strstr(sh, "/false")) return 0;
    int ok = 0;
    setusershell();
    for (char *s; (s = getusershell());) if (!strcmp(s, sh)) { ok = 1; break; }
    endusershell();
    return ok;
}

int sd_user_allowed(const struct passwd *pw, int remote) {
    if (!pw || pw->pw_uid < 1000 || pw->pw_uid >= 60000) return 0;
    if (!pw->pw_shell || strstr(pw->pw_shell, "nologin") || strstr(pw->pw_shell, "/false")) return 0;
    if (!remote) return 1;
    /* from the network, stricter: never root's name or group, and a shell from /etc/shells */
    if (!strcmp(pw->pw_name, "root") || pw->pw_gid == 0 || !login_shell(pw->pw_shell)) return 0;
    if (!rd_conf.group_only) return 1;
    struct group *g = getgrnam("lordos-remote");
    if (!g) return 0;
    gid_t groups[256];
    int n = 256;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0) return 0;
    for (int i = 0; i < n; i++) if (groups[i] == g->gr_gid) return 1;
    return 0;
}

/* is it the autologin user (/etc/lordos/session.conf)? */
static int is_autologin(const char *user) {
    FILE *f = fopen(SD_SESSION_CONF, "r");
    char line[128], u[40] = "";
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "AUTOLOGIN=%32[a-z0-9_-]", u) == 1) break;
    if (f) fclose(f);
    return u[0] && !strcmp(u, user);
}

/* A hash setting with the cost of a real one, to spend the same time on unknown or refused
 * accounts as on a wrong password (no user enumeration by timing). Taken once at startup from the
 * first hash in /etc/shadow (only its "$id$params$salt" part), or libxcrypt's default. */
static char dummy[128];

void sd_dummy_init(void) {
    dummy[0] = 0;
    setspent();
    for (struct spwd *sp; (sp = getspent());) {
        const char *h = sp->sp_pwdp, *last = h ? strrchr(h, '$') : NULL;
        if (!h || h[0] != '$' || !last || last == h || (size_t)(last - h) >= sizeof dummy) continue;
        memcpy(dummy, h, (size_t)(last - h));
        dummy[last - h] = 0;
        break;
    }
    endspent();
    if (!dummy[0] && !crypt_gensalt_rn(NULL, 0, NULL, 0, dummy, sizeof dummy)) snprintf(dummy, sizeof dummy, "$6$lordos.dummy");
}

static void waste(const char *pass) {
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *h = crypt_r(pass, dummy[0] ? dummy : "$6$lordos.dummy", &cd);
    volatile int x = h ? ct_equal(h, "*") : 0;
    (void)x;
    explicit_bzero(&cd, sizeof cd);
}

/* Shadow restrictions for logins from the network: no /etc/nologin, password set and not locked,
 * not forced to change (last change 0), not expired (max days) and the account not expired. */
static int shadow_ok_remote(const struct spwd *sp) {
    struct stat st;
    if (!stat("/etc/nologin", &st)) return 0;
    if (!sp || !sp->sp_pwdp || !sp->sp_pwdp[0] || sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*') return 0;
    long today = (long)(time(NULL) / 86400);
    if (sp->sp_lstchg == 0) return 0;
    if (sp->sp_lstchg > 0 && sp->sp_max >= 0 && sp->sp_max < 99999 && today > sp->sp_lstchg + sp->sp_max) return 0;
    if (sp->sp_expire > 0 && sp->sp_expire <= today) return 0;
    return 1;
}

int sd_remote_account_ok(const char *user) {
    struct passwd *pw = sd_valid_username(user) ? getpwnam(user) : NULL;
    if (!sd_user_allowed(pw, 1)) return 0;
    return shadow_ok_remote(getspnam(user));
}

int sd_check_password(const char *user, const char *pass, int remote) {
    if (!sd_valid_username(user) || strlen(pass) > 1024) return 0;
    struct passwd *pw = getpwnam(user);
    struct spwd *sp = sd_user_allowed(pw, remote) ? getspnam(user) : NULL;
    /* account without a password (the live ISO user): only on the monitor, only if it's the autologin
     * user and with the field empty (to come back after locking). Never from the network. */
    if (!remote && sp && sp->sp_pwdp && !sp->sp_pwdp[0] && !pass[0] && is_autologin(user)) return 1;
    int usable = sp && sp->sp_pwdp && sp->sp_pwdp[0] && sp->sp_pwdp[0] != '!' && sp->sp_pwdp[0] != '*' &&
                 !(sp->sp_expire > 0 && sp->sp_expire <= (long)(time(NULL) / 86400));
    if (usable && remote) usable = shadow_ok_remote(sp);
    if (!usable) { waste(pass); return 0; }       /* same work as a wrong password */
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *h = crypt_r(pass, sp->sp_pwdp, &cd);
    int ok = h && h[0] != '*' && ct_equal(h, sp->sp_pwdp);
    explicit_bzero(&cd, sizeof cd);
    return ok;
}

/* Failed attempts (per IP on the network, per user on the monitor): the first two don't wait;
 * then 1, 2, 4… seconds, up to 5 minutes. */
double sd_fail_wait(const char *key) {
    double w = 0, now = rd_now();
    rd_lock();
    for (int i = 0; i < 128; i++)
        if (!strcmp(rd_shared->fails[i].key, key) && rd_shared->fails[i].until > now) w = rd_shared->fails[i].until - now;
    rd_unlock();
    return w;
}

int sd_fail_count(const char *key) {
    int n = 0;
    rd_lock();
    for (int i = 0; i < 128; i++) if (!strcmp(rd_shared->fails[i].key, key)) n = rd_shared->fails[i].fails;
    rd_unlock();
    return n;
}

void sd_fail_note(const char *key, int failed) {
    double now = rd_now();
    rd_lock();
    int slot = -1, oldest = 0;
    for (int i = 0; i < 128; i++) {
        if (!strcmp(rd_shared->fails[i].key, key)) { slot = i; break; }
        if (rd_shared->fails[i].until < rd_shared->fails[oldest].until) oldest = i;
    }
    if (failed) {
        if (slot < 0) {
            slot = oldest;
            memset(&rd_shared->fails[slot], 0, sizeof rd_shared->fails[slot]);
            snprintf(rd_shared->fails[slot].key, sizeof rd_shared->fails[slot].key, "%s", key);
        }
        RdFail *f = &rd_shared->fails[slot];
        f->fails++;
        f->until = now + (f->fails < 3 ? 0 : f->fails >= 12 ? 300 : (double)(1 << (f->fails - 3)));
    } else if (slot >= 0) memset(&rd_shared->fails[slot], 0, sizeof rd_shared->fails[slot]);
    rd_unlock();
}

int sd_run_wait(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    int st = 0;
    if (pid > 0) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

struct passwd *sd_system_user(const char *name, const char *home) {
    struct passwd *pw = getpwnam(name);
    if (!pw) {
        const char *const a[] = { "/usr/sbin/useradd", "--system", "--home-dir", home, "--create-home", "--shell", "/usr/sbin/nologin",
                                  "--comment", "LordOS", name, NULL };
        sd_run_wait(a);
        pw = getpwnam(name);
        if (pw) sd_log("system user %s created", name);
    }
    return pw && pw->pw_uid != 0 ? pw : NULL;
}

/* ---- sessions ---- */

static int connect_path(const char *p) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (fd < 0 || strlen(p) >= sizeof a.sun_path) { if (fd >= 0) close(fd); return -1; }
    memcpy(a.sun_path, p, strlen(p) + 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
    close(fd);
    return -1;
}

int sd_session_connect(uid_t uid) {
    char p[108];
    snprintf(p, sizeof p, "/run/user/%u/lordos/remote.sock", (unsigned)uid);
    return connect_path(p);
}

int sd_session_running(uid_t uid) {
    int r = 0;
    rd_lock();
    for (int i = 0; i < SD_MAX_SESSIONS; i++) {
        SdSession *s = &rd_shared->sessions[i];
        if (s->pid && s->uid == uid && kill(s->pid, 0) == 0) r = 1;
    }
    rd_unlock();
    return r;
}

/* LANG from a locale.conf-style file (validated) */
static void read_lang(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    char buf[512];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return;
    buf[r] = 0;
    char *l = strstr(buf, "LANG=");
    if (!l) return;
    l += 5;
    if (*l == '"') l++;
    size_t k = 0;
    while (l[k] && (isalnum((unsigned char)l[k]) || strchr("_.@-", l[k])) && k < 40) k++;
    if (k) snprintf(out, n, "LANG=%.*s", (int)k, l);
}

static int binary_ok(void) {
    struct stat st;
    if (stat(rd_conf.server, &st) || (!rd_conf.insecure_dev && (st.st_uid != 0 || (st.st_mode & 022)))) {
        sd_log("%s does not exist or is not owned by root (see insecure_dev in docs/SESSIONS.md)", rd_conf.server);
        return 0;
    }
    return 1;
}

/* lord-server --headless as that user (starts as root and drops privileges with --user,
 * same as the monitor's). The grandchild records itself in the shared list before exec. */
static void spawn(const struct passwd *pw, const char *rt, const char *lang, int w, int h, int greeter) {
    char size[32], xdg[128], user[40], home[600];
    snprintf(size, sizeof size, "%dx%d", w & ~1, h & ~1);
    snprintf(xdg, sizeof xdg, "XDG_RUNTIME_DIR=%s", rt);
    snprintf(user, sizeof user, "%s", pw->pw_name);
    snprintf(home, sizeof home, "HOME=%s", pw->pw_dir);
    uid_t uid = pw->pw_uid;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);         /* grandchild: not left as anyone's child here */
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        if (!greeter) {
            rd_lock();
            for (int i = 0; i < SD_MAX_SESSIONS; i++) {
                SdSession *s = &rd_shared->sessions[i];
                if (s->pid && kill(s->pid, 0) == 0) continue;
                *s = (SdSession){ .pid = getpid(), .uid = uid, .since = rd_now() };
                snprintf(s->user, sizeof s->user, "%.32s", user);
                break;
            }
            rd_shared->version++;
            rd_unlock();
        }
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (chdir(rd_conf.workdir)) { /* paths are absolute */ }
        /* GPU: the session uses it while it's on the monitor; lordos.gpu=0 (or the service's LORD_GPU)
         * turns it off in all of them */
        char *env[8 + SD_TUNE_N];
        int ne = 0;
        env[ne++] = "PATH=/usr/local/bin:/usr/bin:/bin:/usr/games";
        env[ne++] = (char *)lang;
        env[ne++] = xdg;
        env[ne++] = home;
        env[ne++] = "LORDOS_SESSIOND=1";
        if (sd_gpu_env[0]) env[ne++] = sd_gpu_env;
        if (sd_input_debug && !greeter) env[ne++] = "LORD_INPUT_DEBUG=1";   /* key latency trace, never the login screen */
        for (int i = 0; i < SD_TUNE_N && !greeter; i++) if (sd_tune_env[i][0]) env[ne++] = sd_tune_env[i];
        env[ne] = NULL;
        if (!greeter) cgroup_delegate_session(uid, pw->pw_gid);   /* cgroup subtree for the apps (docs/SESSIONS.md) */
        /* real-time priority for the compositor's threads: lord-server takes it in par_init and lowers
         * the limit to 0 before it starts anything, so no app inherits it; a thread that runs 5 s
         * without blocking gets SIGXCPU and goes back to the normal policy (par.h) */
        struct rlimit rtp = { PAR_RT_PRIO, PAR_RT_PRIO }, rtt = { PAR_RT_TIME_US, RLIM_INFINITY };
        setrlimit(RLIMIT_RTPRIO, &rtp);
        setrlimit(RLIMIT_RTTIME, &rtt);
        const char *const a_user[] = { rd_conf.server, "--headless", size, "--user", user, "--theme", rd_conf.theme,
                                       "--fonts", rd_conf.fonts, "--shell", rd_conf.shell, NULL };
        const char *const a_greeter[] = { rd_conf.server, "--headless", size, "--greeter", "--user", user, "--theme", rd_conf.greeter_theme,
                                          "--fonts", rd_conf.fonts, "--shell", rd_conf.shell, NULL };
        execve(rd_conf.server, (char *const *)(greeter ? a_greeter : a_user), env);
        _exit(127);
    }
    if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static int wait_socket(const char *path, uid_t uid, int greeter) {
    for (int i = 0; i < 1000; i++) {              /* up to 20 s while the theme loads, every 20 ms */
        int fd = greeter ? connect_path(path) : sd_session_connect(uid);
        if (fd >= 0) return fd;
        usleep(20000);
    }
    return -1;
}

int sd_session_get(const struct passwd *pw, int w, int h) {
    mkdir(SD_RUN_DIR, 0755);
    char lockp[96];
    snprintf(lockp, sizeof lockp, SD_RUN_DIR "/spawn-%u.lock", (unsigned)pw->pw_uid);
    int lk = open(lockp, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lk >= 0) flock(lk, LOCK_EX);
    int fd = sd_session_connect(pw->pw_uid);
    if (fd < 0 && binary_ok()) {
        char rt[64];
        snprintf(rt, sizeof rt, "/run/user/%u", (unsigned)pw->pw_uid);
        struct stat st;
        char bus[80];
        snprintf(bus, sizeof bus, "%s/bus", rt);
        if (stat(bus, &st)) {
            /* no logind session, or one whose service manager is still starting (autologin at
             * boot): /run/user/<uid> comes first, from user-runtime-dir@, and the session bus
             * socket a moment later, from user@. A session started before the bus has no Secret
             * Service and no notification server for its whole life (lord-vault and notifyd
             * connect once), and the first app asking for org.freedesktop.secrets gets
             * gnome-keyring by D-Bus activation when it is installed. "systemctl start" returns
             * once the manager is up (its dbus.socket listening); then wait for the socket. */
            char unit[64];
            snprintf(unit, sizeof unit, "user@%u.service", (unsigned)pw->pw_uid);
            const char *const a[] = { "/usr/bin/systemctl", "start", unit, NULL };
            sd_run_wait(a);
            for (int i = 0; i < 50 && stat(bus, &st); i++) usleep(100000);
            if (stat(bus, &st)) sd_log("no session bus at %s: the session starts without it", bus);
        }
        if (stat(rt, &st) || st.st_uid != pw->pw_uid) sd_log("no %s", rt);
        else {
            char lang[64] = "LANG=en_US.UTF-8", p[600];
            read_lang("/etc/default/locale", lang, sizeof lang);
            snprintf(p, sizeof p, "%s/.config/locale.conf", pw->pw_dir);
            read_lang(p, lang, sizeof lang);
            sd_log("new session for %s (%dx%d)", pw->pw_name, w, h);
            spawn(pw, rt, lang, w, h, 0);
            fd = wait_socket(NULL, pw->pw_uid, 0);
            if (fd < 0) sd_log("%s's session did not start (see ~/.cache/lordos/session.log)", pw->pw_name);
        }
    }
    if (lk >= 0) close(lk);
    return fd;
}

int sd_greeter_get(int w, int h) {
    const char *sock = SD_GREETER_RUN "/lordos/remote.sock";
    int fd = connect_path(sock);
    if (fd >= 0 || !binary_ok()) return fd;
    struct passwd *pw = sd_system_user(SD_GREETER_USER, "/var/lib/" SD_GREETER_USER);
    if (!pw) { sd_log("no user %s", SD_GREETER_USER); return -1; }
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    mkdir(SD_RUN_DIR, 0755);
    mkdir(SD_GREETER_RUN, 0700);
    struct stat st;
    if (lstat(SD_GREETER_RUN, &st) || !S_ISDIR(st.st_mode) || chown(SD_GREETER_RUN, uid, gid) || chmod(SD_GREETER_RUN, 0700)) {
        sd_log("could not prepare %s", SD_GREETER_RUN);
        return -1;
    }
    char lang[64] = "LANG=en_US.UTF-8";
    read_lang("/etc/default/locale", lang, sizeof lang);
    spawn(pw, SD_GREETER_RUN, lang, w, h, 1);
    fd = wait_socket(sock, 0, 1);
    if (fd < 0) sd_log("the login screen did not start (see /var/lib/%s/.cache/lordos/greeter.log)", SD_GREETER_USER);
    return fd;
}

/* ---- messages between the main process and the monitor ---- */

int sd_ctl_send(int ctl, const char *text, int fd) {
    struct iovec iov = { (void *)text, strlen(text) };
    char ctlbuf[CMSG_SPACE(sizeof(int))];
    memset(ctlbuf, 0, sizeof ctlbuf);
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        mh.msg_control = ctlbuf;
        mh.msg_controllen = sizeof ctlbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    ssize_t r;
    while ((r = sendmsg(ctl, &mh, MSG_NOSIGNAL)) < 0 && errno == EINTR) {}
    return r == (ssize_t)iov.iov_len ? 0 : -1;
}

int sd_ctl_recv(int ctl, char *buf, size_t cap, int *fd) {
    *fd = -1;
    struct iovec iov = { buf, cap - 1 };
    char ctlbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctlbuf, .msg_controllen = sizeof ctlbuf };
    ssize_t r;
    while ((r = recvmsg(ctl, &mh, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR) {}
    if (r < 0) return -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        int n = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < n; i++) {
            int f;
            memcpy(&f, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
            if (*fd < 0) *fd = f; else close(f);
        }
    }
    if (mh.msg_flags & MSG_TRUNC) { if (*fd >= 0) close(*fd); *fd = -1; return -1; }
    buf[r] = 0;
    return (int)r;
}
