/* backup.c — the backup scheduler and worker thread (see backup.h).
 *
 *   ~/.config/axia/backup.json   the configuration: destination, schedule, exclusions, battery.
 *                                  Only backup_configure writes it (lord.fs cannot: it decides
 *                                  where the data goes; it is in fsops_protected).
 *
 * The thread sleeps in pthread_cond_timedwait until the next backup (no polling). While copying:
 *   - nice 19 and "idle" I/O priority (setpriority/ioprio_set on its own tid), and
 *     posix_fadvise DONTNEED on what was copied: it does not evict the desktop's page cache;
 *   - on battery (with "pause on battery"), it stops and checks again every 60 s;
 *   - a user pause waits on the same condition variable (no polling).
 * The time of the last good backup is the newest snapshot on the destination: there is no other
 * state file. The first scheduled backup waits 5 minutes after the session starts. */
#include "backup.h"
#include "../errcode.h"
#include "engine.h"
#include "crypt.h"
#include "../fsops.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define START_DELAY 300                 /* s after the session starts */
#define RETRY_FAIL 900                  /* s before retrying after an error */
#define RETRY_NODEST 300                /* s before checking again whether the disk is back */
#define BATTERY_RECHECK 60

static const char *const default_excl[] = { ".cache", ".local/share/Trash", ".local/share/axia/apps",
                                            ".local/share/axia/trash", ".thumbnails", NULL };

typedef enum { S_IDLE, S_RUNNING, S_PAUSED, S_NODEST, S_RESTORING, S_DELETING, S_LOCKED, S_UNLOCKING } State;
static const char *const state_names[] = { "idle", "running", "paused", "nodest", "restoring", "deleting", "locked", "unlocking" };

typedef struct {
    char dest[PATH_MAX];                /* chosen folder (real path) */
    int interval;                       /* seconds; 0 = manual */
    int battery_pause;
    int encrypt;                        /* new destination is encrypted (crypt.h) */
    int nexcl;
    char excl[BK_MAX_EXCL][256];
} Config;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static int started, efd = -1;
static char home[PATH_MAX], user[64], host[64], cfg_path[PATH_MAX];
static Config cfg;
static State state = S_IDLE;
static int want_run, user_pause, on_batt_pause;
static volatile int cancel_flag;
static time_t started_at, next_due, last_ok, last_try;
static char last_error[256];
static BkRun run;                       /* progress of the running backup */
static char cur_file[512];
static double last_emit;
/* result of the last backup */
static struct { uint64_t files, copied, linked, bytes, errors; double secs; int ok; time_t when; char id[BK_ID_LEN]; } last;
/* pending restore / delete */
static struct { int pending; char id[BK_ID_LEN], rel[PATH_MAX / 2], suffix[80], to[PATH_MAX]; int replace; } rjob;
static struct { int pending; char id[BK_ID_LEN]; } djob;
/* encryption: the key lives only in memory (mlock'd), for as long as the session lasts */
static uint8_t key[BKC_KEY];
static int have_key, dest_enc;
static struct { int pending, create; char pass[256]; } ujob;
static struct { int done, ok; char error[160]; } ures;
static struct { int done; int ok; uint64_t files, kept, replaced, skipped, errors; char error[256]; char id[BK_ID_LEN]; } rres;

/* snprintf for paths: if it does not fit, leaves the string empty and returns -1 */
__attribute__((format(printf, 3, 4)))
static int pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) { if (n) out[0] = 0; return -1; }
    return 0;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }

static void emit(void) { if (efd >= 0) { uint64_t one = 1; if (write(efd, &one, sizeof one) < 0) { /* full: a notification is already pending */ } } }

/* ---- configuration ---- */

static int machine_dir(const Config *c, char *out, size_t n) {
    if (!c->dest[0]) return -1;
    int r = snprintf(out, n, "%s/" BK_DIRNAME "/%s-%s", c->dest, host, user);
    return r < 0 || (size_t)r >= n ? -1 : 0;
}

static int valid_dest(const char *in, char *real, size_t n, char *err, size_t en) {
    char r[PATH_MAX];
    struct stat st;
    if (!in[0]) { real[0] = 0; return 0; }
    if (in[0] != '/' || !realpath(in, r) || stat(r, &st) || !S_ISDIR(st.st_mode)) { lerr_set(err, en, "backup.destMissing", NULL); return -1; }
    if (!fsops_allowed(r) || fsops_protected(r)) { lerr_set(err, en, "backup.destOutside", NULL); return -1; }
    if (access(r, W_OK)) { lerr_set(err, en, "backup.destUnwritable", NULL); return -1; }
    snprintf(real, n, "%s", r);
    return 0;
}

