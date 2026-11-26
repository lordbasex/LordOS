/* spellsvc.c — lord-spell started on demand, spoken to on a thread of its own, and the person's dictionary (see
 * spellsvc.h and docs/SPELLING.md). */
#include "spellsvc.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define QUEUE 16
#define RESULTS 32
#define DICT_FILE_MAX (64u << 20)
#define WORDS_FILE_MAX (512u * 1024)

enum { J_CHECK = 1, J_SUGGEST };
typedef struct {
    int id, kind, n;
    SpellWord w[SPELL_BATCH_MAX];
    uint8_t skip[SPELL_BATCH_MAX];          /* not a word lord-spell may be asked about: good, never sent */
} Job;
typedef struct { int id; char *json; } Result;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, next_id, stop_now;
    Job q[QUEUE];
    int nq;
    Result r[RESULTS];
    SpellTag want[SPELL_LANGS_MAX];
    int nwant, gen;
    /* the helper (the worker's; read under mu for the state) */
    pid_t pid;
    int fd, have_gen;
    uint8_t loaded[SPELL_LANGS_MAX];
    const char *state;                      /* "off", "starting", "ready", "failed" */
    double last_used, failed_at;
    int fails;
    int idle_ms;
    char dict_dir[PATH_MAX], helper[PATH_MAX], words_file[PATH_MAX];
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .pid = -1, .fd = -1, .have_gen = -1,
        .state = "off", .idle_ms = SPELL_IDLE_MS, .dict_dir = SPELL_DIR };

static Job cur;                             /* the job the worker runs (too big for its stack) */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

void spell_set_paths(const char *dict_dir, const char *helper, const char *words_file) {
    pthread_mutex_lock(&S.mu);
    if (dict_dir) snprintf(S.dict_dir, sizeof S.dict_dir, "%s", dict_dir);
    if (helper) snprintf(S.helper, sizeof S.helper, "%s", helper);
    if (words_file) snprintf(S.words_file, sizeof S.words_file, "%s", words_file);
    pthread_mutex_unlock(&S.mu);
}

/* ---- the dictionaries ---- */

static int cmp_tag(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int installed_in(const char *dir, SpellTag *out, int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l - 4 >= SPELL_TAG_MAX || strcmp(e->d_name + l - 4, ".dic")) continue;
        SpellTag t;
        memcpy(t, e->d_name, l - 4);
        t[l - 4] = 0;
        if (!spell_tag_ok(t)) continue;
        char aff[PATH_MAX];
        snprintf(aff, sizeof aff, "%s/%s.aff", dir, t);
        struct stat st;
        if (stat(aff, &st) || !S_ISREG(st.st_mode)) continue;
        memcpy(out[n++], t, sizeof t);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], cmp_tag);
    return n;
}

int spell_installed(SpellTag *out, int max) {
    char dir[PATH_MAX];
    pthread_mutex_lock(&S.mu);
    snprintf(dir, sizeof dir, "%s", S.dict_dir);
    pthread_mutex_unlock(&S.mu);
    return installed_in(dir, out, max);
}

char *spell_installed_json(void) {
    SpellTag t[SPELL_INSTALLED_MAX];
    int n = spell_installed(t, SPELL_INSTALLED_MAX);
    char *o = malloc((size_t)n * (SPELL_TAG_MAX + 4) + 4);
    if (!o) return NULL;
    size_t at = 0;
    o[at++] = '[';
    for (int i = 0; i < n; i++) at += (size_t)sprintf(o + at, "%s\"%s\"", i ? "," : "", t[i]);
    o[at++] = ']';
    o[at] = 0;
    return o;
}

/* a dictionary file of the folder, opened read-only: a regular file of at most 64 MB whose real path is in the
 * folder (Debian's es_AR.aff is a link to es_ES.aff) or in /usr/share/myspell */
