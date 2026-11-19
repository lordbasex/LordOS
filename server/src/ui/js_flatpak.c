/* js_flatpak.c — lord.flatpak: the Store's Flathub management commands (docs/FLATPAK.md, Problem 4; AGENTS.md
 * "Flatpak as an app source", E3). Only the closed list of fpak_cmd_argv, one at a time (Flatpak locks its repository),
 * each in its own child that enters lordos-net-flathub.scope (the firewall component lordos:flathub, a row in
 * Monitor → Network) before it execs /usr/bin/flatpak with a minimal environment, stdin /dev/null and its output on a
 * pipe. JS asks:
 *   lord.flatpak.available()      → Flatpak installed (root-owned /usr/bin/flatpak) on a supported PC (phase 1: x86-64)
 *   lord.flatpak.start(cmd, ref)  → a job id (cmd one of the names below; ref only where the command takes one), or -1
 *   lord.flatpak.take()           → [{ id, line } | { id, done: true, code }] since the last call
 *   lord.flatpak.cancel(id)       → stops that job (its whole process group)
 * Lines are the command's output, cut to printable UTF-8; nothing in them is acted on here. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "js_flatpak.h"
#include "../fpak.h"
#include "../sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const names[FPAK_CMD_COUNT] = {
    [FPAK_CMD_REMOTE_ADD] = "remoteAdd", [FPAK_CMD_APPSTREAM] = "appstream", [FPAK_CMD_INFO] = "info",
    [FPAK_CMD_INSTALL] = "install", [FPAK_CMD_UPDATE] = "update", [FPAK_CMD_UNINSTALL] = "uninstall",
    [FPAK_CMD_UNUSED] = "uninstallUnused", [FPAK_CMD_LIST] = "list",
};

#define QMAX 16
#define EMAX 512
#define LINE_MAX_BYTES 400
typedef struct { int id, cmd; char ref[FPAK_REF_MAX]; } Job;
typedef struct { int id, done, code; char line[LINE_MAX_BYTES + 1]; } Ev;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Job queue[QMAX];
static int nq, next_id = 1, worker_on, running_id;
static pid_t running_pid;
static Ev evs[EMAX];
static int nev;

static int binary_ok(void) {
    struct stat st;
    return !lstat(FPAK_FLATPAK, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && (st.st_mode & 0111);
}
static const char *native_arch(void) {
#if defined(__x86_64__)
    return "x86_64";
#else
    return NULL;                                /* phase 1: x86-64 PCs only */
#endif
}

/* under mu: one more event; when the buffer is full the oldest lines go (never a done) */
static void push(int id, const char *line, int done, int code) {
    if (nev == EMAX) {
        int drop = -1;
        for (int i = 0; i < nev && drop < 0; i++) if (!evs[i].done) drop = i;
        if (drop < 0) return;
        memmove(&evs[drop], &evs[drop + 1], (size_t)(nev - drop - 1) * sizeof evs[0]);
        nev--;
    }
    Ev *e = &evs[nev++];
    e->id = id; e->done = done; e->code = code; e->line[0] = 0;
    if (line) snprintf(e->line, sizeof e->line, "%s", line);
}

/* printable UTF-8 only (a control character or a broken sequence becomes nothing), at most LINE_MAX_BYTES */
static void clean(const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n && o < LINE_MAX_BYTES;) {
        unsigned char c = (unsigned char)in[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + (size_t)len > n) { i++; continue; }
        int ok = 1;
        for (int k = 1; k < len; k++) if (((unsigned char)in[i + k] >> 6) != 2) ok = 0;
        if (!ok || (len == 1 && (c < 0x20 || c == 0x7f) && c != '\t')) { i++; continue; }
        if (o + (size_t)len > LINE_MAX_BYTES) break;
        memcpy(out + o, in + i, (size_t)len);
        o += (size_t)len;
        i += (size_t)len;
    }
    out[o] = 0;
}

