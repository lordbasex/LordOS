/* speak.c — reading text aloud from the desktop (docs/VOICE.md, phase 2): lord-server starts lord-say.
 *
 * lord-say is the `say` command (say.c); it holds no speech code and starts the sandboxed lord-tts itself. Here it is
 * started from the fixed path next to lord-server, never with the text in its arguments: the text is its stdin, a
 * sealed memfd, so the compositor never blocks writing it, and lord-say tells its language. A minimal environment: HOME, LANG (the
 * interface's language, for a text too short to tell), PATH (for PipeWire's pw-cat) and XDG_RUNTIME_DIR (PipeWire's
 * socket).
 * One reading at a time, in its own process group: a new one, or speak_stop, ends the one before (lord-say ends
 * lord-tts and pw-cat on SIGTERM). A pause stops the whole group (SIGSTOP), so pw-cat stops at once. Children are
 * reaped here, never by a global waitpid.
 *
 * The player's wave: lord-say --levels writes the bands of each SAY_LEVEL_MS of audio to a pipe at its fd 3 as it plays
 * it (levels.c), up to a pipe ahead of what is heard. speak_levels tells which one is playing by the time since the
 * first one came, less the pauses; when it runs out (the next sentence isn't ready) that clock waits. */
#include "speak.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char say_path[PATH_MAX];
static pid_t cur = -1;                  /* the reading in progress */
static int last_exit = -1;              /* how the last one ended: lord-say's exit status, 130 when stopped */
static pid_t reap[16];                  /* stopped readings not yet reaped */
static int nreap;
static int paused;
static int saving;                      /* the one in progress writes a file (speak_save) */
static char *last_text;                 /* for speak_replay */
static size_t last_n;
static char last_lang[3], last_voice[64];

/* the wave: the bands lord-say wrote, the last RING of them */
#define RING 256
static int lev_fd = -1;
static uint8_t ring[RING][SAY_BANDS];
static long nlev;                       /* how many came */
static uint8_t part[SAY_BANDS];
static size_t npart;
static double t0, pause_at, pause_total;    /* ms: the first one came, this pause began, the pauses before */

static void save_end(int ok);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void levels_close(void) {
    if (lev_fd >= 0) close(lev_fd);
    lev_fd = -1;
    nlev = 0;
    npart = 0;
    pause_total = 0;
}

static void levels_drain(void) {
    uint8_t buf[SAY_BANDS * 64];
    while (lev_fd >= 0) {
        ssize_t r = read(lev_fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r == 0) { close(lev_fd); lev_fd = -1; } return; }
        for (ssize_t i = 0; i < r; i++) {
            part[npart++] = buf[i];
            if (npart < SAY_BANDS) continue;
            if (!nlev) { t0 = now_ms(); pause_total = 0; }
            memcpy(ring[nlev % RING], part, SAY_BANDS);
            nlev++;
            npart = 0;
        }
    }
}

/* next to lord-server, owned by its user or root, writable by nobody else */
static int find_say(void) {
    if (say_path[0]) return 0;
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return -1;
    self[n] = 0;
    char *slash = strrchr(self, '/');
    size_t dir = slash ? (size_t)(slash - self) : 0;
    if (!slash || dir + sizeof "/lord-say" > sizeof say_path) return -1;
    memcpy(say_path, self, dir);
    memcpy(say_path + dir, "/lord-say", sizeof "/lord-say");
    return 0;
}

static int path_ok(const char *path) {
    struct stat st, me;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return 0;
    return !stat("/proc/self/exe", &me) && (st.st_uid == 0 || st.st_uid == me.st_uid) && !(st.st_mode & 022);
}

int speak_available(void) {
    if (find_say()) return 0;
    char tts[PATH_MAX + 16];
    snprintf(tts, sizeof tts, "%.*s/lord-tts", (int)(strrchr(say_path, '/') - say_path), say_path);
    return path_ok(say_path) && path_ok(tts);
}

static void reap_stopped(void) {
    for (int i = 0; i < nreap;) {
        if (waitpid(reap[i], NULL, WNOHANG) != 0) reap[i] = reap[--nreap];
        else i++;
    }
}

void speak_stop(void) {
    reap_stopped();
    levels_close();
    if (cur <= 0) return;
    kill(-cur, SIGTERM);                /* lord-say, and pw-cat with it; lord-say ends lord-tts */
    if (paused) kill(-cur, SIGCONT);    /* a stopped process takes the signal when it goes on */
    paused = 0;
    if (saving) { saving = 0; save_end(0); }      /* the half-made file goes now; lord-say is reaped later */
    if (nreap < (int)(sizeof reap / sizeof reap[0])) reap[nreap++] = cur;
    else { kill(-cur, SIGKILL); waitpid(cur, NULL, 0); }
    cur = -1;
    last_exit = 130;
}

