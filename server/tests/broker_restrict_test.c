/* broker_restrict_test.c — the firewall's non-network restrictions in the broker (src/sec/restrict.c,
 * brokersrv.c, brokercam.c; docs/FIREWALL.md, "Non-network restrictions").
 *
 * The real broker server code runs against a fake isolated app on a socketpair: it sends requests
 * like js_broker.c does and reads the replies. For every restriction the test checks that the
 * request goes through without it and is refused with it, without a prompt, with the code line
 * fw.restricted.<what>, even when the permission was granted before. It also checks live
 * revocation (restricting a running app stops its camera and its microphone), the "restricted
 * meanwhile" paths (prompt answered or file picked after the restriction) and the built-in apps'
 * hook (firewall id "lordos", used by js_media.c).
 *
 * Only the edges are stand-ins: the terminal's clipboard, PipeWire (maudio_*), axia-vault's FIDO
 * attach, the keyring, the keyboard and axia-media's spawn. open() is wrapped so /dev/video9 is
 * /dev/null. Temporary HOME. `make build/broker-restrict-test && build/broker-restrict-test`. */
#include "sec/broker_int.h"
#include "sec/perms.h"
#include "sec/restrict.h"
#include "fw/rules.h"
#include "media/media_int.h"
#include "errcode.h"

#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

/* ---- stand-ins for the rest of axia-server ---- */
const char *term_clipboard(void) { return "clipboard text"; }
void term_set_clipboard(const char *s, size_t n) { (void)s; (void)n; }
void term_clipboard_mark(const char *app, int secret) { (void)app; (void)secret; }
const char *kb_layouts(void) { return "us"; }
const char *kb_variants(void) { return ""; }
int kb_group(void) { return 0; }
void sha256(const void *d, size_t n, uint8_t out[32]) { (void)d; (void)n; memset(out, 0, 32); }
int vc_fido_attach(char *name, size_t n) { snprintf(name, n, "test key"); return 0; }
void brokerkr_handle(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len) { (void)conn; (void)fd; (void)reqid; (void)app; (void)payload; (void)len; }
int brokerkr_resolve(uint32_t prompt, int allow) { (void)prompt; (void)allow; return 0; }
void brokerkr_conn_closed(int conn) { (void)conn; }
void brokerkr_tick(void) {}
int brokerkr_pollfd(void) { return -1; }
void brokermail_handle(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len, int *fds, int nfds) {
    (void)conn; (void)app; (void)payload; (void)len;
    for (int i = 0; i < nfds; i++) if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
    brokersrv_reply(fd, reqid, BST_DENIED, NULL, 0, -1);
}
void brokermail_sys_open(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len) { (void)conn; (void)app; (void)payload; (void)len; brokersrv_reply(fd, reqid, BST_DENIED, NULL, 0, -1); }
void brokermail_conn_closed(int conn) { (void)conn; }
void brokermail_tick(void) {}
int brokermail_pollfd(void) { return -1; }
int brokermail_wants_write(int conn) { (void)conn; return 0; }
int lpk_valid_id(const char *id) { return id && id[0] && strlen(id) < 48; }

struct MAudio { int open; };
static struct MAudio mics[8];
static int mics_open, mics_closed;
MAudio *maudio_open(int capture, const char *app, const char *title, const char *src, int *ringfd) {
    (void)capture; (void)app; (void)title; (void)src;
    for (int i = 0; i < 8; i++) if (!mics[i].open) {
        mics[i].open = 1;
        mics_open++;
        *ringfd = memfd_create("ring", MFD_CLOEXEC);
        if (*ringfd >= 0 && ftruncate(*ringfd, 4096)) { close(*ringfd); *ringfd = -1; }
        return &mics[i];
    }
    return NULL;
}
void maudio_close(MAudio *a) { if (a && a->open) { a->open = 0; mics_closed++; } }
int maudio_pollfd(const MAudio *a, short *events) { (void)a; *events = 0; return -1; }
void maudio_service(MAudio *a) { (void)a; }
void maudio_mic_drain(MAudio *a) { (void)a; }

