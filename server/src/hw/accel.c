/* accel.c — the screen's accelerometer for automatic rotation (see accel.h). */
#include "accel.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define G 9.80665

/* ---- pure parts ---- */

int accel_parse_matrix(const char *s, double m[9]) {
    if (!s) return -1;
    for (int i = 0; i < 9; i++) {
        char *end;
        while (*s == ' ' || *s == '\t') s++;
        m[i] = strtod(s, &end);
        if (end == s || !isfinite(m[i])) return -1;
        s = end;
        while (*s == ' ' || *s == '\t') s++;
        char want = i == 8 ? 0 : i % 3 == 2 ? ';' : ',';
        if (want && *s != want) return -1;
        if (want) s++;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s) return -1;
    for (int i = 0; i < 9; i++) if (m[i] != 0) return 0;
    return -1;                                  /* all zeros: not a matrix anyone meant */
}

/* how far the edge that rotation r puts on top is tilted up (degrees) */
static double tilt(int r, double px, double py) { return r == 0 ? -py : r == 1 ? -px : r == 2 ? py : px; }

int accel_orientation(int prev, double x, double y, double z) {
    if (!(x * x + y * y + z * z > 1e-9)) return prev;
    const double deg = 180.0 / M_PI;
    double px = atan2(x, hypot(y, z)) * deg;    /* > 0: gravity pulls to the right edge */
    double py = atan2(y, hypot(x, z)) * deg;    /* < 0: gravity pulls to the bottom edge (upright) */
    if (prev >= 0 && prev <= 3 && tilt(prev, px, py) > ACCEL_KEEP_DEG) return prev;
    int ax = fabs(px) > ACCEL_ENTER_DEG, ay = fabs(py) > ACCEL_ENTER_DEG;
    if (ax && ay) return prev;                  /* on a corner: not sure */
    /* gravity to the right: the left edge is up, the picture turns 270° clockwise (and so on) */
    if (ax) return px > 0 ? 3 : 1;
    if (ay) return py > 0 ? 2 : 0;
    return prev;                                /* flat on a table, or in between */
}

void accel_filter_init(AccelFilter *f) { f->cur = f->cand = -1; f->since = 0; }

int accel_filter_feed(AccelFilter *f, double now, double x, double y, double z) {
    double g = sqrt(x * x + y * y + z * z);
    if (g < 0.5 * G || g > 1.5 * G) { f->cand = -1; return 0; }     /* moving: not gravity alone */
    int o = accel_orientation(f->cur, x, y, z);
    if (o == f->cur) { f->cand = -1; return 0; }
    if (f->cur < 0) { f->cur = o; f->cand = -1; return 1; }
    if (o != f->cand) { f->cand = o; f->since = now; return 0; }
    if (now - f->since < ACCEL_SETTLE_MS) return 0;
    f->cur = o;
    f->cand = -1;
    return 1;
}

static int read_text(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

int accel_chassis_tablet(const char *path) {
    char b[16];
    if (read_text(path ? path : "/sys/class/dmi/id/chassis_type", b, sizeof b)) return 0;
    int t = atoi(b);
    return t == 30 || t == 31 || t == 32;
}

/* ---- sysfs ---- */

static int read_attr(const char *dir, const char *name, char *buf, size_t n) {
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return read_text(p, buf, n);
}

/* udev's property for the device (E:KEY=value in its database entry), or -1 */
static int udev_prop(const char *udev_root, const char *dir, const char *key, char *out, size_t n) {
    char dev[32], p[320], line[256];
    unsigned maj, min;
    if (read_attr(dir, "dev", dev, sizeof dev) || sscanf(dev, "%u:%u", &maj, &min) != 2) return -1;
    snprintf(p, sizeof p, "%s/c%u:%u", udev_root, maj, min);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    size_t kl = strlen(key);
    int ret = -1;
    while (ret && fgets(line, sizeof line, f)) {
        if (strncmp(line, "E:", 2) || strncmp(line + 2, key, kl) || line[2 + kl] != '=') continue;
        line[strcspn(line, "\n")] = 0;
        snprintf(out, n, "%s", line + 3 + kl);
        ret = 0;
    }
    fclose(f);
    return ret;
}

static void close_fds(AccelDev *a) {
    for (int i = 0; i < 3; i++) { if (a->fd[i] >= 0) close(a->fd[i]); a->fd[i] = -1; }
}

static int try_dev(const char *dir, const char *udev_root, AccelDev *a) {
    char b[160];
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 3; i++) a->fd[i] = -1;
    if (!read_attr(dir, "label", b, sizeof b) && !strcmp(b, "accel-base")) return -1;
    if (!udev_prop(udev_root, dir, "ACCEL_LOCATION", b, sizeof b) && !strcmp(b, "base")) return -1;
    for (int i = 0; i < 3; i++) {
        char p[320];
        snprintf(p, sizeof p, "%s/in_accel_%c_raw", dir, "xyz"[i]);
        a->fd[i] = open(p, O_RDONLY | O_CLOEXEC);
        if (a->fd[i] < 0) { close_fds(a); return -1; }
    }
    snprintf(a->dir, sizeof a->dir, "%s", dir);
    if (read_attr(dir, "name", a->name, sizeof a->name)) snprintf(a->name, sizeof a->name, "iio");
    a->scale = 0;                               /* unknown: readings are normalized to 1 g */
    if (!read_attr(dir, "in_accel_scale", b, sizeof b) || !read_attr(dir, "in_accel_x_scale", b, sizeof b)) {
        double s = atof(b);
        if (s > 0 && isfinite(s)) a->scale = s;
    }
    if (!read_attr(dir, "in_accel_offset", b, sizeof b) || !read_attr(dir, "in_accel_x_offset", b, sizeof b)) {
        double o = atof(b);
        if (isfinite(o)) a->offset = o;
    }
    static const double id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    memcpy(a->m, id, sizeof id);
    if (!udev_prop(udev_root, dir, "ACCEL_MOUNT_MATRIX", b, sizeof b) && !accel_parse_matrix(b, a->m)) a->matrix_from = 1;
    else {
        const char *names[3] = { "in_mount_matrix", "in_accel_mount_matrix", "mount_matrix" };
        for (int i = 0; i < 3 && !a->matrix_from; i++)
            if (!read_attr(dir, names[i], b, sizeof b) && !accel_parse_matrix(b, a->m)) a->matrix_from = 2;
        if (!a->matrix_from) memcpy(a->m, id, sizeof id);
    }
    return 0;
}

