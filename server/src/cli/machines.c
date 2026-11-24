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
 *   machines exec NAME [--timeout S] [--] PROGRAM [ARGS...]      machines cp [--force] SRC DST   (one is NAME:PATH)
 *   machines logs NAME [--serial|--machine] [--follow] [--lines N]   machines console NAME
 *   machines mcp                               machines apply FILE
 *   machines import SRC NAME [--arch A] [--devices compat|virtio] [--network shared|isolated] [--firmware uefi|bios]
 *   machines help
 * Also installed as `lordvm` (the same program; docs/VIRTUALIZATION.md 6.15).
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
#include "../vm/vmexec.h"
#include "../vm/vmproto.h"
#include "../pkg/crypto.h"
#include "../fsops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/statvfs.h>
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
    /* the resource budget (docs/VIRTUALIZATION.md 3.7), as the session's start paths check it (js_vm.c budget_ok) */
    if (x->ok && !library_path()) {
        VmHost h;
        vm_host_probe(&h);
        int n = 0;
        long used = vm_budget_used_mb(library, x->dir, lordvm_alive, vm_group_session_mb, &n), need = vm_mem_limit_mb(&x->m) + vm_group_session_mb(x->dir, &x->m), mx = vm_mem_max_mb(&h);
        if (n && !vm_budget_fits(&h, used, need)) {
            char ns[24], fs[24];
            snprintf(ns, sizeof ns, "%ld", need);
            snprintf(fs, sizeof fs, "%ld", mx > used ? mx - used : 0);
            ERR("overBudget", "name", x->m.name, "need", ns, "free", fs, NULL);
            return VMCLI_FAILED;
        }
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

/* a snapshot's tag from its tag or its unique label (snapshots.json through lord-vm --snapshot list); 0, else said why */
static int snap_tag_of(Machine *x, const char *q, char *tag, size_t tn) {
    Run r;
    char *la[] = { NULL, "--snapshot", x->dir, "list", NULL };
    int st = run_lordvm(la, &r);
    if (st) { if (st > 0) ERR("opFailed", "name", x->m.name, "why", why_of(&r), NULL); return -1; }
    JDoc d;
    if (json_parse(r.out, r.no, &d, NULL, 0)) { ERR("noSnap", "name", x->m.name, "tag", q, NULL); return -1; }
    const JVal *snaps = json_get(d.root, "snapshots");
    int n = 0;
    tag[0] = 0;
    for (const JVal *s = snaps ? snaps->kid : NULL; s; s = s->next) {
        const char *t = json_str(s, "tag"), *nm = json_str(s, "name");
        if (t && !strcmp(t, q)) { snprintf(tag, tn, "%s", t); n = 1; break; }
        if (t && nm && !strcasecmp(nm, q)) { snprintf(tag, tn, "%s", t); n++; }
    }
    json_free(&d);
    if (n != 1) { ERR("noSnap", "name", x->m.name, "tag", q, NULL); return -1; }
    return 0;
}

/* start --no-window --snapshot TAG (TAG, or a snapshot's unique label): lord-vm --start --loadvm, run by this command
 * itself, only for a machine that needs no admin step (as a start without a session) */
static int cmd_start_snapshot(Machine *x, const char *tag) {
    if (x->st.active) { SAY("alreadyRunning", "name", x->m.name, NULL); return 0; }
    if (x->ok && (x->m.network == VM_NET_BRIDGED || x->m.vgpu)) {
        ERR("needsSession", "name", x->m.name, "why", T(x->m.vgpu ? "why.gpu" : "why.bridged"), NULL);
        return VMCLI_NO_SESSION;
    }
    /* a label names its snapshot when only one has it (snapshots.json, as `snapshot revert` does) */
    char t[40];
    if (snap_tag_of(x, tag, t, sizeof t)) return VMCLI_USAGE;
    Run r;
    char *a[] = { NULL, "--start", x->dir, "--detach", "--keep-running", "--loadvm", t, NULL };
    int st = run_lordvm(a, &r);
    if (st < 0) return VMCLI_FAILED;
    if (st) { read_state(x); ERR("failed", "name", x->m.name, "why", r.ne || !x->st.detail[0] ? why_of(&r) : x->st.detail, NULL); return our_rc(st); }
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
        { "machines start NAME [--no-window [--snapshot TAG]]", "help.start" },
        { "machines open NAME", "help.open" },
        { "machines stop NAME", "help.stop" },
        { "machines poweroff NAME", "help.poweroff" },
        { "machines pause|resume NAME", "help.pause" },
        { "machines suspend NAME", "help.suspend" },
        { "machines reset NAME", "help.reset" },
        { "machines snapshot NAME list | take LABEL | revert TAG | delete TAG", "help.snapshot" },
        { "machines clone NAME NEW_NAME [--duplicate] [--keep-identity]", "help.clone" },
        { "machines rename NAME NEW_NAME", "help.rename" },
        { "machines fresh GROUP [--gateway] [--yes]", "help.fresh" },
        { "machines start|stop|poweroff|status GROUP --member gateway|workstation", "help.member" },
        { "machines exec NAME [--timeout S] [--] PROGRAM [ARGS...]", "help.exec" },
        { "machines cp [--force] FILE NAME:PATH | NAME:PATH FILE", "help.cp" },
        { "machines logs NAME [--serial|--machine] [--follow] [--lines N]", "help.logs" },
        { "machines console NAME", "help.console" },
        { "machines shell NAME", "help.shell" },
        { "machines mcp", "help.mcp" },
        { "machines apply FILE", "help.apply" },
        { "machines import SRC NAME [--arch A] [--devices compat|virtio] [--network shared|isolated] [--firmware uefi|bios]", "help.import" },
        { "machines set NAME [--memory 8G] [--cpus 6] [--disk 64G] [--network shared|isolated|bridged|hostonly] [--bridge CARD]\n"
          "             [--clipboard off|to-guest|from-guest|both] [--clipboard-ask on|off] [--graphics 2d|3d]\n"
          "             [--gpu none|intel-whole|intel-shared] [--name NAME] [--iso FILE|none] [--display window|none]\n"
          "             [--port tcp|udp:HOST:GUEST[:this-computer|lan] ...] [--ports none] [--share ro|rw:TAG:FOLDER ...] [--shares none]\n"
          "             [--sound on|off] [--volume 0..100] [--microphone on|off] [--agent on|off] [--transfer on|off]\n"
          "             [--boot-order auto|disk,cd,network] [--boot-menu on|off] [--resolution follow|WxH] [--pointer lordos|guest]\n"
          "             [--cpu-pin off|auto|default] [--perf-profile on|off] [--capture-fullscreen on|off]", "help.set" },
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
/* --member gateway|workstation anywhere in argv: the role (VM_ROLE_*), the option taken out of argv; 0 none, -1 bad */
static int take_member(int *argc, char **argv) {
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], "--member")) continue;
        if (i + 1 >= *argc) return -1;
        int role = !strcmp(argv[i + 1], "gateway") ? VM_ROLE_GATEWAY : !strcmp(argv[i + 1], "workstation") ? VM_ROLE_WORKSTATION : -1;
        for (int j = i; j + 2 <= *argc; j++) argv[j] = j + 2 < *argc ? argv[j + 2] : NULL;
        *argc -= 2;
        return role;
    }
    return 0;
}

/* Start Fresh (19.7): lord-vm --group-fresh with the group off; asks first unless --yes (and never without a terminal) */
static int cmd_group_fresh(Group *G, int argc, char **argv) {
    int gw = 0, yes = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--gateway")) gw = 1;
        else if (!strcmp(argv[i], "--yes")) yes = 1;
        else { ERR("unknown", "arg", argv[i], NULL); return VMCLI_USAGE; }
    }
    if (strcmp(group_view(G), "off")) { ERR("freshRunning", "name", G->g.name, NULL); return VMCLI_FAILED; }
    if (!yes) {
        if (!isatty(0)) { ERR("freshNeedsYes", NULL); return VMCLI_USAGE; }
        printf("%s ", say(gw ? "freshAskBoth" : "freshAsk", "name", G->g.name, NULL));
        fflush(stdout);
        char ans[16] = "";
        if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) { SAY("freshKept", "name", G->g.name, NULL); return VMCLI_OK; }
    }
    Run r;
    char *a[] = { NULL, "--group-fresh", G->dir, gw ? "--gateway" : NULL, NULL };
    int st = run_lordvm(a, &r);
    if (st) { ERR("opFailed", "name", G->g.name, "why", why_of(&r), NULL); return our_rc(st); }
    SAY(gw ? "freshDoneBoth" : "freshDone", "name", G->g.name, NULL);
    return VMCLI_OK;
}

/* rename GROUP NEW_NAME (19.8): lord-vm --group-rename, with the group off: its name, its members' names and, when the
 * name asks for another folder, the group's folder (renameat2, never over anything) */
static int cmd_group_rename(Group *G, int argc, char **argv) {
    if (argc < 1) { ERR("missing", "what", "NEW_NAME", NULL); return VMCLI_USAGE; }
    if (argc > 1) { ERR("unknown", "arg", argv[1], NULL); return VMCLI_USAGE; }
    const char *name = argv[0];
    if (!vm_rename_name_ok(name)) { ERR("badNewName", NULL); return VMCLI_USAGE; }
    VmGroupRenamePlan p;
    int pr = vm_group_rename_plan(G->dir, name, &p);
    if (!pr && !strcmp(name, G->g.name) && !p.moves) { SAY("renameNothing", "name", name, NULL); return 0; }
    if (pr == VMG_RN_NAME_TAKEN && !strcmp(name, G->g.name)) pr = VMG_RN_OK;
    if (pr == VMG_RN_NAME_TAKEN) { ERR("groupNameTaken", "name", p.other[0] ? p.other : name, NULL); return VMCLI_FAILED; }
    if (pr == VMG_RN_FOLDER_TAKEN) { ERR("groupFolderTaken", "folder", p.folder, NULL); return VMCLI_FAILED; }
    if (pr) { ERR("badNewName", NULL); return VMCLI_USAGE; }
    char old[VM_NAME_MAX + 1];
    snprintf(old, sizeof old, "%s", G->g.name);
    char *a[] = { NULL, "--group-rename", G->dir, (char *)name, NULL };
    Run r;
    int st = run_lordvm(a, &r);
    if (st < 0) return VMCLI_FAILED;
    if (st) { ERR("opFailed", "name", old, "why", why_of(&r), NULL); return our_rc(st); }
    JDoc d;
    int have = r.no && !json_parse(r.out, r.no, &d, NULL, 0);
    const char *folder = have ? json_str(d.root, "folder") : NULL, *dir = have ? json_str(d.root, "dir") : NULL;
    if (folder && !strcmp(folder, "moved")) SAY("renamedFolder", "old", old, "name", name, "dir", dir ? dir : "", NULL);
    else SAY("renamed", "old", old, "name", name, NULL);
    if (have) json_free(&d);
    return 0;
}

