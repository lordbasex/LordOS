/* boottest.c — see boottest.h. */
#include "boottest.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int valid(const BootTest *b) {
    return b->total >= BT_MIN && b->total <= BT_MAX && b->left >= 1 && b->left <= b->total && b->at > 0;
}

int bt_format(const BootTest *b, char *out, unsigned n) {
    if (!valid(b)) return -1;
    int k = snprintf(out, n, "lordos-boot-test 1\ntotal %d\nleft %d\nat %lld\n", b->total, b->left, b->at);
    return k > 0 && (unsigned)k < n ? 0 : -1;
}

/* "<key> <digits>\n" at *p; the number in *v */
static int field(const char **p, const char *key, long long max, long long *v) {
    size_t kl = strlen(key);
    if (strncmp(*p, key, kl) || (*p)[kl] != ' ') return -1;
    const char *d = *p + kl + 1;
    long long x = 0;
    int digits = 0;
    while (*d >= '0' && *d <= '9') {
        if (++digits > 12) return -1;
        x = x * 10 + (*d++ - '0');
    }
    if (!digits || *d != '\n' || x > max) return -1;
    *v = x;
    *p = d + 1;
    return 0;
}

int bt_parse(const char *text, BootTest *b) {
    static const char head[] = "lordos-boot-test 1\n";
    if (!text || strncmp(text, head, sizeof head - 1)) return -1;
    const char *p = text + sizeof head - 1;
    long long total, left, at;
    if (field(&p, "total", BT_MAX, &total) || field(&p, "left", BT_MAX, &left) || field(&p, "at", 999999999999LL, &at) || *p) return -1;
    BootTest t = { (int)total, (int)left, at };
    if (!valid(&t)) return -1;
    *b = t;
    return 0;
}

int bt_read(const char *path, BootTest *b) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    char buf[257];
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && st.st_size > 0 && st.st_size <= 256)
        n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return bt_parse(buf, b);
}

int bt_write(const char *path, const BootTest *b) {
    char text[128], tmp[256];
    if (bt_format(b, text, sizeof text) || snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) return -1;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    size_t l = strlen(text);
    int ok = write(fd, text, l) == (ssize_t)l && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int bt_next(int have, const BootTest *b, long long now, int *k, BootTest *after) {
    if (!have) return BT_NONE;
    if (!valid(b) || now < b->at - 60 || now > b->at + BT_STALE_S) return BT_STALE;
    *k = b->total - b->left + 1;
    if (b->left <= 1) return BT_LAST;
    *after = (BootTest){ b->total, b->left - 1, now };
    return BT_AGAIN;
}