static int open_dict(const char *dir, const char *tag, const char *ext) {
    char p[PATH_MAX], real[PATH_MAX], rdir[PATH_MAX];
    if (!spell_tag_ok(tag) || snprintf(p, sizeof p, "%s/%s.%s", dir, tag, ext) >= (int)sizeof p) return -1;
    if (!realpath(dir, rdir) || !realpath(p, real)) return -1;
    size_t dl = strlen(rdir);
    int inside = (!strncmp(real, rdir, dl) && real[dl] == '/') || !strncmp(real, "/usr/share/myspell/", 19);
    if (!inside) return -1;
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (unsigned long long)st.st_size > DICT_FILE_MAX ||
        (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 002)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    return fd;
}

int spell_set_langs(const char *const *tags, int n) {
    SpellTag have[SPELL_INSTALLED_MAX], keep[SPELL_LANGS_MAX];
    int nh = spell_installed(have, SPELL_INSTALLED_MAX), k = 0;
    for (int i = 0; i < n && k < SPELL_LANGS_MAX; i++) {
        if (!tags[i] || !spell_tag_ok(tags[i])) continue;
        int found = 0, dup = 0;
        for (int j = 0; j < nh; j++) if (!strcmp(have[j], tags[i])) found = 1;
        for (int j = 0; j < k; j++) if (!strcmp(keep[j], tags[i])) dup = 1;
        if (found && !dup) snprintf(keep[k++], SPELL_TAG_MAX, "%s", tags[i]);
    }
    pthread_mutex_lock(&S.mu);
    int same = k == S.nwant;
    for (int i = 0; same && i < k; i++) same = !strcmp(keep[i], S.want[i]);
    if (!same) {
        memcpy(S.want, keep, sizeof keep);
        S.nwant = k;
        S.gen++;
        S.fails = 0;
        pthread_cond_signal(&S.cv);
    }
    pthread_mutex_unlock(&S.mu);
    return k;
}

/* ---- the helper ---- */

static int helper_path(char *out, size_t n) {
    pthread_mutex_lock(&S.mu);
    int set = S.helper[0] != 0;
    if (set) snprintf(out, n, "%s", S.helper);
    pthread_mutex_unlock(&S.mu);
    if (!set) {
        ssize_t l = readlink("/proc/self/exe", out, n - 32);
        if (l <= 0) return -1;
        out[l] = 0;
        char *s = strrchr(out, '/');
        if (!s) return -1;
        snprintf(s + 1, n - (size_t)(s + 1 - out), "lord-spell");
    }
    struct stat st;
    return stat(out, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022) ? -1 : 0;
}

static void helper_kill(void) {
    pid_t pid;
    int fd;
    pthread_mutex_lock(&S.mu);
    pid = S.pid;
    fd = S.fd;
    S.pid = -1;
    S.fd = -1;
    S.have_gen = -1;
    memset(S.loaded, 0, sizeof S.loaded);
    pthread_mutex_unlock(&S.mu);
    if (fd >= 0) close(fd);
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
}

