/* machines.c — the `machines` command: LordOS's Machines from the terminal (docs/VIRTUALIZATION.md "From the
 * terminal"; approved by the maintainer 2026-11-21). Installed as /usr/lib/lordos/bin/machines, /usr/bin/machines a
 * link to it.
 *
 *   machines list [--json]                     machines status NAME [--json]
 *   machines start NAME [--no-window]          machines open NAME
 *   machines stop|poweroff|pause|resume|suspend|reset NAME
 *   machines snapshot NAME list [--json] | take LABEL | revert TAG | delete TAG
 *   machines clone NAME NEW_NAME [--duplicate] [--keep-identity]
 *   machines rename NAME NEW_NAME
 *   machines set NAME [--memory 8G] [--cpus 6] [--disk 64G] [--clipboard both] [--network shared] ...
 *   machines help
 *
 * The library is the Machines app's: ~/Machines, each machine a <name>.lordvm folder (ui/js_vm.c library()). NAME is a
 * machine's name (ASCII case ignored, a unique start is enough), its id (machine.json "id") or a machine folder's path. Everything but start and open
 * runs lord-vm (the installed one beside this program) on the folder, as the person, exactly as the app does; nothing
 * here needs root, and no admin action is ever asked from here.
 *
 * start and open go through the person's running LordOS session (vm/vmcli.h: $XDG_RUNTIME_DIR/lordos-machines.sock,
 * same uid only, a closed set of requests), where the Machines app does what its own buttons do: the window, the bridged
 * network's and the graphics card's admin steps, vm-memlock. Without a session, `start --no-window` runs lord-vm --start
 * --detach itself, only for a machine that needs no admin step (not bridged, no graphics card), and says why otherwise.
 *
 * Messages come from assets/locales/<lang>/machinescli.json, by LC_ALL, LC_MESSAGES
 * or LANG, English for anything missing. lord-vm's own reasons are English. Exit status: vm/vmcli.h VMCLI_*. --json is
 * stable: keys are only ever added. */
#include "mcli.h"
#include "../sec/json.h"
#include "../vm/machine.h"
#include "../vm/vmgroup.h"
#include "../vm/vmcli.h"
#include "../web/weblangid.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_MACHINES 256
#define START_WAIT_S 120            /* a start in its window: how long the command waits to say "running" */

/* ---- texts ---- */

static JDoc cat_lang, cat_en;
static char self_dir[PATH_MAX];

static int load_json(const char *path, JDoc *d) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (4 << 20)) { close(fd); return -1; }
    char *b = malloc((size_t)st.st_size);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    int bad = r != st.st_size || json_parse(b, (size_t)r, d, NULL, 0) || !d->root || d->root->type != J_OBJ;
    free(b);
    if (bad) { json_free(d); memset(d, 0, sizeof *d); return -1; }
    return 0;
}

static void load_catalog(const char *lang, const char *ns, JDoc *d) {
    /* installed: /usr/lib/lordos/bin → /usr/lib/lordos/assets; built: server/build → assets */
    const char *tries[] = { "%s/../assets/locales/%s/%s.json", "%s/../../assets/locales/%s/%s.json" };
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        char p[PATH_MAX + 96];
        snprintf(p, sizeof p, tries[i], self_dir, lang, ns);
        if (!load_json(p, d)) return;
    }
}

static void texts_init(void) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    char lang[WEBLANG_ID_MAX];
    weblang_id(l, lang);
    load_catalog("en", "machinescli", &cat_en);
    if (strcmp(lang, "en")) load_catalog(lang, "machinescli", &cat_lang);
}

static const char *raw(const JDoc *a, const JDoc *b, const char *key) {
    const char *s = a->root ? json_str(a->root, key) : NULL;
    if (!s || !s[0]) s = b->root ? json_str(b->root, key) : NULL;
    return s && s[0] ? s : key;
}
static const char *T(const char *key) { return raw(&cat_lang, &cat_en, key); }
/* a state's name as Machines shows it (machinescli.state.<view>, the same words as machines.state.<view>) */
static const char *state_text(const char *view) {
    static char k[48];
    snprintf(k, sizeof k, "state.%s", view);
    return T(k);
}

/* key's text with placeholders ("name", value, ..., NULL) */
static const char *say(const char *key, ...) {
    static char out[4][2048];
    static int slot;
    const char *kv[17];
    int n = 0;
    va_list ap;
    va_start(ap, key);
    while (n < 16) {
        const char *k = va_arg(ap, const char *);
        if (!k) break;
        kv[n++] = k;
        kv[n++] = va_arg(ap, const char *);
    }
    va_end(ap);
    kv[n] = NULL;
    char *o = out[slot++ & 3];
    mcli_fill(T(key), kv, o, sizeof out[0]);
    return o;
}
#define SAY(...)  (puts(say(__VA_ARGS__)))
#define ERR(...)  (fprintf(stderr, "machines: %s\n", say(__VA_ARGS__)))

/* ---- the library and its machines ---- */

typedef struct {
    char dir[PATH_MAX + 260], folder[256];
    int ok;                          /* machine.json read */
    VmMachine m;
    char system[48];
    McliState st;
    double used;                     /* bytes its disks take */
    int group;                       /* a machine group's member: index in gs + 1 (0: none) */
} Machine;

/* the library's machine groups (docs/VIRTUALIZATION.md 19.9): <name>.lordgroup with its group.json */
#define MAX_GROUPS 32
typedef struct { char dir[PATH_MAX + 260]; VmGroup g; int member[VMG_MEMBERS_MAX]; } Group;   /* member: index in ms, -1 */
static Group gs[MAX_GROUPS];
static int ngs;

static char library[PATH_MAX];
static Machine *ms;
static int nms;

static int library_path(void) {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return -1;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/Machines", home);
    if (!realpath(p, library)) { snprintf(library, sizeof library, "%s", p); return -1; }
    return 0;
}

static ssize_t read_small(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r >= 0) buf[r] = 0;
    return r;
}

/* whether pid is a lord-vm process (its comm) */
static int lordvm_alive(long pid) {
    if (pid <= 0) return 0;
    char p[64], comm[32];
    snprintf(p, sizeof p, "/proc/%ld/comm", pid);
    ssize_t k = read_small(p, comm, sizeof comm);
    return k > 0 && !strncmp(comm, "lord-vm", 7);
}

static void read_state(Machine *x) {
    char p[PATH_MAX + 300], buf[4096];
    snprintf(p, sizeof p, "%s/run.json", x->dir);
    ssize_t n = read_small(p, buf, sizeof buf);
    if (n < 0) n = 0;
    mcli_state(buf, (size_t)n, lordvm_alive(mcli_run_pid(buf, (size_t)n)), &x->st);
}

