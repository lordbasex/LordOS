/* js_flatpak.c — lord.flatpak: the Store's Flathub management commands (docs/FLATPAK.md, Problem 4; AGENTS.md
 * "Flatpak as an app source", E3). Only the closed list of fpak_cmd_argv, one at a time (Flatpak locks its repository),
 * each in its own child that enters lordos-net-flathub.scope (the firewall component lordos:flathub, a row in
 * Monitor → Network) before it execs /usr/bin/flatpak with a minimal environment, stdin /dev/null and its output on a
 * pipe. JS asks:
 *   lord.flatpak.supported()      → this PC can have Flathub (phase 1: x86-64), installed or not
 *   lord.flatpak.available()      → Flatpak installed (root-owned /usr/bin/flatpak) on a supported PC (phase 1: x86-64)
 *   lord.flatpak.start(cmd, ref)  → a job id (cmd one of the names below; ref only where the command takes one), or -1
 *   lord.flatpak.take()           → [{ id, line } | { id, done: true, code }] since the last call
 *   lord.flatpak.cancel(id)       → stops that job (its whole process group)
 *   lord.flatpak.loadCatalog(lang) → a job id: Flathub's AppStream read by the sandboxed lord-appstream (E7)
 *   lord.flatpak.catalog()        → [{ id, branch, ref, name, summary, icon, categories, license, developer, verified }]
 *                                   of the last load, every line checked again here (fpakas_line)
 *   lord.flatpak.description(id)  → that app's description from the same load (plain text, paragraphs apart by an
 *                                   empty line), checked again here (fpakas_desc_line), or "": kept out of catalog()
 *                                   so the list the Store reads at every load stays small
 * Lines are the command's output, cut to printable UTF-8; nothing in them is acted on here. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "js_flatpak.h"
#include "../fpak.h"
#include "../fpakrun.h"
#include "../fpakas.h"
#include "../sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <ftw.h>

static const char *const names[FPAK_CMD_COUNT] = {
    [FPAK_CMD_REMOTE_ADD] = "remoteAdd", [FPAK_CMD_APPSTREAM] = "appstream", [FPAK_CMD_INFO] = "info",
    [FPAK_CMD_INSTALL] = "install", [FPAK_CMD_UPDATE] = "update", [FPAK_CMD_UNINSTALL] = "uninstall",
    [FPAK_CMD_UNUSED] = "uninstallUnused", [FPAK_CMD_LIST] = "list", [FPAK_CMD_REMOTE_DELETE] = "remoteDelete",
};

#define QMAX 16
#define EMAX 512
#define LINE_MAX_BYTES FPAK_LINE_MAX
#define JOB_CATALOG (-2)
typedef struct { int id, cmd; char ref[FPAK_REF_MAX]; } Job;          /* cmd: an FPAK_CMD_*, or JOB_CATALOG with ref = the language */
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


/* ---- the catalog (E7) ---- */
static FpakasApp *cat;                  /* the last catalog read, under mu */
static char **cat_desc;                 /* its descriptions (cat_desc[i] cat[i]'s, or NULL), asked for one at a time */
static int ncat;
#define DESC_TOTAL (24u << 20)          /* every description together; Flathub's whole catalog is a few MB */
static char cat_dir[PATH_MAX];          /* the appstream folder its icon names are in */

/* the helper next to lord-server, root's (or the user's in development) and not writable by others */
static int appstream_path(char *bin, size_t size) {
    ssize_t n = readlink("/proc/self/exe", bin, size - 20);
    if (n <= 0) return -1;
    bin[n] = 0;
    char *slash = strrchr(bin, '/');
    if (!slash) return -1;
    strcpy(slash + 1, "lord-appstream");
    struct stat st;
    return stat(bin, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022) ? -1 : 0;
}

/* ~/.local/share/flatpak/appstream/flathub/<arch>/active/appstream.xml, opened step by step without following a link
 * but "active" (Flatpak's link to the deployed commit, a 64-hex name next to it); the file the user's own, regular, and
 * at most 256 MB. dir gets the real active folder. → the fd, or -1. */