static int parse_config(const char *text, size_t len, Config *c, char *err, size_t en, int strict) {
    JDoc d;
    char jerr[96];
    memset(c, 0, sizeof *c);
    c->interval = 3600;
    c->battery_pause = 1;
    if (json_parse(text, len, &d, jerr, sizeof jerr)) { lerr_set(err, en, "backup.badConfig", NULL); return -1; }
    const JVal *o = d.root;
    int r = 0;
    if (!o || o->type != J_OBJ) { lerr_set(err, en, "backup.badConfig", NULL); r = -1; goto out; }
    const char *dest = json_str(o, "dest");
    if (dest && strict && (strlen(dest) >= PATH_MAX || valid_dest(dest, c->dest, sizeof c->dest, err, en))) { r = -1; goto out; }
    if (dest && !strict) {
        if (dest[0] != '/' || strlen(dest) >= sizeof c->dest || !bk_rel_valid(dest + 1)) { lerr_set(err, en, "backup.badDest", NULL); r = -1; goto out; }
        snprintf(c->dest, sizeof c->dest, "%s", dest);
    }
    const char *iv = json_str(o, "interval");
    if (iv) {
        if (!strcmp(iv, "hourly")) c->interval = 3600;
        else if (!strcmp(iv, "daily")) c->interval = 86400;
        else if (!strcmp(iv, "weekly")) c->interval = 7 * 86400;
        else if (!strcmp(iv, "manual")) c->interval = 0;
        else { lerr_set(err, en, "backup.badSchedule", NULL); r = -1; goto out; }
    }
    c->battery_pause = json_bool(o, "pauseOnBattery", 1);
    const JVal *ex = json_get(o, "exclude");
    if (ex && ex->type == J_ARR) {
        for (const JVal *e = ex->kid; e; e = e->next) {
            if (e->type != J_STR || !e->len || e->len > 255 || !bk_rel_valid(e->str)) { lerr_set(err, en, "backup.badExclusion", NULL); r = -1; goto out; }
            if (c->nexcl >= BK_MAX_EXCL - 8) { lerr_set(err, en, "backup.tooManyExclusions", NULL); r = -1; goto out; }
            snprintf(c->excl[c->nexcl++], sizeof c->excl[0], "%s", e->str);
        }
    }
    c->encrypt = json_bool(o, "encrypt", 0);
out:
    json_free(&d);
    return r;
}

static char *config_text(const Config *c) {
    size_t cap = 4096 + (size_t)c->nexcl * 600;
    char *s = malloc(cap), q[PATH_MAX * 2];
    if (!s) return NULL;
    json_quote(q, sizeof q, c->dest);
    const char *iv = c->interval == 0 ? "manual" : c->interval == 86400 ? "daily" : c->interval == 7 * 86400 ? "weekly" : "hourly";
    size_t o = (size_t)snprintf(s, cap, "{\n  \"dest\": %s,\n  \"interval\": \"%s\",\n  \"pauseOnBattery\": %s,\n  \"encrypt\": %s,\n  \"exclude\": [",
                                q, iv, c->battery_pause ? "true" : "false", c->encrypt ? "true" : "false");
    for (int i = 0; i < c->nexcl && o < cap; i++) {
        json_quote(q, sizeof q, c->excl[i]);
        o += (size_t)snprintf(s + o, cap - o, "%s%s", i ? ", " : "", q);
    }
    if (o < cap) snprintf(s + o, cap - o, "]\n}\n");
    return s;
}

static void load_config(void) {
    int fd = open(cfg_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    char err[256];
    memset(&cfg, 0, sizeof cfg);
    cfg.interval = 3600;
    cfg.battery_pause = 1;
    if (fd < 0) return;
    static char buf[65536];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    Config c;
    /* the destination need not be present (an unplugged disk): it is validated before each backup */
    if (!parse_config(buf, (size_t)n, &c, err, sizeof err, 0)) cfg = c;
    else fprintf(stderr, "backup: %s: %s\n", cfg_path, err);
}

static int save_config(const Config *c) {
    char *t = config_text(c), tmp[PATH_MAX + 8];
    if (!t) return -1;
    snprintf(tmp, sizeof tmp, "%s.new", cfg_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    size_t n = strlen(t);
    int ok = fd >= 0 && write(fd, t, n) == (ssize_t)n;
    if (fd >= 0) ok = close(fd) == 0 && ok;
    free(t);
    if (!ok || rename(tmp, cfg_path)) { unlink(tmp); return -1; }
    return 0;
}

/* ---- battery ---- */

static int read_small(const char *p, char *out, size_t n) {
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* On battery = some battery discharging and no power source connected. */
static int on_battery(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    int discharging = 0, mains = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[320], v[64];
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if (read_small(p, v, sizeof v)) continue;
        if (!strcmp(v, "Battery")) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
            if (!read_small(p, v, sizeof v) && !strcmp(v, "Discharging")) discharging = 1;
        } else if (!strcmp(v, "Mains") || !strcmp(v, "USB") || !strcmp(v, "USB_C")) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
            if (!read_small(p, v, sizeof v) && !strcmp(v, "1")) mains = 1;
        }
    }
    closedir(d);
    return discharging && !mains;
}