static int cmd_group(Group *G, const char *cmd, int argc, char **argv) {
    /* one member (19.9): --member gateway acts on the gateway alone; --member workstation is the group's own start
     * (the gateway first), or the workstation alone for stop, poweroff and status */
    int role = take_member(&argc, argv);
    if (role < 0) { ERR("badMember", NULL); return VMCLI_USAGE; }
    if (!strcmp(cmd, "fresh")) { if (role) { ERR("unknown", "arg", "--member", NULL); return VMCLI_USAGE; } return cmd_group_fresh(G, argc, argv); }
    if (!strcmp(cmd, "rename")) { if (role) { ERR("unknown", "arg", "--member", NULL); return VMCLI_USAGE; } return cmd_group_rename(G, argc, argv); }
    if (role && !(role == VM_ROLE_WORKSTATION && (!strcmp(cmd, "start") || !strcmp(cmd, "open")))) {
        int j = vm_group_role(&G->g, role);
        Machine *x = j >= 0 && G->member[j] >= 0 ? &ms[G->member[j]] : NULL;
        if (!x) { ERR("groupDamaged", "name", G->g.name, NULL); return VMCLI_FAILED; }
        if (!strcmp(cmd, "status")) {
            if (argc > 1 || (argc == 1 && strcmp(argv[0], "--json"))) { ERR("unknown", "arg", argv[argc - 1], NULL); return VMCLI_USAGE; }
            return cmd_status(x, argc == 1);
        }
        if (!strcmp(cmd, "start")) {
            if (argc > 1 || (argc == 1 && strcmp(argv[0], "--no-window"))) { ERR("unknown", "arg", argv[argc - 1], NULL); return VMCLI_USAGE; }
            return cmd_start(x, argc == 0);
        }
        if (argc) { ERR("unknown", "arg", argv[0], NULL); return VMCLI_USAGE; }
        if (!strcmp(cmd, "open")) return cmd_open(x);
        return cmd_power(x, cmd);
    }
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

/* ---- automation: exec, cp, logs, console, mcp, apply (docs/VIRTUALIZATION.md 6.15, 6.24) ----
 * exec and cp reach the guest only through the machine's own lord-vm (VM_EXEC, vm/vmexec.h): its socket is the person's
 * alone, and lord-vm runs a program only for a machine whose machine.json says "automation": true (Configure, with the
 * person's yes; never for the anonymity profile), as the guest's signed-in person. On this computer, cp reads and
 * writes only the person's files under lord.fs's own rules (fsops.c: the home folder and their mounts, never a protected
 * path), checked on the descriptor it opened, not on the name it was given. Every program, copy and MCP action is also
 * written in the machine's log/automation.log. */

#define EXEC_FAILED 125                 /* exec: the program couldn't be run (its own status otherwise) */

static void auto_log(const Machine *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void auto_log(const Machine *x, const char *fmt, ...) {
    char p[PATH_MAX + 300], line[600], ts[32];
    snprintf(p, sizeof p, "%s/log", x->dir);
    int d = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return;
    int fd = openat(d, "automation.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(d);
    if (fd < 0) return;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    va_list ap;
    va_start(ap, fmt);
    int k = snprintf(line, sizeof line, "%s ", ts);
    if (k > 0 && (size_t)k < sizeof line) k += vsnprintf(line + k, sizeof line - (size_t)k, fmt, ap);
    va_end(ap);
    if (k > (int)sizeof line - 2) k = (int)sizeof line - 2;
    if (k > 0) { line[k++] = '\n'; if (write(fd, line, (size_t)k) < 0) {} }
    close(fd);
}

/* the program's name for the log: its last component, printable ASCII, never its arguments */
static const char *prog_name(const char *a) {
    static char o[48];
    const char *b = strrchr(a, '/');
    snprintf(o, sizeof o, "%.40s", b && b[1] ? b + 1 : a);
    for (char *q = o; *q; q++) if ((unsigned char)*q < 0x20 || (unsigned char)*q >= 0x7f) *q = '?';
    return o;
}

/* this machine's lord-vm socket ($XDG_RUNTIME_DIR/lordos-vm/<id>.sock): connected, and the peer is this account; -1 */
static int vm_connect(const Machine *x) {
    if (!x->ok) return -1;
    char base[PATH_MAX], path[sizeof ((struct sockaddr_un *)0)->sun_path];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    if (rt && rt[0] == '/' && !stat(rt, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid()) snprintf(base, sizeof base, "%s/lordos-vm", rt);
    else snprintf(base, sizeof base, "/run/user/%u/lordos-vm", (unsigned)getuid());
    if (snprintf(path, sizeof path, "%s/%s.sock", base, x->m.id) >= (int)sizeof path) return -1;
    if (lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, strlen(path) + 1);
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (connect(s, (struct sockaddr *)&a, sizeof a) || getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != getuid()) { close(s); return -1; }
    return s;
}

/* one program in the guest: 0 with *res (its bytes in *keep, to free), else -1 with why (lord-vm's words, English) and
 * *code: VME_DENIED (automation off), VME_WRONG_STATE (not running, no agent), other */
static int vm_exec(const Machine *x, unsigned timeout, int argc, const char *const *argv, const uint8_t *in, size_t inlen,
                   VmxRes *res, uint8_t **keep, char *why, size_t wn, int *code) {
    *keep = NULL;
    *code = VME_INTERNAL;
    why[0] = 0;
    size_t n;
    uint8_t *req = vmx_req_encode(timeout, argc, argv, in, inlen, &n);
    if (!req) { snprintf(why, wn, "the program, its arguments or its input are too long"); *code = VME_BAD_REQUEST; return -1; }
    int mfd = vmx_memfd("machines-exec", req, n);
    free(req);
    if (mfd < 0) { snprintf(why, wn, "%s", strerror(errno)); return -1; }
    int s = vm_connect(x);
    if (s < 0) { close(mfd); snprintf(why, wn, "the machine isn't running"); *code = VME_WRONG_STATE; return -1; }
    static uint32_t seq = 100;
    seq++;
    VmMsg m = vm_msg(VM_EXEC, seq, 0, 0, 0, 0, "");
    int sent = vm_send(s, &m, mfd);
    close(mfd);
    if (sent) { close(s); snprintf(why, wn, "lord-vm didn't take the request"); return -1; }
    long long end = (long long)time(NULL) + timeout + 60;
    int rc = -1;
    for (;;) {
        long long left = end - (long long)time(NULL);
        if (left <= 0) { snprintf(why, wn, "no answer from lord-vm"); break; }
        struct pollfd pf = { s, POLLIN, 0 };
        int pr = poll(&pf, 1, left > 1000000 ? 1000000000 : (int)(left * 1000));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        VmMsg r;
        int fd = -1;
        int k = vm_recv(s, &r, &fd);
        if (k < 0) { snprintf(why, wn, "lord-vm closed the connection"); break; }
        if (!k) continue;
        if (r.seq != seq || (r.type != VM_REPLY && r.type != VM_EXEC_DONE) || !vm_msg_valid(&r, 0)) { if (fd >= 0) close(fd); continue; }
        if (r.type == VM_REPLY) {
            if (fd >= 0) close(fd);
            *code = r.a;
            snprintf(why, wn, "%s", r.text[0] ? r.text : "refused");
            break;
        }
        size_t bn;
        uint8_t *b = fd >= 0 ? vmx_memfd_read(fd, VMX_RES_MAX, &bn) : NULL;
        if (fd >= 0) close(fd);
        if (!b || vmx_res_parse(b, bn, res)) { free(b); snprintf(why, wn, "lord-vm's answer couldn't be read"); break; }
        *keep = b;
        *code = VME_OK;
        rc = 0;
        break;
    }
    close(s);
    return rc;
}

/* a refusal of exec or cp, in the person's language where it's one we know */
static void exec_refused(const Machine *x, int code, const char *why) {
    if (code == VME_DENIED && strstr(why, "automation is off")) ERR("noAutomation", "name", x->m.name, NULL);
    else if (code == VME_WRONG_STATE && strstr(why, "isn't running")) ERR("notRunning", "name", x->m.name, NULL);
    else ERR("execFailed", "name", x->m.name, "why", why, NULL);
}

static void write_all_fd(int fd, const uint8_t *b, size_t n) {
    while (n) {
        ssize_t k = write(fd, b, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return;
        b += k; n -= (size_t)k;
    }
}

/* machines exec NAME [--timeout S] [--] PROGRAM [ARGS...]: standard input (when it isn't a terminal, at most 1 MB) goes
 * in; the program's output and errors come back when it ends; its exit status is ours (EXEC_FAILED when it couldn't
 * run). Output from the guest is written as it came only when it goes to a file or a pipe: on a terminal, controls are
 * shown as '?' (a guest can't drive this terminal) */
static int cmd_exec(Machine *x, int argc, char **argv) {
    unsigned timeout = 600;
    int i = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            char *e;
            long t = strtol(argv[++i], &e, 10);
            if (*e || t < 1 || t > VMX_TIMEOUT_MAX) { ERR("badValue", "option", "--timeout", "value", argv[i], NULL); return VMCLI_USAGE; }
            timeout = (unsigned)t;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') { ERR("unknown", "arg", argv[i], NULL); return VMCLI_USAGE; }
        break;
    }
    if (i >= argc) { ERR("missing", "what", "PROGRAM", NULL); return VMCLI_USAGE; }
    int n = argc - i;
    if (n > VMX_ARGC_MAX) { ERR("execFailed", "name", x->m.name, "why", "too many arguments", NULL); return EXEC_FAILED; }
    uint8_t *in = NULL;
    size_t inlen = 0;
    if (!isatty(0)) {
        in = malloc(VMX_STDIN_MAX + 1);
        if (!in) return EXEC_FAILED;
        for (;;) {
            ssize_t k = read(0, in + inlen, VMX_STDIN_MAX + 1 - inlen);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            inlen += (size_t)k;
            if (inlen > VMX_STDIN_MAX) { free(in); ERR("execFailed", "name", x->m.name, "why", "more than 1 MB of input", NULL); return EXEC_FAILED; }
        }
    }
    VmxRes res;
    uint8_t *keep;
    char why[300];
    int code;
    auto_log(x, "exec: %s, %d argument%s", prog_name(argv[i]), n - 1, n == 2 ? "" : "s");
    int r = vm_exec(x, timeout, n, (const char *const *)argv + i, in, inlen, &res, &keep, why, sizeof why, &code);
    free(in);
    if (r) { exec_refused(x, code, why); return EXEC_FAILED; }
    for (int k = 0; k < 2; k++) {
        const uint8_t *b = k ? res.err : res.out;
        size_t bl = k ? res.errlen : res.outlen;
        int fd = k ? 2 : 1;
        if (!bl) continue;
        if (isatty(fd)) {
            char *c = malloc(bl * 4 + 8);
            if (c) { size_t cl = mcli_term_clean(b, bl, c, bl * 4 + 8); write_all_fd(fd, (const uint8_t *)c, cl); free(c); }
        } else write_all_fd(fd, b, bl);
    }
    if (res.flags & (VMX_F_OUT_CUT | VMX_F_ERR_CUT)) ERR("outputCut", NULL);
    if (res.flags & VMX_F_TIMEOUT) ERR("timedOut", "name", x->m.name, NULL);
    free(keep);
    return res.exit;
}

/* ---- cp ---- */

static int fs_ready;
static void fs_setup(void) {
    if (fs_ready) return;
    const char *home = getenv("HOME");
    char user[64] = "";
    struct passwd *pw = getpwuid(getuid());
    if (pw) snprintf(user, sizeof user, "%s", pw->pw_name);
    fsops_init(home && home[0] == '/' ? home : pw ? pw->pw_dir : "/nonexistent", user);
    fs_ready = 1;
}

/* the real path of an open descriptor; 0 */
static int fd_path(int fd, char *out, size_t n) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t k = readlink(p, out, n - 1);
    if (k <= 0 || (size_t)k >= n - 1) return -1;
    out[k] = 0;
    return out[0] == '/' ? 0 : -1;
}

/* a path of the person's files lord.fs may write (or read: the same rule), resolved: 1 yes */
static int fs_ok(const char *real) { return fsops_allowed(real) && !fsops_protected(real) && !fsops_protected_resolved(real); }
/* a folder a new file goes into: the same rule, except that a folder may hold protected paths (the home folder does) */
static int fs_dir_ok(const char *real) { return fsops_allowed(real) && !fsops_protected(real); }

/* the guest's two steps for a copy into it, and one for a copy out of it, run as the person through exec. Each takes
 * its paths as arguments ($1...), never as script text. ~ and ~/ are the person's home folder in the guest. */
#define G_EXPAND "case \"$1\" in \"~\") s=\"$HOME\";; \"~/\"*) s=\"$HOME/${1#\\~/}\";; /*) s=\"$1\";; *) s=\"$HOME/$1\";; esac; "
static const char PUT_BEGIN[] =
    G_EXPAND "n=\"$2\"; if [ -d \"$s\" ]; then t=\"${s%/}/$n\"; else t=\"$s\"; fi; "
    "p=$(dirname -- \"$t\"); [ -d \"$p\" ] || { echo \"no folder $p in the machine\" >&2; exit 2; }; "
    "[ -d \"$t\" ] && { echo \"$t is a folder\" >&2; exit 2; }; "
    "w=$(mktemp -p \"$p\" .lordos-cp.XXXXXXXX) || exit 3; printf '%s\\n%s\\n' \"$w\" \"$t\"";
static const char PUT_APPEND[] = "cat >> \"$1\"";
static const char PUT_END[] =
    "w=\"$1\"; t=\"$2\"; z=\"$3\"; h=\"$4\"; "
    "[ \"$(stat -c %s -- \"$w\")\" = \"$z\" ] && [ \"$(sha256sum < \"$w\" | cut -d' ' -f1)\" = \"$h\" ] || "
    "{ rm -f -- \"$w\"; echo 'the copy in the machine differs from the file' >&2; exit 9; }; "
    "m=$(umask); chmod \"$(printf '%o' $(( 0666 & ~0$m )))\" -- \"$w\" 2>/dev/null; "
    "if [ \"$5\" = 1 ]; then mv -f -T -- \"$w\" \"$t\" || { rm -f -- \"$w\"; exit 7; }; "
    "else mv -n -T -- \"$w\" \"$t\"; if [ -e \"$w\" ]; then rm -f -- \"$w\"; echo exists >&2; exit 8; fi; fi; printf '%s' \"$t\"";
static const char GET_BEGIN[] =
    G_EXPAND "[ -f \"$s\" ] || { echo \"$s is not a file in the machine\" >&2; exit 2; }; [ -r \"$s\" ] || { echo \"$s can't be read\" >&2; exit 2; }; "
    "printf '%s\\n' \"$(stat -L -c %s -- \"$s\")\"";
#define GET_CHUNK_BYTES VMX_OUT_MAX
static const char GET_CHUNK[] = G_EXPAND "dd if=\"$s\" bs=131072 skip=\"$2\" count=1 status=none";
static const char GET_END[] = G_EXPAND "sha256sum < \"$s\" | cut -d' ' -f1";
_Static_assert(GET_CHUNK_BYTES == 131072, "GET_CHUNK's block size");

/* one step: /bin/sh -c SCRIPT lordos-cp ARGS..., with input; 0 and its output (NUL-terminated copy, at most cap), else
 * -1 after saying why */
static int cp_step(const Machine *x, const char *script, const char *const *args, int na, const uint8_t *in, size_t inlen,
                   uint8_t *out, size_t cap, size_t *outlen, int *exit_status) {
    const char *av[12];
    int n = 0;
    av[n++] = "/bin/sh"; av[n++] = "-c"; av[n++] = script; av[n++] = "lordos-cp";
    for (int i = 0; i < na && n < 11; i++) av[n++] = args[i];
    VmxRes res;
    uint8_t *keep;
    char why[300];
    int code;
    if (vm_exec(x, 600, n, av, in, inlen, &res, &keep, why, sizeof why, &code)) { exec_refused(x, code, why); return -1; }
    *exit_status = res.exit;
    if (res.exit) {
        char t[400];
        mcli_term_clean(res.err, res.errlen < 300 ? res.errlen : 300, t, sizeof t);
        size_t l = strlen(t);
        while (l && t[l - 1] == '\n') t[--l] = 0;
        if (res.exit != 8) ERR("execFailed", "name", x->m.name, "why", t[0] ? t : "the copy failed", NULL);
        free(keep);
        return -1;
    }
    size_t c = res.outlen < cap ? res.outlen : cap;
    if (out && cap) { memcpy(out, res.out, c); if (c < cap) out[c] = 0; }
    if (outlen) *outlen = c;
    free(keep);
    return 0;
}

static void hexof(const uint8_t d[32], char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]); }

static void progress(double done, double total) {
    if (!isatty(2)) return;
    char a[32], b[32];
    mcli_bytes(done, a, sizeof a);
    mcli_bytes(total, b, sizeof b);
    fprintf(stderr, "\r  %s / %s   ", a, b);
}

static int cp_put(Machine *x, const char *src, const char *gpath, int force) {
    fs_setup();
    char real[PATH_MAX], again[PATH_MAX];
    if (!realpath(src, real) || !fs_ok(real)) { ERR("cpOutside", "path", src, NULL); return VMCLI_FAILED; }
    int fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); ERR("cpOnlyFiles", "path", src, NULL); return VMCLI_FAILED; }
    /* what was opened is what was checked */
    if (fd_path(fd, again, sizeof again) || strcmp(again, real) || !fs_ok(again)) { close(fd); ERR("cpOutside", "path", src, NULL); return VMCLI_FAILED; }
    const char *base = strrchr(real, '/') + 1;
    const char *a1[] = { gpath, base };
    uint8_t out[8200];
    size_t ol;
    int ex;
    auto_log(x, "cp: a file of %lld bytes into the machine", (long long)st.st_size);
    if (cp_step(x, PUT_BEGIN, a1, 2, NULL, 0, out, sizeof out - 1, &ol, &ex)) { close(fd); return VMCLI_FAILED; }
    char *nl = memchr(out, '\n', ol);
    if (!nl) { close(fd); ERR("execFailed", "name", x->m.name, "why", "the machine gave no folder", NULL); return VMCLI_FAILED; }
    *nl = 0;
    char *w = (char *)out, *t = nl + 1, *nl2 = strchr(t, '\n');
    if (nl2) *nl2 = 0;
    if (w[0] != '/' || t[0] != '/' || !vmx_guest_path_ok(w) || !vmx_guest_path_ok(t)) { close(fd); ERR("execFailed", "name", x->m.name, "why", "the machine gave a strange path", NULL); return VMCLI_FAILED; }
    char wtmp[8200], target[8200];
    snprintf(wtmp, sizeof wtmp, "%s", w);
    snprintf(target, sizeof target, "%s", t);
    Sha256 h;
    sha256_init(&h);
    uint8_t *buf = malloc(VMX_STDIN_MAX);
    double done = 0;
    int rc = 0;
    for (;;) {
        size_t got = 0;
        while (buf && got < VMX_STDIN_MAX) {
            ssize_t k = read(fd, buf + got, VMX_STDIN_MAX - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        if (!buf || !got) break;
        sha256_update(&h, buf, got);
        const char *a2[] = { wtmp };
        if (cp_step(x, PUT_APPEND, a2, 1, buf, got, NULL, 0, NULL, &ex)) { rc = VMCLI_FAILED; break; }
        done += (double)got;
        progress(done, (double)st.st_size);
    }
    free(buf);
    close(fd);
    if (isatty(2)) fputc('\n', stderr);
    if (!rc) {
        uint8_t d[32];
        char hex[65], size[32];
        sha256_final(&h, d);
        hexof(d, hex);
        snprintf(size, sizeof size, "%.0f", done);
        const char *a3[] = { wtmp, target, size, hex, force ? "1" : "0" };
        if (cp_step(x, PUT_END, a3, 5, NULL, 0, out, sizeof out - 1, &ol, &ex)) {
            if (ex == 8) ERR("cpExists", "path", target, NULL);
            return VMCLI_FAILED;
        }
        char sz[32], shown[4300];
        mcli_bytes(done, sz, sizeof sz);
        mcli_term_clean((const uint8_t *)target, strlen(target), shown, sizeof shown);
        SAY("cpDone", "size", sz, "where", shown, NULL);
        return 0;
    }
    const char *a4[] = { wtmp };
    cp_step(x, "rm -f -- \"$1\"", a4, 1, NULL, 0, NULL, 0, NULL, &ex);
    return rc;
}

/* a copy out of a machine stopped half way (Ctrl+C): its temporary file goes */
static volatile sig_atomic_t cp_dfd = -1;
static char cp_tmp[64];
static void cp_interrupted(int sig) {
    if (cp_dfd >= 0 && cp_tmp[0]) unlinkat(cp_dfd, cp_tmp, 0);
    signal(sig, SIG_DFL);
    raise(sig);
}

static int cp_get(Machine *x, const char *gpath, const char *dst, int force) {
    fs_setup();
    if (!vmx_guest_path_ok(gpath)) { ERR("cpOnlyFiles", "path", gpath, NULL); return VMCLI_FAILED; }
    /* where it goes: an existing folder (the guest file's name in it) or a new name in one */
    const char *gb = strrchr(gpath, '/');
    gb = gb ? gb + 1 : gpath;
    if (!gb[0] || !strcmp(gb, ".") || !strcmp(gb, "..") || !strcmp(gb, "~")) { ERR("cpOnlyFiles", "path", gpath, NULL); return VMCLI_FAILED; }
    char dir[PATH_MAX], name[NAME_MAX + 1], real[PATH_MAX];
    struct stat st;
    if (!stat(dst, &st) && S_ISDIR(st.st_mode)) {
        if (!realpath(dst, dir)) { ERR("cpOutside", "path", dst, NULL); return VMCLI_FAILED; }
        snprintf(name, sizeof name, "%s", gb);
    } else {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", dst);
        char *sl = strrchr(tmp, '/');
        const char *pd = sl ? (sl == tmp ? "/" : tmp) : ".";
        if (sl) *sl = 0;
        const char *nm = sl ? sl + 1 : dst;
        if (!nm[0] || !strcmp(nm, ".") || !strcmp(nm, "..") || strlen(nm) > NAME_MAX || !realpath(pd, dir)) { ERR("cpOutside", "path", dst, NULL); return VMCLI_FAILED; }
        snprintf(name, sizeof name, "%s", nm);
    }
    if (strchr(name, '/') || snprintf(real, sizeof real, "%s/%s", strcmp(dir, "/") ? dir : "", name) >= (int)sizeof real || !fs_dir_ok(dir) || !fs_ok(real)) {
        ERR("cpOutside", "path", dst, NULL);
        return VMCLI_FAILED;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    char again[PATH_MAX];
    if (dfd < 0 || fd_path(dfd, again, sizeof again) || strcmp(again, dir) || !fs_dir_ok(again)) { if (dfd >= 0) close(dfd); ERR("cpOutside", "path", dst, NULL); return VMCLI_FAILED; }
    if (!force && !fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW)) { close(dfd); ERR("cpExists", "path", real, NULL); return VMCLI_FAILED; }
    uint8_t out[VMX_OUT_MAX + 1];
    size_t ol;
    int ex;
    const char *a1[] = { gpath };
    if (cp_step(x, GET_BEGIN, a1, 1, NULL, 0, out, sizeof out - 1, &ol, &ex)) { close(dfd); return VMCLI_FAILED; }
    char *e;
    unsigned long long total = strtoull((char *)out, &e, 10);
    if (e == (char *)out || *e != '\n') { close(dfd); ERR("execFailed", "name", x->m.name, "why", "the machine gave no size", NULL); return VMCLI_FAILED; }
    /* the size is the guest's word: never more than the destination has room for (and a margin) */
    struct statvfs vf;
    if (!fstatvfs(dfd, &vf) && total > (unsigned long long)vf.f_bavail * vf.f_frsize - ((unsigned long long)vf.f_bavail * vf.f_frsize / 20)) {
        close(dfd);
        ERR("execFailed", "name", x->m.name, "why", strerror(ENOSPC), NULL);
        return VMCLI_FAILED;
    }
    char tmpn[64];
    int wfd = -1;
    for (int k = 0; k < 20 && wfd < 0; k++) {
        unsigned rnd = 0;
        if (getrandom(&rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) rnd = (unsigned)getpid() * 2654435761u + (unsigned)k;
        snprintf(tmpn, sizeof tmpn, ".lordos-cp.%08x", rnd);
        wfd = openat(dfd, tmpn, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    if (wfd < 0) { close(dfd); ERR("execFailed", "name", x->m.name, "why", strerror(errno), NULL); return VMCLI_FAILED; }
    snprintf(cp_tmp, sizeof cp_tmp, "%s", tmpn);
    cp_dfd = dfd;
    signal(SIGINT, cp_interrupted);
    signal(SIGTERM, cp_interrupted);
    signal(SIGHUP, cp_interrupted);
    auto_log(x, "cp: a file of %llu bytes out of the machine", total);
    Sha256 h;
    sha256_init(&h);
    unsigned long long done = 0;
    int rc = 0;
    for (unsigned long long blk = 0; done < total; blk++) {
        char sk[32];
        snprintf(sk, sizeof sk, "%llu", blk);
        const char *a2[] = { gpath, sk };
        if (cp_step(x, GET_CHUNK, a2, 2, NULL, 0, out, sizeof out - 1, &ol, &ex)) { rc = VMCLI_FAILED; break; }
        if (!ol) break;
        if (ol > total - done) { rc = VMCLI_FAILED; ERR("execFailed", "name", x->m.name, "why", "the file grew while it was copied", NULL); break; }
        sha256_update(&h, out, ol);
        size_t w = 0;
        while (w < ol) { ssize_t k = write(wfd, out + w, ol - w); if (k < 0 && errno == EINTR) continue; if (k <= 0) break; w += (size_t)k; }
        if (w < ol) { rc = VMCLI_FAILED; ERR("execFailed", "name", x->m.name, "why", strerror(errno), NULL); break; }
        done += ol;
        progress((double)done, (double)total);
        if (ol < GET_CHUNK_BYTES && done < total) { rc = VMCLI_FAILED; ERR("execFailed", "name", x->m.name, "why", "the file shrank while it was copied", NULL); break; }
    }
    if (isatty(2)) fputc('\n', stderr);
    if (!rc && done != total) { rc = VMCLI_FAILED; ERR("execFailed", "name", x->m.name, "why", "the file changed while it was copied", NULL); }
    if (!rc) {
        uint8_t d[32];
        char hex[65];
        sha256_final(&h, d);
        hexof(d, hex);
        if (cp_step(x, GET_END, a1, 1, NULL, 0, out, sizeof out - 1, &ol, &ex) || ol < 64 || memcmp(out, hex, 64)) {
            rc = VMCLI_FAILED;
            ERR("execFailed", "name", x->m.name, "why", "the file changed while it was copied", NULL);
        }
    }
    if (!rc && fsync(wfd)) rc = VMCLI_FAILED;
    close(wfd);
    if (!rc) {
        mode_t um = umask(0);
        umask(um);
        fchmodat(dfd, tmpn, 0666 & ~um, 0);
        int r = force ? renameat(dfd, tmpn, dfd, name) : renameat2(dfd, tmpn, dfd, name, RENAME_NOREPLACE);
        if (r) { rc = VMCLI_FAILED; if (errno == EEXIST) ERR("cpExists", "path", real, NULL); else ERR("execFailed", "name", x->m.name, "why", strerror(errno), NULL); }
    }
    if (rc) unlinkat(dfd, tmpn, 0);
    cp_dfd = -1;
    cp_tmp[0] = 0;
    close(dfd);
    if (!rc) {
        char sz[32];
        mcli_bytes((double)done, sz, sizeof sz);
        SAY("cpDone", "size", sz, "where", real, NULL);
    }
    return rc;
}

/* machines cp [--force] SRC DST: one of them NAME:PATH. Files only. */
static int cmd_cp(int argc, char **argv) {
    int force = 0;
    const char *a[2];
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--force")) force = 1;
        else if (n < 2 && strcmp(argv[i], "--")) a[n++] = argv[i];
        else if (strcmp(argv[i], "--")) { ERR("unknown", "arg", argv[i], NULL); return VMCLI_USAGE; }
    }
    if (n != 2) { ERR("cpOneSide", NULL); return VMCLI_USAGE; }
    char n0[VM_NAME_MAX + 1], n1[VM_NAME_MAX + 1];
    const char *p0, *p1;
    int g0 = mcli_cp_side(a[0], n0, sizeof n0, &p0), g1 = mcli_cp_side(a[1], n1, sizeof n1, &p1);
    if (g0 == g1) { ERR("cpOneSide", NULL); return VMCLI_USAGE; }
    int rc;
    Machine *x = find(g0 ? n0 : n1, &rc);
    if (!x) return rc;
    if (x->ok && x->m.profile_lock) { ERR("noAutomation", "name", x->m.name, NULL); return VMCLI_FAILED; }
    return g1 ? cp_put(x, a[0], p1, force) : cp_get(x, p0, a[1], force);
}

/* ---- logs ---- */

/* machines logs NAME [--serial|--machine] [--follow] [--lines N]: the guest's journal (log/journal.log, from the agent),
 * its serial console or lord-vm's log; the last lines, then (--follow) what comes, until Ctrl+C. What a guest wrote is
 * made safe for the terminal */
static int open_log(const Machine *x, const char *file) {
    char p[PATH_MAX + 300];
    snprintf(p, sizeof p, "%s/log", x->dir);
    int d = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return -1;
    int fd = openat(d, file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    close(d);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) { close(fd); fd = -1; }
    return fd;
}

static void put_clean(const uint8_t *b, size_t n, int raw) {
    if (raw) { write_all_fd(1, b, n); return; }
    char *c = malloc(n * 4 + 8);
    if (!c) return;
    size_t cl = mcli_term_clean(b, n, c, n * 4 + 8);
    write_all_fd(1, (const uint8_t *)c, cl);
    free(c);
}

static int log_tail(const Machine *x, const char *file, long lines, char **out, size_t *outn) {
    int fd = open_log(x, file);
    if (fd < 0) return -1;
    struct stat st;
    fstat(fd, &st);
    off_t from = st.st_size > (4 << 20) ? st.st_size - (4 << 20) : 0;
    size_t n = (size_t)(st.st_size - from);
    char *b = malloc(n + 1);
    size_t got = 0;
    while (b && got < n) { ssize_t k = pread(fd, b + got, n - got, from + (off_t)got); if (k <= 0) break; got += (size_t)k; }
    close(fd);
    if (!b) return -1;
    size_t s = got;
    long seen = 0;
    while (s > 0) { if (b[s - 1] == '\n' && s != got && ++seen >= lines) break; s--; }
    memmove(b, b + s, got - s);
    *outn = got - s;
    b[*outn] = 0;
    *out = b;
    return 0;
}

static int cmd_logs(Machine *x, int argc, char **argv) {
    const char *file = "journal.log";
    int follow = 0;
    long lines = 50;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--serial")) file = "serial.log";
        else if (!strcmp(argv[i], "--machine")) file = "lord-vm.log";
        else if (!strcmp(argv[i], "--follow") || !strcmp(argv[i], "-f")) follow = 1;
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc) {
            char *e;
            lines = strtol(argv[++i], &e, 10);
            if (*e || lines < 1 || lines > 100000) { ERR("badValue", "option", "--lines", "value", argv[i], NULL); return VMCLI_USAGE; }
        } else { ERR("unknown", "arg", argv[i], NULL); return VMCLI_USAGE; }
    }
    int raw = !isatty(1);
    char *b;
    size_t n;
    if (log_tail(x, file, lines, &b, &n)) {
        if (!follow) { ERR("noLog", "name", x->m.name, NULL); return VMCLI_FAILED; }
    } else { put_clean((const uint8_t *)b, n, raw); free(b); }
    if (!follow) return 0;
    /* following: the file grows, or is rotated (a new, smaller file under the name) */
    int fd = open_log(x, file);
    off_t at = 0;
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st)) at = st.st_size;
    for (;;) {
        struct timespec ts = { 0, 400000000 };
        nanosleep(&ts, NULL);
        if (fd < 0) { fd = open_log(x, file); at = 0; continue; }
        struct stat now;
        int nfd = open_log(x, file);
        if (nfd >= 0 && !fstat(nfd, &now) && !fstat(fd, &st) && (now.st_ino != st.st_ino || now.st_size < at)) { close(fd); fd = nfd; at = 0; }
        else if (nfd >= 0) close(nfd);
        uint8_t buf[65536];
        ssize_t k;
        while ((k = pread(fd, buf, sizeof buf, at)) > 0) { put_clean(buf, (size_t)k, raw); at += k; }
    }
}