static void read_machine(Machine *x) {
    char err[256];
    x->ok = vm_machine_load(x->dir, &x->m, err, sizeof err) == VM_OK;
    if (!x->ok) snprintf(x->m.name, sizeof x->m.name, "%.64s", x->folder);
    /* the system the app noted (app.json osName, else os) */
    char p[PATH_MAX + 300], buf[16384];
    snprintf(p, sizeof p, "%s/app.json", x->dir);
    ssize_t n = read_small(p, buf, sizeof buf);
    JDoc d;
    if (n > 0 && !json_parse(buf, (size_t)n, &d, NULL, 0)) {
        const char *s = json_str(d.root, "osName");
        if (!s || !s[0]) s = json_str(d.root, "os");
        if (s) snprintf(x->system, sizeof x->system, "%.40s", s);
        json_free(&d);
    }
    for (char *c = x->system; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    /* what its disks take */
    snprintf(p, sizeof p, "%s/disks", x->dir);
    DIR *dd = opendir(p);
    struct dirent *e;
    while (dd && (e = readdir(dd))) {
        struct stat st;
        if (e->d_name[0] == '.' || fstatat(dirfd(dd), e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
        x->used += (double)st.st_blocks * 512;
    }
    if (dd) closedir(dd);
    read_state(x);
}

static int by_name(const void *a, const void *b) { return strcasecmp(((const Machine *)a)->m.name, ((const Machine *)b)->m.name); }

/* a group's members as machines of the list (they're machines, each with its own folder) */
static void load_group(const char *name) {
    if (ngs == MAX_GROUPS) return;
    Group *G = &gs[ngs];
    memset(G, 0, sizeof *G);
    snprintf(G->dir, sizeof G->dir, "%s/%s", library, name);
    struct stat st;
    if (lstat(G->dir, &st) || !S_ISDIR(st.st_mode) || !vm_group_dir_ok(G->dir) || vm_group_load(G->dir, &G->g, NULL, 0)) return;
    for (int i = 0; i < G->g.n && nms < MAX_MACHINES; i++) {
        Machine *x = &ms[nms];
        memset(x, 0, sizeof *x);
        snprintf(x->dir, sizeof x->dir, "%s/%s", G->dir, G->g.m[i].dir);
        char mj[PATH_MAX + 600];
        snprintf(mj, sizeof mj, "%s/machine.json", x->dir);
        if (lstat(x->dir, &st) || !S_ISDIR(st.st_mode) || access(mj, F_OK)) continue;
        snprintf(x->folder, sizeof x->folder, "%.*s", (int)(strlen(G->g.m[i].dir) - 7), G->g.m[i].dir);
        read_machine(x);
        x->group = ngs + 1;
        nms++;
    }
    ngs++;
}

/* after sorting: each group's members by role, -1 when missing or not its own (a damaged group) */
static void index_groups(void) {
    for (int k = 0; k < ngs; k++) {
        for (int i = 0; i < VMG_MEMBERS_MAX; i++) gs[k].member[i] = -1;
        for (int i = 0; i < nms; i++) {
            if (ms[i].group != k + 1) continue;
            for (int j = 0; j < gs[k].g.n; j++) {
                char d[PATH_MAX + 520];
                snprintf(d, sizeof d, "%.4000s/%.200s", gs[k].dir, gs[k].g.m[j].dir);
                if (!strcmp(d, ms[i].dir) && ms[i].ok && !strcmp(ms[i].m.group, gs[k].g.id) && ms[i].m.role == gs[k].g.m[j].role) gs[k].member[j] = i;
            }
        }
    }
}

/* the library's machines (js_vm.c f_list: <name>.lordvm folders with a machine.json) */
static void load_library(void) {
    if (ms) return;
    ms = calloc(MAX_MACHINES, sizeof *ms);
    if (!ms || library_path()) return;
    DIR *d = opendir(library);
    struct dirent *e;
    while (d && (e = readdir(d)) && nms < MAX_MACHINES) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] != '.' && l > 10 && !strcmp(e->d_name + l - 10, VMG_SUFFIX)) { load_group(e->d_name); continue; }
        if (e->d_name[0] == '.' || l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
        Machine *x = &ms[nms];
        memset(x, 0, sizeof *x);
        snprintf(x->dir, sizeof x->dir, "%s/%s", library, e->d_name);
        struct stat st;
        char mj[PATH_MAX + 300];
        snprintf(mj, sizeof mj, "%s/machine.json", x->dir);
        if (lstat(x->dir, &st) || !S_ISDIR(st.st_mode) || access(mj, F_OK)) continue;
        snprintf(x->folder, sizeof x->folder, "%.*s", (int)(l - 7), e->d_name);
        read_machine(x);
        nms++;
    }
    if (d) closedir(d);
    qsort(ms, (size_t)nms, sizeof *ms, by_name);
    index_groups();
}

static void json_group(FILE *f, const Group *G);
/* NAME → a group: its folder's path, or its name exactly (any case); NULL when it isn't one */
static Group *find_group(const char *q) {
    load_library();
    char real[PATH_MAX];
    int path = strchr(q, '/') != NULL;
    if (path && !realpath(q, real)) return NULL;
    for (int k = 0; k < ngs; k++) if (path ? !strcmp(gs[k].dir, real) : !strcasecmp(gs[k].g.name, q)) return &gs[k];
    return NULL;
}

/* one word for a group: running (all), off (none), partial, damaged (a member missing or not its own) */
static const char *group_view(const Group *G) {
    int on = 0, run = 0;
    for (int j = 0; j < G->g.n; j++) {
        if (G->member[j] < 0) return "damaged";
        const Machine *x = &ms[G->member[j]];
        on += x->st.active;
        run += !strcmp(x->st.state, "running") || !strcmp(x->st.state, "paused");
    }
    return !on ? "off" : run == G->g.n ? "running" : "partial";
}

/* NAME → a machine: a name in the library, or a machine folder's path. NULL after saying why (exit status in *rc) */
static Machine one;
static Machine *find(const char *q, int *rc) {
    *rc = VMCLI_NO_MACHINE;
    if (strchr(q, '/') || (strlen(q) > 7 && !strcmp(q + strlen(q) - 7, ".lordvm"))) {
        char real[PATH_MAX], mj[PATH_MAX + 300];
        struct stat st;
        if (realpath(q, real) && !stat(real, &st) && S_ISDIR(st.st_mode)) {
            snprintf(mj, sizeof mj, "%s/machine.json", real);
            if (!access(mj, F_OK)) {
                load_library();
                for (int i = 0; i < nms; i++) if (!strcmp(ms[i].dir, real)) return &ms[i];
                memset(&one, 0, sizeof one);
                snprintf(one.dir, sizeof one.dir, "%s", real);
                const char *b = strrchr(real, '/') + 1;
                size_t bl = strlen(b);
                snprintf(one.folder, sizeof one.folder, "%.*s", (int)(bl > 7 && !strcmp(b + bl - 7, ".lordvm") ? bl - 7 : bl), b);
                read_machine(&one);
                return &one;
            }
        }
        ERR("noMachine", "name", q, NULL);
        return NULL;
    }
    load_library();
    /* its id: the one name that never changes (a Duplicate shares it: then more than one) */
    if (vm_uuid_valid(q)) {
        int k = 0, at = -1;
        for (int i = 0; i < nms; i++) if (ms[i].ok && !strcmp(ms[i].m.id, q)) { k++; at = i; }
        if (k == 1) return &ms[at];
    }
    const char *names[MAX_MACHINES];
    for (int i = 0; i < nms; i++) names[i] = ms[i].m.name;
    int idx[MAX_MACHINES];
    int k = mcli_resolve(names, nms, q, idx, MAX_MACHINES);
    if (k == 1) return &ms[idx[0]];
    if (!k) { ERR("noMachine", "name", q, NULL); return NULL; }
    char list[1024] = "";
    size_t o = 0;
    for (int i = 0; i < k && i < MAX_MACHINES && o < sizeof list - 1; i++) {
        int w = snprintf(list + o, sizeof list - o, "%s%s", i ? ", " : "", ms[idx[i]].m.name);
        if (w > 0) o += (size_t)w;
    }
    ERR("ambiguous", "name", q, "list", list, NULL);
    return NULL;
}

/* ---- lord-vm ---- */

static char lordvm[PATH_MAX + 300];

/* the lord-vm beside this program: a regular file of root's or the person's, nobody else may write it */
static int lordvm_path(void) {
    snprintf(lordvm, sizeof lordvm, "%s/lord-vm", self_dir);
    struct stat st;
    return stat(lordvm, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022) ? -1 : 0;
}

typedef struct { char out[65536], err[8192]; size_t no, ne; int status; } Run;

/* lord-vm with args (args[0] is ignored), its output collected; its exit status, or -1 when it couldn't run */
static int run_lordvm(char **args, Run *r) {
    memset(r, 0, sizeof *r);
    r->status = -1;
    if (lordvm_path()) { ERR("noLordVm", NULL); return -1; }
    int po[2], pe[2];
    if (pipe2(po, O_CLOEXEC)) return -1;
    if (pipe2(pe, O_CLOEXEC)) { close(po[0]); close(po[1]); return -1; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    args[0] = "lord-vm";
    pid_t pid;
    int e = posix_spawn(&pid, lordvm, &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(po[1]); close(pe[1]);
    if (e) { close(po[0]); close(pe[0]); return -1; }
    struct pollfd pf[2] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 } };
    int open_n = 2;
    while (open_n) {
        if (poll(pf, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; i++) {
            if (pf[i].fd < 0 || !pf[i].revents) continue;
            char tmp[4096];
            ssize_t k = read(pf[i].fd, tmp, sizeof tmp);
            if (k <= 0) { close(pf[i].fd); pf[i].fd = -1; open_n--; continue; }
            char *dst = i ? r->err : r->out;
            size_t *n = i ? &r->ne : &r->no, cap = i ? sizeof r->err : sizeof r->out;
            size_t c = (size_t)k < cap - 1 - *n ? (size_t)k : cap - 1 - *n;
            memcpy(dst + *n, tmp, c);
            *n += c;
            dst[*n] = 0;
        }
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return r->status;
}

/* lord-vm's own last reason ("lord-vm: …" on stderr), else its last line */
static const char *why_of(const Run *r) {
    static char w[600];
    w[0] = 0;
    const char *s = r->err;
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l) {
            const char *t = s;
            size_t tl = l;
            if (tl > 9 && !strncmp(t, "lord-vm: ", 9)) { t += 9; tl -= 9; }
            snprintf(w, sizeof w, "%.*s", (int)(tl < sizeof w - 1 ? tl : sizeof w - 1), t);
        }
        s = e ? e + 1 : s + l;
    }
    if (!w[0]) snprintf(w, sizeof w, "lord-vm exited with %d", r->status);
    return w;
}

/* lord-vm's exit status as ours: 0, 2 (a value it refused), 3 (not running), else 1 */
static int our_rc(int st) { return st == 0 ? VMCLI_OK : st == 2 ? VMCLI_USAGE : st == 3 ? VMCLI_NOT_RUNNING : VMCLI_FAILED; }

/* ---- the session ---- */

/* one request to the person's lord-server; its answer's code (text in text), or -1 when there's no session */
static int session_ask(int verb, const char *dir, char *text, size_t tn, int wait_s) {
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char def[64];
    if (!rt || rt[0] != '/') { snprintf(def, sizeof def, "/run/user/%u", (unsigned)getuid()); rt = def; }
    if (snprintf(path, sizeof path, "%s/%s", rt, VMCLI_SOCK) >= (int)sizeof path) return -1;
    struct stat st;
    if (lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, strlen(path) + 1);
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (connect(s, (struct sockaddr *)&a, sizeof a) || getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != getuid()) { close(s); return -1; }
    char line[VMCLI_LINE_MAX + 1];
    size_t n = vmcli_req_format(verb, dir, line, sizeof line);
    if (!n || send(s, line, n, MSG_NOSIGNAL) != (ssize_t)n) { close(s); return -1; }
    char ans[VMCLI_ANSWER_MAX + 2];
    size_t got = 0;
    long long end = (long long)time(NULL) + wait_s;
    while (got < sizeof ans - 1) {
        struct pollfd pf = { s, POLLIN, 0 };
        long long left = end - (long long)time(NULL);
        if (left <= 0) break;
        int pr = poll(&pf, 1, (int)(left * 1000));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        ssize_t k = recv(s, ans + got, sizeof ans - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
        if (memchr(ans, '\n', got)) break;
    }
    close(s);
    int code;
    if (!got || vmcli_answer_parse(ans, got, &code, text, tn)) return -1;
    return code;
}

/* ---- the commands ---- */

static const char *gfx_of(const Machine *x) {
    if (!x->ok) return "";
    if (x->m.vgpu) return VM_VGPU_NAMES[x->m.vgpu];
    return x->m.graphics == VM_GFX_3D ? "3d" : "2d";
}

static void json_machine(FILE *f, const Machine *x, int full) {
    fprintf(f, "{\"name\":");
    mcli_json_str(f, x->m.name);
    fprintf(f, ",\"folder\":");
    mcli_json_str(f, x->dir);
    fprintf(f, ",\"id\":");
    mcli_json_str(f, x->ok ? x->m.id : "");
    fprintf(f, ",\"valid\":%s,\"system\":", x->ok ? "true" : "false");
    mcli_json_str(f, x->system);
    fprintf(f, ",\"state\":\"%s\",\"reason\":\"%s\",\"view\":\"%s\"", x->st.state, x->st.reason, x->st.view);
    if (x->ok) {
        fprintf(f, ",\"arch\":\"%s\",\"vcpus\":%d,\"memory_mb\":%d,\"disk_gb\":%d,\"disk_used_bytes\":%.0f,\"graphics\":\"%s\",\"gpu\":\"%s\",\"network\":\"%s\"",
                vm_arch_name(x->m.arch), x->m.vcpus, x->m.memory_mb, x->m.disk_gb, x->used, x->m.graphics == VM_GFX_3D ? "3d" : "2d",
                VM_VGPU_NAMES[x->m.vgpu], VM_NET_NAMES[x->m.network]);
    }
    if (full) {
        fprintf(f, ",\"since\":");
        mcli_json_str(f, x->st.since);
        fprintf(f, ",\"detail\":");
        mcli_json_str(f, x->st.detail);
        fprintf(f, ",\"qemu_pid\":%d", x->st.qemu_pid);
    }
    fprintf(f, "}");
}

/* display width of UTF-8 text: two columns for East Asian wide characters, one for the others */
static int width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned cp = *p, len = 1;
        if (cp >= 0xf0 && p[1] && p[2] && p[3]) { cp = (cp & 7) << 18 | (p[1] & 63u) << 12 | (p[2] & 63u) << 6 | (p[3] & 63u); len = 4; }
        else if (cp >= 0xe0 && p[1] && p[2]) { cp = (cp & 15) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); len = 3; }
        else if (cp >= 0xc0 && p[1]) { cp = (cp & 31) << 6 | (p[1] & 63u); len = 2; }
        p += len;
        w += (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
             (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x20000 && cp <= 0x3fffd) ? 2 : 1;
    }
    return w;
}
static void cell(const char *s, int w, int last) {
    fputs(s, stdout);
    if (!last) for (int i = width(s); i < w + 2; i++) fputc(' ', stdout);
}