/* ---- the thread ---- */

static void wait_until(time_t when) {
    struct timespec ts = { .tv_sec = when, .tv_nsec = 0 };
    pthread_cond_timedwait(&cv, &mu, &ts);
}

static double last_batt_check;

/* Between files: progress, user pause and battery. Called with the mutex released. */
static int checkpoint(BkRun *r) {
    (void)r;
    double t = now_s();
    pthread_mutex_lock(&mu);
    int batt = 0;
    if (cfg.battery_pause && t - last_batt_check > 2) { last_batt_check = t; batt = on_battery(); }
    while (!cancel_flag && (user_pause || batt)) {
        on_batt_pause = batt;
        state = S_PAUSED;
        emit();
        if (batt) {
            wait_until(time(NULL) + BATTERY_RECHECK);
            batt = cfg.battery_pause && on_battery();
        } else pthread_cond_wait(&cv, &mu);
    }
    if (state == S_PAUSED) { state = S_RUNNING; on_batt_pause = 0; emit(); }
    int c = cancel_flag;
    if (t - last_emit > 0.5) { last_emit = t; emit(); }
    pthread_mutex_unlock(&mu);
    return c;
}

static void progress(BkRun *r, const char *rel) {
    (void)r;
    pthread_mutex_lock(&mu);
    snprintf(cur_file, sizeof cur_file, "%s", rel);
    pthread_mutex_unlock(&mu);
}

static int latest_id(const char *machine, char out[BK_ID_LEN]) {
    static char ids[BK_MAX_SNAPS][BK_ID_LEN];
    int n = bk_list(machine, ids, BK_MAX_SNAPS);
    if (!n) return -1;
    memcpy(out, ids[n - 1], BK_ID_LEN);
    return 0;
}

static void do_snapshot(void) {
    /* with the mutex held */
    Config c = cfg;
    char machine[PATH_MAX], backups_dir[PATH_MAX];
    struct stat st;
    char dreal[PATH_MAX];
    if (machine_dir(&c, machine, sizeof machine) || stat(c.dest, &st) || !S_ISDIR(st.st_mode) || access(c.dest, W_OK) ||
        !realpath(c.dest, dreal) || !fsops_allowed(dreal) || fsops_protected(dreal)) {
        state = S_NODEST;
        emit();
        return;
    }
    if (pathf(backups_dir, sizeof backups_dir, "%s/" BK_DIRNAME, c.dest)) { state = S_NODEST; emit(); return; }
    memset(&run, 0, sizeof run);
    run.home = home;
    run.machine = machine;
    for (int i = 0; default_excl[i]; i++) run.excl[run.nexcl++] = default_excl[i];
    for (int i = 0; i < c.nexcl && run.nexcl < BK_MAX_EXCL; i++) run.excl[run.nexcl++] = c.excl[i];
    run.cancel = &cancel_flag;
    run.key = dest_enc && have_key ? key : NULL;
    run.checkpoint = checkpoint;
    run.progress = progress;
    state = S_RUNNING;
    cancel_flag = 0;
    last_try = time(NULL);
    cur_file[0] = 0;
    emit();
    pthread_mutex_unlock(&mu);

    mkdir(backups_dir, 0700);
    char skip[PATH_MAX];
    run.skip_real = realpath(backups_dir, skip) ? skip : NULL;
    double t0 = now_s();
    int r = bk_snapshot(&run, time(NULL));
    double secs = now_s() - t0;

    pthread_mutex_lock(&mu);
    state = S_IDLE;
    cur_file[0] = 0;
    if (r == 0) {
        last.ok = 1;
        last_error[0] = 0;
        last_ok = time(NULL);
    } else if (r < 0) {
        last.ok = 0;
        snprintf(last_error, sizeof last_error, "%s", run.error[0] ? run.error : "sys.unknown");
    } else {
        last.ok = -1;                   /* interrupted: resumed next time */
        snprintf(last_error, sizeof last_error, "backup.interrupted");
    }
    last.files = run.files;
    last.copied = run.copied;
    last.linked = run.linked;
    last.bytes = run.copied_bytes;
    last.errors = run.errors;
    last.secs = secs;
    last.when = time(NULL);
    snprintf(last.id, sizeof last.id, "%s", r == 0 ? run.id : "");
    fprintf(stderr, "backup: %s in %.1f s: %llu files, %llu copied (%llu MB), %llu linked, %llu errors%s%s\n",
            r == 0 ? "done" : r > 0 ? "interrupted" : "failed", secs, (unsigned long long)run.files, (unsigned long long)run.copied,
            (unsigned long long)(run.copied_bytes >> 20), (unsigned long long)run.linked, (unsigned long long)run.errors,
            r < 0 ? ": " : "", r < 0 ? last_error : "");
    emit();
}