/* ---- console ---- */

#include <termios.h>
static struct termios saved_tio;
static void tio_restore(void) { tcsetattr(0, TCSANOW, &saved_tio); }

#include <sys/ioctl.h>
static volatile sig_atomic_t winch;
static void on_winch(int sig) { (void)sig; winch = 1; }

/* machines console NAME: the guest's serial console (lord-vm's VM_CONSOLE); machines shell NAME: a shell as the person
 * signed in to the guest, through its LordOS tools (VM_SHELL, 6.18; the app's Shell), sized to this terminal and
 * following its size. Ctrl+] leaves either */
static int cmd_console(Machine *x, int shell) {
    int s = vm_connect(x);
    if (s < 0) { ERR("notRunning", "name", x->m.name, NULL); return VMCLI_NOT_RUNNING; }
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
    if (isatty(0) && (ioctl(0, TIOCGWINSZ, &ws) || !ws.ws_col || !ws.ws_row)) { ws.ws_row = 24; ws.ws_col = 80; }
    if (ws.ws_col > 1000) ws.ws_col = 1000;
    if (ws.ws_row > 500) ws.ws_row = 500;
    VmMsg m = shell ? vm_msg(VM_SHELL, 77, VSH_OPEN, ws.ws_col, ws.ws_row, 0, "") : vm_msg(VM_CONSOLE, 77, 0, 0, 0, 0, "");
    if (vm_send(s, &m, -1)) { close(s); return VMCLI_FAILED; }
    int cfd = -1;
    for (int tries = 0; tries < 100 && cfd < 0; tries++) {
        struct pollfd pf = { s, POLLIN, 0 };
        if (poll(&pf, 1, 100) <= 0) continue;
        VmMsg r;
        int fd = -1;
        int k = vm_recv(s, &r, &fd);
        if (k < 0) break;
        if (!k) continue;
        if (r.seq == 77 && r.type == (shell ? VM_SHELL_FD : VM_CONSOLE_FD) && fd >= 0) { cfd = fd; break; }
        if (fd >= 0) close(fd);
        if (r.seq == 77 && r.type == VM_REPLY) { ERR("opFailed", "name", x->m.name, "why", r.text, NULL); close(s); return VMCLI_FAILED; }
    }
    if (cfd < 0) { close(s); ERR("notRunning", "name", x->m.name, NULL); return VMCLI_NOT_RUNNING; }
    auto_log(x, shell ? "shell: opened" : "console: opened");
    if (shell) { struct sigaction sa = { .sa_handler = on_winch }; sigaction(SIGWINCH, &sa, NULL); }
    int tty = isatty(0);
    if (tty) {
        SAY("consoleHint", "name", x->m.name, NULL);
        tcgetattr(0, &saved_tio);
        atexit(tio_restore);
        struct termios raw = saved_tio;
        cfmakeraw(&raw);
        tcsetattr(0, TCSANOW, &raw);
    }
    struct pollfd pf[3] = { { 0, POLLIN, 0 }, { cfd, POLLIN, 0 }, { s, POLLIN, 0 } };
    for (;;) {
        if (shell && winch && isatty(0)) {
            winch = 0;
            if (!ioctl(0, TIOCGWINSZ, &ws) && ws.ws_col && ws.ws_row && ws.ws_col <= 1000 && ws.ws_row <= 500) {
                VmMsg z = vm_msg(VM_SHELL, 78, VSH_SIZE, ws.ws_col, ws.ws_row, 0, "");
                vm_send(s, &z, -1);
            }
        }
        if (poll(pf, 3, -1) < 0) { if (errno == EINTR) continue; break; }
        uint8_t b[4096];
        if (pf[2].revents) {
            /* lord-vm's news to every client (states, agent): read and let go, so its sends never back up */
            VmMsg ev;
            int efd = -1, k = vm_recv(s, &ev, &efd);
            if (efd >= 0) close(efd);
            if (k < 0) pf[2].fd = -1;                       /* lord-vm went: the stream says the rest */
        }
        if (pf[1].revents) {
            ssize_t k = read(cfd, b, sizeof b);
            if (k <= 0) break;
            /* the serial console is the guest's terminal: its escapes pass, as in the app's Shell, only to a terminal
             * the person opened for it */
            write_all_fd(1, b, (size_t)k);
        }
        if (pf[0].revents) {
            ssize_t k = read(0, b, sizeof b);
            if (k <= 0) { pf[0].fd = -1; continue; }      /* input ended (a pipe): what the guest still says is printed until its stream ends */
            uint8_t *q = memchr(b, 0x1d, (size_t)k);
            if (q) { write_all_fd(cfd, b, (size_t)(q - b)); break; }
            write_all_fd(cfd, b, (size_t)k);
        }
    }
    if (tty) { tio_restore(); fputc('\n', stdout); }
    close(cfd);
    close(s);
    return 0;
}