static void mem_text(int mb, char *out, size_t n) {
    if (mb >= 1024 && mb % 1024 == 0) snprintf(out, n, "%d GB", mb / 1024);
    else if (mb >= 1024) snprintf(out, n, "%.1f GB", mb / 1024.0);
    else snprintf(out, n, "%d MB", mb);
}

static int cmd_list(int json, int names) {
    load_library();
    if (names) { for (int k = 0; k < ngs; k++) puts(gs[k].g.name); for (int i = 0; i < nms; i++) puts(ms[i].m.name); return 0; }
    if (json) {
        printf("{\"library\":");
        mcli_json_str(stdout, library);
        printf(",\"machines\":[");
        for (int i = 0; i < nms; i++) { if (i) putchar(','); json_machine(stdout, &ms[i], 0); }
        printf("],\"groups\":[");
        for (int k = 0; k < ngs; k++) { if (k) putchar(','); json_group(stdout, &gs[k]); }
        printf("]}\n");
        return 0;
    }
    if (!nms) { SAY("empty", "library", library, NULL); return 0; }
    enum { C = 7 };
    const char *head[C] = { T("label.name"), T("label.system"), T("label.state"), T("label.cpus"), T("label.memory"), T("label.used"), T("label.gpu") };
    int w[C];
    for (int c = 0; c < C; c++) w[c] = width(head[c]);
    int rows = nms + ngs;
    char (*cells)[C][96] = calloc((size_t)rows + 1, sizeof *cells);
    if (!cells) return 1;
    for (int k = 0; k < ngs; k++) {
        /* a group's row: its name, "group of N", its state in one word, the members' vCPUs and memory together */
        char (*r)[96] = cells[nms + k], n[16];
        snprintf(n, sizeof n, "%d", gs[k].g.n);
        snprintf(r[0], 96, "%s", gs[k].g.name);
        snprintf(r[1], 96, "%s", say("kind.group", "count", n, NULL));
        const char *v = group_view(&gs[k]);
        snprintf(r[2], 96, "%s", !strcmp(v, "damaged") ? "—" : !strcmp(v, "partial") ? state_text("starting") : state_text(!strcmp(v, "running") ? "running" : "stopped"));
        int cpus = 0, mem = 0;
        double used = 0;
        for (int j = 0; j < gs[k].g.n; j++) if (gs[k].member[j] >= 0) { cpus += ms[gs[k].member[j]].m.vcpus; mem += ms[gs[k].member[j]].m.memory_mb; used += ms[gs[k].member[j]].used; }
        snprintf(r[3], 96, "%d", cpus);
        mem_text(mem, r[4], 96);
        mcli_bytes(used, r[5], 96);
        snprintf(r[6], 96, "—");
        for (int c = 0; c < C; c++) if (width(r[c]) > w[c]) w[c] = width(r[c]);
    }
    for (int i = 0; i < nms; i++) {
        Machine *x = &ms[i];
        snprintf(cells[i][0], 96, "%s", x->m.name);
        snprintf(cells[i][1], 96, "%s", x->system[0] ? x->system : "—");
        snprintf(cells[i][2], 96, "%s", state_text(x->st.view));
        if (x->ok) {
            snprintf(cells[i][3], 96, "%d", x->m.vcpus);
            mem_text(x->m.memory_mb, cells[i][4], 96);
        } else { snprintf(cells[i][3], 96, "—"); snprintf(cells[i][4], 96, "—"); }
        mcli_bytes(x->used, cells[i][5], 96);
        snprintf(cells[i][6], 96, "%s", x->ok ? gfx_of(x) : "—");
        for (int c = 0; c < C; c++) if (width(cells[i][c]) > w[c]) w[c] = width(cells[i][c]);
    }
    for (int c = 0; c < C; c++) cell(head[c], w[c], c == C - 1);
    putchar('\n');
    for (int k = 0; k < ngs; k++) { for (int c = 0; c < C; c++) cell(cells[nms + k][c], w[c], c == C - 1); putchar('\n'); }
    for (int i = 0; i < nms; i++) { for (int c = 0; c < C; c++) cell(cells[i][c], w[c], c == C - 1); putchar('\n'); }
    free(cells);
    return 0;
}

