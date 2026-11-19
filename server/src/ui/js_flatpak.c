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
#include "../fpakrun.h"
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
#define LINE_MAX_BYTES FPAK_LINE_MAX
typedef struct { int id, cmd; char ref[FPAK_REF_MAX]; } Job;
typedef struct { int id, done, code; char line[LINE_MAX_BYTES + 1]; } Ev;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Job queue[QMAX];
static int nq, next_id = 1, worker_on, running_id;
static pid_t running_pid;
static Ev evs[EMAX];
static int nev;

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

static void job_line(void *ud, const char *line) {
    pthread_mutex_lock(&mu);
    push(*(int *)ud, line, 0, 0);
    pthread_mutex_unlock(&mu);
}
static void job_pid(void *ud, pid_t pid) {
    (void)ud;
    pthread_mutex_lock(&mu);
    running_pid = pid;
    pthread_mutex_unlock(&mu);
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
        int code = fpak_run(j.cmd, j.ref, job_line, &j.id, job_pid);
        pthread_mutex_lock(&mu);
        running_id = 0;
        push(j.id, NULL, 1, code);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, fpak_native_arch() && fpak_binary_ok());
}

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *name = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    const char *ref = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    int cmd = -1;
    for (int i = 0; name && i < FPAK_CMD_COUNT; i++) if (!strcmp(name, names[i])) cmd = i;
    char tmp[2048], *av[FPAK_ARGV_MAX];
    const char *arch = fpak_native_arch();
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