/* ---- apply (6.24) ---- */

/* asks the person on their terminal (never from a pipe): 1 yes */
static int ask_tty(const char *q) {
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return 0;
    if (write(fd, q, strlen(q)) < 0 || write(fd, " ", 1) < 0) {}
    char a[16] = "";
    ssize_t k = read(fd, a, sizeof a - 1);
    close(fd);
    return k > 0 && (a[0] == 'y' || a[0] == 'Y' || a[0] == 's' || a[0] == 'S');
}

/* machines apply FILE: a machine made, or changed, from a short text file kept beside a project (6.24). The file is
 * untrusted (it came with a repository): shared folders ask the person on their terminal; automation, root access and
 * packages are not things such a file can ask for. */
static int cmd_apply(int argc, char **argv) {
    if (argc != 1) { ERR("missing", "what", "FILE", NULL); return VMCLI_USAGE; }
    int fd = open(argv[0], O_RDONLY | O_CLOEXEC | O_NOCTTY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > MCLI_APPLY_MAX) { if (fd >= 0) close(fd); ERR("applyBad", "file", argv[0], "line", "0", "why", "not a file of at most 64 KB", NULL); return VMCLI_USAGE; }
    char *t = malloc((size_t)st.st_size + 1);
    ssize_t n = t ? read(fd, t, (size_t)st.st_size) : -1;
    close(fd);
    McliApply *a = calloc(1, sizeof *a);
    int line;
    const char *why;
    if (n < 0 || !a || mcli_apply_parse(t, (size_t)n, a, &line, &why)) {
        char ln[16];
        snprintf(ln, sizeof ln, "%d", line);
        ERR("applyBad", "file", argv[0], "line", ln, "why", n < 0 || !a ? strerror(errno) : why, NULL);
        free(t); free(a);
        return VMCLI_USAGE;
    }
    free(t);
    /* the file came with a repository: an ISO only from the person's Downloads folder or the Machines library, resolved */
    if (a->iso[0]) {
        char real[PATH_MAX], dl[PATH_MAX + 16], dlr[PATH_MAX] = "", libr[PATH_MAX] = "";
        const char *home = getenv("HOME");
        struct stat ist;
        snprintf(dl, sizeof dl, "%s/Downloads", home ? home : "/nonexistent");
        if (!realpath(dl, dlr)) dlr[0] = 0;
        library_path();
        if (!realpath(library, libr)) libr[0] = 0;
        const char *roots[3] = { dlr[0] ? dlr : NULL, NULL, NULL };
        roots[dlr[0] ? 1 : 0] = libr[0] ? libr : NULL;
        if (!realpath(a->iso, real) || lstat(real, &ist) || !S_ISREG(ist.st_mode) || !mcli_iso_place_ok(real, roots)) {
            ERR("applyBad", "file", argv[0], "line", "0", "why", "iso must be a .iso or .img file in your Downloads folder or the Machines library", NULL);
            free(a);
            return VMCLI_USAGE;
        }
        if (strlen(real) >= sizeof a->iso) { ERR("applyBad", "file", argv[0], "line", "0", "why", "the iso path is too long", NULL); free(a); return VMCLI_USAGE; }
        memcpy(a->iso, real, strlen(real) + 1);
    }
    /* everything that crosses this computer's boundary asks the person on their terminal; no terminal, no change */
    {
        char q[4000];
        if (mcli_apply_risks(a, q, sizeof q) && !ask_tty(say("applyAskShares", "file", argv[0], "name", a->name, "list", q, NULL))) {
            ERR("applyNoShares", NULL);
            free(a);
            return VMCLI_FAILED;
        }
    }
    load_library();
    Machine *x = NULL;
    for (int i = 0; i < nms; i++) if (ms[i].ok && !strcasecmp(ms[i].m.name, a->name)) x = &ms[i];
    int rc = 0, made = 0;
    char cpus[8];
    snprintf(cpus, sizeof cpus, "%d", a->cpus);
    if (!x) {
        /* a new machine: lord-vm --create with what it takes, then --set with the rest */
        if (library_path() && mkdir(library, 0700) && errno != EEXIST) { ERR("opFailed", "name", a->name, "why", strerror(errno), NULL); free(a); return VMCLI_FAILED; }
        library_path();
        char folder[64], dst[PATH_MAX + 80];
        mcli_folder_name(a->name, folder, sizeof folder);
        snprintf(dst, sizeof dst, "%s/%s.lordvm", library, folder);
        for (int j = 2; !access(dst, F_OK) && j < 100; j++) snprintf(dst, sizeof dst, "%s/%s %d.lordvm", library, folder, j);
        char *c[20];
        int ck = 0;
        McliStore store;
        char *conv[16];
        const char *bo, *bv;
        /* sizes through `set`'s own conversion */
        char *sz[6];
        int szn = 0;
        if (a->memory[0]) { sz[szn++] = "--memory"; sz[szn++] = a->memory; }
        if (a->disk[0]) { sz[szn++] = "--disk"; sz[szn++] = a->disk; }
        if (a->cpus > 0) { sz[szn++] = "--cpus"; sz[szn++] = cpus; }
        if (szn && mcli_set_args(szn, sz, conv, 16, &store, &bo, &bv)) { ERR("badValue", "option", bo ? bo : "", "value", bv ? bv : "", NULL); free(a); return VMCLI_USAGE; }
        c[ck++] = NULL; c[ck++] = "--create"; c[ck++] = dst; c[ck++] = "--name"; c[ck++] = a->name;
        if (a->arch[0]) { c[ck++] = "--arch"; c[ck++] = a->arch; }
        for (int j = 0; szn && conv[j]; j++) c[ck++] = conv[j];
        if (a->network[0] && strcmp(a->network, "bridged")) { c[ck++] = "--network"; c[ck++] = !strcmp(a->network, "host-only") ? "hostonly" : a->network; }
        if (a->iso[0]) { c[ck++] = "--iso"; c[ck++] = a->iso; }
        c[ck] = NULL;
        Run r;
        int st2 = run_lordvm(c, &r);
        if (st2) { if (st2 > 0) ERR("opFailed", "name", a->name, "why", why_of(&r), NULL); free(a); return st2 < 0 ? VMCLI_FAILED : our_rc(st2); }
        SAY("applyCreated", "name", a->name, "file", argv[0], NULL);
        made = 1;
        load_library();
        for (int i = 0; i < nms; i++) if (ms[i].ok && !strcasecmp(ms[i].m.name, a->name)) x = &ms[i];
        if (!x) { free(a); return 0; }
    }
    /* the settings the file gives (for a machine just made, the ones --create didn't take) */
    char *so[80];
    int k = 0;
    if (a->memory[0] && !made) { so[k++] = "--memory"; so[k++] = a->memory; }
    if (a->cpus > 0 && !made) { so[k++] = "--cpus"; so[k++] = cpus; }
    if (a->disk[0] && !made) { so[k++] = "--disk"; so[k++] = a->disk; }
    if (a->network[0] && (!made || !strcmp(a->network, "bridged"))) { so[k++] = "--network"; so[k++] = a->network; }
    if (a->graphics[0]) { so[k++] = "--graphics"; so[k++] = a->graphics; }
    if (a->clipboard[0]) { so[k++] = "--clipboard"; so[k++] = a->clipboard; }
    if (a->agent >= 0) { so[k++] = "--agent"; so[k++] = a->agent ? "on" : "off"; }
    if (a->iso[0] && !made) { so[k++] = "--iso"; so[k++] = a->iso; }
    if (a->ports_set && !a->n_ports) { so[k++] = "--ports"; so[k++] = "none"; }
    for (int i = 0; i < a->n_ports; i++) { so[k++] = "--port"; so[k++] = a->ports[i]; }
    if (a->shares_set && !a->n_shares) { so[k++] = "--shares"; so[k++] = "none"; }
    for (int i = 0; i < a->n_shares; i++) { so[k++] = "--share"; so[k++] = a->shares[i]; }
    if (k) {
        so[k] = NULL;
        if (x->st.active) { ERR("applyRunning", "name", x->m.name, NULL); free(a); return VMCLI_FAILED; }
        rc = cmd_set(x, k, so);
        if (!rc) SAY("applyUpdated", "name", x->m.name, "file", argv[0], NULL);
    }
    free(a);
    return rc;
}