static MediaSrc srcs[4];
static int srcs_closed;
MediaSrc *media_src_new(int kind, const char *app) {
    for (int i = 0; i < 4; i++) if (!srcs[i].used) {
        memset(&srcs[i], 0, sizeof srcs[i]);
        srcs[i].used = 1; srcs[i].id = i + 1; srcs[i].kind = kind; srcs[i].sock = -1; srcs[i].in_fd = -1;
        snprintf(srcs[i].app, sizeof srcs[i].app, "%s", app);
        return &srcs[i];
    }
    return NULL;
}
MediaSrc *media_src_get(int id) { return id >= 1 && id <= 4 && srcs[id - 1].used ? &srcs[id - 1] : NULL; }
void media_src_close(MediaSrc *s) { if (s && s->used) { if (s->sock >= 0) close(s->sock); s->used = 0; srcs_closed++; } }
int media_spawn(int kind, const char *params, const int *fds, int nfds, pid_t *pid) {
    (void)kind; (void)params; (void)fds; (void)nfds;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    *pid = 0;                                        /* no real helper: nothing to kill */
    return sv[0];                                    /* sv[1] leaks on purpose: the "helper" side */
}
int media_render_fd(void) { return -1; }
const char *media_hwaccel(void) { return "none"; }
int media_cameras(char *json, size_t n) { snprintf(json, n, "[{\"dev\":\"/dev/video9\"}]"); return 1; }

/* /dev/video9 → /dev/null, so the broker's camera open succeeds without a camera */
int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (!strcmp(path, "/dev/video9")) return __real_open("/dev/null", O_RDWR | O_CLOEXEC);
    return __real_open(path, flags, mode);
}

/* ---- the fake app ---- */
static int app_end, srv_end;
static uint32_t next_req = 1;
static char got[1 << 16];
static uint32_t got_len;
static int got_status, got_fd;
static char evs[64][300];
static int nevs;

static void collect_events(const char *kind, const char *json) { if (nevs < 64) snprintf(evs[nevs++], sizeof evs[0], "%s %s", kind, json); }
static int has_event(const char *needle) {
    for (int i = 0; i < nevs; i++) if (strstr(evs[i], needle)) return 1;
    return 0;
}

/* sends one request and runs the server's dispatch; 1 if a reply came back */
static int call(int op, const void *payload, uint32_t len) {
    BrokerHdr h = { .reqid = next_req++, .op = (uint16_t)op, .status = BROKER_REQ, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)payload, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    if (sendmsg(app_end, &m, MSG_NOSIGNAL) < 0) return 0;
    struct pollfd pfd[8];
    int n = brokersrv_pollfds(pfd, 8);
    for (int i = 0; i < n; i++) pfd[i].revents = pfd[i].fd == srv_end ? POLLIN : 0;
    brokersrv_dispatch(pfd, n);
    nevs = 0;
    brokersrv_events(collect_events);
    got_fd = -1;
    for (;;) {
        BrokerHdr r;
        char cm[CMSG_SPACE(sizeof(int))];
        struct iovec riov[2] = { { &r, sizeof r }, { got, sizeof got - 1 } };
        struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2, .msg_control = cm, .msg_controllen = sizeof cm };
        ssize_t k = recvmsg(app_end, &rm, MSG_DONTWAIT);
        if (k < (ssize_t)sizeof r) return 0;
        got_len = (uint32_t)(k - (ssize_t)sizeof r);
        got[got_len] = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&rm); c; c = CMSG_NXTHDR(&rm, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&got_fd, CMSG_DATA(c), sizeof got_fd);
        if (r.op == BOP_EVENT) continue;             /* unrequested events: skipped here */
        got_status = r.status;
        if (got_fd >= 0) close(got_fd);
        return r.reqid == h.reqid;
    }
}
#define CALLS(op, s) call(op, s, (uint32_t)strlen(s))

