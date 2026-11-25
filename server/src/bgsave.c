/* bgsave.c — settings.json and the clipboard's stores saved on a worker thread (GitHub #250).
 *
 * lord-server is one thread for input, layout, painting and JS. lord.settings.save and lord.clipboard.saveStore wrote
 * their file there: a new file, renamed over the old one (and, for the clipboard, an fsync). On an SSD that is under a
 * millisecond; on a laptop's spinning disk the rename waits for ext4's journal (auto_da_alloc flushes the replaced
 * file's data) and the fsync for the platter: the report in #250 (an HDD, a Core i3-6100U) logged 80–650 ms stalls in
 * "call settings.save" after every window move and 56–360 ms in "call clipboard.saveStore" after every copy, the whole
 * desktop frozen meanwhile. Here the write happens on one worker; the call only copies the text.
 *
 * Order and freshness: one slot per file holds the newest text; a save that comes while an older one is queued replaces
 * it (only the last one matters, as each is the whole file), and the worker writes one file at a time, so a file never
 * goes back to older content. Whoever needs the file on the disk first waits with bgsave_flush: lord.settings.load,
 * a durable lord.settings.save (session restore's marker), a theme switch (the new theme reads both stores) and the end
 * of the session (main.c). */
#include "bgsave.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef BGSAVE_TEST
void (*bgsave_test_hook)(const char *dst);
int bgsave_writes;
#endif

typedef struct {
    char dst[PATH_MAX], tmp[PATH_MAX];
    char *data;                 /* the queued text (NULL: nothing queued) */
    size_t len;
    unsigned flags;
    int busy;                   /* the worker is writing this file now */
    int used;
} Slot;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static Slot slots[BGS_SLOTS];
static int started, failed;

int bgsave_write_now(const char *dst, const char *tmp, const char *data, size_t len, unsigned flags) {
    char t[PATH_MAX];
    if (snprintf(t, sizeof t, "%s", tmp) >= (int)sizeof t) { errno = ENAMETOOLONG; return -1; }
    int fd;
    if (flags & BGS_UNIQUE) fd = mkstemp(t);                      /* 0600 */
    else fd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, (flags & BGS_PRIVATE) ? 0600 : 0666);
    if (fd < 0) return -1;
    int ok = !(flags & BGS_PRIVATE) || !fchmod(fd, 0600);
    for (size_t off = 0; ok && off < len;) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0; else off += (size_t)w;
    }
    if (ok && (flags & BGS_FSYNC) && fsync(fd)) ok = 0;
    int e = errno;
    if (close(fd)) { if (ok) e = errno; ok = 0; }
    if (ok && rename(t, dst)) { e = errno; ok = 0; }
    if (!ok) { unlink(t); errno = e ? e : EIO; return -1; }
    return 0;
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        Slot *s = NULL;
        for (int i = 0; i < BGS_SLOTS && !s; i++) if (slots[i].used && slots[i].data && !slots[i].busy) s = &slots[i];
        if (!s) { pthread_cond_wait(&work_cv, &mu); continue; }
        char *data = s->data;
        size_t len = s->len;
        unsigned flags = s->flags;
        char dst[PATH_MAX], tmp[PATH_MAX];
        memcpy(dst, s->dst, sizeof dst);
        memcpy(tmp, s->tmp, sizeof tmp);
        s->data = NULL;
        s->busy = 1;
        pthread_mutex_unlock(&mu);
#ifdef BGSAVE_TEST
        if (bgsave_test_hook) bgsave_test_hook(dst);
#endif
        if (bgsave_write_now(dst, tmp, data, len, flags))
            fprintf(stderr, "lord-server: %s not saved: %s\n", dst, strerror(errno));
        free(data);
        pthread_mutex_lock(&mu);
#ifdef BGSAVE_TEST
        bgsave_writes++;
#endif
        s->busy = 0;
        if (!s->data) s->used = 0;          /* nothing newer came meanwhile: the slot is free again */
        pthread_cond_broadcast(&done_cv);
    }
    return NULL;
}

/* with mu held */
static int start(void) {
    if (started) return 0;
    if (failed) return -1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);             /* signals stay with the main thread */
    int e = pthread_create(&th, &at, worker, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (e) { failed = 1; return -1; }
    started = 1;
    return 0;
}

int bgsave_put(const char *dst, const char *tmp, const char *data, size_t len, unsigned flags) {
    if (strlen(dst) >= PATH_MAX || strlen(tmp) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char *copy = malloc(len ? len : 1);
    if (!copy) return bgsave_write_now(dst, tmp, data, len, flags);
    memcpy(copy, data, len);
    pthread_mutex_lock(&mu);
    Slot *s = NULL, *fresh = NULL;
    for (int i = 0; i < BGS_SLOTS; i++) {
        if (slots[i].used && !strcmp(slots[i].dst, dst)) { s = &slots[i]; break; }
        if (!slots[i].used && !fresh) fresh = &slots[i];
    }
    if (!s && fresh && !start()) {
        s = fresh;
        memset(s, 0, sizeof *s);
        s->used = 1;
        snprintf(s->dst, sizeof s->dst, "%s", dst);
    }
    if (!s) {                           /* no worker or no free slot: as before, on this thread */
        pthread_mutex_unlock(&mu);
        free(copy);
        return bgsave_write_now(dst, tmp, data, len, flags);
    }
    free(s->data);                      /* an older text not written yet: only the newest matters */
    s->data = copy;
    s->len = len;
    s->flags = flags;
    snprintf(s->tmp, sizeof s->tmp, "%s", tmp);
    pthread_cond_signal(&work_cv);
    pthread_mutex_unlock(&mu);
    return 0;
}

void bgsave_flush(const char *dst) {
    pthread_mutex_lock(&mu);
    for (;;) {
        int waiting = 0;
        for (int i = 0; i < BGS_SLOTS && !waiting; i++)
            waiting = slots[i].used && (slots[i].data || slots[i].busy) && (!dst || !strcmp(slots[i].dst, dst));
        if (!waiting) break;
        pthread_cond_wait(&done_cv, &mu);
    }
    pthread_mutex_unlock(&mu);
}