static void do_restore(void) {
    Config c = cfg;
    char machine[PATH_MAX];
    rjob.pending = 0;
    char id[BK_ID_LEN], rel[PATH_MAX / 2], suffix[80], to[PATH_MAX];
    snprintf(id, sizeof id, "%s", rjob.id);
    snprintf(rel, sizeof rel, "%s", rjob.rel);
    snprintf(suffix, sizeof suffix, "%s", rjob.suffix);
    snprintf(to, sizeof to, "%s", rjob.to[0] ? rjob.to : home);
    int replace = rjob.replace;
    memset(&rres, 0, sizeof rres);
    snprintf(rres.id, sizeof rres.id, "%s", id);
    if (machine_dir(&c, machine, sizeof machine)) { rres.done = 1; snprintf(rres.error, sizeof rres.error, "backup.noDestination"); emit(); return; }
    state = S_RESTORING;
    cancel_flag = 0;
    emit();
    pthread_mutex_unlock(&mu);
    if (dest_enc && !have_key) { state = S_IDLE; rres.done = 1; snprintf(rres.error, sizeof rres.error, "backup.locked"); emit(); return; }
    BkRestore o = { .cancel = &cancel_flag, .key = dest_enc ? key : NULL };
    int r = bk_restore(machine, id, rel, to, replace, suffix, &o);
    pthread_mutex_lock(&mu);
    state = S_IDLE;
    rres.done = 1;
    rres.ok = r == 0 && !o.errors;
    rres.files = o.files;
    rres.kept = o.kept_both;
    rres.replaced = o.replaced;
    rres.skipped = o.skipped;
    rres.errors = o.errors;
    snprintf(rres.error, sizeof rres.error, "%s", r > 0 ? "backup.interrupted" : o.error);
    fprintf(stderr, "backup: restored %s/%s: %llu files, %llu kept alongside, %llu replaced, %llu errors\n", id, rel,
            (unsigned long long)o.files, (unsigned long long)o.kept_both, (unsigned long long)o.replaced, (unsigned long long)o.errors);
    emit();
}

static void do_delete(void) {
    Config c = cfg;
    char machine[PATH_MAX], id[BK_ID_LEN];
    djob.pending = 0;
    snprintf(id, sizeof id, "%s", djob.id);
    if (machine_dir(&c, machine, sizeof machine)) return;
    state = S_DELETING;
    emit();
    pthread_mutex_unlock(&mu);
    bk_delete(machine, id);
    pthread_mutex_lock(&mu);
    state = S_IDLE;
    emit();
}

/* Unlock (or set up) encryption: Argon2 is slow, so it runs in this thread, never in the compositor. */
static void do_unlock(void) {
    char machine[PATH_MAX], pass[256];
    int create = ujob.create;
    snprintf(pass, sizeof pass, "%s", ujob.pass);
    bkc_wipe(ujob.pass, sizeof ujob.pass);
    ujob.pending = 0;
    memset(&ures, 0, sizeof ures);
    if (machine_dir(&cfg, machine, sizeof machine)) { bkc_wipe(pass, sizeof pass); ures.done = 1; snprintf(ures.error, sizeof ures.error, "backup.noDestination"); emit(); return; }
    State before = state;
    state = S_UNLOCKING;
    emit();
    pthread_mutex_unlock(&mu);
    uint8_t salt[BKC_SALT], chk[16], k[BKC_KEY];
    int enc = !bkc_info_read(machine, salt, chk), r;
    static char ids[4][BK_ID_LEN];
    if (enc) r = bkc_unlock(machine, pass, k);
    else if (!create) r = -2;
    else if (bk_list(machine, ids, 4) > 0) r = -3;           /* unencrypted backups already exist here */
    else {
        char m2[PATH_MAX + 4];
        snprintf(m2, sizeof m2, "%s", machine);
        for (char *p = m2 + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(m2, 0700); *p = '/'; }
        mkdir(m2, 0700);
        r = bkc_setup(machine, pass, k);
    }
    bkc_wipe(pass, sizeof pass);
    pthread_mutex_lock(&mu);
    ures.done = 1;
    ures.ok = r == 0;
    if (r == 0) { memcpy(key, k, sizeof key); have_key = 1; dest_enc = 1; }
    else snprintf(ures.error, sizeof ures.error, "%s", r == -1 ? "backup.wrongPassphrase" : r == -2 ? "backup.notEncrypted" :
                  r == -3 ? "backup.plainBackupsExist" : "backup.setupFailed");   /* code lines: the UI translates them */
    bkc_wipe(k, sizeof k);
    state = before == S_UNLOCKING ? S_IDLE : before;
    if (state == S_LOCKED) state = S_IDLE;
    emit();
}