static int send_all(int fd, const uint8_t *b, size_t n) {
    for (size_t o = 0; o < n;) {
        ssize_t w = send(fd, b + o, n - o, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    return 0;
}
static int recv_all(int fd, uint8_t *b, size_t n, double until) {
    for (size_t o = 0; o < n;) {
        int left = (int)(until - now_ms());
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { errno = ETIMEDOUT; return -1; }
        ssize_t r = recv(fd, b + o, n - o, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        o += (size_t)r;
    }
    return 0;
}

static uint8_t fbuf[SPELL_HEAD + SPELL_PAYLOAD_MAX], pbuf[SPELL_PAYLOAD_MAX];

/* one request and its answer, which must be the operation expected; the payload in fbuf + SPELL_HEAD */
static int ask(int fd, int op, const uint8_t *pay, size_t len, int want, size_t *alen, int ms) {
    int n = spell_frame(fbuf, sizeof fbuf, op, pay, len);
    if (n < 0 || send_all(fd, fbuf, (size_t)n)) return -1;
    double until = now_ms() + ms;
    int aop;
    if (recv_all(fd, fbuf, SPELL_HEAD, until) || spell_head(fbuf, &aop, alen) || aop != want) return -1;
    return *alen ? recv_all(fd, fbuf + SPELL_HEAD, *alen, until) : 0;
}

/* lord-spell with these languages' dictionaries; 0 when it answered which it loaded */
static int helper_start(const SpellTag *tags, int n, int gen) {
    char bin[PATH_MAX], dir[PATH_MAX];
    if (helper_path(bin, sizeof bin)) return -1;
    pthread_mutex_lock(&S.mu);
    snprintf(dir, sizeof dir, "%s", S.dict_dir);
    pthread_mutex_unlock(&S.mu);
    int fds[2 * SPELL_LANGS_MAX], sv[2] = { -1, -1 }, nul = -1, ok = 0;
    for (int i = 0; i < 2 * n; i++) fds[i] = -1;
    for (int i = 0; i < n; i++) {
        fds[2 * i] = open_dict(dir, tags[i], "aff");
        fds[2 * i + 1] = open_dict(dir, tags[i], "dic");
    }
    /* above every number the child will use, so moving them into place never closes one still needed */
    for (int i = 0; i < 2 * n; i++) {
        if (fds[i] < 0) { fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC); }  /* that language just won't load */
        if (fds[i] < 0) goto out;
        int hi = fcntl(fds[i], F_DUPFD_CLOEXEC, 64);
        close(fds[i]);
        fds[i] = hi;
        if (hi < 0) goto out;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) goto out;
    nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    int child_sock = nul >= 0 ? fcntl(sv[1], F_DUPFD_CLOEXEC, 64) : -1;
    int child_nul = child_sock >= 0 ? fcntl(nul, F_DUPFD_CLOEXEC, 64) : -1;
    if (child_nul < 0) { if (child_sock >= 0) close(child_sock); goto out; }
    pid_t pid = fork();
    if (pid == 0) {
        if (dup2(child_sock, 0) != 0 || dup2(child_nul, 1) != 1 || dup2(child_nul, 2) != 2) _exit(127);
        for (int i = 0; i < 2 * n; i++) if (dup2(fds[i], 3 + i) != 3 + i) _exit(127);
        syscall(SYS_close_range, 3 + 2 * n, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        char *const args[] = { "lord-spell", NULL }, *const envp[] = { NULL };
        execve(bin, args, envp);
        _exit(127);
    }
    close(child_sock);
    close(child_nul);
    if (pid < 0) goto out;
    close(sv[1]);
    sv[1] = -1;
    pthread_mutex_lock(&S.mu);
    S.pid = pid;
    S.fd = sv[0];
    S.state = "starting";
    pthread_mutex_unlock(&S.mu);
    sv[0] = -1;
    int len = spell_build_load(pbuf, sizeof pbuf, tags, n);
    size_t alen = 0;
    uint8_t loaded[SPELL_LANGS_MAX] = { 0 };
    if (len < 0 || ask(S.fd, 'L', pbuf, (size_t)len, 'R', &alen, SPELL_START_MS) ||
        spell_parse_status(fbuf + SPELL_HEAD, alen, n, loaded)) { helper_kill(); goto out; }
    pthread_mutex_lock(&S.mu);
    memcpy(S.loaded, loaded, sizeof loaded);
    S.have_gen = gen;
    S.state = "ready";
    pthread_mutex_unlock(&S.mu);
    ok = 1;
out:
    for (int i = 0; i < 2 * n; i++) if (fds[i] >= 0) close(fds[i]);
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    if (nul >= 0) close(nul);
    return ok ? 0 : -1;
}

static char *errjson(const char *code) {
    char *o = malloc(64);
    if (o) snprintf(o, 64, "{\"ok\":false,\"error\":\"%s\"}", code);
    return o;
}

/* the worker's job, against a running helper; NULL when the helper failed */
static char *run(Job *j, unsigned mask) {
    size_t alen = 0;
    if (j->kind == J_CHECK) {
        SpellWord send[SPELL_BATCH_MAX];
        int idx[SPELL_BATCH_MAX], k = 0;
        uint8_t good[SPELL_BATCH_MAX];
        for (int i = 0; i < j->n; i++) if (!j->skip[i]) { memcpy(send[k], j->w[i], sizeof send[k]); idx[k++] = i; }
        uint8_t ans[SPELL_BATCH_MAX] = { 0 };
        if (k) {
            int len = spell_build_check(pbuf, sizeof pbuf, mask, send, k);
            if (len < 0 || ask(S.fd, 'C', pbuf, (size_t)len, 'c', &alen, SPELL_REPLY_MS) || spell_parse_checked(fbuf + SPELL_HEAD, alen, k, ans)) return NULL;
        }
        for (int i = 0; i < j->n; i++) good[i] = 1;
        for (int i = 0; i < k; i++) good[idx[i]] = ans[i];
        char *o = malloc((size_t)j->n * 2 + 32);
        if (!o) return errjson("spell.failed");
        size_t at = (size_t)sprintf(o, "{\"ok\":true,\"good\":[");
        for (int i = 0; i < j->n; i++) at += (size_t)sprintf(o + at, "%s%d", i ? "," : "", good[i]);
        sprintf(o + at, "]}");
        return o;
    }
    if (j->skip[0]) return strdup("{\"ok\":true,\"suggestions\":[]}");
    int len = spell_build_suggest(pbuf, sizeof pbuf, mask, j->w[0]);
    SpellWord sg[SPELL_SUGG_MAX];
    int n = 0;
    if (len < 0 || ask(S.fd, 'G', pbuf, (size_t)len, 'g', &alen, SPELL_REPLY_MS) || spell_parse_suggestions(fbuf + SPELL_HEAD, alen, sg, &n)) return NULL;
    char *o = malloc((size_t)n * (SPELL_WORD_MAX * 6 + 4) + 48);
    if (!o) return errjson("spell.failed");
    size_t at = (size_t)sprintf(o, "{\"ok\":true,\"suggestions\":[");
    for (int i = 0; i < n; i++) {
        if (i) o[at++] = ',';
        at += json_quote(o + at, SPELL_WORD_MAX * 6 + 3, sg[i]);
    }
    sprintf(o + at, "]}");
    return o;
}

static void put_result(int id, char *json) {
    pthread_mutex_lock(&S.mu);
    int slot = 0;
    for (int i = 0; i < RESULTS; i++) if (!S.r[i].id) { slot = i; break; } else if (S.r[i].id < S.r[slot].id) slot = i;
    free(S.r[slot].json);                   /* the oldest one never taken */
    S.r[slot].id = id;
    S.r[slot].json = json;
    pthread_mutex_unlock(&S.mu);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&S.mu);
        while (!S.nq) {
            int idle = S.pid > 0 && now_ms() - S.last_used > S.idle_ms;
            if (S.stop_now || idle || (S.pid > 0 && S.have_gen != S.gen)) {
                S.stop_now = 0;
                pthread_mutex_unlock(&S.mu);
                helper_kill();
                pthread_mutex_lock(&S.mu);
                if (strcmp(S.state, "failed")) S.state = "off";
                continue;
            }
            if (S.pid > 0) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 5;
                pthread_cond_timedwait(&S.cv, &S.mu, &ts);
            } else pthread_cond_wait(&S.cv, &S.mu);
        }
        cur = S.q[0];
        memmove(S.q, S.q + 1, sizeof S.q[0] * (size_t)(--S.nq));
        SpellTag tags[SPELL_LANGS_MAX];
        int n = S.nwant, gen = S.gen;
        memcpy(tags, S.want, sizeof tags);
        int running = S.pid > 0 && S.have_gen == gen;
        /* three failures in a row: no new helper for a minute (the jobs answer at once) */
        int resting = S.fails >= 3 && now_ms() - S.failed_at < 60000;
        pthread_mutex_unlock(&S.mu);

        char *res = NULL;
        if (!n) res = errjson("spell.nolang");
        else if (!running && resting) res = errjson("spell.unavailable");
        else {
            if (!running) {
                helper_kill();
                if (helper_start(tags, n, gen)) {
                    pthread_mutex_lock(&S.mu);
                    S.fails++;
                    S.failed_at = now_ms();
                    S.state = "failed";
                    pthread_mutex_unlock(&S.mu);
                    res = errjson("spell.unavailable");
                }
            }
            if (!res) {
                unsigned mask = 0;
                pthread_mutex_lock(&S.mu);
                for (int i = 0; i < n; i++) if (S.loaded[i]) mask |= 1u << i;
                pthread_mutex_unlock(&S.mu);
                if (!mask) res = errjson("spell.nolang");
                else if (!(res = run(&cur, mask))) {
                    helper_kill();
                    pthread_mutex_lock(&S.mu);
                    S.fails++;
                    S.failed_at = now_ms();
                    S.state = "failed";
                    pthread_mutex_unlock(&S.mu);
                    res = errjson("spell.failed");
                } else {
                    pthread_mutex_lock(&S.mu);
                    S.fails = 0;
                    S.last_used = now_ms();
                    pthread_mutex_unlock(&S.mu);
                }
            }
        }
        put_result(cur.id, res ? res : errjson("spell.failed"));
    }
    return NULL;
}

