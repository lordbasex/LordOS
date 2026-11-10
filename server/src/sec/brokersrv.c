/* brokersrv.c — lado servidor del broker (ver broker.h). Corre dentro de lord-server (el
 * compositor). Atiende los pedidos de cada app .lpk aislada, verifica los permisos
 * (perms.c) y, cuando hace falta consentimiento, le pide a la interfaz que dibuje el aviso del
 * sistema (nunca lo dibuja la app). */
#include "broker_int.h"
#include "perms.h"
#include "restrict.h"
#include "../fw/rules.h"
#include "../errcode.h"
#include "json.h"
#include "../lpk.h"
#include "../term/term.h"
#include "../media/media_int.h"
#include "../media/aring.h"
#include "../keyboard.h"
#include "../vault/vclient.h"
#include "../pkg/crypto.h"
#include "../app/app.h"
#include "../remote/share.h"
#include "pkgapp.h"
#include "statusitem.h"
#include "brokerclip.h"
#include "../wl/wl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_APPS 32
#define MAX_PENDING 32

typedef struct {
    int fd, pid, used, resync; char id[48]; MAudio *mic; int mic_ringfd; int vault, sys;
    int packaged, comp_pid, comp_starts, status_on, comp_link;
    double status_at;
} AppConn;
static AppConn apps[MAX_APPS];

#define COMP_STARTS 5                   /* a companion that keeps dying isn't started forever */
#define STATUS_GAP 0.1                  /* seconds between updates of a status item */
static int comp_dying[MAX_APPS];        /* killed, not reaped yet */

/* prompts waiting for the user's answer (ui id → app + fd + permission + the app's reqid) */
typedef struct { int used; uint32_t ui, reqid; int fd; char id[48]; Perm perm; } Pending;
static Pending pend[MAX_PENDING];

/* Ids shown to the system UI for permission prompts and file pickers. They are generated here,
 * never taken from the app's reqid: an app chooses its reqids, so using them would let it collide
 * with another app's prompt or with the keyring (0xC0000000 tag) and camera (0x80000000 tag)
 * ranges, and have the user's answer applied to a request it did not make. Range 1..0x3fffffff. */
static uint32_t ui_seq;
static uint32_t next_ui_id(void) { ui_seq = (ui_seq % 0x3fffffffu) + 1; return ui_seq; }

/* cola de eventos para JS (prompts a mostrar, cambios de indicador) */
typedef struct { char kind[16]; char json[4096]; } Evt;
static Evt evq[64];
static int nevt;

static void queue_evt(const char *kind, const char *fmt, ...) {
    if (nevt >= 64) return;
    Evt *e = &evq[nevt++];
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    va_list ap; va_start(ap, fmt); vsnprintf(e->json, sizeof e->json, fmt, ap); va_end(ap);
}

/* ---- estado publicado por el tema (tokens, idioma) y teclado: en vivo hacia las apps ---- */

#define STATE_MAX (64 * 1024)
static char last_theme[STATE_MAX], last_locale[4096];
static char sent_kb[512];

static void keyboard_json(char *out, size_t n) {
    char l[160], v[160];
    json_quote(l, sizeof l, kb_layouts() ? kb_layouts() : "");
    json_quote(v, sizeof v, kb_variants() ? kb_variants() : "");
    snprintf(out, n, "{\"layouts\":%s,\"variants\":%s,\"group\":%d}", l, v, kb_group());
}

/* Todo el estado en un JSON: { theme, locale, keyboard } (null lo que todavía no se publicó). */
static size_t state_json(char *out, size_t n) {
    char kb[512];
    keyboard_json(kb, sizeof kb);
    int r = snprintf(out, n, "{\"theme\":%s,\"locale\":%s,\"keyboard\":%s}",
                     last_theme[0] ? last_theme : "null", last_locale[0] ? last_locale : "null", kb);
    return r > 0 && (size_t)r < n ? (size_t)r : 0;
}