static void *thread_main(void *arg) {
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    pid_t tid = (pid_t)syscall(SYS_gettid);
    const char *np = getenv("LORD_BACKUP_NOPRIO");
    if (!np || strcmp(np, "1")) {
        setpriority(PRIO_PROCESS, (id_t)tid, 19);
        /* IOPRIO_WHO_PROCESS = 1, IOPRIO_CLASS_IDLE = 3 (<< 13) */
        syscall(SYS_ioprio_set, 1, (int)tid, 3 << 13);
    }
    pthread_mutex_lock(&mu);
    for (;;) {
        if (rjob.pending) { do_restore(); continue; }
        if (djob.pending) { do_delete(); continue; }
        if (ujob.pending) { do_unlock(); continue; }
        time_t now = time(NULL);
        char machine[PATH_MAX], lid[BK_ID_LEN];
        if (!cfg.dest[0]) { next_due = 0; state = S_IDLE; want_run = 0; pthread_cond_wait(&cv, &mu); continue; }
        {   /* is the destination encrypted? (encryption.json in the machine folder) */
            uint8_t salt[BKC_SALT], chk[16];
            dest_enc = !machine_dir(&cfg, machine, sizeof machine) && !bkc_info_read(machine, salt, chk);
        }
        if (!last_ok && !machine_dir(&cfg, machine, sizeof machine) && !latest_id(machine, lid)) last_ok = bk_id_time(lid);
        time_t due = 0;
        if (want_run) due = now;
        else if (cfg.interval > 0) {
            due = last_ok ? last_ok + cfg.interval : now;
            if (last.ok == 0 && last_try && due < last_try + RETRY_FAIL) due = last_try + RETRY_FAIL;
            if (state == S_NODEST && due < last_try + RETRY_NODEST) due = last_try + RETRY_NODEST;
            if (due < started_at + START_DELAY) due = started_at + START_DELAY;
        }
        next_due = due;
        if (due && due <= now && (dest_enc || cfg.encrypt) && !have_key) {
            /* encrypted and no passphrase: wait for the user to enter it (never copy unencrypted) */
            state = S_LOCKED;
            emit();
            pthread_cond_wait(&cv, &mu);
            continue;
        }
        if (due && due <= now) {
            want_run = 0;
            last_try = now;
            if (cfg.battery_pause && on_battery() && !user_pause) {
                /* on battery: do not start; check again in a minute */
                on_batt_pause = 1;
                state = S_PAUSED;
                emit();
                want_run = 1;
                wait_until(now + BATTERY_RECHECK);
                continue;
            }
            on_batt_pause = 0;
            do_snapshot();
            continue;
        }
        if (state != S_NODEST) state = S_IDLE;
        emit();
        if (due) wait_until(due); else pthread_cond_wait(&cv, &mu);
    }
    return NULL;
}

int backup_start(const char *h, const char *u) {
    if (started || !h || !h[0] || !u) return -1;
    if (!realpath(h, home)) return -1;
    snprintf(user, sizeof user, "%s", u);
    for (char *p = user; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) *p = '_';
    char hn[64] = "lordos";
    gethostname(hn, sizeof hn - 1);
    hn[sizeof hn - 1] = 0;
    size_t k = 0;
    for (char *p = hn; *p && k < sizeof host - 1; p++)
        host[k++] = ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-') ? *p : '_';
    host[k] = 0;
    if (pathf(cfg_path, sizeof cfg_path, "%s/.config/axia/backup.json", home)) return -1;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_cond_init(&cv, &ca);        /* CLOCK_REALTIME: backups follow wall-clock time */
    efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    mlock(key, sizeof key);              /* the key never goes to swap */
    load_config();
    started_at = time(NULL);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 << 10);
    if (pthread_create(&t, &a, thread_main, NULL)) { close(efd); efd = -1; return -1; }
    started = 1;
    return 0;
}

int backup_fd(void) { return efd; }

