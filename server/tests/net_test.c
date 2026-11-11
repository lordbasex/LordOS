/* net_test.c — tests of the network module without lord-server (see docs/NETWORK.md).
 *
 *   net-test crypto                  X25519 (RFC 7748), WPA PBKDF2 (IEEE 802.11i), base64
 *   net-test state                   what lord.net sees without root (interfaces, routes, DNS, VPN)
 *   net-test monitor <seconds>       rtnetlink notifications
 *   net-test ovpn-inline <path>      the .ovpn with its files embedded (lord.net.prepareOvpn)
 *   net-test errors                  config errors are code lines (errcode.h), never prose
 *   net-test ifwatch                 an interface gets its saved state once, whenever it appears
 *   net-test wake                    Wi-Fi after waking from sleep: checked, re-associated, turned off and on
 *   net-test wifi-saved              a saved network's key file: what a join without a password keeps
 *   sudo net-test health <if>        the after-sleep check of one Wi-Fi interface (needs its wpa_supplicant)
 *   sudo net-test arp <if> <src> <ip> does ip answer an ARP request from src (the after-sleep gateway check)
 *   sudo net-test dhcp <if>          the DHCP client in the foreground (SIGUSR2: confirm the lease, as after sleep)
 *   sudo net-test act <action> <arg> runs a "net-*" action the way the helper would;
 *                                    "\n" in arg is a newline, "@file" reads it
 *   sudo net-test daemons            the tests' resident processes
 *
 * Replaces the helper's functions (admin_say, admin_daemon_*...) with its own: the
 * resident processes stay in the background, with their output in /run/lordos/test/<name>.log,
 * so one action after another can be run from the command line. */
#define LORD_NET_ACTIONS
#include "net/net.h"
#include "net/netpriv.h"
#include "net/vpn.h"
#include "net/ifwatch.h"
#include "net/wake.h"
#include "errcode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_DIR "/run/lordos/test"

/* ---- helper replacement ---- */

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