/* Evento sin pedido. Nunca bloquea: si el socket está lleno, la app queda para resincronizar. */
static int send_event(AppConn *a, const char *kind, const char *json, size_t len) {
    BrokerHdr h = { .reqid = 0, .op = BOP_EVENT, .status = 0, .len = (uint32_t)(strlen(kind) + 1 + len) };
    struct iovec iov[3] = { { &h, sizeof h }, { (void *)kind, strlen(kind) + 1 }, { (void *)json, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 3 };
    if (sendmsg(a->fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { a->resync = 1; return -1; }
    return 0;
}

int brokersrv_publish(const char *kind, const char *json, size_t len) {
    char *dst;
    size_t cap;
    if (!strcmp(kind, "theme")) { dst = last_theme; cap = sizeof last_theme; }
    else if (!strcmp(kind, "locale")) { dst = last_locale; cap = sizeof last_locale; }
    else return -1;
    if (len + 1 > cap) return -1;
    if (!strncmp(dst, json, len) && dst[len] == 0) return 0;          /* sin cambios */
    memcpy(dst, json, len);
    dst[len] = 0;
    for (int i = 0; i < MAX_APPS; i++) if (apps[i].used) send_event(&apps[i], kind, json, len);
    return 0;
}

static int send_reply_fd(int fd, uint32_t reqid, int status, const void *data, uint32_t len, int passfd);
static void clip_secret_tick(void);
static double mono_s(void);
static int send_reply(int fd, uint32_t reqid, int status, const void *data, uint32_t len);

/* picker (powerbox): avisos pendientes de que el usuario elija un archivo */
typedef struct { int used, save; uint32_t ui, reqid; int fd; char id[48]; } Pick;
static Pick picks[MAX_PENDING];

void brokersrv_pick_result(uint32_t ui, const char *path) {
    if (brokercam_pick_result(ui, path)) return;     /* a camera photo/video save */
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!picks[i].used || picks[i].ui != ui) continue;
        uint32_t reqid = picks[i].reqid;
        int f = -1;
        const char *name = path ? strrchr(path, '/') : NULL;
        name = name ? name + 1 : path;
        if (path && path[0] && restrict_blocks_pkg(picks[i].id, FW_R_NO_FILES)) path = NULL;   /* restricted meanwhile */
        if (path && path[0]) {
            /* lord-server abre el archivo con los permisos del usuario; la app recibe solo el fd */
            f = open(path, picks[i].save ? (O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC), 0644);
        }
        if (f >= 0) { send_reply_fd(picks[i].fd, reqid, BST_OK, name, (uint32_t)strlen(name), f); close(f); }
        else send_reply(picks[i].fd, reqid, BST_DENIED, NULL, 0);
        picks[i].used = 0;
        return;
    }
}

/* últimos menús publicados por una app (el tema los lee con brokersrv_menus) */
static char last_menu[32 * 1024], last_menu_app[48];
const char *brokersrv_menus(const char *app) { return !strcmp(app, last_menu_app) ? last_menu : "[]"; }

void brokersrv_menu_click(const char *app, const char *item) {
    char json[256], q[200];
    json_quote(q, sizeof q, item);
    snprintf(json, sizeof json, "{\"item\":%s}", q);
    for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && !strcmp(apps[i].id, app)) send_event(&apps[i], "menu", json, strlen(json));
}

static void status_off(AppConn *a) {
    if (!a->status_on) return;
    a->status_on = 0;
    queue_evt("status", "{\"app\":\"%s\",\"off\":true}", a->id);
}

void brokersrv_status_click(const char *app, const char *item) {
    if (!app || !item || !statusitem_id_ok(item)) return;
    char json[64];
    int n = snprintf(json, sizeof json, "{\"item\":\"%s\"}", item);
    for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && apps[i].status_on && !strcmp(apps[i].id, app)) send_event(&apps[i], "status", json, (size_t)n);
}

void brokersrv_revoke(const char *app, const char *perm) {
    char json[128];
    snprintf(json, sizeof json, "{\"perm\":\"%s\"}", perm);
    for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && !strcmp(apps[i].id, app)) send_event(&apps[i], "revoke", json, strlen(json));
    if (!strcmp(perm, "microphone")) for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].used && !strcmp(apps[i].id, app) && apps[i].mic) { maudio_close(apps[i].mic); apps[i].mic = NULL; }
    if (!strcmp(perm, "microphone")) brokercam_revoke_mic(app);
    if (!strcmp(perm, "camera")) brokercam_revoke(app);     /* kills its lord-media: the stream stops now */
    if (!strcmp(perm, "status-item")) for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && !strcmp(apps[i].id, app)) status_off(&apps[i]);
    if (!strcmp(perm, "camera") || !strcmp(perm, "microphone"))
        queue_evt("device", "{\"app\":\"%s\",\"kind\":\"%s\",\"on\":false}", app, perm);
}

/* fd 6 of a companion: 0 means none, so a connection's zeroed slot is right as it is */
static void comp_link_close(AppConn *a) {
    if (a->comp_link > 0) close(a->comp_link);
    a->comp_link = 0;
}

/* Companions are lord-server's children: gone ones are reaped here, never waited for. */
static void comp_tick(void) {
    for (int i = 0; i < MAX_APPS; i++) {
        if (comp_dying[i] > 0 && waitpid(comp_dying[i], NULL, WNOHANG) != 0) comp_dying[i] = 0;
        if (apps[i].used && apps[i].comp_pid > 0 && waitpid(apps[i].comp_pid, NULL, WNOHANG) != 0) {
            apps[i].comp_pid = 0;
            comp_link_close(&apps[i]);
        }
    }
}

void brokersrv_tick(void) {
    comp_tick();
    brokercam_tick();
    brokerkr_tick();
    brokermail_tick();
    clip_secret_tick();
    char kb[512];
    keyboard_json(kb, sizeof kb);
    int kb_changed = strcmp(kb, sent_kb) != 0;
    if (kb_changed) snprintf(sent_kb, sizeof sent_kb, "%s", kb);
    for (int i = 0; i < MAX_APPS; i++) {
        AppConn *a = &apps[i];
        if (!a->used) continue;
        if (a->resync) {
            static char st[STATE_MAX + 8192];
            size_t n = state_json(st, sizeof st);
            a->resync = 0;
            if (n) send_event(a, "state", st, n);
        } else if (kb_changed) send_event(a, "keyboard", kb, strlen(kb));
    }
}

/* Args waiting for an isolated app that is starting (brokersrv_open_args). */
#define OPEN_ARGS_MAX (16 * 1024)
static struct { char id[48]; char *json; double at; } open_wait[8];

static void send_open(AppConn *a, const char *json, size_t len, int new_window) {
    char *ev = malloc(len + 40);
    if (!ev) return;
    int n = snprintf(ev, len + 40, "{\"new\":%s,\"args\":%.*s}", new_window ? "true" : "false", (int)len, json);
    if (n > 0) send_event(a, "open", ev, (size_t)n);
    free(ev);
}

