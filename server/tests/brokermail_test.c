/* brokermail_test.c — axia-maild for the isolated system apps (src/sec/brokermail.c, BOP_MAIL and
 * BOP_SYS_OPEN): who may ask, which requests each app may send, which files may travel to
 * axia-maild, the Mail-only verbs (accounts file, remote images, attachments) and opening another
 * app. The real brokermail.c runs against stand-ins for brokersrv.c and mailc.c that record what
 * would have reached axia-maild or the session. Temporary HOME.
 * `make build/brokermail-test && build/brokermail-test`. */
#include "sec/broker_int.h"
#include "mail/mailc.h"

#include <fcntl.h>
#include <limits.h>
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

/* ---- stand-ins for brokersrv.c ---- */
static int sys_conn[8];
static const char *conn_app[8] = { "mail", "calendar", "reminders", "contacts", "mail", "notes" };
const char *brokersrv_conn_app(int conn) { return conn >= 0 && conn < 8 ? conn_app[conn] : NULL; }
int brokersrv_conn_system(int conn) { return conn >= 0 && conn < 8 && sys_conn[conn]; }
int brokersrv_conn_event(int conn, const char *kind, const char *data, size_t len) { (void)conn; (void)kind; (void)data; (void)len; return 0; }
static int last_status, last_fd = -1;
static char last_reply[4096];
int brokersrv_reply(int fd, uint32_t reqid, int status, const void *data, uint32_t len, int passfd) {
    (void)fd; (void)reqid;
    last_status = status;
    snprintf(last_reply, sizeof last_reply, "%.*s", (int)len, data ? (const char *)data : "");
    if (last_fd >= 0) close(last_fd);
    last_fd = passfd >= 0 ? dup(passfd) : -1;
    return 0;
}
static char shell_kind[32], shell_json[4096];
void brokersrv_shell_event(const char *kind, const char *json) { snprintf(shell_kind, sizeof shell_kind, "%s", kind); snprintf(shell_json, sizeof shell_json, "%s", json); }

/* ---- stand-ins for mailc.c ---- */
static char home[PATH_MAX], cache[PATH_MAX + 32], cfg[PATH_MAX + 64];
static int owners_open, owner_mask[MAIL_OWNERS + 1];
static int req_count, req_owner, req_nfds;
static char req_json[4096], saved[4096];
static int images_n;
int mail_available(void) { return 0; }
int mail_start(void) { return 0; }
const char *mail_config_path(void) { return cfg; }
const char *mail_cache_path(void) { return cache; }
int mail_owner_open(int mask) { if (owners_open >= MAIL_OWNERS) return -1; owner_mask[++owners_open] = mask; return owners_open; }
void mail_owner_close(int owner) { (void)owner; }
int mail_request_as(int owner, const char *json, size_t len, int *fds, int nfds) {
    req_count++;
    req_owner = owner;
    req_nfds = nfds;
    snprintf(req_json, sizeof req_json, "%.*s", (int)len, json);
    for (int i = 0; i < nfds; i++) close(fds[i]);
    return 100 + req_count;
}
char *mail_poll_owner(int owner) { (void)owner; return NULL; }
int mail_owner_pollfd(void) { return -1; }
void mail_owner_clear_wake(void) {}
int mail_image_fd(int owner, int handle, int *w, int *h) { (void)owner; (void)handle; *w = *h = 0; return -1; }
void mail_image_release_as(int owner, int handle) { (void)owner; (void)handle; }
int mail_save_config(const char *json, size_t len) { snprintf(saved, sizeof saved, "%.*s", (int)len, json); return strstr(saved, "password") ? 1 : 0; }
void mail_allow_hosts(const char *const *urls, int n, int seconds) { (void)urls; (void)seconds; images_n = n; }

/* ---- helpers ---- */
static uint32_t reqid;
/* payload "verb\0rest" (rest may hold NULs: pass its length) */
static int call(int conn, const char *verb, const char *rest, size_t rl, int *fds, int nfds) {
    char p[8192];
    size_t vl = strlen(verb);
    memcpy(p, verb, vl + 1);
    memcpy(p + vl + 1, rest, rl);
    p[vl + 1 + rl] = 0;
    int copy[16];
    for (int i = 0; i < nfds; i++) copy[i] = fds[i];
    brokermail_handle(conn, 9, ++reqid, conn_app[conn], p, (uint32_t)(vl + 1 + rl), copy, nfds);
    int leaked = 0;                                   /* the handler closes or keeps every fd */
    for (int i = 0; i < nfds; i++) if (copy[i] >= 0) leaked = 1;
    if (leaked) { printf("FAIL: an fd was neither used nor closed\n"); fails++; }
    return last_status;
}
static int req(int conn, const char *json, int *fds, int nfds) { return call(conn, "req", json, strlen(json), fds, nfds); }
static int sysopen(int conn, const char *target, int nw, const char *json) {
    char p[2048];
    size_t n = (size_t)snprintf(p, sizeof p, "%s%c%c%c%s", target, 0, nw ? '1' : '0', 0, json);
    shell_kind[0] = 0;
    brokermail_sys_open(conn, 9, ++reqid, conn_app[conn], p, (uint32_t)n);
    return last_status;
}
static int file_fd(const char *name, const char *text) {
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", home, name);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0 && write(fd, text, strlen(text)) < 0) { close(fd); return -1; }
    return fd;
}