static int submit(Job *j) {
    pthread_mutex_lock(&S.mu);
    if (!S.started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (!pthread_create(&th, &at, worker, NULL)) S.started = 1;
        pthread_attr_destroy(&at);
        if (!S.started) { pthread_mutex_unlock(&S.mu); errno = EAGAIN; return -1; }
    }
    if (S.nq >= QUEUE) { pthread_mutex_unlock(&S.mu); errno = EBUSY; return -1; }
    if (++S.next_id <= 0) S.next_id = 1;
    j->id = S.next_id;
    S.q[S.nq++] = *j;
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
    return j->id;
}

static Job sub;                             /* built on the JS thread (only that thread submits) */

static void put_word(Job *j, int i, const char *w) {
    size_t l = w ? strlen(w) : 0;
    j->skip[i] = !spell_word_ok(w, l);
    if (!j->skip[i]) memcpy(j->w[i], w, l + 1);
    else j->w[i][0] = 0;
}

int spell_submit_check(const char *const *words, int n) {
    if (n < 1 || n > SPELL_BATCH_MAX) { errno = EINVAL; return -1; }
    memset(&sub, 0, sizeof sub);
    sub.kind = J_CHECK;
    sub.n = n;
    for (int i = 0; i < n; i++) put_word(&sub, i, words[i]);
    return submit(&sub);
}