int brokersrv_open_args(const char *id, const char *json, size_t len, int new_window) {
    if (!id || !lpk_valid_id(id) || !json || !len || len > OPEN_ARGS_MAX) return -1;
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0)) return -1;
    int obj = d.root && d.root->type == J_OBJ;
    json_free(&d);
    if (!obj) return -1;
    for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && !strcmp(apps[i].id, id)) { send_open(&apps[i], json, len, new_window); return 0; }
    /* not running yet (it is being launched): keep it for a few seconds */
    int slot = 0;
    for (int i = 0; i < 8; i++) if (!open_wait[i].json || !strcmp(open_wait[i].id, id)) { slot = i; break; }
    free(open_wait[slot].json);
    open_wait[slot].json = malloc(len + 2);
    if (!open_wait[slot].json) return -1;
    open_wait[slot].json[0] = new_window ? '1' : '0';
    memcpy(open_wait[slot].json + 1, json, len);
    open_wait[slot].json[len + 1] = 0;
    snprintf(open_wait[slot].id, sizeof open_wait[slot].id, "%s", id);
    open_wait[slot].at = mono_s();
    return 0;
}

int brokersrv_add(int fd, const char *id, int pid) {
    for (int i = 0; i < MAX_APPS; i++) if (!apps[i].used) {
        apps[i] = (AppConn){ .fd = fd, .pid = pid, .used = 1 };
        snprintf(apps[i].id, sizeof apps[i].id, "%s", id);
        for (int k = 0; k < 8; k++) if (open_wait[k].json && !strcmp(open_wait[k].id, id)) {
            if (mono_s() - open_wait[k].at < 10)
                send_open(&apps[i], open_wait[k].json + 1, strlen(open_wait[k].json + 1), open_wait[k].json[0] == '1');
            free(open_wait[k].json);
            open_wait[k].json = NULL;
        }
        return 0;
    }
    return -1;
}

void brokersrv_set_packaged(int fd) { for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && apps[i].fd == fd) apps[i].packaged = 1; }
void brokersrv_set_system(int fd) { for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && apps[i].fd == fd) apps[i].sys = 1; }
int brokersrv_conn_system(int conn) { return conn >= 0 && conn < MAX_APPS && apps[conn].used && apps[conn].sys; }
void brokersrv_shell_event(const char *kind, const char *json) { queue_evt(kind, "%s", json); }

/* The app is already running: ask its process for another window (Dock → «New window»). */
int brokersrv_new_window(const char *id) {
    static const char json[] = "{\"new\":true}";
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].used && !strcmp(apps[i].id, id)) return send_event(&apps[i], "window", json, sizeof json - 1) ? -1 : 0;
    return -1;
}

int brokersrv_quit(const char *id) {
    static const char json[] = "{}";
    int sent = -1;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].used && !strcmp(apps[i].id, id) && !send_event(&apps[i], "quit", json, sizeof json - 1)) sent = 0;
    return sent;
}

void brokersrv_paste_gesture(const char *app) { bclip_paste_gesture(app, mono_s()); }

const char *brokersrv_app_of_pid(int pid) {
    if (pid <= 0) return NULL;
    for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && apps[i].pid == pid) return apps[i].id;
    return NULL;
}

void brokersrv_set_vault(int fd) { for (int i = 0; i < MAX_APPS; i++) if (apps[i].used && apps[i].fd == fd) apps[i].vault = 1; }

static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* The clipboard's generation: any copy, text or picture, changes it (brokerclip.h). */
static unsigned clip_gen(void) { return term_clipboard_gen() * 2654435761u + wl_clip_image_gen() * 40503u + 1; }

/* A secret an isolated app put on the clipboard is cleared on time if nothing replaced it (brokerclip.c). */
static void clip_secret_tick(void) {
    int due = bclip_due(clip_gen(), term_clipboard_gen(), mono_s());
    if (due == BCLIP_WIPE_TEXT) term_clipboard_wipe();      /* a picture came after it: it stays */
    if (due != BCLIP_CLEAR_ALL) return;
    term_set_clipboard("", 0);
    term_clipboard_mark("lordos", 0);
    bclip_cleared(clip_gen());
}

/* Hands the companion of connection `conn` one end of a live link (media/live.h). Never waits: a companion that isn't
 * reading loses the stream, nothing else. */
int brokersrv_conn_has_link(int conn) { return conn >= 0 && conn < MAX_APPS && apps[conn].used && apps[conn].comp_link > 0; }

int brokersrv_conn_link(int conn, const char *msg, int passfd) {
    if (conn < 0 || conn >= MAX_APPS || !apps[conn].used || apps[conn].comp_link <= 0) return -1;
    struct iovec iov = { (void *)msg, strlen(msg) };
    char cmsg[CMSG_SPACE(sizeof(int))];
    memset(cmsg, 0, sizeof cmsg);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cmsg, .msg_controllen = sizeof cmsg };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &passfd, sizeof(int));
    return sendmsg(apps[conn].comp_link, &m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 ? -1 : 0;
}

int brokersrv_count(void) { int n = 0; for (int i = 0; i < MAX_APPS; i++) n += apps[i].used; return n; }