static int cmd_status(Machine *x, int json) {
    /* a running machine: its lord-vm says the state itself */
    if (x->st.active) {
        Run r;
        char *a[] = { NULL, "--status", x->dir, "--json", NULL };
        JDoc d;
        if (run_lordvm(a, &r) == 0 && !json_parse(r.out, r.no, &d, NULL, 0)) {
            const char *s = json_str(d.root, "state"), *re = json_str(d.root, "reason"), *de = json_str(d.root, "detail");
            char fake[1024];
            int k = snprintf(fake, sizeof fake, "{\"state\":\"%.15s\",\"reason\":\"%.23s\",\"pid\":%ld}", s ? s : "", re ? re : "", x->st.pid);
            McliState ns;
            mcli_state(fake, (size_t)k, 1, &ns);
            ns.qemu_pid = (int)json_num(d.root, "qemu_pid", 0);
            snprintf(ns.since, sizeof ns.since, "%s", x->st.since);
            snprintf(ns.detail, sizeof ns.detail, "%s", de ? de : "");
            ns.pid = x->st.pid;
            x->st = ns;
            json_free(&d);
        }
    }
    if (json) {
        json_machine(stdout, x, 1);
        printf("\n");
        return 0;
    }
    char mem[32], used[32];
    mem_text(x->ok ? x->m.memory_mb : 0, mem, sizeof mem);
    mcli_bytes(x->used, used, sizeof used);
    printf("%s\n", x->m.name);
    printf("  %s: %s\n", T("label.state"), state_text(x->st.view));
    if (x->system[0]) printf("  %s: %s\n", T("label.system"), x->system);
    if (x->ok) {
        printf("  %s: %d\n", T("label.cpus"), x->m.vcpus);
        printf("  %s: %s\n", T("label.memory"), mem);
        printf("  %s: %d GB\n", T("label.disk"), x->m.disk_gb);
    }
    printf("  %s: %s\n", T("label.used"), used);
    if (x->ok) {
        printf("  %s: %s\n", T("label.gpu"), gfx_of(x));
        printf("  %s: %s%s%s\n", T("label.network"), VM_NET_NAMES[x->m.network], x->m.network == VM_NET_BRIDGED && x->m.bridge[0] ? " " : "", x->m.network == VM_NET_BRIDGED ? x->m.bridge : "");
    }
    printf("  %s: %s\n", T("label.folder"), x->dir);
    if (x->st.detail[0]) printf("  %s: %s\n", T("label.detail"), x->st.detail);
    return 0;
}