int spell_submit_suggest(const char *word) {
    memset(&sub, 0, sizeof sub);
    sub.kind = J_SUGGEST;
    sub.n = 1;
    put_word(&sub, 0, word);
    return submit(&sub);
}

char *spell_take(int id) {
    char *r = NULL;
    pthread_mutex_lock(&S.mu);
    for (int i = 0; i < RESULTS; i++) if (S.r[i].id == id && id > 0) { r = S.r[i].json; S.r[i].json = NULL; S.r[i].id = 0; break; }
    pthread_mutex_unlock(&S.mu);
    return r;
}

char *spell_state_json(void) {
    char *o = malloc(256);
    if (!o) return NULL;
    pthread_mutex_lock(&S.mu);
    size_t at = (size_t)sprintf(o, "{\"langs\":[");
    for (int i = 0; i < S.nwant; i++) at += (size_t)sprintf(o + at, "%s\"%s\"", i ? "," : "", S.want[i]);
    at += (size_t)sprintf(o + at, "],\"loaded\":[");
    for (int i = 0; i < S.nwant; i++) at += (size_t)sprintf(o + at, "%s%d", i ? "," : "", S.have_gen == S.gen ? S.loaded[i] : 0);
    sprintf(o + at, "],\"state\":\"%s\",\"queued\":%d}", S.state, S.nq);
    pthread_mutex_unlock(&S.mu);
    return o;
}