int accel_find(const char *iio_root, const char *udev_root, AccelDev *a) {
    if (!iio_root) iio_root = "/sys/bus/iio/devices";
    if (!udev_root) udev_root = "/run/udev/data";
    DIR *d = opendir(iio_root);
    if (!d) return -1;
    /* the lowest numbered device first, so the choice does not depend on readdir's order */
    int best = -1;
    for (struct dirent *e; (e = readdir(d));) {
        int n;
        char tail;
        if (sscanf(e->d_name, "iio:device%d%c", &n, &tail) != 1 || n < 0) continue;
        char dir[256];
        int l = snprintf(dir, sizeof dir, "%s/%s", iio_root, e->d_name);
        if (l < 0 || l >= (int)sizeof dir) continue;
        AccelDev t;
        if ((best >= 0 && n >= best) || try_dev(dir, udev_root, &t)) continue;
        if (best >= 0) close_fds(a);
        *a = t;
        best = n;
    }
    closedir(d);
    return best >= 0 ? 0 : -1;
}

int accel_read(AccelDev *a, double v[3]) {
    double raw[3];
    for (int i = 0; i < 3; i++) {
        char b[32];
        ssize_t r = a->fd[i] >= 0 ? pread(a->fd[i], b, sizeof b - 1, 0) : -1;
        if (r <= 0) return -1;
        b[r] = 0;
        raw[i] = (atof(b) + a->offset) * (a->scale > 0 ? a->scale : 1);
    }
    for (int i = 0; i < 3; i++) v[i] = a->m[3 * i] * raw[0] + a->m[3 * i + 1] * raw[1] + a->m[3 * i + 2] * raw[2];
    if (a->scale <= 0) {                        /* no scale: only the direction is known */
        double g = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (g <= 0) return -1;
        for (int i = 0; i < 3; i++) v[i] *= G / g;
    }
    return 0;
}

void accel_close(AccelDev *a) { close_fds(a); }

/* ---- the runtime: a reader thread ---- */

static struct {
    int looked, found, started, want, fresh;
    AccelDev dev;
    double v[3];
    pthread_mutex_t mu;
    pthread_cond_t cv;
} R = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

int accel_present(void) {
    if (!R.looked) {
        R.looked = 1;
        R.found = accel_find(NULL, NULL, &R.dev) == 0;
        if (R.found)
            fprintf(stderr, "accel: %s (%s), mount matrix from %s\n", R.dev.name, R.dev.dir,
                    R.dev.matrix_from == 1 ? "the hwdb" : R.dev.matrix_from == 2 ? "the driver" : "nowhere (identity)");
    }
    return R.found;
}

static void *reader(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&R.mu);
        while (!R.want) pthread_cond_wait(&R.cv, &R.mu);
        pthread_mutex_unlock(&R.mu);
        double v[3];
        if (accel_read(&R.dev, v) == 0) {
            pthread_mutex_lock(&R.mu);
            memcpy(R.v, v, sizeof v);
            R.fresh = 1;
            pthread_mutex_unlock(&R.mu);
        }
        struct timespec ts = { 0, ACCEL_PERIOD_MS * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void accel_want(int on) {
    if (!accel_present()) return;
    if (on && !R.started) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        R.started = pthread_create(&t, &at, reader, NULL) == 0;
        pthread_attr_destroy(&at);
        if (!R.started) { fprintf(stderr, "accel: no reader thread\n"); return; }
    }
    pthread_mutex_lock(&R.mu);
    if (R.want != (on != 0)) {
        R.want = on != 0;
        R.fresh = 0;
        pthread_cond_signal(&R.cv);
    }
    pthread_mutex_unlock(&R.mu);
}

int accel_latest(double v[3]) {
    if (!R.started) return 0;
    pthread_mutex_lock(&R.mu);
    int f = R.fresh;
    if (f) { memcpy(v, R.v, sizeof R.v); R.fresh = 0; }
    pthread_mutex_unlock(&R.mu);
    return f;
}