/* machines import SRC NAME [--arch A] [--devices compat|virtio] [--network shared|isolated] [--firmware uefi|bios]: a
 * machine of another virtualization app (6.22), as the app's Migrate tile does it (lord-vm --import-machine) */
static int cmd_import(int argc, char **argv) {
    if (argc < 2) { ERR("missing", "what", argc ? "NAME" : "SRC", NULL); return VMCLI_USAGE; }
    const char *src = argv[0], *name = argv[1];
    if (!vm_name_valid(name)) { ERR("badName", NULL); return VMCLI_USAGE; }
    char *a[24];
    int k = 0;
    char real[PATH_MAX];
    if (!realpath(src, real)) { ERR("noMachine", "name", src, NULL); return VMCLI_NO_MACHINE; }
    load_library();
    for (int i = 0; i < nms; i++) if (!strcasecmp(ms[i].m.name, name)) { ERR("nameTaken", "name", name, NULL); return VMCLI_FAILED; }
    if (library_path() && mkdir(library, 0700) && errno != EEXIST) { ERR("opFailed", "name", name, "why", strerror(errno), NULL); return VMCLI_FAILED; }
    library_path();
    char folder[64], dst[PATH_MAX + 80];
    mcli_folder_name(name, folder, sizeof folder);
    snprintf(dst, sizeof dst, "%s/%s.lordvm", library, folder);
    for (int j = 2; !access(dst, F_OK) && j < 100; j++) snprintf(dst, sizeof dst, "%s/%s %d.lordvm", library, folder, j);
    if (!access(dst, F_OK)) { ERR("nameTaken", "name", name, NULL); return VMCLI_FAILED; }
    a[k++] = NULL; a[k++] = "--import-machine"; a[k++] = real; a[k++] = dst; a[k++] = "--name"; a[k++] = (char *)name;
    static const char *const OPT[][3] = { { "--arch", "amd64", "arm64" }, { "--devices", "compat", "virtio" }, { "--network", "shared", "isolated" }, { "--firmware", "uefi", "bios" } };
    for (int i = 2; i < argc; i++) {
        int ok = 0;
        for (size_t o = 0; o < sizeof OPT / sizeof OPT[0]; o++)
            if (!strcmp(argv[i], OPT[o][0]) && i + 1 < argc && (!strcmp(argv[i + 1], OPT[o][1]) || !strcmp(argv[i + 1], OPT[o][2]))) { a[k++] = argv[i]; a[k++] = argv[++i]; ok = 1; break; }
        if (!ok) { ERR("badOption", "option", argv[i], NULL); return VMCLI_USAGE; }
    }
    a[k] = NULL;
    Run r;
    int st = run_lordvm(a, &r);
    if (st) { if (st > 0) ERR("opFailed", "name", name, "why", why_of(&r), NULL); return st < 0 ? VMCLI_FAILED : our_rc(st); }
    SAY("cloned", "name", name, "dir", dst, NULL);
    return 0;
}