/* reads the unrequested events the server sent to the app ("revoke", "cam" …) */
static int app_event(const char *needle) {
    int found = 0;
    for (;;) {
        BrokerHdr r;
        char buf[2048];
        struct iovec riov[2] = { { &r, sizeof r }, { buf, sizeof buf - 1 } };
        struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2 };
        ssize_t k = recvmsg(app_end, &rm, MSG_DONTWAIT);
        if (k < (ssize_t)sizeof r) return found;
        size_t pl = (size_t)k - sizeof r;
        for (size_t i = 0; i < pl; i++) if (!buf[i]) buf[i] = ' ';
        buf[pl] = 0;
        if (r.op == BOP_EVENT && strstr(buf, needle)) found = 1;
    }
}

/* the next reply (not an event) that is already waiting: its status, payload in got */
static int read_reply(void) {
    for (;;) {
        BrokerHdr r;
        struct iovec iov[2] = { { &r, sizeof r }, { got, sizeof got - 1 } };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        ssize_t k = recvmsg(app_end, &m, MSG_DONTWAIT);
        if (k < (ssize_t)sizeof r) { got[0] = 0; return got_status = -1; }
        got[k - sizeof r] = 0;
        if (r.op != BOP_EVENT) return got_status = r.status;
    }
}

static int denied_with(const char *code) { return got_status == BST_DENIED && lerr_has(got, code); }

static FwStore *store;
static void restrict_app(const char *fwid, uint32_t flags) {
    uint32_t before = fw_restricted(store, fwid);
    fw_set_restricted(store, fwid, flags);
    restrict_changed(fwid, before, fw_restricted(store, fwid));
}

static uint32_t local_added;
static void local_hook(uint32_t added) { local_added |= added; }