int main(void) {
    snprintf(home, sizeof home, "/tmp/brokermail-test-%d", (int)getpid());
    mkdir(home, 0700);
    snprintf(cache, sizeof cache, "%s/cache", home);
    snprintf(cfg, sizeof cfg, "%s/accounts.json", home);
    mkdir(cache, 0700);
    char d[PATH_MAX + 64];
    snprintf(d, sizeof d, "%s/att", cache); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/att/acct", cache); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/att/acct/7-1", cache); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/att/acct/7-1/report.pdf", cache);
    { int f = open(d, O_WRONLY | O_CREAT, 0600); if (f >= 0) { if (write(f, "%PDF", 4) < 0) { /* checked below */ } close(f); } }
    { char secret[PATH_MAX + 64]; snprintf(secret, sizeof secret, "%s/secret.txt", home); int f = open(secret, O_WRONLY | O_CREAT, 0600); if (f >= 0) close(f);
      snprintf(d, sizeof d, "%s/att/acct/7-1/link", cache); if (symlink(secret, d)) { /* the test below fails */ } }
    for (int i = 0; i < 5; i++) sys_conn[i] = 1;      /* conn 5 ("notes") is a user package */

    /* who may ask */
    check(req(5, "{\"op\":\"list\"}", NULL, 0) == BST_DENIED, "a user package (not a system app) gets nothing");
    sys_conn[4] = 0;
    check(req(4, "{\"op\":\"list\"}", NULL, 0) == BST_DENIED, "a user .lpk named \"mail\" gets nothing either");
    sys_conn[4] = 1;

    /* each app sends only its own requests */
    check(req(0, "{\"op\":\"list\",\"acct\":\"a\"}", NULL, 0) == BST_OK && owner_mask[req_owner] == MAIL_EV_ALL, "Mail: list (all events)");
    int before = req_count;
    check(req(1, "{\"op\":\"list\"}", NULL, 0) == BST_DENIED && req_count == before, "Calendar cannot list mail");
    check(req(1, "{\"op\":\"open\",\"uid\":1}", NULL, 0) == BST_DENIED, "Calendar cannot open a message");
    check(req(1, "{\"op\":\"send\"}", NULL, 0) == BST_DENIED, "Calendar cannot send mail");
    check(req(1, "{\"op\":\"ev-list\"}", NULL, 0) == BST_OK && owner_mask[req_owner] == MAIL_EV_PIM, "Calendar: ev-list (PIM events only)");
    check(req(1, "{\"op\":\"contact-list\"}", NULL, 0) == BST_DENIED, "Calendar cannot read contacts");
    check(req(2, "{\"op\":\"todo-save\"}", NULL, 0) == BST_OK, "Reminders: todo-save");
    check(req(2, "{\"op\":\"ev-list\"}", NULL, 0) == BST_DENIED, "Reminders cannot read events");
    check(req(3, "{\"op\":\"contact-list\"}", NULL, 0) == BST_OK && req(3, "{\"op\":\"autocomplete\",\"q\":\"a\"}", NULL, 0) == BST_OK, "Contacts: contact-list, autocomplete");
    check(req(3, "{\"op\":\"thread\"}", NULL, 0) == BST_DENIED, "Contacts cannot read threads");
    check(req(2, "{\"op\":\"status\"}", NULL, 0) == BST_OK, "everyone: status");

    /* malformed requests */
    check(req(0, "not json", NULL, 0) == BST_ERROR, "not JSON → refused");
    check(req(0, "{\"x\":1}", NULL, 0) == BST_ERROR, "no op → refused");
    check(req(0, "{\"op\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}", NULL, 0) == BST_ERROR, "an op over 32 bytes → refused");

    /* files */
    int f = file_fd("budget.txt", "Budget\n");
    check(req(0, "{\"op\":\"send\"}", &f, 1) == BST_OK && req_nfds == 1, "Mail: send with a regular file");
    f = file_fd("x.txt", "x");
    check(req(0, "{\"op\":\"list\"}", &f, 1) == BST_ERROR, "a file with a request that takes none → refused");
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    check(req(0, "{\"op\":\"send\"}", &sp[0], 1) == BST_ERROR && !strcmp(last_reply, "fs.outsideHome"), "a socket instead of a file → refused");
    close(sp[1]);
    int dfd = open(home, O_RDONLY | O_DIRECTORY);
    check(req(3, "{\"op\":\"contact-import\"}", &dfd, 1) == BST_ERROR, "a folder fd → refused");
    char wpath[PATH_MAX + 64];
    snprintf(wpath, sizeof wpath, "%s/w.txt", home);
    int wo = open(wpath, O_WRONLY | O_CREAT, 0600);
    check(req(3, "{\"op\":\"contact-import\"}", &wo, 1) == BST_ERROR, "a write-only fd → refused");
    f = file_fd("dana.vcf", "BEGIN:VCARD\r\nEND:VCARD\r\n");
    check(req(3, "{\"op\":\"contact-import\"}", &f, 1) == BST_OK && req_nfds == 1, "Contacts: import a .vcf fd");
    f = file_fd("e.ics", "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
    check(req(1, "{\"op\":\"ev-import\"}", &f, 1) == BST_OK, "Calendar: import an .ics fd");
    f = file_fd("mail.txt", "x");
    check(req(1, "{\"op\":\"send\"}", &f, 1) == BST_DENIED, "Calendar cannot send a file as mail");

    /* a large request in a memfd ("reqm") */
    {
        int m = memfd_create("req", 0);
        const char *big = "{\"op\":\"send\",\"text\":\"long body\"}";
        if (write(m, big, strlen(big)) < 0) { /* checked below */ }
        int fds2[2] = { m, file_fd("a2.txt", "attachment") };
        check(call(0, "reqm", "", 0, fds2, 2) == BST_OK && strstr(req_json, "long body") && req_nfds == 1, "reqm: JSON from a memfd, one attachment");
    }

    /* Mail-only verbs */
    check(call(1, "config", "", 0, NULL, 0) == BST_DENIED, "Calendar cannot read the accounts file");
    check(call(0, "config", "", 0, NULL, 0) == BST_OK, "Mail reads the accounts file");
    const char *acc = "{\"accounts\":[]}";
    check(call(3, "save", acc, strlen(acc), NULL, 0) == BST_DENIED, "Contacts cannot write the accounts file");
    check(call(0, "save", acc, strlen(acc), NULL, 0) == BST_OK && !strcmp(saved, acc), "Mail writes the accounts file (validated by mailc)");
    const char *bad = "{\"accounts\":[{\"password\":\"x\"}]}";
    check(call(0, "save", bad, strlen(bad), NULL, 0) == BST_ERROR && strstr(last_reply, "mail.badAccounts"), "a password in the accounts file → mail.badAccounts");
    check(call(2, "images", "https://a.example/x.png\0", 24, NULL, 0) == BST_DENIED, "Reminders cannot open image hosts");
    images_n = 0;
    check(call(0, "images", "https://a.example/x.png\0https://b.example/y.png\0", 48, NULL, 0) == BST_OK && images_n == 2, "Mail: remote images of one message");

    /* attachments */
    check(call(0, "att", "att/acct/7-1/report.pdf", 23, NULL, 0) == BST_OK && last_fd >= 0, "Mail: a read-only fd of a cached attachment");
    if (last_fd >= 0) { int fl = fcntl(last_fd, F_GETFL); check((fl & O_ACCMODE) == O_RDONLY, "…opened read-only"); }
    check(call(0, "att", "att/../../secret.txt", 20, NULL, 0) == BST_DENIED, "\"..\" in an attachment path → refused");
    check(call(0, "att", "accounts.json", 13, NULL, 0) == BST_DENIED, "outside att/ → refused");
    check(call(0, "att", "att/acct/7-1/link", 17, NULL, 0) == BST_DENIED, "a symlink out of the cache → refused");
    check(call(1, "att", "att/acct/7-1/report.pdf", 23, NULL, 0) == BST_DENIED, "Calendar cannot read attachments");
    shell_kind[0] = 0;
    check(call(0, "open", "att/acct/7-1/report.pdf\0report.pdf", 34, NULL, 0) == BST_OK && !strcmp(shell_kind, "mailopen") && strstr(shell_json, "report.pdf"),
          "Mail: the session opens a cached attachment");
    shell_kind[0] = 0;
    check(call(0, "open", "att/../secret.txt\0x", 19, NULL, 0) == BST_DENIED && !shell_kind[0], "…never a path outside the cache");

    /* opening another system app */
    check(sysopen(3, "mail", 1, "{\"compose\":{\"to\":[{\"email\":\"dana@lordos.test\"}]}}") == BST_OK && !strcmp(shell_kind, "launch") && strstr(shell_json, "\"new\":true"),
          "Contacts → Mail: a new message (session event)");
    check(sysopen(0, "calendar", 0, "{\"at\":1,\"uid\":\"x\"}") == BST_OK, "Mail → Calendar: an invitation");
    check(sysopen(0, "files", 0, "{}") == BST_DENIED && !shell_kind[0], "not to an app outside the four");
    check(sysopen(0, "pdf", 0, "{\"path\":\"/home/x/a.pdf\"}") == BST_DENIED, "not to the PDF viewer with a path");
    check(sysopen(0, "calendar", 0, "{\"path\":\"/etc/passwd\"}") == BST_DENIED, "no \"path\" in the args");
    check(sysopen(0, "calendar", 0, "[1,2]") == BST_DENIED, "args must be an object");
    check(sysopen(5, "mail", 0, "{}") == BST_DENIED, "a user package cannot open them");

    brokermail_conn_closed(0);
    if (last_fd >= 0) close(last_fd);
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) { /* best effort */ }
    printf("brokermail-test: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