/* ---- MCP (6.15): a local server over stdin and stdout, for a coding agent the person started ---- */

/* what a command printed while it ran, kept from the MCP channel (stdout) */
typedef struct { int saved1, saved2, mfd; } Capture;
static void cap_begin(Capture *c) {
    fflush(stdout); fflush(stderr);
    c->mfd = memfd_create("machines-mcp", MFD_CLOEXEC);
    c->saved1 = dup(1); c->saved2 = dup(2);
    if (c->mfd >= 0) { dup2(c->mfd, 1); dup2(c->mfd, 2); }
}
static char *cap_end(Capture *c) {
    fflush(stdout); fflush(stderr);
    dup2(c->saved1, 1); dup2(c->saved2, 2);
    close(c->saved1); close(c->saved2);
    char *t = NULL;
    if (c->mfd >= 0) {
        off_t n = lseek(c->mfd, 0, SEEK_END);
        if (n > 65536) n = 65536;
        t = malloc((size_t)(n > 0 ? n : 0) + 1);
        ssize_t k = t && n > 0 ? pread(c->mfd, t, (size_t)n, 0) : 0;
        if (t) t[k > 0 ? k : 0] = 0;
        close(c->mfd);
    }
    return t;
}

static FILE *mcp_out;

static void mcp_error(const char *id, int code, const char *msg) {
    fprintf(mcp_out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":", id && id[0] ? id : "null", code);
    mcli_json_str(mcp_out, msg);
    fputs("}}\n", mcp_out);
    fflush(mcp_out);
}