static void drop(AppConn *a) {
    brokercam_conn_closed((int)(a - apps));
    brokerkr_conn_closed((int)(a - apps));
    brokermail_conn_closed((int)(a - apps));
    /* its open prompts and pickers die with it: their fd may be reused by another app */
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pend[i].used && pend[i].fd == a->fd) pend[i].used = 0;
        if (picks[i].used && picks[i].fd == a->fd) picks[i].used = 0;
    }
    /* the companion stops with its app: the end of its socket is a request, this isn't */
    if (a->comp_pid > 0) {
        kill(a->comp_pid, SIGKILL);
        int i = (int)(a - apps);
        if (comp_dying[i] > 0) waitpid(comp_dying[i], NULL, 0);    /* just killed: immediate */
        comp_dying[i] = a->comp_pid;
        a->comp_pid = 0;
    }
    comp_link_close(a);
    status_off(a);
    /* los indicadores de esta app se apagan */
    queue_evt("device", "{\"app\":\"%s\",\"off\":true}", a->id);
    if (a->mic) { maudio_close(a->mic); a->mic = NULL; }
    close(a->fd);
    a->used = 0;
}

static int send_reply_fd(int fd, uint32_t reqid, int status, const void *data, uint32_t len, int passfd) {
    BrokerHdr h = { .reqid = reqid, .op = 0, .status = (uint16_t)status, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)data, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    char cmsg[CMSG_SPACE(sizeof(int))];
    if (passfd >= 0) {
        memset(cmsg, 0, sizeof cmsg);
        m.msg_control = cmsg;
        m.msg_controllen = sizeof cmsg;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &passfd, sizeof(int));
    }
    return sendmsg(fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 ? -1 : 0;     /* nunca esperar a una app */
}
static int send_reply(int fd, uint32_t reqid, int status, const void *data, uint32_t len) {
    return send_reply_fd(fd, reqid, status, data, len, -1);
}

/* Ruta segura dentro de la carpeta de datos de la app. */
static int data_path(const char *id, const char *rel, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !rel || rel[0] == '/' || strstr(rel, "..")) return -1;
    char base[PATH_MAX], real[PATH_MAX];
    if ((size_t)snprintf(base, sizeof base, "%s/.local/share/lordos/appdata/%s", home, id) >= sizeof base) return -1;
    if ((size_t)snprintf(out, n, "%s/%s", base, rel) >= n) return -1;
    /* el padre tiene que caer dentro de base (sin symlinks que escapen) */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", out);
    char *slash = strrchr(tmp, '/');
    if (slash) { *slash = 0; if (realpath(tmp, real) && strncmp(real, base, strlen(base))) return -1; }
    return 0;
}

/* A firewall restriction (Settings → Privacy → "Apps' network") covers this request: it is
 * refused without a prompt, whatever the permission says, and the reply carries the code line
 * ("fw.restricted.camera app=<id>") so the app can say why. See restrict.h. */
static int deny_restricted(int fd, uint32_t reqid, const char *id, uint32_t flag) {
    if (!restrict_blocks_pkg(id, flag)) return 0;
    char line[160];
    lerr_set(line, sizeof line, restrict_code(flag), "app", id, NULL);
    fprintf(stderr, "broker[%s]: refused by a restriction (%s)\n", id, fw_flag_name(flag));
    send_reply(fd, reqid, BST_DENIED, line, (uint32_t)strlen(line));
    return 1;
}

/* ---- System Information's sandbox probe (BOP_PROBE) ---- */

static char probe_json[12288];
static int probe_have;

/* "Key:\tvalue" of /proc/<pid>/status, the first word, [0-9a-f] only */
static void status_field(const char *st, const char *key, char *out, size_t n) {
    out[0] = 0;
    const char *p = st ? strstr(st, key) : NULL;
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    size_t o = 0;
    while (o + 1 < n && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) out[o++] = *p++;
    out[o] = 0;
}

