/* brokerkr_e2e.c — the keyring op of isolated .lpk apps (src/sec/brokerkr.c, BOP_KEYRING) end to
 * end with a real axia-vault: the payload an app sends, the owner axia-server sets ("app.<id>"),
 * isolation between two app ids, the consent prompt (deny and allow), a shell owner that a .lpk
 * with the same id cannot reach, a missing item, bad requests and a locked keyring.
 * brokersrv_reply and brokersrv_prompt_event are stand-ins here: replies go to a socketpair (the
 * "app"), prompts are recorded. Temporary HOME, no D-Bus. Needs build/axia-vault next to it:
 * `make build/brokerkr-e2e && build/brokerkr-e2e`. */
#include "sec/broker_int.h"
#include "vault/vclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---- stand-ins for brokersrv.c ---- */
static char prompt[256];
static int nprompts;
int brokersrv_reply(int fd, uint32_t reqid, int status, const void *data, uint32_t len, int passfd) {
    (void)passfd;
    BrokerHdr h = { .reqid = reqid, .op = 0, .status = (uint16_t)status, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)data, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    return sendmsg(fd, &m, MSG_NOSIGNAL) < 0 ? -1 : 0;
}
void brokersrv_prompt_event(const char *json) { snprintf(prompt, sizeof prompt, "%s", json); nprompts++; }
/* connection 7 stands for a system package (assets/sysapps): its owner is its plain id */
int brokersrv_conn_system(int conn) { return conn == 7; }

static int app_end, srv_end;
static uint32_t next_req = 1;
static char got[1 << 16];
static int got_status;

/* the app sends op/service/account/label/secret; waits for the reply (or a prompt) */
static int call_conn;      /* broker connection the request comes from (7 = a system package) */
static int call(const char *app, const char *op, const char *service, const char *account, const char *label, const char *secret, size_t secret_len) {
    char p[4096];
    size_t o = 0;
    const char *f[4] = { op, service, account, label };
    for (int i = 0; i < 4; i++) { size_t n = strlen(f[i]); memcpy(p + o, f[i], n); o += n; p[o++] = 0; }
    memcpy(p + o, secret, secret_len);
    o += secret_len;
    p[o] = 0;
    int before = nprompts;
    brokerkr_handle(call_conn, srv_end, next_req++, app, p, (uint32_t)o);
    double t0 = now_ms();
    while (now_ms() - t0 < 20000) {
        brokerkr_tick();
        if (nprompts != before) return 'p';
        BrokerHdr h;
        struct iovec iov[2] = { { &h, sizeof h }, { got, sizeof got - 1 } };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        ssize_t r = recvmsg(app_end, &m, MSG_DONTWAIT);
        if (r >= (ssize_t)sizeof h) { got[r - sizeof h] = 0; got_status = h.status; return 'r'; }
        usleep(2000);
    }
    snprintf(got, sizeof got, "timeout");
    got_status = -1;
    return 't';
}
#define CALL(app, op, svc, acct, sec) call(app, op, svc, acct, "", sec, strlen(sec))

/* after a prompt: the user's answer, then the reply */
static int answer(int allow) {
    unsigned id = 0;
    const char *q = strstr(prompt, "\"reqid\":");
    if (q) id = (unsigned)strtoul(q + 8, NULL, 10);
    if (!brokerkr_resolve(id, allow)) return 'x';
    double t0 = now_ms();
    while (now_ms() - t0 < 20000) {
        brokerkr_tick();
        BrokerHdr h;
        struct iovec iov[2] = { { &h, sizeof h }, { got, sizeof got - 1 } };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        ssize_t r = recvmsg(app_end, &m, MSG_DONTWAIT);
        if (r >= (ssize_t)sizeof h) { got[r - sizeof h] = 0; got_status = h.status; return 'r'; }
        usleep(2000);
    }
    return 't';
}