void spell_stop(void) {
    pthread_mutex_lock(&S.mu);
    S.nq = 0;
    S.stop_now = 1;
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
}

/* ---- the person's dictionary ---- */

static int words_path(char *out, size_t n) {
    pthread_mutex_lock(&S.mu);
    int set = S.words_file[0] != 0;
    if (set) snprintf(out, n, "%s", S.words_file);
    pthread_mutex_unlock(&S.mu);
    if (set) return 0;
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return -1;
    return snprintf(out, n, "%s/.config/lordos/dictionary.txt", home) >= (int)n ? -1 : 0;
}

static SpellWord dict_words[SPELL_DICT_WORDS_MAX];

/* the words in the file (none when it is missing), into dict_words */
static int read_words(void) {
    char p[PATH_MAX];
    if (words_path(p, sizeof p)) return -1;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)WORDS_FILE_MAX) { close(fd); return -1; }
    char *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!b) return -1;
    int n = spelldict_parse(b, got, dict_words, SPELL_DICT_WORDS_MAX);
    free(b);
    return n;
}

/* the folders up to the file, made 0700 when missing; the new text into <file>.new (O_EXCL, no link followed), then
 * renamed over the file */
static int write_words(int n) {
    char p[PATH_MAX], tmp[PATH_MAX + 8];
    if (words_path(p, sizeof p)) return -1;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", p);
    for (char *s = strchr(dir + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        if (mkdir(dir, 0700) && errno != EEXIST) return -1;
        *s = '/';
    }
    size_t cap = (size_t)n * (SPELL_WORD_MAX + 1) + 1;
    char *text = malloc(cap);
    if (!text) return -1;
    int len = spelldict_format(dict_words, n, text, cap);
    if (len < 0) { free(text); return -1; }
    snprintf(tmp, sizeof tmp, "%s.new", p);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { free(text); return -1; }
    size_t o = 0;
    while (o < (size_t)len) {
        ssize_t w = write(fd, text + o, (size_t)len - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        o += (size_t)w;
    }
    free(text);
    int bad = o != (size_t)len || fsync(fd);
    close(fd);
    if (bad || rename(tmp, p)) { unlink(tmp); return -1; }
    return 0;
}

char *spell_words_json(void) {
    int n = read_words();
    if (n < 0) n = 0;
    size_t cap = (size_t)n * (SPELL_WORD_MAX * 6 + 3) + 4;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t at = 0;
    o[at++] = '[';
    for (int i = 0; i < n; i++) {
        if (i) o[at++] = ',';
        at += json_quote(o + at, cap - at - 2, dict_words[i]);
    }
    o[at++] = ']';
    o[at] = 0;
    return o;
}

int spell_add_word(const char *w) {
    size_t l = w ? strlen(w) : 0;
    if (!spell_word_ok(w, l)) { errno = EINVAL; return -1; }
    int n = read_words();
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(dict_words[i], w)) return 0;
    if (n >= SPELL_DICT_WORDS_MAX) { errno = ENOSPC; return -1; }
    memcpy(dict_words[n], w, l + 1);
    return write_words(n + 1);
}

int spell_remove_word(const char *w) {
    size_t l = w ? strlen(w) : 0;
    if (!spell_word_ok(w, l)) { errno = EINVAL; return -1; }
    int n = read_words();
    if (n < 0) return -1;
    int k = 0;
    for (int i = 0; i < n; i++) if (strcmp(dict_words[i], w)) { if (k != i) memcpy(dict_words[k], dict_words[i], sizeof dict_words[0]); k++; }
    return k == n ? 0 : write_words(k);
}