/* a tool's answer: text (bytes, made valid UTF-8) and whether it is an error */
static void mcp_result(const char *id, const uint8_t *text, size_t n, int is_error) {
    fprintf(mcp_out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":", id);
    mcli_json_bytes(mcp_out, text, n);
    fprintf(mcp_out, "}],\"isError\":%s}}\n", is_error ? "true" : "false");
    fflush(mcp_out);
}
static void mcp_text(const char *id, const char *t, int is_error) { mcp_result(id, (const uint8_t *)(t ? t : ""), t ? strlen(t) : 0, is_error); }

static const char MCP_TOOLS[] =
    "{\"tools\":["
    "{\"name\":\"list_machines\",\"description\":\"The person's machines in LordOS Machines: name, system, state, and whether automation is on (only those can be acted on).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
    "{\"name\":\"machine_status\",\"description\":\"One machine's state, resources and network.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"],\"additionalProperties\":false}},"
    "{\"name\":\"start_machine\",\"description\":\"Starts a machine without a window (through the person's LordOS session when it runs).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"],\"additionalProperties\":false}},"
    "{\"name\":\"stop_machine\",\"description\":\"Shuts a machine down (the guest is asked first, as Shut Down does).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"],\"additionalProperties\":false}},"
    "{\"name\":\"wait_until_ready\",\"description\":\"Waits until a machine runs and its guest agent can run a program (at most timeout seconds, 600 by default).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600}},\"required\":[\"name\"],\"additionalProperties\":false}},"
    "{\"name\":\"run_command\",\"description\":\"Runs a program in a Linux machine as its signed-in person (never as root), with optional standard input; returns its exit status, output and errors.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"command\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1,\"maxItems\":64},"
    "\"input\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600}},\"required\":[\"name\",\"command\"],\"additionalProperties\":false}},"
    "{\"name\":\"get_logs\",\"description\":\"The last lines of a machine's guest journal, serial console or supervisor log.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"which\":{\"type\":\"string\",\"enum\":[\"journal\",\"serial\",\"machine\"]},"
    "\"lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2000}},\"required\":[\"name\"],\"additionalProperties\":false}},"
    "{\"name\":\"clone_machine\",\"description\":\"Clones a machine that is off into a new one with its own identity.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"new_name\":{\"type\":\"string\"}},\"required\":[\"name\",\"new_name\"],\"additionalProperties\":false}}"
    "]}";

/* the arguments' keys must be among `allowed`; each a string unless listed in ints/arrays */
static int args_ok(const JVal *a, const char *const *allowed) {
    if (!a) return 1;
    if (a->type != J_OBJ) return 0;
    for (const JVal *k = a->kid; k; k = k->next) {
        int ok = 0;
        for (int i = 0; allowed[i]; i++) if (!strcmp(k->key, allowed[i])) ok = 1;
        if (!ok) return 0;
    }
    return 1;
}

/* the machine an MCP call names, only when its person turned automation on for it */
static Machine *mcp_machine(const char *id, const JVal *args) {
    const char *name = args ? json_str(args, "name") : NULL;
    if (!name || !mcli_mcp_name_ok(name)) { mcp_error(id, -32602, "name is a machine's name in the library"); return NULL; }
    Capture c;
    int rc;
    cap_begin(&c);
    Machine *x = find(name, &rc);
    char *t = cap_end(&c);
    if (!x) { mcp_text(id, t && t[0] ? t : "no such machine", 1); free(t); return NULL; }
    free(t);
    /* its folder: a real folder (no link) directly in the library, and its machine.json read again from there */
    char real[PATH_MAX], lib[PATH_MAX];
    struct stat st;
    const char *sl = realpath(x->dir, real) ? strrchr(real, '/') : NULL;
    int inside = sl && !lstat(x->dir, &st) && S_ISDIR(st.st_mode) && !library_path() && realpath(library, lib) &&
                 (size_t)(sl - real) == strlen(lib) && !strncmp(real, lib, strlen(lib)) && strlen(sl) > 8 && !strcmp(sl + strlen(sl) - 7, ".lordvm");
    if (!inside || vm_machine_load(real, &x->m, NULL, 0) != VM_OK) { mcp_text(id, "no such machine in the library", 1); return NULL; }
    if (!x->ok || !x->m.automation || x->m.profile_lock) {
        char m[400];
        snprintf(m, sizeof m, "%s", say("noAutomation", "name", x->m.name, NULL));
        mcp_text(id, m, 1);
        return NULL;
    }
    return x;
}

static void mcp_list(const char *id) {
    load_library();
    char *b = NULL;
    size_t bn = 0;
    FILE *f = open_memstream(&b, &bn);
    if (!f) { mcp_error(id, -32603, "out of memory"); return; }
    fputs("[", f);
    for (int i = 0; i < nms; i++) {
        const Machine *x = &ms[i];
        fprintf(f, "%s{\"name\":", i ? "," : "");
        mcli_json_str(f, x->m.name);
        fprintf(f, ",\"system\":");
        mcli_json_str(f, x->system);
        fprintf(f, ",\"state\":\"%s\",\"automation\":%s}", x->st.view, x->ok && x->m.automation && !x->m.profile_lock ? "true" : "false");
    }
    fputs("]", f);
    fclose(f);
    mcp_result(id, (const uint8_t *)b, bn, 0);
    free(b);
}

static void mcp_run(const char *id, Machine *x, const JVal *args) {
    const JVal *cmd = json_get(args, "command");
    const char *av[VMX_ARGC_MAX + 1];
    int n = 0;
    if (!cmd || cmd->type != J_ARR) { mcp_error(id, -32602, "command is a list of strings"); return; }
    for (const JVal *v = cmd->kid; v; v = v->next) {
        if (v->type != J_STR || !v->str[0] || strlen(v->str) != v->len || n >= VMX_ARGC_MAX) { mcp_error(id, -32602, "command is a list of 1 to 64 non-empty strings"); return; }
        av[n++] = v->str;
    }
    if (!n) { mcp_error(id, -32602, "command is empty"); return; }
    const JVal *in = json_get(args, "input"), *to = json_get(args, "timeout");
    if ((in && (in->type != J_STR || in->len > VMX_STDIN_MAX)) || (to && (to->type != J_NUM || to->num < 1 || to->num > VMX_TIMEOUT_MAX || to->num != (int)to->num))) {
        mcp_error(id, -32602, "input is a string of at most 1 MB, timeout 1 to 3600 seconds");
        return;
    }
    auto_log(x, "mcp: run_command %s, %d argument%s", prog_name(av[0]), n - 1, n == 2 ? "" : "s");
    VmxRes res;
    uint8_t *keep;
    char why[300];
    int code;
    if (vm_exec(x, to ? (unsigned)to->num : 600, n, av, in ? (const uint8_t *)in->str : NULL, in ? in->len : 0, &res, &keep, why, sizeof why, &code)) { mcp_text(id, why, 1); return; }
    char *b = NULL;
    size_t bn = 0;
    FILE *f = open_memstream(&b, &bn);
    if (f) {
        fprintf(f, "{\"exit\":%d,\"timed_out\":%s,\"output_cut\":%s,\"stdout\":", res.exit, res.flags & VMX_F_TIMEOUT ? "true" : "false",
                res.flags & (VMX_F_OUT_CUT | VMX_F_ERR_CUT) ? "true" : "false");
        mcli_json_bytes(f, res.out, res.outlen);
        fputs(",\"stderr\":", f);
        mcli_json_bytes(f, res.err, res.errlen);
        fputs("}", f);
        fclose(f);
        mcp_result(id, (const uint8_t *)b, bn, res.exit != 0);
        free(b);
    } else mcp_error(id, -32603, "out of memory");
    free(keep);
}