static int is_ok(void) { return got_status == BST_OK && strstr(got, "\"ok\":true"); }
static int is_err(int st, const char *code) { return got_status == st && !strcmp(got, code); }

/* a request on the system channel (as the desktop's own apps do), waiting for its reply */
static void sys_req(const char *json) {
    int id = vc_request(json, strlen(json), NULL, 0);
    double t0 = now_ms();
    while (id > 0 && now_ms() - t0 < 20000) {
        char *s = vc_poll();
        if (s) { int done = strstr(s, "\"kind\":\"rep\"") != NULL; vc_poll_free(s); if (done) return; }
        usleep(2000);
    }
}

int main(void) {
    char home[] = "/tmp/brokerkr-e2e-XXXXXX";
    if (!mkdtemp(home)) return 1;
    setenv("HOME", home, 1);
    setenv("XDG_RUNTIME_DIR", "/nonexistent", 1);
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return 1;
    app_end = sv[0];
    srv_end = sv[1];
    check(vc_available(), "axia-vault next to the binary");

    /* no session keyring yet */
    CALL("alpha", "set", "sync.example.org", "ana", "s");
    check(is_err(BST_ERROR, "keyring.noKeyring"), "no keyring before the session password → keyring.noKeyring");
    vc_session_password("session password e2e");
    usleep(1500000);

    CALL("alpha", "set", "sync.example.org", "ana", "alpha-secret");
    check(is_ok(), "alpha: set");
    CALL("alpha", "get", "sync.example.org", "ana", "");
    check(is_ok() && strstr(got, "alpha-secret") && strstr(got, "\"owner\":\"app.alpha\""), "alpha: get its own item, owner app.alpha");
    CALL("alpha", "list", "", "", "");
    check(is_ok() && strstr(got, "sync.example.org") && !strstr(got, "alpha-secret"), "alpha: list has the item, without secrets");
    CALL("alpha", "get", "missing.example.org", "", "");
    check(is_err(BST_ERROR, "keyring.notFound"), "alpha: missing item → keyring.notFound");

    /* beta cannot see alpha's item without the user's consent */
    CALL("beta", "list", "", "", "");
    check(is_ok() && !strstr(got, "sync.example.org"), "beta: list is empty");
    CALL("beta", "delete", "sync.example.org", "ana", "");
    check(is_err(BST_ERROR, "keyring.notFound"), "beta: cannot delete alpha's item (notFound)");
    CALL("beta", "set", "sync.example.org", "ana", "beta-secret");
    check(is_ok(), "beta: set with the same service makes beta's own item");
    CALL("alpha", "get", "sync.example.org", "ana", "");
    check(is_ok() && strstr(got, "alpha-secret") && !strstr(got, "beta-secret"), "alpha's item unchanged by beta");
    CALL("beta", "delete", "sync.example.org", "ana", "");
    check(is_ok(), "beta: delete its own item");
    int k = CALL("beta", "get", "sync.example.org", "ana", "");
    check(k == 'p' && strstr(prompt, "\"app\":\"beta\"") && strstr(prompt, "\"perm\":\"keyring\"") && strstr(prompt, "\"owner\":\"alpha\""),
          "beta: get of alpha's item → system prompt (app beta, owner alpha)");
    check(answer(0) == 'r' && is_err(BST_DENIED, "keyring.notShared"), "user denies → keyring.notShared");
    k = CALL("beta", "get", "sync.example.org", "ana", "");
    check(k == 'r' && is_err(BST_DENIED, "keyring.notShared"), "after a denial, no new prompt this session");
    k = CALL("delta", "get", "sync.example.org", "ana", "");
    check(k == 'p' && strstr(prompt, "\"app\":\"delta\""), "another app is still asked");
    check(answer(1) == 'r' && is_ok() && strstr(got, "alpha-secret"), "user allows → delta reads the shared item");
    k = CALL("delta", "get", "sync.example.org", "ana", "");
    check(k == 'r' && is_ok() && strstr(got, "alpha-secret"), "no prompt once shared");
    CALL("delta", "delete", "sync.example.org", "ana", "");
    check(is_err(BST_ERROR, "keyring.notFound"), "a shared item can be read, not deleted, by delta");
    CALL("beta", "list", "", "", "");
    check(is_ok() && !strstr(got, "sync.example.org"), "beta still sees nothing (shared with delta only)");
    CALL("gamma", "list", "", "", "");
    check(is_ok() && !strstr(got, "sync.example.org"), "gamma still sees nothing");

    /* the owner is set by axia-server: a .lpk named "mail" does not reach the desktop's "mail" items */
    sys_req("{\"op\":\"kr-set\",\"owner\":\"mail\",\"service\":\"imap.example.com\",\"account\":\"ana\",\"secret\":\"desk-secret\"}");
    k = CALL("mail", "get", "imap.example.com", "ana", "");
    check(k == 'p' && strstr(prompt, "\"owner\":\"mail\""), ".lpk \"mail\" (owner app.mail) does not own the desktop's \"mail\" item");
    check(answer(0) == 'r' && is_err(BST_DENIED, "keyring.notShared"), "denied");
    /* the system Mail app (assets/sysapps, its own process) keeps the desktop's owner "mail" */
    call_conn = 7;
    k = CALL("mail", "get", "imap.example.com", "ana", "");
    check(k == 'r' && is_ok() && strstr(got, "desk-secret"), "system package \"mail\" (owner mail) reads the desktop's \"mail\" item");
    CALL("mail", "list", "", "", "");
    check(is_ok() && !strstr(got, "sync.example.org"), "system \"mail\" still does not see other apps' items");
    call_conn = 0;

    /* bad requests */
    CALL("alpha", "nope", "x", "", "");
    check(is_err(BST_ERROR, "keyring.badRequest"), "unknown op → keyring.badRequest");
    CALL("alpha", "get", "", "", "");
    check(is_err(BST_ERROR, "keyring.badRequest"), "empty service → keyring.badRequest");
    call("alpha", "set", "x.example", "", "", "a\0b", 3);
    check(is_err(BST_ERROR, "keyring.badRequest"), "NUL inside the secret → keyring.badRequest");
    {
        static char big[300];
        memset(big, 'a', sizeof big - 1);
        CALL("alpha", "get", big, "", "");
        check(is_err(BST_ERROR, "keyring.badRequest"), "service over 255 bytes → keyring.badRequest");
    }
    { const char *sec = "with \"quotes\" and \\ and \n"; call("alpha", "set", "escape.example", "\"quoted\"", "", sec, strlen(sec)); }
    CALL("alpha", "get", "escape.example", "\"quoted\"", "");
    check(is_ok() && strstr(got, "with \\\"quotes\\\" and \\\\ and \\n"), "quotes, backslashes and newlines round-trip");

    /* locked keyring (logout locks it) */
    vc_lock_all("logout");
    usleep(300000);
    CALL("alpha", "get", "sync.example.org", "ana", "");
    check(is_err(BST_ERROR, "keyring.locked"), "locked keyring → keyring.locked");

    /* the app closed while its request was waiting: the late reply is dropped */
    vc_session_password("session password e2e");
    usleep(800000);
    {
        char p[64];
        size_t n = (size_t)snprintf(p, sizeof p, "list%c%c%c%c", 0, 0, 0, 0);
        brokerkr_handle(5, srv_end, 999, "alpha", p, (uint32_t)n);
        brokerkr_conn_closed(5);
        double t0 = now_ms();
        while (vc_private_pending() && now_ms() - t0 < 5000) { brokerkr_tick(); usleep(2000); }
        char tmp[64];
        ssize_t r = recv(app_end, tmp, sizeof tmp, MSG_DONTWAIT);
        check(r < 0 && !vc_private_pending(), "reply for a closed app is drained, not sent");
    }

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) { /* best effort */ }
    printf("brokerkr-e2e: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