static int open_appstream(const char *arch, char *dir, size_t dn) {
    const char *h = getenv("HOME");
    char base[PATH_MAX];
    if (!h || h[0] != '/' || snprintf(base, sizeof base, "%s/.local/share/flatpak/appstream/flathub/%s", h, arch) >= (int)sizeof base) return -1;
    int d = open(base, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return -1;
    char link[96];
    ssize_t l = readlinkat(d, "active", link, sizeof link - 1);
    int fd = -1;
    if (l == 64) {
        link[l] = 0;
        int hex = 1;
        for (int i = 0; i < 64 && hex; i++) hex = isxdigit((unsigned char)link[i]) && !isupper((unsigned char)link[i]);
        int a = hex ? openat(d, link, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
        if (a >= 0) {
            fd = openat(a, "appstream.xml", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            struct stat st;
            if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size <= 0 || st.st_size > (256L << 20))) {
                close(fd);
                fd = -1;
            }
            if (fd >= 0 && snprintf(dir, dn, "%s/%s", base, link) >= (int)dn) { close(fd); fd = -1; }
            close(a);
        }
    }
    close(d);
    return fd;
}

static int load_catalog(const char *lang) {
    const char *arch = fpak_native_arch();
    char bin[PATH_MAX], dir[PATH_MAX];
    if (!arch || appstream_path(bin, sizeof bin)) return 127;
    int xml = open_appstream(arch, dir, sizeof dir);
    if (xml < 0) return 2;
    int mem = memfd_create("lordos-appstream-index", MFD_CLOEXEC);
    if (mem < 0) { close(xml); return 127; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(xml, 0);
        dup2(mem, 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        char l[17], a[17];
        snprintf(l, sizeof l, "%s", lang);
        snprintf(a, sizeof a, "%s", arch);
        char *argv[] = { "lord-appstream", l, a, NULL };
        char *envp[] = { NULL };
        execve(bin, argv, envp);
        _exit(127);
    }
    close(xml);
    int st = 0;
    if (pid > 0) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int code = pid < 0 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    off_t size = lseek(mem, 0, SEEK_END);
    if (code || size <= 0 || size > (64L << 20)) { close(mem); return code ? code : 3; }
    char *buf = malloc((size_t)size);
    size_t have = 0;
    lseek(mem, 0, SEEK_SET);
    while (buf && have < (size_t)size) {
        ssize_t r = read(mem, buf + have, (size_t)size - have);
        if (r <= 0) break;
        have += (size_t)r;
    }
    close(mem);
    if (!buf) return 127;
    int cap = 0, n = 0, last_ok = 0;
    size_t desc_total = 0;
    FpakasApp *list = NULL;
    char **descs = NULL;
    static char text[FPAKAS_DESC_MAX + 1];             /* only this worker thread loads */
    for (size_t at = 0; at < have;) {
        char *nl = memchr(buf + at, '\n', have - at);
        size_t len = (size_t)((nl ? nl + 1 : buf + have) - (buf + at));
        FpakasApp a;
        char id[64];
        if (len > 3 && !memcmp(buf + at, "#d\t", 3)) {
            /* a description: only right after its own app's line, and once; all of them kept to DESC_TOTAL bytes */
            int dl = last_ok ? fpakas_desc_line(buf + at, len, id, text) : -1;
            if (dl > 0 && !strcmp(id, list[n - 1].id) && !descs[n - 1] && desc_total + (size_t)dl <= DESC_TOTAL) {
                descs[n - 1] = strdup(text);
                if (descs[n - 1]) desc_total += (size_t)dl + 1;
            }
            last_ok = 0;
        } else if (fpakas_line(buf + at, len, &a) == 0 && n < 20000) {
            if (n == cap) {
                int nc = cap ? cap * 2 : 1024;
                FpakasApp *g = realloc(list, (size_t)nc * sizeof *g);
                if (!g) break;
                list = g;
                char **gd = realloc(descs, (size_t)nc * sizeof *gd);
                if (!gd) break;
                descs = gd;
                memset(descs + cap, 0, (size_t)(nc - cap) * sizeof *gd);
                cap = nc;
            }
            descs[n] = NULL;
            list[n++] = a;
            last_ok = 1;
        } else last_ok = 0;
        at += len;
    }
    free(buf);
    pthread_mutex_lock(&mu);
    for (int i = 0; i < ncat; i++) free(cat_desc[i]);
    free(cat_desc);
    free(cat);
    cat = list;
    cat_desc = descs;
    ncat = n;
    snprintf(cat_dir, sizeof cat_dir, "%s", dir);
    pthread_mutex_unlock(&mu);
    return 0;
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
        int code = j.cmd == JOB_CATALOG ? load_catalog(j.ref) : fpak_run(j.cmd, j.ref, job_line, &j.id, job_pid);
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

/* lord.flatpak.forget() → true when done: once the flatpak package is gone (Settings → Flatpak, "Remove Flatpak
 * completely"), Flatpak's own folders of this user, ~/.local/share/flatpak and ~/.cache/flatpak, which lord.fs may not
 * touch (fsops_protected): what's left of the catalog and the repository. Refused while flatpak is installed. Never
 * follows a link (a link in their place is removed as a link) nor crosses into another filesystem. */
static int forget_cb(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)f;
    return flag == FTW_DP ? rmdir(p) : unlink(p);
}
static int forget_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    return nftw(p, forget_cb, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}
static JSValue f_forget(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct stat st;
    const char *home = getenv("HOME");
    if (!lstat(FPAK_FLATPAK, &st) || !home || home[0] != '/' || strstr(home, "/../") || strlen(home) > PATH_MAX - 64) return JS_FALSE;
    char p[PATH_MAX];
    int bad = 0;
    snprintf(p, sizeof p, "%s/.local/share/flatpak", home);
    bad |= forget_tree(p);
    snprintf(p, sizeof p, "%s/.cache/flatpak", home);
    bad |= forget_tree(p);
    return JS_NewBool(c, !bad);
}

/* this machine can have Flathub at all (phase 1: x86-64 only), whether or not flatpak is installed yet */
static JSValue f_supported(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, fpak_native_arch() != NULL);
}

static int enqueue(int cmd, const char *ref);

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
    int id = ok ? enqueue(cmd, ref) : -1;
    if (name) JS_FreeCString(c, name);
    if (ref) JS_FreeCString(c, ref);
    return JS_NewInt32(c, id);
}