void admin_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    printf("[%ld] EVENT ", (long)t.tv_sec);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static int run_child(const char *const argv[], int out) {
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(out >= 0 ? out : null, 1);
        dup2(out >= 0 ? out : null, 2);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

int admin_run(const char *const argv[]) { return run_child(argv, 1); }

int admin_run_capture(const char *const argv[], char *out, size_t cap) {
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(p[1], 1); dup2(null, 2);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(p[0], out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static void paths(const char *name, char *pid, char *log) {
    snprintf(pid, 128, TEST_DIR "/%s.pid", name);
    snprintf(log, 128, TEST_DIR "/%s.log", name);
}

int admin_daemon_running(const char *name) {
    char pp[128], lp[128];
    paths(name, pp, lp);
    FILE *f = fopen(pp, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    return pid > 0 && kill(pid, 0) == 0;
}

static int daemonize(const char *name) {
    mkdir("/run/lordos", 0755);
    mkdir(TEST_DIR, 0755);
    char pp[128], lp[128];
    paths(name, pp, lp);
    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) { FILE *f = fopen(pp, "w"); if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); } usleep(100000); }
        return pid < 0 ? -1 : 1;
    }
    setsid();
    int fd = open(lp, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
    int null = open("/dev/null", O_RDONLY);
    dup2(null, 0);
    return 0;
}

int admin_daemon_fork(const char *name, void (*fn)(void *), void *arg) {
    if (admin_daemon_running(name)) return -1;
    int r = daemonize(name);
    if (r) return r < 0 ? -1 : 0;
    fn(arg);
    admin_event("daemon %s exit 0", name);
    _exit(0);
}

int admin_daemon_exec(const char *name, const char *const argv[], const char *const env[]) {
    if (admin_daemon_running(name)) return -1;
    int r = daemonize(name);
    if (r) return r < 0 ? -1 : 0;
    char *const def[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
    execve(argv[0], (char *const *)argv, env ? (char *const *)env : def);
    _exit(127);
}

int admin_daemon_stop(const char *name) {
    char pp[128], lp[128];
    paths(name, pp, lp);
    FILE *f = fopen(pp, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid <= 0 || kill(pid, SIGTERM)) return -1;
    for (int i = 0; i < 60 && kill(pid, 0) == 0; i++) usleep(100000);
    unlink(pp);
    return 0;
}

int admin_daemon_signal(const char *name, int sig) {
    char pp[128], lp[128];
    paths(name, pp, lp);
    FILE *f = fopen(pp, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    return pid > 0 && kill(pid, sig) == 0 ? 0 : -1;
}

/* ---- tabla ---- */

enum { A_FORK = 1, A_ANYONE = 2 };
typedef void (*ActionFn)(const AdminReq *r, const char *arg, size_t len);
static const struct { const char *name; ActionFn fn; int flags; } actions[] = { NET_ADMIN_ACTIONS };

/* ---- pruebas ---- */

static int hexeq(const uint8_t *b, size_t n, const char *hex) {
    char s[200];
    for (size_t i = 0; i < n; i++) sprintf(s + 2 * i, "%02x", b[i]);
    return !strcmp(s, hex);
}

static void unhex(const char *h, uint8_t *out) {
    for (size_t i = 0; h[2 * i]; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}

static int test_crypto(void) {
    int fails = 0;
    /* RFC 7748 5.2 */
    uint8_t k[32], u[32], out[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
    x25519(out, k, u);
    int ok = hexeq(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    printf("x25519 vector 1: %s\n", ok ? "ok" : "BAD"); fails += !ok;
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u);
    x25519(out, k, u);
    ok = hexeq(out, 32, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    printf("x25519 vector 2: %s\n", ok ? "ok" : "BAD"); fails += !ok;
    /* RFC 7748 6.1: Alice's and Bob's keys */
    uint8_t a[32], b[32], ap[32], bp[32], s1[32], s2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    x25519_public(ap, a);
    x25519_public(bp, b);
    ok = hexeq(ap, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a") &&
         hexeq(bp, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(s1, a, bp);
    x25519(s2, b, ap);
    ok = ok && !memcmp(s1, s2, 32) && hexeq(s1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    printf("x25519 Alice/Bob: %s\n", ok ? "ok" : "BAD"); fails += !ok;
    /* IEEE 802.11i H.4.1 */
    uint8_t psk[32];
    pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"IEEE", 4, 4096, psk, 32);
    ok = hexeq(psk, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    printf("pbkdf2 WPA: %s\n", ok ? "ok" : "BAD"); fails += !ok;
    pbkdf2_sha1((const uint8_t *)"ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, 4096, psk, 32);
    ok = hexeq(psk, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
    printf("pbkdf2 WPA 2: %s\n", ok ? "ok" : "BAD"); fails += !ok;
    char enc[64];
    uint8_t dec[40];
    b64_encode(ap, 32, enc, sizeof enc);
    ok = b64_decode(enc, dec, sizeof dec) == 32 && !memcmp(dec, ap, 32) && strlen(enc) == 44;
    printf("base64: %s (%s)\n", ok ? "ok" : "BAD", enc); fails += !ok;
    return fails;
}

/* The UI translates these with t('errors.<code>'): the test matches codes and params, not text. */
static int test_errors(void) {
    int fails = 0, ok;
    char err[256], v[64];
    static const char *const keys[] = { "server", "user", NULL }, *const pem[] = { "ca", NULL };
    VpnKv kv;
    ok = vpn_kv_parse("server=a.example\nnot a pair\n", keys, pem, &kv, err, sizeof err) && lerr_has(err, "vpn.expectedKeyValue")
         && lerr_param(err, "line", v, sizeof v) && !strcmp(v, "2");
    printf("kv without '=': %s (%s)\n", ok ? "ok" : "BAD", err); fails += !ok;
    ok = vpn_kv_parse("color=red\n", keys, pem, &kv, err, sizeof err) && lerr_has(err, "vpn.unknownKey")
         && lerr_param(err, "key", v, sizeof v) && !strcmp(v, "color");
    printf("unknown key: %s (%s)\n", ok ? "ok" : "BAD", err); fails += !ok;
    ok = vpn_kv_parse("ca=not pem\n", keys, pem, &kv, err, sizeof err) && lerr_has(err, "vpn.pemRequired");
    printf("PEM required: %s (%s)\n", ok ? "ok" : "BAD", err); fails += !ok;
    ok = !vpn_kv_parse("server=a.example\nuser=me\n", keys, pem, &kv, err, sizeof err) && !strcmp(vpn_kv(&kv, "user"), "me");
    if (ok) vpn_kv_free(&kv);
    printf("valid kv: %s\n", ok ? "ok" : "BAD"); fails += !ok;
    ok = !net_ovpn_inline("/nonexistent/x y.ovpn", err, sizeof err) && lerr_has(err, "sys.readFailed")
         && lerr_param(err, "path", v, sizeof v) && !strcmp(v, "/nonexistent/x y.ovpn") && strstr(err, "x%20y");
    printf("ovpn missing file: %s (%s)\n", ok ? "ok" : "BAD", err); fails += !ok;
    return fails;
}

static void print_json(const char *what, char *j) {
    printf("%s: %s\n", what, j ? j : "(error)");
    free(j);
}

static void on_change(const char *what) { printf("change: %s\n", what); fflush(stdout); }

/* ---- ifwatch: fakes for what actions.c supplies ---- */

static int fw_init = 1, fw_managed, fw_busy, fw_fail, fw_applied, fw_stopped;
static char fw_last[16];
static int fw_candidate(const char *n) { return n[0] != 'v'; }             /* v…: virtual, no config */
static int fw_initialized(int index) { (void)index; return fw_init; }
static int fw_is_managed(const char *n) { (void)n; return fw_managed; }
static int fw_is_busy(const char *n) { (void)n; return fw_busy; }
static int fw_apply(const char *n) { snprintf(fw_last, sizeof fw_last, "%s", n); fw_applied++; return fw_fail ? -1 : 0; }
static void fw_stop(const char *n) { (void)n; fw_stopped++; }

static int test_ifwatch(void) {
    static const IfOps o = { fw_candidate, fw_initialized, fw_is_managed, fw_is_busy, fw_apply, fw_stop };
    IfTable t = {0};
    int bad = 0, got = 0;
#define IW(x) do { if (!(x)) { printf("FAIL ifwatch:%d: %s\n", __LINE__, #x); bad++; } } while (0)
    /* boot: the wired NIC is there, the radio isn't yet */
    IfNow boot[] = { { 1, "eth0" }, { 2, "veth0" } };
    IW(ifwatch_step(&t, boot, 2, &o, 0, 1, &got) == 0 && got == 1 && fw_applied == 1);
    /* the same interfaces: a notification changes nothing, also after net-down */
    IW(ifwatch_step(&t, boot, 2, &o, 100, 0, &got) == 0 && fw_applied == 1);
    /* the radio appears 9 s later: never at boot, not forgotten either */
    IfNow late[] = { { 1, "eth0" }, { 2, "veth0" }, { 3, "wlan0" } };
    fw_init = 0;
    IW(ifwatch_step(&t, late, 3, &o, 9000, 0, &got) == 1 && fw_applied == 1);
    /* udev renames it: the config of the final name is the one applied */
    IfNow named[] = { { 1, "eth0" }, { 2, "veth0" }, { 3, "wlp3s0" } };
    fw_init = 1;
    IW(ifwatch_step(&t, named, 3, &o, 9500, 0, &got) == 1 && fw_applied == 1);     /* the grace second */
    IW(ifwatch_step(&t, named, 3, &o, 10100, 0, &got) == 0 && fw_applied == 2 && !strcmp(fw_last, "wlp3s0"));
    IW(ifwatch_step(&t, named, 3, &o, 10600, 0, &got) == 0 && fw_applied == 2);
    /* it leaves (a driver reload): its daemons stop, and the new device waits for them */
    IW(ifwatch_step(&t, boot, 2, &o, 11000, 0, &got) == 0 && fw_stopped == 1);
    IfNow back[] = { { 1, "eth0" }, { 2, "veth0" }, { 4, "wlp3s0" } };
    fw_busy = 1;
    IW(ifwatch_step(&t, back, 3, &o, 12100, 0, &got) == 1 && fw_applied == 2);
    fw_busy = 0;
    IW(ifwatch_step(&t, back, 3, &o, 13200, 0, &got) == 0 && fw_applied == 3);
    /* it belongs to the system's network manager: left alone */
    IfNow sys[] = { { 1, "eth0" }, { 2, "veth0" }, { 4, "wlp3s0" }, { 5, "enx1" } };
    fw_managed = 1;
    IW(ifwatch_step(&t, sys, 4, &o, 13300, 0, &got) == 1);
    IW(ifwatch_step(&t, sys, 4, &o, 14400, 0, &got) == 0 && fw_applied == 3);
    fw_managed = 0;
    /* a failure is retried a few times, then it stops trying */
    IfNow bad6[] = { { 1, "eth0" }, { 2, "veth0" }, { 4, "wlp3s0" }, { 5, "enx1" }, { 6, "wlan1" } };
    fw_fail = 1;
    IW(ifwatch_step(&t, bad6, 5, &o, 20000, 0, &got) == 1);
    IW(ifwatch_step(&t, bad6, 5, &o, 21100, 0, &got) == 1 && fw_applied == 4);
    IW(ifwatch_step(&t, bad6, 5, &o, 22000, 0, &got) == 1 && fw_applied == 4);     /* not before its time */
    IW(ifwatch_step(&t, bad6, 5, &o, 26200, 0, &got) == 1 && fw_applied == 5);
    IW(ifwatch_step(&t, bad6, 5, &o, 31300, 0, &got) == 0 && fw_applied == 6);
    IW(ifwatch_step(&t, bad6, 5, &o, 40000, 0, &got) == 0 && fw_applied == 6);
    fw_fail = 0;
    /* udev never finishes: not forever */
    IfNow slow[] = { { 1, "eth0" }, { 2, "veth0" }, { 7, "wlan2" } };
    fw_init = 0;
    IW(ifwatch_step(&t, slow, 3, &o, 50000, 0, &got) == 1);
    IW(ifwatch_step(&t, slow, 3, &o, 50000 + IFW_WAIT_MS, 0, &got) == 0 && !strcmp(fw_last, "wlan2"));
#undef IW
    printf("ifwatch: %s\n", bad ? "FAILED" : "ok");
    return bad;
}

/* ---- wake: fakes for what actions.c supplies ---- */

static int wk_on = 1, wk_state = WK_OK, wk_busy_n, wk_renews, wk_reassocs, wk_stops, wk_starts, wk_fix_on;
static char wk_last[32];
static int wkf_active(const char *n) { (void)n; return wk_on; }
static int wkf_health(const char *n) { (void)n; return wk_state; }
static void wkf_renew(const char *n) { (void)n; wk_renews++; }
static void wkf_reassoc(const char *n) { (void)n; wk_reassocs++; if (wk_fix_on == 1) wk_state = WK_OK; }
static void wkf_stop(const char *n) { (void)n; wk_stops++; wk_busy_n = 2; }
static int wkf_busy(const char *n) { (void)n; return wk_busy_n > 0 ? wk_busy_n-- : 0; }
static void wkf_start(const char *n) { (void)n; wk_starts++; if (wk_fix_on == 2) wk_state = WK_OK; }
static void wkf_log(const char *n, const char *what, int why) { (void)n; (void)why; snprintf(wk_last, sizeof wk_last, "%s", what); }

/* Runs the steps it asks for until it's done or ms reaches until; returns the ms it finished at (-1: not done). */
static long wk_run(WakeTable *t, const WakeOps *o, long from, long until) {
    long at = from + WAKE_SETTLE_MS;
    while (at >= 0 && at <= until) {
        long next = wake_step(t, o, at);
        if (next < 0) return at;
        at = next;
    }
    return -1;
}

static int test_wake(void) {
    static const WakeOps o = { wkf_active, wkf_health, wkf_renew, wkf_reassoc, wkf_stop, wkf_busy, wkf_start, wkf_log };
    const char *one[] = { "wlan0" };
    WakeTable t = {0};
    int bad = 0;
#define WK(x) do { if (!(x)) { printf("FAIL wake:%d: %s\n", __LINE__, #x); bad++; } } while (0)
    /* it works after waking: nothing touched but the lease, which gets confirmed */
    wake_begin(&t, one, 1, 0);
    WK(wake_step(&t, &o, 1000) == WAKE_SETTLE_MS);                 /* not before it settles */
    WK(wk_run(&t, &o, 0, 60000) == WAKE_SETTLE_MS && wk_renews == 1 && !wk_reassocs && !wk_stops && !strcmp(wk_last, "ok"));
    /* it associates late but on its own (within the first seconds): no re-association */
    wk_state = WK_NOT_ASSOCIATED;
    wake_begin(&t, one, 1, 100000);
    WK(wake_step(&t, &o, 100000 + WAKE_SETTLE_MS) == 100000 + WAKE_SETTLE_MS + WAKE_CHECK_MS);
    wk_state = WK_OK;
    WK(wake_step(&t, &o, 100000 + WAKE_SETTLE_MS + WAKE_CHECK_MS) == -1 && wk_renews == 2 && !wk_reassocs);
    /* the case of issue #14: associated, "up", but the gateway never answers; re-associating fixes it */
    wk_state = WK_NO_GATEWAY;
    wk_fix_on = 1;
    wake_begin(&t, one, 1, 200000);
    long done = wk_run(&t, &o, 200000, 300000);
    WK(done >= 200000 + WAKE_REASSOC_MS && done < 200000 + WAKE_REASSOC_MS + WAKE_SETTLE_MS + WAKE_CHECK_MS + 1);
    WK(wk_reassocs == 1 && !wk_stops && !strcmp(wk_last, "recovered-reassociate"));
    /* re-associating isn't enough: the card off and on, as by hand, once its daemons have exited */
    wk_state = WK_NO_GATEWAY;
    wk_fix_on = 2;
    wake_begin(&t, one, 1, 400000);
    done = wk_run(&t, &o, 400000, 500000);
    WK(done > 400000 + WAKE_REASSOC_MS + WAKE_RESET_MS && wk_reassocs == 2 && wk_stops == 1 && wk_starts == 1 && !wk_busy_n);
    WK(!strcmp(wk_last, "recovered-reset"));
    /* nothing helps: it gives up, says so, and doesn't loop */
    wk_state = WK_NO_CARRIER;
    wk_fix_on = 0;
    wake_begin(&t, one, 1, 600000);
    done = wk_run(&t, &o, 600000, 700000);
    WK(done >= 600000 + WAKE_REASSOC_MS + WAKE_RESET_MS + WAKE_GIVEUP_MS && wk_reassocs == 3 && wk_stops == 2 && wk_starts == 2);
    WK(!strcmp(wk_last, "failed") && t.n == 0);
    /* turned off (net-down) while being looked at: left alone */
    wake_begin(&t, one, 1, 800000);
    wk_on = 0;
    WK(wk_run(&t, &o, 800000, 900000) == 800000 + WAKE_SETTLE_MS && wk_reassocs == 3 && !strcmp(wk_last, "skipped"));
    wk_on = 1;
    /* a new wake replaces a round still going; no interfaces: nothing to do */
    wake_begin(&t, one, 1, 1000000);
    wake_begin(&t, one, 0, 1000500);
    WK(t.n == 0 && wake_step(&t, &o, 1010000) == -1);
#undef WK
    printf("wake: %s\n", bad ? "FAILED" : "ok");
    return bad;
}

/* GitHub #124: a click on a saved network joins it without a password, and that join must keep the
 * saved key (a password, or the derived key the first-run file leaves) instead of saving the network
 * again as an open one. wifi_act_connect keeps the saved file whenever this says it holds a key. */
static int test_wifi_saved(void) {
    int bad = 0;
#define WS(x) do { if (!(x)) { printf("FAIL wifi-saved:%d: %s\n", __LINE__, #x); bad++; } } while (0)
    WS(wifi_saved_kind(NULL) == WIFI_SAVED_OPEN);                           /* nothing saved */
    WS(wifi_saved_kind("pass=\n") == WIFI_SAVED_OPEN);                      /* a saved open network */
    WS(wifi_saved_kind("") == WIFI_SAVED_OPEN);
    WS(wifi_saved_kind("pass=hunter22\n") == WIFI_SAVED_PASS);
    WS(wifi_saved_kind("pass=a password with spaces\n") == WIFI_SAVED_PASS);
    WS(wifi_saved_kind("psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n") == WIFI_SAVED_PSK);
    /* a broken file isn't a key: joining it as before is all that can be done */
    WS(wifi_saved_kind("psk=0123\n") == WIFI_SAVED_OPEN);
    WS(wifi_saved_kind("psk=0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\n") == WIFI_SAVED_OPEN);
    WS(wifi_saved_kind("pass=bad\x01key\n") == WIFI_SAVED_OPEN);
#undef WS
    printf("wifi-saved: %s\n", bad ? "FAILED" : "ok");
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: net-test crypto | errors | state | monitor <s> | act <action> <arg> | daemons\n"); return 2; }
    if (!strcmp(argv[1], "crypto")) return test_crypto() ? 1 : 0;
    if (!strcmp(argv[1], "errors")) return test_errors() ? 1 : 0;
    if (!strcmp(argv[1], "ifwatch")) return test_ifwatch() ? 1 : 0;
    if (!strcmp(argv[1], "wake")) return test_wake() ? 1 : 0;
    if (!strcmp(argv[1], "wifi-saved")) return test_wifi_saved() ? 1 : 0;
    if (!strcmp(argv[1], "dhcp") && argc > 2) { dhcp_run(argv[2], 0); return 0; }
    if (!strcmp(argv[1], "arp") && argc > 4) {
        int fam;
        uint8_t src[16], dst[16];
        if (parse_ip(argv[3], &fam, src) || fam != AF_INET || parse_ip(argv[4], &fam, dst) || fam != AF_INET) return 2;
        uint32_t s4, d4;
        memcpy(&s4, src, 4);
        memcpy(&d4, dst, 4);
        int r = arp_probe(argv[2], s4, d4, 3, 300);
        printf("%s %s\n", argv[4], r ? "no answer" : "answers");
        return r ? 1 : 0;
    }
    if (!strcmp(argv[1], "health") && argc > 2) {
        int h = net_wake_health(argv[2]);
        printf("%s: %s\n", argv[2], wake_why(h));
        return h ? 1 : 0;
    }
    if (!strcmp(argv[1], "state")) {
        print_json("interfaces", net_interfaces_json());
        print_json("routes", net_routes_json());
        print_json("dns", net_dns_json());
        print_json("vpn", net_vpn_list_json());
        print_json("wifi", net_wifi_known_json());
        print_json("vpnTypes", net_vpn_types_json());
        return 0;
    }
    if (!strcmp(argv[1], "ovpn-inline") && argc > 2) {
        char err[256];
        char *t = net_ovpn_inline(argv[2], err, sizeof err);
        if (!t) { fprintf(stderr, "%s\n", err); return 1; }
        fputs(t, stdout);
        free(t);
        return 0;
    }
    if (!strcmp(argv[1], "monitor")) {
        if (net_open()) { perror("net_open"); return 1; }
        int secs = argc > 2 ? atoi(argv[2]) : 10;
        time_t end = time(NULL) + secs;
        while (time(NULL) < end) {
            struct pollfd pf = { net_fd(), POLLIN, 0 };
            if (poll(&pf, 1, 500) > 0) net_dispatch(on_change);
        }
        return 0;
    }
    if (!strcmp(argv[1], "daemons")) {
        DIR *d = opendir(TEST_DIR);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcmp(e->d_name + l - 4, ".pid")) {
                char n[128];
                snprintf(n, sizeof n, "%.*s", (int)(l - 4), e->d_name);
                printf("%s %s\n", n, admin_daemon_running(n) ? "corriendo" : "terminado");
            }
        }
        if (d) closedir(d);
        return 0;
    }
    if (!strcmp(argv[1], "act") && argc >= 3) {
        const char *src = argc > 3 ? argv[3] : "";
        char *arg;
        if (src[0] == '@') {
            /* "@file" or "prefix\n@file": the file is appended at the end */
            arg = read_file(src + 1, ADMIN_ARG_MAX, NULL);
        } else {
            arg = malloc(strlen(src) + 1);
            size_t o = 0;
            for (const char *p = src; *p; p++) {
                if (p[0] == '\\' && p[1] == 'n') { arg[o++] = '\n'; p++; }
                else arg[o++] = *p;
            }
            arg[o] = 0;
            char *at = strstr(arg, "\n@");
            if (at) {
                char *f = read_file(at + 2, ADMIN_ARG_MAX, NULL);
                if (!f) { fprintf(stderr, "could not read %s\n", at + 2); return 1; }
                *at = 0;
                char *joined = malloc(strlen(arg) + strlen(f) + 2);
                sprintf(joined, "%s\n%s", arg, f);
                free(arg); free(f);
                arg = joined;
            }
        }
        if (!arg) return 1;
        AdminReq req = { .id = 1, .user = "root", .home = "/root" };
        for (size_t i = 0; i < sizeof actions / sizeof actions[0]; i++)
            if (!strcmp(actions[i].name, argv[2])) { actions[i].fn(&req, arg, strlen(arg)); free(arg); return 0; }
        fprintf(stderr, "unknown action\n");
        return 2;
    }
    fprintf(stderr, "comando desconocido\n");
    return 2;
}