static void probe_store(int pid, const char *payload, uint32_t len) {
    char path[64], st[8192] = "", cg[512] = "", nnp[8], sec[8], cap[24], mem[32] = "";
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    int f = pid > 0 ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (f >= 0) { ssize_t n = read(f, st, sizeof st - 1); st[n > 0 ? n : 0] = 0; close(f); }
    status_field(st, "\nNoNewPrivs:", nnp, sizeof nnp);
    status_field(st, "\nSeccomp:", sec, sizeof sec);
    status_field(st, "\nCapEff:", cap, sizeof cap);
    snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    f = pid > 0 ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (f >= 0) { ssize_t n = read(f, cg, sizeof cg - 1); cg[n > 0 ? n : 0] = 0; close(f); }
    char *c = strstr(cg, "0::");
    char scope[256] = "";
    if (c) {
        c += 3;
        size_t k = strcspn(c, "\n");
        if (k < sizeof scope && !memmem(c, k, "..", 2)) { memcpy(scope, c, k); scope[k] = 0; }
    }
    for (char *q = scope; *q; q++) if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20) *q = '_';
    if (scope[0] == '/') {
        char mp[400];
        snprintf(mp, sizeof mp, "/sys/fs/cgroup%s/memory.max", scope);
        f = open(mp, O_RDONLY | O_CLOEXEC);
        if (f >= 0) { ssize_t n = read(f, mem, sizeof mem - 1); mem[n > 0 ? n : 0] = 0; mem[strcspn(mem, "\n")] = 0; close(f); }
        for (char *q = mem; *q; q++) if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z'))) *q = '_';
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int k = snprintf(probe_json, sizeof probe_json,
                     "{\"probe\":%.*s,\"proc\":{\"noNewPrivs\":\"%s\",\"seccomp\":\"%s\",\"capEff\":\"%s\",\"cgroup\":\"%s\",\"memoryMax\":\"%s\"},\"at\":%lld}",
                     (int)len, payload, nnp, sec, cap, scope, mem, (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    probe_have = k > 0 && (size_t)k < sizeof probe_json;
}

const char *brokersrv_probe_result(void) { return probe_have ? probe_json : NULL; }

/* The app asks; everything else comes from what lord-server knows about this connection. The package is read here
 * only for what it declares: checking it takes time, so that happens in the child, before anything of it runs. */
static void companion_start(AppConn *a, uint32_t reqid) {
    int fd = a->fd;
    PkgManifest m;
    char dir[PATH_MAX];
    if (!a->packaged || pkgapp_dir(a->id, dir, sizeof dir) || pkgapp_manifest(dir, a->id, &m) || !m.companion[0]) {
        send_reply(fd, reqid, BST_DENIED, NULL, 0);
        return;
    }
    /* it reads the home folder: the same restriction as the file dialog */
    if (deny_restricted(fd, reqid, a->id, FW_R_NO_FILES)) return;
    if (a->comp_pid > 0 || a->comp_starts >= COMP_STARTS) { send_reply(fd, reqid, BST_ERROR, NULL, 0); return; }
    int share = -1;
    if (m.screen && (share = share_companion(a->id, m.input)) < 0) { send_reply(fd, reqid, BST_ERROR, NULL, 0); return; }
    int appfd = -1, link = -1, net = m.network && perm_get(a->id, PERM_NETWORK) == PERM_ALLOW;
    int pid = app_companion_spawn(a->id, net, share, &appfd, &link);
    if (pid < 0) { send_reply(fd, reqid, BST_ERROR, NULL, 0); return; }
    comp_link_close(a);
    a->comp_link = link;
    a->comp_pid = pid;
    a->comp_starts++;
    send_reply_fd(fd, reqid, BST_OK, NULL, 0, appfd);
    close(appfd);
}

static void status_set(AppConn *a, uint32_t reqid, const char *payload, uint32_t len) {
    int fd = a->fd;
    if (!len) { status_off(a); send_reply(fd, reqid, BST_OK, NULL, 0); return; }
    if (perm_get(a->id, PERM_STATUS) != PERM_ALLOW) { send_reply(fd, reqid, BST_DENIED, NULL, 0); return; }
    double t = mono_s();
    if (t - a->status_at < STATUS_GAP) { send_reply(fd, reqid, BST_ERROR, NULL, 0); return; }
    static char out[3800];
    if (statusitem_clean(a->id, payload, len, out, sizeof out)) { send_reply(fd, reqid, BST_ERROR, NULL, 0); return; }
    a->status_at = t;
    a->status_on = 1;
    queue_evt("status", "%s", out);
    send_reply(fd, reqid, BST_OK, NULL, 0);
}

static void handle(AppConn *a, BrokerHdr *h, char *payload, int *fds, int nfds) {
    int fd = a->fd;
    if (h->op == BOP_MAIL) { brokermail_handle((int)(a - apps), fd, h->reqid, a->id, payload, h->len, fds, nfds); return; }
    for (int i = 0; i < nfds; i++) if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }   /* no other request carries fds */
    switch (h->op) {
    case BOP_SYS_OPEN:
        brokermail_sys_open((int)(a - apps), fd, h->reqid, a->id, payload, h->len);
        break;
    case BOP_FS_HOME: {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/.local/share/lordos/appdata/%s", getenv("HOME"), a->id);
        send_reply(fd, h->reqid, BST_OK, p, (uint32_t)strlen(p));
        break;
    }
    case BOP_FS_READ: {
        char path[PATH_MAX];
        if (data_path(a->id, payload, path, sizeof path)) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }
        int f = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st;
        if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode) || st.st_size > BROKER_MAX_MSG) { if (f >= 0) close(f); send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
        char *buf = malloc((size_t)st.st_size);
        ssize_t got = buf ? read(f, buf, (size_t)st.st_size) : -1;
        close(f);
        if (got == st.st_size) send_reply(fd, h->reqid, BST_OK, buf, (uint32_t)got); else send_reply(fd, h->reqid, BST_ERROR, NULL, 0);
        free(buf);
        break;
    }
    case BOP_FS_WRITE: {
        char *nul = memchr(payload, 0, h->len);
        if (!nul) { send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
        char path[PATH_MAX];
        if (data_path(a->id, payload, path, sizeof path)) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }
        const char *data = nul + 1;
        size_t dlen = h->len - (size_t)(nul + 1 - payload);
        int f = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        int ok = f >= 0 && write(f, data, dlen) == (ssize_t)dlen;
        if (f >= 0) close(f);
        send_reply(fd, h->reqid, ok ? BST_OK : BST_ERROR, NULL, 0);
        break;
    }
    case BOP_FS_LIST: {
        /* [{ "name", "dir" }] de una carpeta dentro de la carpeta de datos de la app */
        char path[PATH_MAX];
        const char *rel = payload[0] && strcmp(payload, ".") ? payload : "";
        if (rel[0] ? data_path(a->id, rel, path, sizeof path) : data_path(a->id, ".", path, sizeof path)) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }
        DIR *d = opendir(path);
        if (!d) { send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
        size_t cap = 64 * 1024, len = 0;
        char *out = malloc(cap);
        if (!out) { closedir(d); send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
        out[len++] = '[';
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) && len + 600 < cap && n < 2000) {
            if (e->d_name[0] == '.') continue;
            char q[560];
            json_quote(q, sizeof q, e->d_name);
            len += (size_t)snprintf(out + len, cap - len, "%s{\"name\":%s,\"dir\":%s}", n ? "," : "", q, e->d_type == DT_DIR ? "true" : "false");
            n++;
        }
        closedir(d);
        out[len++] = ']';
        send_reply(fd, h->reqid, BST_OK, out, (uint32_t)len);
        free(out);
        break;
    }
    case BOP_CLIP_GET: {
        if (deny_restricted(fd, h->reqid, a->id, FW_R_NO_CLIPBOARD)) break;
        /* the clipboard permission, or, while the person hasn't decided it, a paste they just asked for in this app's
         * window (brokerclip.h); a permission they denied stays denied */
        PermState cst = restrict_perm_pkg(a->id, PERM_CLIPBOARD);
        if (cst != PERM_ALLOW && !(cst == PERM_ASK && bclip_paste_take(a->id, mono_s()))) {
            send_reply(fd, h->reqid, BST_DENIED, NULL, 0);
            break;
        }
        const char *txt = term_clipboard();
        send_reply(fd, h->reqid, BST_OK, txt ? txt : "", txt ? (uint32_t)strlen(txt) : 0);
        break;
    }
    case BOP_CLIP_SET:
        term_set_clipboard(payload, h->len);
        term_clipboard_mark(a->id, 0);
        bclip_copied(a->id, clip_gen(), term_clipboard_gen(), 0, 0, mono_s());
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    case BOP_CLIP_STATUS: {
        /* only about the app's own copy and only for a short while (brokerclip.h), never the clipboard's text:
         * "mine <seconds left>" | "cleared" | "replaced" | "none"; payload "keep": its secret stays. A firewall
         * restriction on reading the clipboard covers it too */
        if (deny_restricted(fd, h->reqid, a->id, FW_R_NO_CLIPBOARD)) break;
        unsigned g = clip_gen();
        double now = mono_s();
        if (h->len == 4 && !memcmp(payload, "keep", 4)) bclip_keep(a->id, g, now);
        int left = 0, st = bclip_status(a->id, g, now, &left);
        char out[32];
        int n = st == BCLIP_MINE ? snprintf(out, sizeof out, "mine %d", left)
                                 : snprintf(out, sizeof out, "%s", st == BCLIP_CLEARED ? "cleared" : st == BCLIP_REPLACED ? "replaced" : "none");
        send_reply(fd, h->reqid, BST_OK, out, (uint32_t)n);
        break;
    }
    case BOP_PERM: {
        int p = perm_from_name(payload);
        if (p < 0 || p == PERM_SHORTCUTS) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }   /* never an isolated app's */
        uint32_t rf = restrict_flag_of((Perm)p);
        if (rf && restrict_blocks_pkg(a->id, rf)) {
            /* restricted: "no", without a prompt; the code line follows the "0" */
            char line[160] = "0 ";
            lerr_set(line + 2, sizeof line - 2, restrict_code(rf), "app", a->id, NULL);
            fprintf(stderr, "broker[%s]: permission %s refused by a restriction\n", a->id, payload);
            send_reply(fd, h->reqid, BST_OK, line, (uint32_t)strlen(line));
            break;
        }
        PermState st = perm_get(a->id, (Perm)p);
        fprintf(stderr, "broker[%s]: permission %s = %s\n", a->id, payload, st == PERM_ALLOW ? "allow" : st == PERM_DENY ? "deny" : "ask (prompt)");
        if (st == PERM_ALLOW) { send_reply(fd, h->reqid, BST_OK, "1", 1); break; }
        if (st == PERM_DENY) { send_reply(fd, h->reqid, BST_OK, "0", 1); break; }
        /* preguntar: el aviso lo dibuja el sistema; se responde cuando el usuario decide */
        for (int i = 0; i < MAX_PENDING; i++) if (!pend[i].used) {
            pend[i] = (Pending){ .used = 1, .ui = next_ui_id(), .reqid = h->reqid, .fd = fd, .perm = (Perm)p };
            snprintf(pend[i].id, sizeof pend[i].id, "%s", a->id);
            queue_evt("prompt", "{\"reqid\":%u,\"app\":\"%s\",\"perm\":\"%s\"}", pend[i].ui, a->id, perm_name((Perm)p));
            return;
        }
        send_reply(fd, h->reqid, BST_DENIED, NULL, 0);
        break;
    }
    case BOP_DEVICE: {
        /* "camera on" / "microphone off": solo si tiene el permiso */
        char kind[16] = ""; int on = 0;
        sscanf(payload, "%15s %d", kind, &on);
        int p = perm_from_name(kind);
        if (p >= 0 && restrict_perm_pkg(a->id, (Perm)p) == PERM_ALLOW)
            queue_evt("device", "{\"app\":\"%s\",\"kind\":\"%s\",\"on\":%s}", a->id, kind, on ? "true" : "false");
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    }
    case BOP_PICK: {
        /* payload: "save\0suggestedName"; the dialog is drawn by lord-server, never by the app */
        if (deny_restricted(fd, h->reqid, a->id, FW_R_NO_FILES)) break;
        int save = payload[0] == '1';
        const char *suggest = memchr(payload, 0, h->len);
        suggest = suggest ? suggest + 1 : "";
        for (int i = 0; i < MAX_PENDING; i++) if (!picks[i].used) {
            picks[i] = (Pick){ .used = 1, .save = save, .ui = next_ui_id(), .reqid = h->reqid, .fd = fd };
            snprintf(picks[i].id, sizeof picks[i].id, "%s", a->id);
            char sq[300];
            json_quote(sq, sizeof sq, suggest);
            queue_evt("pick", "{\"reqid\":%u,\"app\":\"%s\",\"save\":%s,\"suggest\":%s}", picks[i].ui, a->id, save ? "true" : "false", sq);
            return;
        }
        send_reply(fd, h->reqid, BST_DENIED, NULL, 0);
        break;
    }
    case BOP_MENU:
        queue_evt("menu", "{\"app\":\"%s\"}", a->id);          /* los menús van aparte (payload grande) */
        /* guardamos el JSON de los menús para que el tema lo lea */
        {
            size_t n = h->len < sizeof last_menu - 1 ? h->len : sizeof last_menu - 1;
            memcpy(last_menu, payload, n); last_menu[n] = 0;
            snprintf(last_menu_app, sizeof last_menu_app, "%s", a->id);
        }
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    case BOP_TEXT_FOCUS: {
        int on = 0, y = 0, fh = 0;
        if (sscanf(payload, "%d %d %d", &on, &y, &fh) == 3 && y >= -1 && y <= 32767 && fh >= 0 && fh <= 32767)
            queue_evt("textfocus", "{\"app\":\"%s\",\"on\":%s,\"y\":%d,\"h\":%d}", a->id, on ? "true" : "false", y, fh);
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    }
    case BOP_STATE: {
        static char st[STATE_MAX + 8192];
        size_t n = state_json(st, sizeof st);
        send_reply(fd, h->reqid, n ? BST_OK : BST_ERROR, st, (uint32_t)n);
        break;
    }
    case BOP_MIC: {
        int want = payload[0] == '1';
        if (!want) { if (a->mic) { maudio_close(a->mic); a->mic = NULL; } send_reply(fd, h->reqid, BST_OK, NULL, 0); break; }
        if (deny_restricted(fd, h->reqid, a->id, FW_R_NO_MIC)) break;
        if (perm_get(a->id, PERM_MICROPHONE) != PERM_ALLOW) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }
        if (!a->mic) {
            int rfd = -1;
            a->mic = maudio_open(1, a->id, a->id, NULL, &rfd);
            if (!a->mic) { send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
            a->mic_ringfd = rfd;
        }
        /* fd de SOLO LECTURA del anillo: la app solo lo puede mapear PROT_READ (no toca rpos) */
        char proc[64];
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", a->mic_ringfd);
        int ro = open(proc, O_RDONLY | O_CLOEXEC);
        if (ro < 0) { send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
        /* tamaño y formato del anillo (datos de confianza del servidor, no de la cabecera) */
        char info[64];
        int n = snprintf(info, sizeof info, "%u %u %u", 1u << 18, (unsigned)AR_RATE, 1u);
        send_reply_fd(fd, h->reqid, BST_OK, info, (uint32_t)n, ro);
        close(ro);
        break;
    }
    case BOP_CLIP_SECRET: {
        /* "seconds\0text": sensitive copy (Passwords), cleared later if unchanged */
        char *nul = memchr(payload, 0, h->len);
        if (!nul) { send_reply(fd, h->reqid, BST_ERROR, NULL, 0); break; }
        int secs = atoi(payload);
        const char *txt = nul + 1;
        size_t tl = h->len - (size_t)(txt - payload);
        term_set_clipboard(txt, tl);
        term_clipboard_mark(a->id, 1);
        bclip_copied(a->id, clip_gen(), term_clipboard_gen(), 1, secs, mono_s());
        explicit_bzero(payload, h->len);
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    }
    case BOP_PROBE: {
        /* System Information's sandbox probe (app/probe.c): only the system package `sysprobe`, a
         * bounded JSON payload; lord-server adds what only it can read about that process */
        JDoc doc;
        if (!a->sys || strcmp(a->id, "sysprobe") || !h->len || h->len > 8192 || json_parse(payload, h->len, &doc, NULL, 0)) {
            send_reply(fd, h->reqid, BST_DENIED, NULL, 0);
            break;
        }
        json_free(&doc);
        probe_store(a->pid, payload, h->len);
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    }
    case BOP_VAULT_FIDO: {
        /* only the system Passwords app (spawned with a vault channel), only with `usb` allowed; the
         * hidraw fd goes from here straight to lord-vault */
        if (!a->vault) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }
        if (deny_restricted(fd, h->reqid, a->id, FW_R_NO_USB)) break;
        if (perm_get(a->id, PERM_USB) != PERM_ALLOW) { send_reply(fd, h->reqid, BST_DENIED, NULL, 0); break; }
        char name[128] = "";
        int r = vc_fido_attach(name, sizeof name);
        if (r) send_reply(fd, h->reqid, BST_ERROR, r == -1 ? "no-device" : "no-access", r == -1 ? 9 : 9);
        else send_reply(fd, h->reqid, BST_OK, name, (uint32_t)strlen(name));
        break;
    }
    case BOP_COMPANION:
        companion_start(a, h->reqid);
        break;
    case BOP_STATUS:
        status_set(a, h->reqid, payload, h->len);
        break;
    case BOP_KEYRING:
        brokerkr_handle((int)(a - apps), fd, h->reqid, a->id, payload, h->len);
        break;
    case BOP_QUIT:
        send_reply(fd, h->reqid, BST_OK, NULL, 0);
        break;
    case BOP_CAMERA:
        brokercam_handle((int)(a - apps), fd, h->reqid, payload, h->len);
        break;
    default:
        send_reply(fd, h->reqid, BST_ERROR, NULL, 0);
        break;
    }
}