static int run(const Job *j) {
    const char *arch = native_arch();
    char *argv[FPAK_ARGV_MAX], buf[2048];
    if (!arch || !binary_ok() || fpak_cmd_argv(j->cmd, j->ref, arch, argv, buf, sizeof buf) < 0) return 127;
    /* the environment it needs and nothing else: no session bus (no portal, no system helper), no display */
    static const char *const keep[] = { "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "LC_MESSAGES", "XDG_RUNTIME_DIR", NULL };
    char envbuf[8][1024], *envp[10];
    int ne = 0;
    for (int i = 0; keep[i]; i++) {
        const char *v = getenv(keep[i]);
        if (v && strlen(v) < 900) { snprintf(envbuf[ne], sizeof envbuf[ne], "%s=%s", keep[i], v); envp[ne] = envbuf[ne]; ne++; }
    }
    snprintf(envbuf[ne], sizeof envbuf[ne], "PATH=/usr/bin:/bin");
    envp[ne] = envbuf[ne];
    envp[++ne] = NULL;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return 127;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 127; }
    if (pid == 0) {
        setsid();                                       /* its own process group: cancel stops all of it */
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
        const char *home = getenv("HOME");
        if (home && chdir(home)) {}
        /* its own firewall component; in a LordOS session a command that can't be placed there never runs */
        CgroupLimits lim = { 0 };
        if (cgroup_enter("lordos-net-flathub.scope", &lim) && getenv("LORDOS_SESSIOND")) {
            static const char msg[] = "flatpak: could not enter lordos-net-flathub.scope: not run\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        execve(FPAK_FLATPAK, argv, envp);
        _exit(127);
    }
    close(p[1]);
    pthread_mutex_lock(&mu);
    running_pid = pid;
    pthread_mutex_unlock(&mu);
    char acc[4096], line[LINE_MAX_BYTES + 1];
    size_t have = 0;
    for (;;) {
        ssize_t r = read(p[0], acc + have, sizeof acc - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += (size_t)r;
        size_t start = 0;
        for (size_t i = 0; i < have; i++) {
            if (acc[i] != '\n' && acc[i] != '\r') continue;
            clean(acc + start, i - start, line);
            if (line[0]) { pthread_mutex_lock(&mu); push(j->id, line, 0, 0); pthread_mutex_unlock(&mu); }
            start = i + 1;
        }
        if (start == 0 && have == sizeof acc) start = have;     /* a line longer than the buffer: dropped */
        memmove(acc, acc + start, have - start);
        have -= start;
    }
    if (have) {
        clean(acc, have, line);
        if (line[0]) { pthread_mutex_lock(&mu); push(j->id, line, 0, 0); pthread_mutex_unlock(&mu); }
    }
    close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    pthread_mutex_lock(&mu);
    running_pid = 0;
    pthread_mutex_unlock(&mu);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!nq) pthread_cond_wait(&cv, &mu);
        Job j = queue[0];
        memmove(&queue[0], &queue[1], (size_t)(nq - 1) * sizeof queue[0]);
        nq--;
        running_id = j.id;
        pthread_mutex_unlock(&mu);
        int code = run(&j);
        pthread_mutex_lock(&mu);
        running_id = 0;
        push(j.id, NULL, 1, code);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, native_arch() && binary_ok());
}

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *name = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    const char *ref = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    int cmd = -1;
    for (int i = 0; name && i < FPAK_CMD_COUNT; i++) if (!strcmp(name, names[i])) cmd = i;
    char tmp[2048], *av[FPAK_ARGV_MAX];
    const char *arch = native_arch();
    /* checked now too, so a bad call is refused at once instead of failing in the worker */
    int ok = cmd >= 0 && arch && (!ref || strlen(ref) < FPAK_REF_MAX) && fpak_cmd_argv(cmd, ref, arch, av, tmp, sizeof tmp) >= 0;
    int id = -1;
    if (ok) {
        pthread_mutex_lock(&mu);
        if (nq < QMAX) {
            if (!worker_on) {
                pthread_t th;
                pthread_attr_t a;
                pthread_attr_init(&a);
                pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
                worker_on = pthread_create(&th, &a, worker, NULL) == 0;
                pthread_attr_destroy(&a);
            }
            if (worker_on) {
                Job *j = &queue[nq++];
                j->id = id = next_id++;
                if (next_id > 1 << 30) next_id = 1;
                j->cmd = cmd;
                snprintf(j->ref, sizeof j->ref, "%s", ref ? ref : "");
                pthread_cond_signal(&cv);
            }
        }
        pthread_mutex_unlock(&mu);
    }
    if (name) JS_FreeCString(c, name);
    if (ref) JS_FreeCString(c, ref);
    return JS_NewInt32(c, id);
}

static JSValue f_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nev; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewInt32(c, evs[i].id));
        if (evs[i].done) {
            JS_SetPropertyStr(c, o, "done", JS_TRUE);
            JS_SetPropertyStr(c, o, "code", JS_NewInt32(c, evs[i].code));
        } else JS_SetPropertyStr(c, o, "line", JS_NewString(c, evs[i].line));
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    nev = 0;
    pthread_mutex_unlock(&mu);
    return arr;
}

static JSValue f_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0]) || id <= 0) return JS_FALSE;
    int done = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nq; i++)
        if (queue[i].id == id) {                     /* not started yet: dropped, and said done */
            memmove(&queue[i], &queue[i + 1], (size_t)(nq - i - 1) * sizeof queue[0]);
            nq--;
            push(id, NULL, 1, 130);
            done = 1;
            break;
        }
    if (!done && running_id == id && running_pid > 0) { kill(-running_pid, SIGTERM); done = 1; }
    pthread_mutex_unlock(&mu);
    return JS_NewBool(c, done);
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("start", 2, f_start),
    JS_CFUNC_DEF("take", 0, f_take),
    JS_CFUNC_DEF("cancel", 1, f_cancel),
};

void js_flatpak_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "flatpak", o);
}