int speak_state(int *exit_code) {
    reap_stopped();
    if (cur > 0) {
        int st;
        pid_t r = waitpid(cur, &st, WNOHANG);
        if (r == cur || (r < 0 && errno == ECHILD)) {
            last_exit = r == cur && WIFEXITED(st) ? WEXITSTATUS(st) : r == cur && WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
            cur = -1;
            paused = 0;
            levels_close();
            if (saving) { saving = 0; save_end(last_exit == 0); }
        }
    }
    levels_drain();
    if (exit_code) *exit_code = last_exit;
    return cur > 0;
}

int speak_pause(int on) {
    if (cur <= 0 || saving) return -1;
    on = !!on;
    if (on == paused) return 0;
    if (kill(-cur, on ? SIGSTOP : SIGCONT)) return -1;
    paused = on;
    if (on) pause_at = now_ms();
    else pause_total += now_ms() - pause_at;
    return 0;
}

int speak_paused(void) { return cur > 0 && paused; }

int speak_saving(void) { return cur > 0 && saving; }

int speak_replay(void) {
    if (!last_text) return -1;
    return speak_start(last_text, last_n, last_lang[0] ? last_lang : NULL, last_voice[0] ? last_voice : NULL);
}

int speak_levels(uint8_t out[SAY_BANDS]) {
    memset(out, 0, SAY_BANDS);
    if (!speak_state(NULL) || !nlev) return 0;
    double t = (paused ? pause_at : now_ms()) - t0 - pause_total;
    long i = t > 0 ? (long)(t / SAY_LEVEL_MS) : 0;
    if (i >= nlev) {                    /* the next sentence isn't here yet: its sound isn't either */
        t0 += (double)(i - nlev + 1) * SAY_LEVEL_MS;
        i = nlev - 1;
    }
    if (i < nlev - RING) i = nlev - RING;
    memcpy(out, ring[i % RING], SAY_BANDS);
    return 1;
}

/* the text in a sealed memfd at offset 0: lord-say reads it as stdin */
static int text_fd(const char *text, size_t n) {
    int fd = memfd_create("lordos-say", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, text + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); return -1; }
        done += (size_t)w;
    }
    if (lseek(fd, 0, SEEK_SET) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) { close(fd); return -1; }
    return fd;
}

/* lord-say, in its own process group, with a minimal environment: nothing else from lord-server's. stdin the text,
 * stdout out (or /dev/null), fd 3 lev (or nothing). */
static int spawn_say(int in, int out, int lev, const char *lang, char *const argv[]) {
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in, 0);
    posix_spawn_file_actions_adddup2(&fa, out >= 0 ? out : devnull, 1);
    posix_spawn_file_actions_adddup2(&fa, devnull, 2);
    if (lev >= 0) posix_spawn_file_actions_adddup2(&fa, lev, 3);
    posix_spawn_file_actions_addclosefrom_np(&fa, lev >= 0 ? 4 : 3);    /* nothing of lord-server's (DRM, sockets) reaches it */
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

    char henv[PATH_MAX + 8], renv[PATH_MAX + 20], lenv[24], *envp[6];
    int ne = 0;
    const char *home = getenv("HOME"), *rt = getenv("XDG_RUNTIME_DIR");
    if (home && strlen(home) < PATH_MAX) { snprintf(henv, sizeof henv, "HOME=%s", home); envp[ne++] = henv; }
    if (rt && strlen(rt) < PATH_MAX) { snprintf(renv, sizeof renv, "XDG_RUNTIME_DIR=%s", rt); envp[ne++] = renv; }
    snprintf(lenv, sizeof lenv, "LANG=%s.UTF-8", lang ? lang : "C");
    envp[ne++] = lenv;
    envp[ne++] = "PATH=/usr/bin:/bin";
    envp[ne] = NULL;
    pid_t pid;
    int r = posix_spawn(&pid, say_path, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(devnull);
    if (r) { errno = r; return -1; }
    cur = pid;
    last_exit = -1;
    return 0;
}

static int check_args(const char *text, size_t n, const char **lang) {
    if (!text || !n || n > SPEAK_TEXT_MAX) { errno = EINVAL; return -1; }
    if (*lang && strcmp(*lang, "es") && strcmp(*lang, "en") && strcmp(*lang, "pt")) *lang = NULL;
    if (!speak_available()) { errno = ENOENT; return -1; }
    return 0;
}

/* a voice name as say.c takes it (-v): letters, digits, - and _, at most 60 */
static int plain_voice(const char *v) {
    size_t n = v ? strlen(v) : 0;
    if (!n || n > 60) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((v[i] >= 'a' && v[i] <= 'z') || (v[i] >= 'A' && v[i] <= 'Z') || (v[i] >= '0' && v[i] <= '9') || v[i] == '_' || v[i] == '-')) return 0;
    return 1;
}