void backup_dispatch(void (*fn)(const char *json)) {
    uint64_t v;
    if (efd < 0 || read(efd, &v, sizeof v) != sizeof v) return;
    char *j = backup_status_json();
    if (j) { fn(j); free(j); }
}

/* ---- API (main thread) ---- */

char *backup_status_json(void) {
    if (!started) return NULL;
    pthread_mutex_lock(&mu);
    static char qf[1100], qe[600], qd[PATH_MAX * 2], qr[600];
    json_quote(qf, sizeof qf, cur_file);
    json_quote(qe, sizeof qe, last_error);
    json_quote(qd, sizeof qd, cfg.dest);
    json_quote(qr, sizeof qr, rres.error);
    size_t cap = 2048 + strlen(qf) + strlen(qe) + strlen(qd) + strlen(qr);
    char *s = malloc(cap);
    if (!s) { pthread_mutex_unlock(&mu); return NULL; }
    struct stat st;
    int avail = cfg.dest[0] && !stat(cfg.dest, &st) && S_ISDIR(st.st_mode);
    double freeb = -1, totalb = -1;
    struct statvfs v;
    if (avail && !statvfs(cfg.dest, &v)) { freeb = (double)v.f_bavail * v.f_frsize; totalb = (double)v.f_blocks * v.f_frsize; }
    snprintf(s, cap,
             "{\"state\":\"%s\",\"battery\":%s,\"userPause\":%s,\"dest\":%s,\"destAvailable\":%s,\"destFree\":%.0f,\"destTotal\":%.0f,"
             "\"next\":%lld,\"lastOk\":%lld,"
             "\"progress\":{\"files\":%llu,\"totalFiles\":%llu,\"bytes\":%llu,\"totalBytes\":%llu,\"copied\":%llu,\"linked\":%llu,\"file\":%s},"
             "\"last\":{\"when\":%lld,\"ok\":%d,\"id\":\"%s\",\"files\":%llu,\"copied\":%llu,\"linked\":%llu,\"bytes\":%llu,\"errors\":%llu,\"secs\":%.1f,\"error\":%s},"
             "\"restore\":{\"done\":%s,\"ok\":%s,\"id\":\"%s\",\"files\":%llu,\"keptBoth\":%llu,\"replaced\":%llu,\"skipped\":%llu,\"errors\":%llu,\"error\":%s},"
             "\"encryption\":true,\"destEncrypted\":%s,\"encrypted\":%s,\"locked\":%s,\"unlock\":{\"done\":%s,\"ok\":%s,\"error\":\"%s\"}}",
             state_names[state], on_batt_pause ? "true" : "false", user_pause ? "true" : "false", qd, avail ? "true" : "false", freeb, totalb,
             (long long)next_due, (long long)last_ok,
             (unsigned long long)run.files, (unsigned long long)run.total_files, (unsigned long long)run.bytes,
             (unsigned long long)run.total_bytes, (unsigned long long)run.copied, (unsigned long long)run.linked, qf,
             (long long)last.when, last.ok, last.id, (unsigned long long)last.files, (unsigned long long)last.copied,
             (unsigned long long)last.linked, (unsigned long long)last.bytes, (unsigned long long)last.errors, last.secs, qe,
             rres.done ? "true" : "false", rres.ok ? "true" : "false", rres.id, (unsigned long long)rres.files, (unsigned long long)rres.kept,
             (unsigned long long)rres.replaced, (unsigned long long)rres.skipped, (unsigned long long)rres.errors, qr,
             dest_enc ? "true" : "false", dest_enc || cfg.encrypt ? "true" : "false", (dest_enc || cfg.encrypt) && !have_key ? "true" : "false",
             ures.done ? "true" : "false", ures.ok ? "true" : "false", ures.error);
    pthread_mutex_unlock(&mu);
    return s;
}

char *backup_config_json(void) {
    if (!started) return NULL;
    pthread_mutex_lock(&mu);
    char *t = config_text(&cfg);
    pthread_mutex_unlock(&mu);
    return t;
}

int backup_configure(const char *json, char *err, size_t n) {
    if (!started) { lerr_set(err, n, "backup.notRunning", NULL); return -1; }
    if (strlen(json) > 60000) { lerr_set(err, n, "backup.configTooLarge", NULL); return -1; }
    Config c;
    if (parse_config(json, strlen(json), &c, err, n, 1)) return -1;
    if (save_config(&c)) { lerr_set(err, n, "backup.saveFailed", NULL); return -1; }
    pthread_mutex_lock(&mu);
    int changed_dest = strcmp(c.dest, cfg.dest) != 0;
    cfg = c;
    if (changed_dest) {
        last_ok = 0; last.ok = 1; last_try = 0;
        if (state == S_NODEST || state == S_LOCKED) state = S_IDLE;
        bkc_wipe(key, sizeof key);           /* the key belonged to another destination */
        have_key = 0;
        dest_enc = 0;
    }
    pthread_cond_broadcast(&cv);
    emit();
    pthread_mutex_unlock(&mu);
    return 0;
}

