/* vmlog.c — whole lines, repeats counted and a log rotated when full (see vmlog.h) */
#include "vmlog.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void vmlog_lines(VmLineBuf *b, const char *p, size_t n, VmLineFn fn, void *ctx) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            if (b->n) fn(ctx, b->buf, b->n);
            b->n = 0;
            continue;
        }
        if (b->n == sizeof b->buf) { fn(ctx, b->buf, b->n); b->n = 0; }
        b->buf[b->n++] = p[i];
    }
}

void vmlog_lines_flush(VmLineBuf *b, VmLineFn fn, void *ctx) {
    if (b->n) fn(ctx, b->buf, b->n);
    b->n = 0;
}

static int digits(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

static int has_text(const char *p, size_t n, const char *w) {
    size_t wn = strlen(w);
    for (size_t i = 0; i + wn <= n; i++) if (!memcmp(p + i, w, wn)) return 1;
    return 0;
}

int vmlog_qemu_3d_abort(const char *line, size_t n) {
    return has_text(line, n, "dbus_scanout_texture") && has_text(line, n, "Assertion") && has_text(line, n, "tex_id");
}

int vmlog_3d_abort_end(int asked, int notice, int announces, long long ran_ms) {
    if (asked || notice == 1) return VMLOG_3D_SHUTDOWN;
    if (ran_ms < VMLOG_3D_MIN_RUN_MS) return VMLOG_3D_CRASH;
    if (notice == 2) return VMLOG_3D_RESTART;
    if (announces) return VMLOG_3D_CRASH;
    return VMLOG_3D_GUEST_STOP;
}

const char *vmlog_key(const char *line, size_t n, size_t *kn) {
    /* YYYY-MM-DDTHH:MM:SS, then an optional fraction, 'Z' or not, then one space */
    size_t i = 0;
    if (n >= 20 && digits(line, 4) && line[4] == '-' && digits(line + 5, 2) && line[7] == '-' && digits(line + 8, 2) && line[10] == 'T' &&
        digits(line + 11, 2) && line[13] == ':' && digits(line + 14, 2) && line[16] == ':' && digits(line + 17, 2)) {
        i = 19;
        if (i < n && line[i] == '.') { i++; while (i < n && line[i] >= '0' && line[i] <= '9') i++; }
        if (i < n && line[i] == 'Z') i++;
        if (i < n && line[i] == ' ') i++;
        else i = 0;                     /* not followed by a space: not a prefix */
    }
    *kn = n - i;
    return line + i;
}

static void summary(VmLogRep *r, long long now, char *sum, size_t cap) {
    long long s = (now - r->rep_since + 999) / 1000;
    snprintf(sum, cap, "(the line before came %lu more time%s in %lld s)", r->rep, r->rep == 1 ? "" : "s", s < 1 ? 1 : s);
    r->rep = 0;
    r->said_at = now;
}

int vmlog_repeat(VmLogRep *r, const char *line, size_t n, long long now, char *sum, size_t cap) {
    if (cap) sum[0] = 0;
    size_t kn;
    const char *k = vmlog_key(line, n, &kn);
    if (kn > sizeof r->last) kn = sizeof r->last;
    if (r->last_n && kn == r->last_n && !memcmp(k, r->last, kn)) {
        if (!r->rep++) r->rep_since = now;
        if (now - r->said_at >= VMLOG_SUMMARY_MS && cap) summary(r, now, sum, cap);
        return 0;
    }
    if (r->rep && cap) summary(r, now, sum, cap);
    r->rep = 0;
    memcpy(r->last, k, kn);
    r->last_n = kn;
    r->said_at = now;
    return 1;
}

int vmlog_repeat_tick(VmLogRep *r, long long now, char *sum, size_t cap) {
    if (!r->rep || !cap || now - r->said_at < VMLOG_SUMMARY_MS) return 0;
    summary(r, now, sum, cap);
    return 1;
}

int vmlog_rotate(const char *dir, const char *base, int keep) {
    if (!dir || !base || keep < 1 || keep > 9) return -1;
    char a[PATH_MAX], b[PATH_MAX];
    for (int i = keep - 1; i >= 1; i--) {
        if (i == 1) snprintf(a, sizeof a, "%s/%s.log", dir, base);
        else snprintf(a, sizeof a, "%s/%s.%d.log", dir, base, i - 1);
        snprintf(b, sizeof b, "%s/%s.%d.log", dir, base, i);
        rename(a, b);                   /* over the oldest; a missing one is nothing to move */
    }
    snprintf(a, sizeof a, "%s/%s.log", dir, base);
    if (keep == 1) unlink(a);
    return open(a, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
}