int main(void) {
    char home[] = "/tmp/broker-restrict-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    perm_init(home);
    store = fw_store_new();
    restrict_set_store(store);
    restrict_set_local_hook(local_hook);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { perror("socketpair"); return 1; }
    app_end = sv[0]; srv_end = sv[1];
    brokersrv_add(srv_end, "game", 4242);
    brokersrv_set_vault(srv_end);                    /* lets the test reach the FIDO op too */
    const char *perms_on[] = { "camera", "microphone", "clipboard", "usb", "files", "screen", "bluetooth", "notifications" };
    for (size_t i = 0; i < sizeof perms_on / sizeof *perms_on; i++) perm_set("game", (Perm)perm_from_name(perms_on[i]), PERM_ALLOW);

    /* ---- clipboard ---- */
    check(call(BOP_CLIP_GET, NULL, 0) && got_status == BST_OK && !strcmp(got, "clipboard text"), "clipboard: read with the permission");
    restrict_app("lpk:game", FW_R_NO_CLIPBOARD);
    check(call(BOP_CLIP_GET, NULL, 0) && denied_with("fw.restricted.clipboard"), "clipboard: no-clipboard refuses it with fw.restricted.clipboard");
    check(strstr(got, "app=game") != NULL, "clipboard: the code line names the app");
    restrict_app("lpk:game", 0);

    /* ---- permissions: refused without a prompt ---- */
    for (size_t i = 0; i < sizeof perms_on / sizeof *perms_on; i++) {
        Perm p = (Perm)perm_from_name(perms_on[i]);
        uint32_t f = restrict_flag_of(p);
        char what[128];
        CALLS(BOP_PERM, perms_on[i]);
        snprintf(what, sizeof what, "permission %s: granted without a restriction", perms_on[i]);
        check(got_status == BST_OK && got[0] == '1', what);
        restrict_app("lpk:game", f);
        CALLS(BOP_PERM, perms_on[i]);
        snprintf(what, sizeof what, "permission %s: restricted → \"0\" with %s, no prompt", perms_on[i], restrict_code(f));
        check(got_status == BST_OK && got[0] == '0' && lerr_has(got + 2, restrict_code(f)) && !has_event("prompt"), what);
        perm_set("game", p, PERM_ASK);               /* never asked: still no prompt */
        CALLS(BOP_PERM, perms_on[i]);
        snprintf(what, sizeof what, "permission %s: restricted and never asked → no prompt either", perms_on[i]);
        check(got[0] == '0' && !has_event("prompt"), what);
        perm_set("game", p, PERM_ALLOW);
        restrict_app("lpk:game", 0);
    }

    /* a prompt answered "allow" after the app was restricted meanwhile still gets "0" */
    perm_set("game", PERM_CAMERA, PERM_ASK);
    CALLS(BOP_PERM, "camera");
    unsigned ui = 0;
    for (int i = 0; i < nevs; i++) { const char *q = strstr(evs[i], "\"reqid\":"); if (strstr(evs[i], "prompt") && q) ui = (unsigned)strtoul(q + 8, NULL, 10); }
    check(ui != 0, "camera: without a restriction an undecided permission prompts");
    restrict_app("lpk:game", FW_R_NO_CAMERA);
    brokersrv_resolve(ui, 1);
    read_reply();
    check(got[0] == '0', "camera: a prompt allowed after the restriction still answers \"0\"");
    restrict_app("lpk:game", 0);
    perm_set("game", PERM_CAMERA, PERM_ALLOW);

    /* ---- device indicator ---- */
    CALLS(BOP_DEVICE, "camera 1");
    check(has_event("\"kind\":\"camera\",\"on\":true"), "camera: the indicator lights with the permission");
    restrict_app("lpk:game", FW_R_NO_CAMERA);
    CALLS(BOP_DEVICE, "camera 1");
    check(!has_event("\"kind\":\"camera\",\"on\":true"), "camera: a restricted app cannot light the camera indicator");
    restrict_app("lpk:game", 0);

    /* ---- microphone, and live revocation ---- */
    check(call(BOP_MIC, "1", 1) && got_status == BST_OK, "microphone: the ring is handed over with the permission");
    int closed0 = mics_closed;
    app_event("");                                   /* drain */
    restrict_app("lpk:game", FW_R_NO_MIC);
    check(mics_closed == closed0 + 1, "microphone: restricting the running app closes its stream now");
    check(app_event("revoke {\"perm\":\"microphone\"}"), "microphone: the app is told (revoke event)");
    nevs = 0; brokersrv_events(collect_events);
    check(has_event("\"kind\":\"microphone\",\"on\":false"), "microphone: the indicator goes off");
    check(call(BOP_MIC, "1", 1) && denied_with("fw.restricted.microphone"), "microphone: no-mic refuses it with fw.restricted.microphone");
    restrict_app("lpk:game", 0);

    /* ---- camera, and live revocation ---- */
    check(CALLS(BOP_CAMERA, "list") && strstr(got, "video9"), "camera: listed with the permission");
    restrict_app("lpk:game", FW_R_NO_CAMERA);
    check(CALLS(BOP_CAMERA, "list") && !strcmp(got, "[]"), "camera: a restricted app sees no cameras");
    check(CALLS(BOP_CAMERA, "open /dev/video9 640 480") && denied_with("fw.restricted.camera"), "camera: no-camera refuses open with fw.restricted.camera");
    restrict_app("lpk:game", 0);
    check(CALLS(BOP_CAMERA, "open /dev/video9 640 480") && got_status == BST_OK, "camera: opens without the restriction");
    int sid = atoi(got);
    int sclosed0 = srcs_closed;
    app_event("");
    restrict_app("lpk:game", FW_R_NO_CAMERA);
    check(srcs_closed == sclosed0 + 1 && !media_src_get(sid), "camera: restricting the running app ends its stream now");
    char needle[64];
    snprintf(needle, sizeof needle, "closed %d revoked", sid);
    check(app_event(needle), "camera: the app gets \"closed … revoked\"");
    restrict_app("lpk:game", 0);

    /* ---- files: the picker and the camera's save dialog ---- */
    check(call(BOP_PICK, "0\0x.txt", 7) == 0 && has_event("pick"), "files: the picker opens without the restriction");
    unsigned pick = 0;
    for (int i = 0; i < nevs; i++) { const char *q = strstr(evs[i], "\"reqid\":"); if (strstr(evs[i], "pick") && q) pick = (unsigned)strtoul(q + 8, NULL, 10); }
    restrict_app("lpk:game", FW_R_NO_FILES);
    char picked[600];
    snprintf(picked, sizeof picked, "%s/picked.txt", home);
    { int f = open(picked, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f); }
    brokersrv_pick_result(pick, picked);
    read_reply();
    check(got_status == BST_DENIED, "files: a file picked after the restriction is not handed over");
    check(call(BOP_PICK, "0\0x.txt", 7) && denied_with("fw.restricted.files"), "files: no-files refuses the picker with fw.restricted.files");
    check(!has_event("pick"), "files: and no dialog is shown");
    restrict_app("lpk:game", 0);
    CALLS(BOP_CAMERA, "open /dev/video9 640 480");
    sid = atoi(got);
    restrict_app("lpk:game", FW_R_NO_FILES);
    char ph[64];
    snprintf(ph, sizeof ph, "photo %d 0 shot.jpg", sid);
    check(CALLS(BOP_CAMERA, ph) && denied_with("fw.restricted.files"), "files: no-files refuses saving a camera photo");
    restrict_app("lpk:game", 0);
    snprintf(ph, sizeof ph, "close %d", sid);
    CALLS(BOP_CAMERA, ph);

    /* ---- USB (the FIDO2 key of Passwords) ---- */
    check(call(BOP_VAULT_FIDO, NULL, 0) && got_status == BST_OK && !strcmp(got, "test key"), "usb: the key is attached with the permission");
    restrict_app("lpk:game", FW_R_NO_USB);
    check(call(BOP_VAULT_FIDO, NULL, 0) && denied_with("fw.restricted.usb"), "usb: no-usb refuses it with fw.restricted.usb");
    restrict_app("lpk:game", 0);

    /* ---- other apps and ids ---- */
    restrict_app("lpk:other", FW_R_ALL);
    check(call(BOP_CLIP_GET, NULL, 0) && got_status == BST_OK, "another app's restriction does not touch this one");
    restrict_app("lpk:other", 0);
    check(restrict_perm_pkg("game", PERM_CAMERA) == PERM_ALLOW, "restrict_perm_pkg: the permission when not restricted");
    restrict_app("lpk:game", FW_R_NO_CAMERA);
    check(restrict_perm_pkg("game", PERM_CAMERA) == PERM_DENY, "restrict_perm_pkg: DENY when restricted, over an allow");
    restrict_app("lpk:game", 0);

    /* ---- built-in apps: firewall id "lordos" (js_media.c) ---- */
    check(!restrict_blocks("lordos", FW_R_NO_CAMERA), "lordos: not restricted by default");
    restrict_app("lordos", FW_R_NO_CAMERA | FW_R_NO_MIC);
    check(restrict_blocks("lordos", FW_R_NO_CAMERA) && restrict_blocks("lordos", FW_R_NO_MIC) && !restrict_blocks("lordos", FW_R_NO_SCREEN),
          "lordos: js_media.c sees exactly the restrictions set");
    check(local_added == (FW_R_NO_CAMERA | FW_R_NO_MIC), "lordos: the live hook gets what was just added (stops camera and recordings)");
    local_added = 0;
    restrict_app("lordos", FW_R_NO_CAMERA | FW_R_NO_MIC | FW_R_NO_SCREEN);
    check(local_added == FW_R_NO_SCREEN, "lordos: only the new restriction is passed on");
    restrict_app("lordos", 0);
    check(!restrict_blocks_pkg("lordos", FW_R_NO_CAMERA), "a package named \"lordos\" is lpk:lordos, not the built-in apps");
    restrict_set_store(NULL);
    check(!restrict_blocks("lpk:game", FW_R_NO_CAMERA), "no store: no restrictions (and no crash)");

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
    fw_store_free(store);
    printf("%d/%d %s\n", total - fails, total, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