void brokersrv_resolve(uint32_t ui, int allow) {
    if (brokerkr_resolve(ui, allow)) return;
    for (int i = 0; i < MAX_PENDING; i++) if (pend[i].used && pend[i].ui == ui) {
        perm_set(pend[i].id, pend[i].perm, allow ? PERM_ALLOW : PERM_DENY);
        if (allow && restrict_perm_pkg(pend[i].id, pend[i].perm) != PERM_ALLOW) allow = 0;   /* restricted meanwhile */
        send_reply(pend[i].fd, pend[i].reqid, BST_OK, allow ? "1" : "0", 1);
        pend[i].used = 0;
        return;
    }
}

int brokersrv_pollfds(struct pollfd *pfd, int max) {
    int n = 0;
    for (int i = 0; i < MAX_APPS && n < max; i++) if (apps[i].used) {
        pfd[n].fd = apps[i].fd;
        pfd[n].events = POLLIN | (brokermail_wants_write(i) ? POLLOUT : 0);   /* mail replies still to send */
        pfd[n].revents = 0;
        n++;
    }
    int mf = brokermail_pollfd();                /* lord-maild answered an isolated app */
    if (mf >= 0 && n < max) { pfd[n].fd = mf; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
    /* last: wakes the loop when lord-vault answers a keyring request (brokersrv_tick reads it) */
    int kf = brokerkr_pollfd();
    if (kf >= 0 && n < max) { pfd[n].fd = kf; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
    return n;
}

void brokersrv_dispatch(struct pollfd *pfd, int n) {
    int k = 0;
    for (int i = 0; i < MAX_APPS && k < n; i++) {
        if (!apps[i].used) continue;
        struct pollfd *p = &pfd[k++];
        if (!(p->revents & (POLLIN | POLLHUP | POLLERR))) continue;
        BrokerHdr h;
        char *buf = malloc(BROKER_MAX_MSG + 1);
        if (!buf) continue;
        struct iovec iov[2] = { { &h, sizeof h }, { buf, BROKER_MAX_MSG } };
        /* up to 16 fds (files for lord-maild, BOP_MAIL only; closed for anything else) */
        char cbuf[CMSG_SPACE(16 * sizeof(int))];
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        ssize_t r = recvmsg(apps[i].fd, &m, MSG_CMSG_CLOEXEC);
        int fds[16], nfds = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int j = 0; j < k; j++) {
                    int f;
                    memcpy(&f, CMSG_DATA(c) + j * sizeof(int), sizeof f);
                    if (nfds < 16) fds[nfds++] = f; else close(f);
                }
            }
        if (r < (ssize_t)sizeof h || (m.msg_flags & MSG_CTRUNC)) { for (int j = 0; j < nfds; j++) close(fds[j]); free(buf); drop(&apps[i]); continue; }
        uint32_t len = (uint32_t)r - sizeof h;
        if (h.len != len || len > BROKER_MAX_MSG) { for (int j = 0; j < nfds; j++) close(fds[j]); free(buf); drop(&apps[i]); continue; }
        buf[len] = 0;
        handle(&apps[i], &h, buf, fds, nfds);
        for (int j = 0; j < nfds; j++) if (fds[j] >= 0) close(fds[j]);
        free(buf);
    }
}