/* queues one job (under nothing); -1 if the queue is full or there is no worker */
static int enqueue(int cmd, const char *ref) {
    int id = -1;
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
    return id;
}

static JSValue f_load_catalog(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *lang = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    int ok = lang && fpak_native_arch() && strlen(lang) >= 2 && strlen(lang) <= 16;
    for (const char *q = lang; ok && *q; q++) ok = isalnum((unsigned char)*q) || *q == '-' || *q == '_';
    int id = ok ? enqueue(JOB_CATALOG, lang) : -1;
    if (lang) JS_FreeCString(c, lang);
    return JS_NewInt32(c, id);
}

static JSValue f_catalog(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    const char *arch = fpak_native_arch();
    pthread_mutex_lock(&mu);
    for (int i = 0; i < ncat && arch; i++) {
        const FpakasApp *a = &cat[i];
        char ref[FPAK_REF_MAX], icon[PATH_MAX + 200] = "";
        snprintf(ref, sizeof ref, "app/%s/%s/%s", a->id, arch, a->branch);
        if (a->icon[0]) snprintf(icon, sizeof icon, "%s/icons/%s", cat_dir, a->icon);
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewString(c, a->id));
        JS_SetPropertyStr(c, o, "branch", JS_NewString(c, a->branch));
        JS_SetPropertyStr(c, o, "ref", JS_NewString(c, ref));
        JS_SetPropertyStr(c, o, "name", JS_NewString(c, a->name));
        JS_SetPropertyStr(c, o, "summary", JS_NewString(c, a->summary));
        JS_SetPropertyStr(c, o, "icon", JS_NewString(c, icon));
        JS_SetPropertyStr(c, o, "categories", JS_NewString(c, a->categories));
        JS_SetPropertyStr(c, o, "license", JS_NewString(c, a->license));
        JS_SetPropertyStr(c, o, "developer", JS_NewString(c, a->developer));
        JS_SetPropertyStr(c, o, "verified", JS_NewBool(c, a->verified));
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    pthread_mutex_unlock(&mu);
    return arr;
}

/* the description of one app of the last catalog (checked when it was loaded, fpakas_desc_line), or "" */
static JSValue f_description(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    JSValue r = JS_UNDEFINED;
    if (id && strlen(id) < 64 && fpak_id_ok(id)) {
        pthread_mutex_lock(&mu);
        for (int i = 0; i < ncat; i++)
            if (!strcmp(cat[i].id, id)) { if (cat_desc && cat_desc[i]) r = JS_NewString(c, cat_desc[i]); break; }
        pthread_mutex_unlock(&mu);
    }
    if (id) JS_FreeCString(c, id);
    return JS_IsUndefined(r) ? JS_NewString(c, "") : r;
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
    JS_CFUNC_DEF("forget", 0, f_forget),
    JS_CFUNC_DEF("supported", 0, f_supported),
    JS_CFUNC_DEF("start", 2, f_start),
    JS_CFUNC_DEF("take", 0, f_take),
    JS_CFUNC_DEF("cancel", 1, f_cancel),
    JS_CFUNC_DEF("loadCatalog", 1, f_load_catalog),
    JS_CFUNC_DEF("catalog", 0, f_catalog),
    JS_CFUNC_DEF("description", 1, f_description),
};

void js_flatpak_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "flatpak", o);
}