static void mcp_call(const char *id, const JVal *params) {
    const char *tool = params ? json_str(params, "name") : NULL;
    const JVal *args = params ? json_get(params, "arguments") : NULL;
    if (!tool) { mcp_error(id, -32602, "params.name is missing"); return; }
    if (args && args->type != J_OBJ) { mcp_error(id, -32602, "params.arguments is an object"); return; }
    static const char *const K_NAME[] = { "name", NULL }, *const K_WAIT[] = { "name", "timeout", NULL }, *const K_RUN[] = { "name", "command", "input", "timeout", NULL },
                             *const K_LOGS[] = { "name", "which", "lines", NULL }, *const K_CLONE[] = { "name", "new_name", NULL }, *const K_NONE[] = { NULL };
    if (!strcmp(tool, "list_machines")) { if (!args_ok(args, K_NONE)) { mcp_error(id, -32602, "no arguments"); return; } mcp_list(id); return; }
    const char *const *allowed = !strcmp(tool, "wait_until_ready") ? K_WAIT : !strcmp(tool, "run_command") ? K_RUN : !strcmp(tool, "get_logs") ? K_LOGS
                               : !strcmp(tool, "clone_machine") ? K_CLONE : K_NAME;
    if (strcmp(tool, "machine_status") && strcmp(tool, "start_machine") && strcmp(tool, "stop_machine") && strcmp(tool, "wait_until_ready") &&
        strcmp(tool, "run_command") && strcmp(tool, "get_logs") && strcmp(tool, "clone_machine")) { mcp_error(id, -32602, "no such tool"); return; }
    if (!args_ok(args, allowed)) { mcp_error(id, -32602, "unknown argument"); return; }
    Machine *x = mcp_machine(id, args);
    if (!x) return;
    if (!strcmp(tool, "run_command")) { mcp_run(id, x, args); return; }
    Capture c;
    int rc = 0;
    if (!strcmp(tool, "get_logs")) {
        const char *w = json_str(args, "which");
        const JVal *ln = json_get(args, "lines");
        if ((w && strcmp(w, "journal") && strcmp(w, "serial") && strcmp(w, "machine")) || (ln && (ln->type != J_NUM || ln->num < 1 || ln->num > 2000 || ln->num != (int)ln->num))) {
            mcp_error(id, -32602, "which is journal, serial or machine; lines 1 to 2000");
            return;
        }
        char *b;
        size_t n;
        auto_log(x, "mcp: get_logs %s", w ? w : "journal");
        if (log_tail(x, !w || !strcmp(w, "journal") ? "journal.log" : !strcmp(w, "serial") ? "serial.log" : "lord-vm.log", ln ? (long)ln->num : 100, &b, &n)) { mcp_text(id, say("noLog", "name", x->m.name, NULL), 1); return; }
        mcp_result(id, (const uint8_t *)b, n, 0);
        free(b);
        return;
    }
    if (!strcmp(tool, "wait_until_ready")) {
        const JVal *to = json_get(args, "timeout");
        if (to && (to->type != J_NUM || to->num < 1 || to->num > 3600 || to->num != (int)to->num)) { mcp_error(id, -32602, "timeout is 1 to 3600 seconds"); return; }
        long long end = (long long)time(NULL) + (to ? (long long)to->num : 600);
        const char *av[] = { "true" };
        while ((long long)time(NULL) < end) {
            VmxRes res;
            uint8_t *keep;
            char why[300];
            int code;
            if (!vm_exec(x, 30, 1, av, NULL, 0, &res, &keep, why, sizeof why, &code)) { free(keep); mcp_text(id, "ready", 0); return; }
            if (code == VME_DENIED || code == VME_BAD_REQUEST) { mcp_text(id, why, 1); return; }
            struct timespec ts = { 2, 0 };
            nanosleep(&ts, NULL);
        }
        mcp_text(id, "not ready in time", 1);
        return;
    }
    auto_log(x, "mcp: %s", tool);
    cap_begin(&c);
    if (!strcmp(tool, "machine_status")) rc = cmd_status(x, 1);
    else if (!strcmp(tool, "start_machine")) rc = cmd_start(x, 0);
    else if (!strcmp(tool, "stop_machine")) rc = cmd_power(x, "stop");
    else {
        const char *nn = json_str(args, "new_name");
        char *ca[] = { (char *)(nn ? nn : ""), NULL };
        rc = nn ? cmd_clone(x, 1, ca) : VMCLI_USAGE;
    }
    char *t = cap_end(&c);
    mcp_text(id, t, rc != 0);
    free(t);
}

/* machines mcp: JSON-RPC 2.0, one message per line on stdin, answers on stdout; nothing else is ever written there */
static int cmd_mcp(void) {
    int out = dup(1);
    mcp_out = out >= 0 ? fdopen(out, "w") : NULL;
    if (!mcp_out) return VMCLI_FAILED;
    /* anything else that would print goes to stderr, never into the protocol */
    dup2(2, 1);
    char *line = malloc(MCLI_RPC_MAX + 2);
    if (!line) return VMCLI_FAILED;
    for (;;) {
        size_t n = 0;
        int c, over = 0;
        while ((c = getchar()) != EOF && c != '\n') { if (n < MCLI_RPC_MAX) line[n++] = (char)c; else over = 1; }
        if (c == EOF && !n) break;
        if (n && line[n - 1] == '\r') n--;
        if (!n) continue;
        McliRpc r;
        if (over || mcli_rpc_parse(line, n, &r)) { mcp_error(NULL, -32700, "not a JSON-RPC 2.0 message"); continue; }
        const char *id = r.id;
        if (!r.has_id) { mcli_rpc_free(&r); continue; }           /* notifications: initialized, cancelled */
        if (!strcmp(r.method, "initialize")) {
            const char *pv = r.params ? json_str(r.params, "protocolVersion") : NULL;
            const char *ver = pv && (!strcmp(pv, "2024-11-05") || !strcmp(pv, "2025-03-26") || !strcmp(pv, "2025-06-18")) ? pv : "2025-06-18";
            fprintf(mcp_out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},"
                    "\"serverInfo\":{\"name\":\"lordos-machines\",\"version\":\"1\"}}}\n", id, ver);
            fflush(mcp_out);
        } else if (!strcmp(r.method, "ping")) {
            fprintf(mcp_out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}\n", id);
            fflush(mcp_out);
        } else if (!strcmp(r.method, "tools/list")) {
            fprintf(mcp_out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id, MCP_TOOLS);
            fflush(mcp_out);
        } else if (!strcmp(r.method, "tools/call")) {
            if (!r.params || r.params->type != J_OBJ) mcp_error(id, -32602, "params is an object");
            else mcp_call(id, r.params);
        } else mcp_error(id, -32601, "no such method");
        mcli_rpc_free(&r);
    }
    free(line);
    return 0;
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
    if (!strcmp(cmd, "cp")) return cmd_cp(argc - 2, argv + 2);
    if (!strcmp(cmd, "mcp")) { if (argc > 2) { ERR("unknown", "arg", argv[2], NULL); return VMCLI_USAGE; } return cmd_mcp(); }
    if (!strcmp(cmd, "apply")) return cmd_apply(argc - 2, argv + 2);
    if (!strcmp(cmd, "import")) return cmd_import(argc - 2, argv + 2);
    static const char *const KNOWN[] = { "status", "start", "open", "stop", "poweroff", "pause", "resume", "suspend", "reset", "snapshot", "clone", "rename", "set", "fresh",
                                         "exec", "logs", "console", "shell", NULL };
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
            if (strcmp(cmd, "status") && strcmp(cmd, "start") && strcmp(cmd, "open") && strcmp(cmd, "stop") && strcmp(cmd, "poweroff") && strcmp(cmd, "fresh") && strcmp(cmd, "rename")) { ERR("unknown", "arg", cmd, NULL); return VMCLI_USAGE; }
            return cmd_group(G, cmd, rest, rv);
        }
    }
    if (!strcmp(cmd, "fresh")) { ERR("freshNotGroup", "name", argv[2], NULL); return VMCLI_USAGE; }
    Machine *x = find(argv[2], &rc);
    if (!x) return rc;
    if (!strcmp(cmd, "status")) {
        if (rest > 1 || (rest == 1 && strcmp(rv[0], "--json"))) { ERR("unknown", "arg", rv[rest - 1], NULL); return VMCLI_USAGE; }
        return cmd_status(x, rest == 1);
    }
    if (!strcmp(cmd, "start")) {
        /* --snapshot TAG: from a snapshot with memory, without a window (lord-vm --loadvm, as the app's Snapshots window) */
        if (rest == 3 && !strcmp(rv[0], "--no-window") && !strcmp(rv[1], "--snapshot")) return cmd_start_snapshot(x, rv[2]);
        if (rest == 3 && !strcmp(rv[0], "--snapshot") && !strcmp(rv[2], "--no-window")) return cmd_start_snapshot(x, rv[1]);
        if (rest > 1 || (rest == 1 && strcmp(rv[0], "--no-window"))) { ERR("unknown", "arg", rv[rest - 1], NULL); return VMCLI_USAGE; }
        return cmd_start(x, rest == 0);
    }
    if (!strcmp(cmd, "snapshot")) return cmd_snapshot(x, rest, rv);
    if (!strcmp(cmd, "clone")) return cmd_clone(x, rest, rv);
    if (!strcmp(cmd, "rename")) return cmd_rename(x, rest, rv);
    if (!strcmp(cmd, "set")) return cmd_set(x, rest, rv);
    if (!strcmp(cmd, "exec")) return cmd_exec(x, rest, rv);
    if (!strcmp(cmd, "logs")) return cmd_logs(x, rest, rv);
    if (rest) { ERR("unknown", "arg", rv[0], NULL); return VMCLI_USAGE; }
    if (!strcmp(cmd, "open")) return cmd_open(x);
    if (!strcmp(cmd, "console")) return cmd_console(x, 0);
    if (!strcmp(cmd, "shell")) return cmd_console(x, 1);
    return cmd_power(x, cmd);
}