/* the machine left something the session gives back when it stops (a bridged card's macvtap, a vGPU) */
static void release(const Machine *x) {
    if (!x->ok || (x->m.network != VM_NET_BRIDGED && !x->m.vgpu)) return;
    char t[VMCLI_TEXT_MAX + 1];
    session_ask(VMCLI_RELEASE, x->dir, t, sizeof t, 10);
}

static int cmd_power(Machine *x, const char *verb) {
    static const struct { const char *verb, *opt, *done; int release; } P[] = {
        { "stop", "--stop", "stopped", 1 }, { "poweroff", "--poweroff", "stopped", 1 }, { "pause", "--pause", "paused", 0 },
        { "resume", "--resume", "resumed", 0 }, { "suspend", "--suspend", "suspended", 1 }, { "reset", "--reset", "restarted", 0 },
    };
    int i = 0;
    while (strcmp(P[i].verb, verb)) i++;
    if (!strcmp(verb, "suspend") && x->ok && x->m.vgpu) { ERR("noSuspendGpu", NULL); return VMCLI_FAILED; }
    Run r;
    char *a[] = { NULL, (char *)P[i].opt, x->dir, NULL };
    int st = run_lordvm(a, &r);
    if (st < 0) return VMCLI_FAILED;
    if (st == 3) { ERR("notRunning", "name", x->m.name, NULL); return VMCLI_NOT_RUNNING; }
    if (st) { ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return our_rc(st); }
    if (P[i].release) release(x);
    SAY(P[i].done, "name", x->m.name, NULL);
    return 0;
}

static int session_said(int code, const char *text) {
    if (text[0]) fprintf(code ? stderr : stdout, "%s%s\n", code ? "machines: " : "", text);
    return code;
}

static int cmd_open(Machine *x) {
    char t[VMCLI_TEXT_MAX + 1];
    int code = session_ask(VMCLI_OPEN, x->dir, t, sizeof t, 30);
    if (code < 0) { ERR("noSession", NULL); return VMCLI_NO_SESSION; }
    return session_said(code, t);
}

static int cmd_start(Machine *x, int window) {
    char t[VMCLI_TEXT_MAX + 1];
    int was_active = x->st.active;
    int code = session_ask(window ? VMCLI_START : VMCLI_START_DETACHED, x->dir, t, sizeof t, 15 * 60);
    if (code >= 0) {
        session_said(code, t);
        if (code || !window || was_active) return code;
        /* in its window: wait until it runs, or say it's still on its way */
        for (int i = 0; i < START_WAIT_S * 2; i++) {
            struct timespec ts = { 0, 500000000 };
            nanosleep(&ts, NULL);
            read_state(x);
            if (!strcmp(x->st.state, "running")) { SAY("running", "name", x->m.name, NULL); return 0; }
            if (i > 20 && !x->st.active && strcmp(x->st.view, "stopped") && strcmp(x->st.view, "suspended")) {
                ERR("failed", "name", x->m.name, "why", x->st.detail[0] ? x->st.detail : state_text(x->st.view), NULL);
                return VMCLI_FAILED;
            }
        }
        SAY("stillStarting", "name", x->m.name, NULL);
        return 0;
    }
    /* no session */
    if (window) { ERR("noSession", NULL); ERR("needsWindow", "name", x->m.name, NULL); return VMCLI_NO_SESSION; }
    if (x->st.active) { SAY("alreadyRunning", "name", x->m.name, NULL); return 0; }
    if (x->ok && (x->m.network == VM_NET_BRIDGED || x->m.vgpu)) {
        ERR("needsSession", "name", x->m.name, "why", T(x->m.vgpu ? "why.gpu" : "why.bridged"), NULL);
        return VMCLI_NO_SESSION;
    }
    Run r;
    char *a[] = { NULL, "--start", x->dir, "--detach", "--keep-running", NULL };
    int st = run_lordvm(a, &r);
    if (st < 0) return VMCLI_FAILED;
    if (st) {
        /* --detach says only {"started":false}: the reason is in run.json */
        read_state(x);
        ERR("failed", "name", x->m.name, "why", r.ne || !x->st.detail[0] ? why_of(&r) : x->st.detail, NULL);
        return our_rc(st);
    }
    SAY("startedNoWindow", "name", x->m.name, NULL);
    return 0;
}