int speak_start(const char *text, size_t n, const char *lang, const char *voice) {
    speak_stop();
    if (check_args(text, n, &lang)) return -1;
    if (voice && !plain_voice(voice)) { errno = EINVAL; return -1; }
    if (text != last_text) {
        char *copy = malloc(n);
        if (!copy) return -1;
        memcpy(copy, text, n);
        free(last_text);
        last_text = copy;
        last_n = n;
        snprintf(last_lang, sizeof last_lang, "%s", lang ? lang : "");
    }
    char vbuf[64];
    snprintf(vbuf, sizeof vbuf, "%s", voice ? voice : "");
    if (voice != last_voice) snprintf(last_voice, sizeof last_voice, "%s", vbuf);
    int in = text_fd(text, n);
    if (in < 0) return -1;
    int lev[2];
    if (pipe2(lev, O_CLOEXEC)) { close(in); return -1; }
    fcntl(lev[0], F_SETFL, O_RDONLY | O_NONBLOCK);
    char *argv[] = { "lord-say", "--levels", vbuf[0] ? "-v" : NULL, vbuf[0] ? vbuf : NULL, NULL };
    int r = spawn_say(in, -1, lev[1], lang, argv);
    close(in);
    close(lev[1]);
    if (r) { close(lev[0]); return -1; }
    lev_fd = lev[0];
    return 0;
}

/* a voice's recorded sample (say --preview): the language from a closed list, the voice a plain name; the player's
 * wave as for reading; never something to read again (speak_replay keeps the text before) */
int speak_preview(const char *lang, const char *voice) {
    speak_stop();
    if (!lang || (strcmp(lang, "es") && strcmp(lang, "en") && strcmp(lang, "pt"))) { errno = EINVAL; return -1; }
    if (!plain_voice(voice)) { errno = EINVAL; return -1; }
    size_t vn = strlen(voice);
    if (!speak_available()) { errno = ENOENT; return -1; }
    int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int lev[2];
    if (pipe2(lev, O_CLOEXEC)) { close(in); return -1; }
    fcntl(lev[0], F_SETFL, O_RDONLY | O_NONBLOCK);
    char l[3], v[61];
    memcpy(l, lang, 3);
    memcpy(v, voice, vn + 1);
    char *argv[] = { "lord-say", "--levels", "--preview", l, v, NULL };
    int r = spawn_say(in, -1, lev[1], lang, argv);
    close(in);
    close(lev[1]);
    if (r) { close(lev[0]); return -1; }
    lev_fd = lev[0];
    return 0;
}

/* Saving: lord-say writes the WAV to its stdout, a new hidden file in the folder (opened here by its resolved path and
 * kept open, so the file is made in that folder whatever happens to the path meanwhile); when lord-say ends well it is
 * renamed to the name asked for, else removed. lord-say never sees a path. */
static int save_dir = -1;
static char save_tmp[32], save_name[256];

static void save_end(int ok) {
    if (save_dir < 0) return;
    if (!ok || renameat(save_dir, save_tmp, save_dir, save_name)) unlinkat(save_dir, save_tmp, 0);
    close(save_dir);
    save_dir = -1;
}

int speak_save(const char *text, size_t n, const char *lang, const char *target) {
    speak_stop();
    if (check_args(text, n, &lang)) return -1;
    const char *slash = target ? strrchr(target, '/') : NULL;
    if (!slash || target[0] != '/' || !slash[1] || strlen(slash + 1) >= sizeof save_name) { errno = EINVAL; return -1; }
    char dir[PATH_MAX];
    size_t dl = (size_t)(slash - target);
    if (dl >= sizeof dir) { errno = ENAMETOOLONG; return -1; }
    memcpy(dir, target, dl);
    dir[dl] = 0;
    int dfd = open(dl ? dir : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) return -1;
    int out = -1;
    for (int tries = 0; out < 0 && tries < 16; tries++) {
        unsigned v;
        if (getentropy(&v, sizeof v)) break;
        snprintf(save_tmp, sizeof save_tmp, ".lordos-say-%08x", v);
        out = openat(dfd, save_tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (out < 0 && errno != EEXIST) break;
    }
    if (out < 0) { close(dfd); return -1; }
    int in = text_fd(text, n);
    char *argv[] = { "lord-say", "-o", "-", NULL };
    int r = in < 0 ? -1 : spawn_say(in, out, -1, lang, argv);
    if (in >= 0) close(in);
    close(out);
    if (r) { unlinkat(dfd, save_tmp, 0); close(dfd); return -1; }
    save_dir = dfd;
    snprintf(save_name, sizeof save_name, "%s", slash + 1);
    saving = 1;
    return 0;
}