int brokersrv_audio_pollfds(struct pollfd *pfd, int max) {
    int n = 0;
    for (int i = 0; i < MAX_APPS && n < max; i++) {
        short ev;
        int f = apps[i].used && apps[i].mic ? maudio_pollfd(apps[i].mic, &ev) : -1;
        if (f >= 0) { pfd[n].fd = f; pfd[n].events = ev; pfd[n].revents = 0; n++; }
    }
    return n;
}
void brokersrv_audio_dispatch(struct pollfd *pfd, int n) {
    int k = 0;
    for (int i = 0; i < MAX_APPS && k < n; i++) {
        if (!apps[i].used || !apps[i].mic) continue;
        short ev;
        int f = maudio_pollfd(apps[i].mic, &ev);
        if (f >= 0 && (pfd[k].revents || 1)) { maudio_service(apps[i].mic); maudio_mic_drain(apps[i].mic); }
        if (f >= 0) k++;
    }
}

/* ---- for brokercam.c (broker_int.h) ---- */

const char *brokersrv_conn_app(int conn) { return conn >= 0 && conn < MAX_APPS && apps[conn].used ? apps[conn].id : NULL; }

int brokersrv_conn_event(int conn, const char *kind, const char *data, size_t len) {
    if (conn < 0 || conn >= MAX_APPS || !apps[conn].used) return -1;
    BrokerHdr h = { .reqid = 0, .op = BOP_EVENT, .status = 0, .len = (uint32_t)(strlen(kind) + 1 + len) };
    struct iovec iov[3] = { { &h, sizeof h }, { (void *)kind, strlen(kind) + 1 }, { (void *)data, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 3 };
    return sendmsg(apps[conn].fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 ? -1 : 0;   /* no resync for frames */
}

int brokersrv_reply(int fd, uint32_t reqid, int status, const void *data, uint32_t len, int passfd) {
    return send_reply_fd(fd, reqid, status, data, len, passfd);
}

void brokersrv_prompt_event(const char *json) { queue_evt("prompt", "%s", json); }

void brokersrv_device(const char *app, const char *kind, int on) {
    queue_evt("device", "{\"app\":\"%s\",\"kind\":\"%s\",\"on\":%s}", app, kind, on ? "true" : "false");
}

void brokersrv_powerbox_save(uint32_t reqid, const char *app, const char *suggest) {
    char sq[300];
    json_quote(sq, sizeof sq, suggest);
    queue_evt("pick", "{\"reqid\":%u,\"app\":\"%s\",\"save\":true,\"suggest\":%s}", reqid, app, sq);
}

void brokersrv_events(BrokerEventFn fn) {
    for (int i = 0; i < nevt; i++) fn(evq[i].kind, evq[i].json);
    nevt = 0;
}