static int cmd_snapshot(Machine *x, int argc, char **argv) {
    if (argc < 1) { ERR("missing", "what", "list|take|revert|delete", NULL); return VMCLI_USAGE; }
    const char *what = argv[0];
    int json = argc > 1 && !strcmp(argv[argc - 1], "--json");
    Run r;
    /* the list: also to find a snapshot by its label */
    char *la[] = { NULL, "--snapshot", x->dir, "list", NULL };
    JDoc d = { 0 };
    int have = 0;
    if (!strcmp(what, "list") || !strcmp(what, "revert") || !strcmp(what, "delete")) {
        int st = run_lordvm(la, &r);
        if (st) { if (st > 0) ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return st < 0 ? VMCLI_FAILED : our_rc(st); }
        have = !json_parse(r.out, r.no, &d, NULL, 0);
    }
    const JVal *snaps = have ? json_get(d.root, "snapshots") : NULL;
    if (!strcmp(what, "list")) {
        if (argc > (json ? 2 : 1)) { ERR("unknown", "arg", argv[1], NULL); json_free(&d); return VMCLI_USAGE; }
        if (json) { fwrite(r.out, 1, r.no, stdout); json_free(&d); return 0; }
        if (!snaps || !snaps->kid) { SAY("noSnaps", "name", x->m.name, NULL); json_free(&d); return 0; }
        const char *head[4] = { T("label.tag"), T("label.name"), T("label.taken"), T("label.memory") };
        int w[4];
        for (int c = 0; c < 4; c++) w[c] = width(head[c]);
        for (const JVal *s = snaps->kid; s; s = s->next) {
            const char *v[3] = { json_str(s, "tag"), json_str(s, "name"), json_str(s, "created") };
            for (int c = 0; c < 3; c++) if (v[c] && width(v[c]) > w[c]) w[c] = width(v[c]);
        }
        for (int c = 0; c < 4; c++) cell(head[c], w[c], c == 3);
        putchar('\n');
        for (const JVal *s = snaps->kid; s; s = s->next) {
            const char *v[3] = { json_str(s, "tag"), json_str(s, "name"), json_str(s, "created") };
            for (int c = 0; c < 3; c++) cell(v[c] ? v[c] : "", w[c], 0);
            cell(json_bool(s, "memory", 0) ? T("yes") : "", w[3], 1);
            putchar('\n');
        }
        json_free(&d);
        return 0;
    }
    if (argc != 2) { json_free(&d); ERR(argc < 2 ? "missing" : "unknown", "what", !strcmp(what, "take") ? "LABEL" : "TAG", "arg", argc > 2 ? argv[2] : "", NULL); return VMCLI_USAGE; }
    if (!strcmp(what, "take")) {
        char *a[] = { NULL, "--snapshot", x->dir, "take", argv[1], NULL };
        int st = run_lordvm(a, &r);
        if (st) { if (st > 0) ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return st < 0 ? VMCLI_FAILED : our_rc(st); }
        JDoc o;
        char tag[40] = "";
        if (!json_parse(r.out, r.no, &o, NULL, 0)) { const char *t = json_str(o.root, "tag"); if (t) snprintf(tag, sizeof tag, "%s", t); json_free(&o); }
        SAY("snapTaken", "label", argv[1], "name", x->m.name, "tag", tag, NULL);
        return 0;
    }
    if (strcmp(what, "revert") && strcmp(what, "delete")) { json_free(&d); ERR("unknown", "arg", what, NULL); return VMCLI_USAGE; }
    /* a tag, or a snapshot's label when only one has it */
    char tag[40] = "";
    int n = 0;
    for (const JVal *s = snaps ? snaps->kid : NULL; s; s = s->next) {
        const char *t = json_str(s, "tag"), *nm = json_str(s, "name");
        if (t && !strcmp(t, argv[1])) { snprintf(tag, sizeof tag, "%s", t); n = 1; break; }
        if (t && nm && !strcasecmp(nm, argv[1])) { snprintf(tag, sizeof tag, "%s", t); n++; }
    }
    json_free(&d);
    if (n != 1) { ERR("noSnap", "name", x->m.name, "tag", argv[1], NULL); return VMCLI_FAILED; }
    char *a[] = { NULL, "--snapshot", x->dir, (char *)what, tag, NULL };
    int st = run_lordvm(a, &r);
    if (st) { if (st > 0) ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return st < 0 ? VMCLI_FAILED : our_rc(st); }
    SAY(!strcmp(what, "revert") ? "snapReverted" : "snapDeleted", "name", x->m.name, "tag", tag, NULL);
    return 0;
}

static int cmd_clone(Machine *x, int argc, char **argv) {
    const char *name = NULL;
    int dup = 0, keep = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--duplicate")) dup = 1;
        else if (!strcmp(argv[i], "--keep-identity")) keep = 1;
        else if (argv[i][0] != '-' && !name) name = argv[i];
        else { ERR("unknown", "arg", argv[i], NULL); return VMCLI_USAGE; }
    }
    if (!name) { ERR("missing", "what", "NEW_NAME", NULL); return VMCLI_USAGE; }
    if (!vm_name_valid(name)) { ERR("badName", NULL); return VMCLI_USAGE; }
    load_library();
    if (library_path() && mkdir(library, 0700) && errno != EEXIST) { ERR("opFailed", "name", x->m.name, "why", strerror(errno), NULL); return VMCLI_FAILED; }
    library_path();
    for (int i = 0; i < nms; i++) if (!strcasecmp(ms[i].m.name, name)) { ERR("nameTaken", "name", name, NULL); return VMCLI_FAILED; }
    char folder[64], dst[PATH_MAX + 80];
    mcli_folder_name(name, folder, sizeof folder);
    snprintf(dst, sizeof dst, "%s/%s.lordvm", library, folder);
    for (int k = 2; !access(dst, F_OK) && k < 100; k++) snprintf(dst, sizeof dst, "%s/%s %d.lordvm", library, folder, k);
    if (!access(dst, F_OK)) { ERR("nameTaken", "name", name, NULL); return VMCLI_FAILED; }
    char *a[10];
    int k = 0;
    a[k++] = NULL; a[k++] = "--clone"; a[k++] = x->dir; a[k++] = dst; a[k++] = "--name"; a[k++] = (char *)name;
    if (dup) a[k++] = "--duplicate";
    else if (keep) a[k++] = "--keep-identity";
    a[k] = NULL;
    Run r;
    int st = run_lordvm(a, &r);
    if (st) { if (st > 0) ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return st < 0 ? VMCLI_FAILED : our_rc(st); }
    SAY("cloned", "name", name, "dir", dst, NULL);
    return 0;
}