int backup_run_now(char *err, size_t n) {
    if (!started) { lerr_set(err, n, "backup.notRunning", NULL); return -1; }
    pthread_mutex_lock(&mu);
    int r = 0;
    if (!cfg.dest[0]) { lerr_set(err, n, "backup.noDestination", NULL); r = -1; }
    else if (state == S_RUNNING || state == S_RESTORING) { lerr_set(err, n, "backup.alreadyRunning", NULL); r = -1; }
    else { want_run = 1; user_pause = 0; if (state == S_NODEST) state = S_IDLE; last_try = 0; pthread_cond_broadcast(&cv); }
    pthread_mutex_unlock(&mu);
    return r;
}

void backup_pause(int on) {
    if (!started) return;
    pthread_mutex_lock(&mu);
    user_pause = on != 0;
    pthread_cond_broadcast(&cv);
    emit();
    pthread_mutex_unlock(&mu);
}

void backup_cancel(void) {
    if (!started) return;
    pthread_mutex_lock(&mu);
    cancel_flag = 1;
    user_pause = 0;
    want_run = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static int machine_now(char *out, size_t n) {
    pthread_mutex_lock(&mu);
    int r = machine_dir(&cfg, out, n);
    pthread_mutex_unlock(&mu);
    return r;
}

char *backup_list_json(void) {
    char machine[PATH_MAX];
    static char ids[BK_MAX_SNAPS][BK_ID_LEN];
    if (!started) return NULL;
    int n = machine_now(machine, sizeof machine) ? 0 : bk_list(machine, ids, BK_MAX_SNAPS);
    size_t cap = 64 + (size_t)n * 64;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t o = (size_t)snprintf(s, cap, "[");
    for (int i = 0; i < n && o < cap; i++)
        o += (size_t)snprintf(s + o, cap - o, "%s{\"id\":\"%s\",\"time\":%lld}", i ? "," : "", ids[i], (long long)bk_id_time(ids[i]));
    if (o < cap) snprintf(s + o, cap - o, "]");
    return s;
}

int backup_snapshot_path(const char *id, char *out, size_t n) {
    char machine[PATH_MAX], p[PATH_MAX];
    if (!started || !bk_id_valid(id) || machine_now(machine, sizeof machine)) return -1;
    if (pathf(p, sizeof p, "%s/%s", machine, id)) return -1;
    char r[PATH_MAX];
    if (!realpath(p, r) || strlen(r) >= n) return -1;
    snprintf(out, n, "%s", r);
    return 0;
}

typedef struct { char *name; int dir, link; double size, mtime; } Ent;
static int ent_cmp(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

char *backup_ls_json(const char *id, const char *rel, char *err, size_t en) {
    char snap[PATH_MAX], dir[PATH_MAX], real[PATH_MAX];
    if (!bk_rel_valid(rel)) { lerr_set(err, en, "backup.badPath", NULL); return NULL; }
    if (backup_snapshot_path(id, snap, sizeof snap)) { lerr_set(err, en, "backup.noSuchBackup", NULL); return NULL; }
    if (pathf(dir, sizeof dir, "%s%s%s", snap, rel[0] ? "/" : "", rel)) { lerr_set(err, en, "backup.badPath", NULL); return NULL; }
    size_t sl = strlen(snap);
    if (!realpath(dir, real) || strncmp(real, snap, sl) || (real[sl] && real[sl] != '/')) { lerr_set(err, en, "backup.badPath", NULL); return NULL; }
    DIR *d = opendir(real);
    if (!d) { lerr_set(err, en, "backup.readFailed", NULL); return NULL; }
    pthread_mutex_lock(&mu);
    int enc = dest_enc;
    pthread_mutex_unlock(&mu);
    Ent *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 20000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        struct stat st;
        if ((size_t)snprintf(p, sizeof p, "%s/%s", real, e->d_name) >= sizeof p || lstat(p, &st)) continue;
        if (n == cap) { size_t nc = cap ? cap * 2 : 128; Ent *nv = realloc(v, nc * sizeof *nv); if (!nv) break; v = nv; cap = nc; }
        uint64_t size = (uint64_t)st.st_size;
        if (enc && S_ISREG(st.st_mode) && bkc_plain_size((uint64_t)st.st_size, &size)) size = 0;
        v[n] = (Ent){ strdup(e->d_name), S_ISDIR(st.st_mode), S_ISLNK(st.st_mode), (double)size, (double)st.st_mtim.tv_sec };
        if (v[n].name) n++;
    }
    closedir(d);
    if (n) qsort(v, n, sizeof *v, ent_cmp);
    size_t scap = 64 + n * 160;
    for (size_t i = 0; i < n; i++) scap += strlen(v[i].name) * 6;
    char *s = malloc(scap);
    size_t o = s ? (size_t)snprintf(s, scap, "[") : 0;
    for (size_t i = 0; i < n; i++) {
        if (s && o < scap) {
            char *q = malloc(strlen(v[i].name) * 6 + 3);
            if (q) {
                json_quote(q, strlen(v[i].name) * 6 + 3, v[i].name);
                o += (size_t)snprintf(s + o, scap - o, "%s{\"name\":%s,\"dir\":%s,\"link\":%s,\"size\":%.0f,\"mtime\":%.0f}", i ? "," : "", q,
                                      v[i].dir ? "true" : "false", v[i].link ? "true" : "false", v[i].size, v[i].mtime);
                free(q);
            }
        }
        free(v[i].name);
    }
    free(v);
    if (s && o < scap) snprintf(s + o, scap - o, "]");
    return s;
}

int backup_restore(const char *id, const char *rel, int replace, const char *suffix, const char *to, char *err, size_t n) {
    if (!started) { lerr_set(err, n, "backup.notRunning", NULL); return -1; }
    if (!bk_id_valid(id) || !bk_rel_valid(rel) || strlen(rel) >= sizeof rjob.rel) { lerr_set(err, n, "backup.badPath", NULL); return -1; }
    size_t sl = strlen(suffix);
    if (!sl || sl >= sizeof rjob.suffix || strchr(suffix, '/')) { lerr_set(err, n, "backup.badSuffix", NULL); return -1; }
    char tr[PATH_MAX] = "";
    if (to && to[0]) {
        if (!realpath(to, tr) || !fsops_allowed(tr) || fsops_protected(tr)) { lerr_set(err, n, "backup.destNotAllowed", NULL); return -1; }
    }
    pthread_mutex_lock(&mu);
    int r = 0;
    if (state == S_RUNNING || state == S_RESTORING || state == S_DELETING || rjob.pending) { lerr_set(err, n, "backup.busy", NULL); r = -1; }
    else {
        snprintf(rjob.id, sizeof rjob.id, "%s", id);
        snprintf(rjob.rel, sizeof rjob.rel, "%s", rel);
        snprintf(rjob.suffix, sizeof rjob.suffix, "%s", suffix);
        snprintf(rjob.to, sizeof rjob.to, "%s", tr);
        rjob.replace = replace != 0;
        rjob.pending = 1;
        rres.done = 0;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int backup_delete(const char *id, char *err, size_t n) {
    if (!started) { lerr_set(err, n, "backup.notRunning", NULL); return -1; }
    if (!bk_id_valid(id)) { lerr_set(err, n, "backup.badId", NULL); return -1; }
    pthread_mutex_lock(&mu);
    int r = 0;
    if (state != S_IDLE && state != S_NODEST) { lerr_set(err, n, "backup.busy", NULL); r = -1; }
    else { snprintf(djob.id, sizeof djob.id, "%s", id); djob.pending = 1; pthread_cond_broadcast(&cv); }
    pthread_mutex_unlock(&mu);
    return r;
}

int backup_unlock(const char *pass, int create, char *err, size_t n) {
    if (!started) { lerr_set(err, n, "backup.notRunning", NULL); return -1; }
    size_t len = strlen(pass);
    if (len < (create ? 8u : 1u) || len >= sizeof ujob.pass) { lerr_set(err, n, create ? "backup.passphraseShort" : "backup.badPassphrase", NULL); return -1; }
    pthread_mutex_lock(&mu);
    int r = 0;
    if (!cfg.dest[0]) { lerr_set(err, n, "backup.noDestination", NULL); r = -1; }
    else if (state == S_RUNNING || state == S_RESTORING || state == S_UNLOCKING || ujob.pending) { lerr_set(err, n, "backup.busy", NULL); r = -1; }
    else {
        snprintf(ujob.pass, sizeof ujob.pass, "%s", pass);
        ujob.create = create != 0;
        ujob.pending = 1;
        ures.done = 0;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return r;
}

void backup_lock(void) {
    if (!started) return;
    pthread_mutex_lock(&mu);
    bkc_wipe(key, sizeof key);
    have_key = 0;
    emit();
    pthread_mutex_unlock(&mu);
}