/* why a rename can't be done, in the person's language (vm_rename_plan's problem) */
static int rename_refused(int problem, const char *name, const char *folder, const char *other) {
    if (problem == VM_RN_NAME_TAKEN) ERR("nameTaken", "name", other && other[0] ? other : name, NULL);
    else if (problem == VM_RN_FOLDER_TAKEN) ERR("folderTaken", "folder", folder, NULL);
    else if (problem == VM_RN_TRANSFER_TAKEN) ERR("transferTaken", "folder", folder, NULL);
    else if (problem == VM_RN_NOT_MACHINE) { ERR("notInLibrary", NULL); return VMCLI_NO_MACHINE; }
    else { ERR("badNewName", NULL); return VMCLI_USAGE; }
    return VMCLI_FAILED;
}

/* rename NAME NEW_NAME (docs/VIRTUALIZATION.md 6.26): the app's rename, through lord-vm --rename. Off: the name, its
 * folder and its transfer folder; running: the name now, the folders when it's renamed again with the machine off */
static int cmd_rename(Machine *x, int argc, char **argv) {
    if (argc < 1) { ERR("missing", "what", "NEW_NAME", NULL); return VMCLI_USAGE; }
    if (argc > 1) { ERR("unknown", "arg", argv[1], NULL); return VMCLI_USAGE; }
    const char *name = argv[0];
    if (!vm_rename_name_ok(name)) { ERR("badNewName", NULL); return VMCLI_USAGE; }
    char old[VM_NAME_MAX + 1];
    snprintf(old, sizeof old, "%s", x->m.name);
    VmRenamePlan rp;
    int same = x->ok && !strcmp(name, x->m.name), pr = vm_rename_plan(x->dir, name, &rp);
    if (pr && !same) return rename_refused(pr, name, rp.folder, rp.other);
    if (same && (pr || !rp.moves)) { SAY("renameNothing", "name", name, NULL); return 0; }
    char *a[] = { NULL, "--rename", x->dir, (char *)name, NULL };
    Run r;
    int st = run_lordvm(a, &r);
    if (st < 0) return VMCLI_FAILED;
    JDoc d;
    int have = r.no && !json_parse(r.out, r.no, &d, NULL, 0);
    const char *folder = have ? json_str(d.root, "folder") : NULL, *dir = have ? json_str(d.root, "dir") : NULL;
    int rc = 0;
    if (st == 5 && have) {
        const char *p = json_str(d.root, "problem");
        int pk = !p ? VM_RN_BAD_NAME : !strcmp(p, "name-taken") ? VM_RN_NAME_TAKEN : !strcmp(p, "folder-taken") ? VM_RN_FOLDER_TAKEN
               : !strcmp(p, "transfer-taken") ? VM_RN_TRANSFER_TAKEN : !strcmp(p, "not-a-machine") ? VM_RN_NOT_MACHINE : VM_RN_BAD_NAME;
        rc = rename_refused(pk, name, json_str(d.root, "folder"), json_str(d.root, "other"));
    } else if (st) { ERR("opFailed", "name", old, "why", why_of(&r), NULL); rc = our_rc(st); }
    else if (folder && !strcmp(folder, "later")) SAY(same ? "folderWhenOff" : "renamedLater", "old", old, "name", name, "folder", json_str(d.root, "to") ? json_str(d.root, "to") : "", NULL);
    else if (folder && !strcmp(folder, "moved")) {
        SAY(same ? "renamedFolderOnly" : "renamedFolder", "old", old, "name", name, "dir", dir ? dir : "", NULL);
        const char *why = json_str(d.root, "why");
        if (why && why[0]) ERR("renameNote", "why", why, NULL);
    } else {
        SAY("renamed", "old", old, "name", name, NULL);
        const char *why = have ? json_str(d.root, "why") : NULL;
        if (folder && !strcmp(folder, "kept")) ERR("folderKept", "why", why ? why : "", NULL);
    }
    if (have) json_free(&d);
    return rc;
}

static int cmd_set(Machine *x, int argc, char **argv) {
    char *out[96];
    McliStore store;
    const char *bo, *bv;
    if (!argc) { ERR("missing", "what", "--memory, --cpus, --disk, --network, --clipboard…", NULL); return VMCLI_USAGE; }
    if (mcli_set_args(argc, argv, out + 3, 96 - 3, &store, &bo, &bv)) {
        if (bv) ERR("badValue", "option", bo, "value", bv, NULL);
        else ERR("badOption", "option", bo ? bo : "", NULL);
        return VMCLI_USAGE;
    }
    out[0] = NULL; out[1] = "--set"; out[2] = x->dir;
    Run r;
    int st = run_lordvm(out, &r);
    if (st) { if (st > 0) ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return st < 0 ? VMCLI_FAILED : our_rc(st); }
    SAY("saved", "name", x->m.name, NULL);
    return 0;
}

static int cmd_help(void) {
    static const struct { const char *use, *key; } H[] = {
        { "machines list [--json]", "help.list" },
        { "machines status NAME [--json]", "help.status" },
        { "machines start NAME [--no-window]", "help.start" },
        { "machines open NAME", "help.open" },
        { "machines stop NAME", "help.stop" },
        { "machines poweroff NAME", "help.poweroff" },
        { "machines pause|resume NAME", "help.pause" },
        { "machines suspend NAME", "help.suspend" },
        { "machines reset NAME", "help.reset" },
        { "machines snapshot NAME list | take LABEL | revert TAG | delete TAG", "help.snapshot" },
        { "machines clone NAME NEW_NAME [--duplicate] [--keep-identity]", "help.clone" },
        { "machines rename NAME NEW_NAME", "help.rename" },
        { "machines set NAME [--memory 8G] [--cpus 6] [--disk 64G] [--network shared|isolated|bridged|hostonly] [--bridge CARD]\n"
          "             [--clipboard off|to-guest|from-guest|both] [--clipboard-ask on|off] [--graphics 2d|3d]\n"
          "             [--gpu none|intel-whole|intel-shared] [--name NAME] [--iso FILE|none] [--display window|none]\n"
          "             [--port tcp|udp:HOST:GUEST[:this-computer|lan] ...] [--ports none] [--share ro|rw:TAG:FOLDER ...] [--shares none]\n"
          "             [--sound on|off] [--volume 0..100] [--microphone on|off] [--agent on|off] [--transfer on|off]\n"
          "             [--boot-order auto|disk,cd,network] [--boot-menu on|off] [--resolution follow|WxH] [--pointer lordos|guest]", "help.set" },
    };
    printf("%s\n\n", T("help.usage"));
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) printf("  %s\n      %s\n", H[i].use, T(H[i].key));
    printf("\n%s\n%s\n", T("help.name"), T("help.exit"));
    return 0;
}

/* ---- machine groups (docs/VIRTUALIZATION.md 19.9) ---- */

static void json_group(FILE *f, const Group *G) {
    fprintf(f, "{\"name\":");
    mcli_json_str(f, G->g.name);
    fprintf(f, ",\"dir\":");
    mcli_json_str(f, G->dir);
    fprintf(f, ",\"id\":\"%s\",\"kind\":\"whonix\",\"state\":\"%s\",\"release\":", G->g.id, group_view(G));
    mcli_json_str(f, G->g.release);
    fprintf(f, ",\"disposable\":%s,\"members\":[", G->g.disposable ? "true" : "false");
    for (int j = 0; j < G->g.n; j++) {
        const Machine *x = G->member[j] >= 0 ? &ms[G->member[j]] : NULL;
        fprintf(f, "%s{\"role\":\"%s\",\"primary\":%s,\"state\":\"%s\",\"name\":", j ? "," : "", VMG_ROLE_NAMES[G->g.m[j].role],
                G->g.m[j].primary ? "true" : "false", x ? x->st.view : "missing");
        mcli_json_str(f, x ? x->m.name : "");
        fprintf(f, ",\"dir\":");
        char d[PATH_MAX + 520];
        snprintf(d, sizeof d, "%s/%s", G->dir, G->g.m[j].dir);
        mcli_json_str(f, d);
        fprintf(f, "}");
    }
    fprintf(f, "]}");
}

/* start: through the session only (its link is lord-server's to make, 19.3): the gateway, then the workstation; open:
 * the workstation's window; stop and poweroff: here, in stop order (the workstation first), each with lord-vm */
static int cmd_group(Group *G, const char *cmd, int argc, char **argv) {
    if (!strcmp(cmd, "status")) {
        if (argc > 1 || (argc == 1 && strcmp(argv[0], "--json"))) { ERR("unknown", "arg", argv[argc - 1], NULL); return VMCLI_USAGE; }
        if (argc == 1) { json_group(stdout, G); printf("\n"); return 0; }
        printf("%s\n  %s: %s\n", G->g.name, T("label.state"), group_view(G));
        for (int j = 0; j < G->g.n; j++) {
            const Machine *x = G->member[j] >= 0 ? &ms[G->member[j]] : NULL;
            printf("  %s: %s\n", x ? x->m.name : G->g.m[j].dir, x ? state_text(x->st.view) : "—");
        }
        printf("  %s: %s\n", T("label.folder"), G->dir);
        return 0;
    }
    if (!strcmp(group_view(G), "damaged")) { ERR("groupDamaged", "name", G->g.name, NULL); return VMCLI_FAILED; }
    if (!strcmp(cmd, "start") || !strcmp(cmd, "open")) {
        int open = !strcmp(cmd, "open");
        if (argc > (open ? 0 : 1) || (argc == 1 && strcmp(argv[0], "--no-window"))) { ERR("unknown", "arg", argv[argc - 1], NULL); return VMCLI_USAGE; }
        char t[VMCLI_TEXT_MAX + 1];
        int code = session_ask(open ? VMCLI_OPEN : argc ? VMCLI_START_DETACHED : VMCLI_START, G->dir, t, sizeof t, 15 * 60);
        if (code < 0) { ERR("noSession", NULL); return VMCLI_NO_SESSION; }
        return session_said(code, t);
    }
    if ((strcmp(cmd, "stop") && strcmp(cmd, "poweroff")) || argc) { ERR("unknown", "arg", argc ? argv[0] : cmd, NULL); return VMCLI_USAGE; }
    int idx[VMG_MEMBERS_MAX], n = vm_group_order(&G->g, 1, idx), rc = 0;
    for (int k = 0; k < n; k++) {
        Machine *x = &ms[G->member[idx[k]]];
        if (!x->st.active) continue;
        Run r;
        char *a[] = { NULL, !strcmp(cmd, "stop") ? "--stop" : "--poweroff", x->dir, NULL };
        int st = run_lordvm(a, &r);
        if (st && st != 3) { ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); rc = our_rc(st); break; }
    }
    if (!rc) SAY("groupStopped", "name", G->g.name, NULL);
    return rc;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    char exe[PATH_MAX];
    if (realpath("/proc/self/exe", exe)) { char *s = strrchr(exe, '/'); if (s) { *s = 0; snprintf(self_dir, sizeof self_dir, "%s", exe); } }
    texts_init();
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) return cmd_help();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "list")) {
        int json = 0, names = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--json")) json = 1;
            else if (!strcmp(argv[i], "--names")) names = 1;      /* for the shell's completion */
            else { ERR("unknown", "arg", argv[i], NULL); return VMCLI_USAGE; }
        }
        return cmd_list(json, names);
    }
    static const char *const KNOWN[] = { "status", "start", "open", "stop", "poweroff", "pause", "resume", "suspend", "reset", "snapshot", "clone", "rename", "set", NULL };
    int known = 0;
    for (int i = 0; KNOWN[i]; i++) if (!strcmp(cmd, KNOWN[i])) known = 1;
    if (!known) { ERR("unknown", "arg", cmd, NULL); return VMCLI_USAGE; }
    if (argc < 3) { ERR("missing", "what", "NAME", NULL); return VMCLI_USAGE; }
    int rc;
    int rest = argc - 3;
    char **rv = argv + 3;
    {
        /* a machine group, by its name or folder (19.9) */
        Group *G = find_group(argv[2]);
        if (G) {
            if (strcmp(cmd, "status") && strcmp(cmd, "start") && strcmp(cmd, "open") && strcmp(cmd, "stop") && strcmp(cmd, "poweroff")) { ERR("unknown", "arg", cmd, NULL); return VMCLI_USAGE; }
            return cmd_group(G, cmd, rest, rv);
        }
    }
    Machine *x = find(argv[2], &rc);
    if (!x) return rc;
    if (!strcmp(cmd, "status")) {
        if (rest > 1 || (rest == 1 && strcmp(rv[0], "--json"))) { ERR("unknown", "arg", rv[rest - 1], NULL); return VMCLI_USAGE; }
        return cmd_status(x, rest == 1);
    }
    if (!strcmp(cmd, "start")) {
        if (rest > 1 || (rest == 1 && strcmp(rv[0], "--no-window"))) { ERR("unknown", "arg", rv[rest - 1], NULL); return VMCLI_USAGE; }
        return cmd_start(x, rest == 0);
    }
    if (!strcmp(cmd, "snapshot")) return cmd_snapshot(x, rest, rv);
    if (!strcmp(cmd, "clone")) return cmd_clone(x, rest, rv);
    if (!strcmp(cmd, "rename")) return cmd_rename(x, rest, rv);
    if (!strcmp(cmd, "set")) return cmd_set(x, rest, rv);
    if (rest) { ERR("unknown", "arg", rv[0], NULL); return VMCLI_USAGE; }
    if (!strcmp(cmd, "open")) return cmd_open(x);
    return cmd_power(x, cmd);
}
