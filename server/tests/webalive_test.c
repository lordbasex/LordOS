/* webalive_test.c — a web app that hangs is started again (docs/WEBAPPS.md, "When a web app stops answering"), end to end
 * on a private session bus (run by tests/run_suites.sh under dbus-run-session) against build/webalive-fake, a stand-in
 * lord-web that answers org.lordos.WebApp1.Ping until told to hang (tests/webalive_fake.c). lord-server's side is the
 * real code: ui/webalive.c (the pings, the verdict, the restart and its event), web/webmgr.c (the processes) and
 * web/webwatch.c (the rules, with short limits here). Checked:
 *   - one that answers is left alone;
 *   - one that hangs is ended, with the child that left its process group, and started again; the shell gets
 *     "webapps-restarted" { app, profile };
 *   - asking for it (webalive_urgent, what a launch of a running app does) finds a hang in one short wait;
 *   - an older lord-web that answers Ping with UnknownMethod is alive;
 *   - after WEBALIVE_RESTARTS restarts it is left alone, unless the person asks for it;
 *   - Force Quit, Restart and Restart all (webmgr_quit, webmgr_restart, webmgr_restart_all): only our processes, the
 *     child that left the group too; another app's process is untouched.
 *
 *   make -C server build/webalive-test && dbus-run-session -- server/build/webalive-test   (also in `make test`) */
#include "../src/ui/webalive.h"
#include "../src/web/webmgr.h"
#include "../src/web/webmem.h"
#include "../src/web/webreg.h"
#include "../src/vault/dbus.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[512];
static DConn conn;
static int restarted_events;
static char last_event[256];

/* the shell's side: the events lord-server emits */
void hw_emit(const char *kind, const char *json) {
    if (strcmp(kind, "webapps-restarted")) return;
    restarted_events++;
    snprintf(last_event, sizeof last_event, "%s", json);
}

static double mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* lord-server's main loop for ms: the bus's messages (replies to webalive), webmgr_reap, webalive_tick */
static void pump(double ms) {
    double end = mono_ms() + ms;
    while (mono_ms() < end) {
        struct pollfd p = { conn.fd, POLLIN, 0 };
        poll(&p, 1, 20);
        DMsg m;
        while (dbus_next(&conn, &m) == 1) {
            if ((m.type == DBUS_RETURN || m.type == DBUS_ERROR) && m.reply_serial) webalive_reply(&m);
            dbus_msg_free(&m);
        }
        dbus_flush(&conn);
        webmgr_reap();
        webalive_tick(mono_ms());
    }
}

static void write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)) printf("FAIL: could not write %s\n", path);
    if (fd >= 0) close(fd);
}

static int read_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = 0;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
    return v;
}

/* the running pid of app/profile in webmgr's table, 0 none */
static pid_t running_pid(const char *app, const char *profile) {
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING);
    for (int i = 0; i < n; i++) if (!strcmp(r[i].app, app) && !strcmp(r[i].profile, profile)) return r[i].pid;
    return 0;
}

/* a process is gone: no /proc entry, or a zombie */
static int gone(pid_t pid) {
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return 1;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *e = strrchr(b, ')');
    return !e || e[2] == 'Z' || e[2] == 'X';
}

/* gone within ms (a KILL is delivered a moment later, and init reaps the orphan) */
static int gone_soon(pid_t pid, double ms) {
    double end = mono_ms() + ms;
    while (!gone(pid) && mono_ms() < end) { struct timespec ts = { 0, 10 * 1000000 }; nanosleep(&ts, NULL); }
    return gone(pid);
}

/* waits (pumping) until the fake for app/profile has taken its name: its start file is there */
static pid_t wait_started(const char *app, const char *profile, double ms) {
    double end = mono_ms() + ms;
    while (mono_ms() < end) {
        pid_t pid = running_pid(app, profile);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/start-%s-%d", root, app, (int)pid);
        struct stat st;
        if (pid > 0 && !stat(p, &st)) return pid;
        pump(20);
    }
    return 0;
}

static pid_t child_of(pid_t pid) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/child-%d", root, (int)pid);
    return (pid_t)read_int(p);
}

static void hang(pid_t pid) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/hang-%d", root, (int)pid);
    write_text(p, "");
}

/* waits (pumping) for a restart event beyond `before`; the time it took, or -1 */
static double wait_restart(int before, double ms) {
    double t0 = mono_ms();
    while (mono_ms() - t0 < ms) {
        pump(20);
        if (restarted_events > before) return mono_ms() - t0;
    }
    return -1;
}

int main(void) {
    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) { printf("webalive-test: skipped: needs a session bus (dbus-run-session)\n"); return 0; }
    char self[PATH_MAX], fake[PATH_MAX + 32];
    ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sn <= 0) return 2;
    self[sn] = 0;
    *strrchr(self, '/') = 0;
    snprintf(fake, sizeof fake, "%s/webalive-fake", self);

    char tmpl[] = "/tmp/webalive-test.XXXXXX";
    if (!mkdtemp(tmpl)) { printf("FAIL: mkdtemp\n"); return 1; }
    snprintf(root, sizeof root, "%s", tmpl);
    setenv("HOME", root, 1);
    char p[1024], q[4096];
    snprintf(p, sizeof p, "%s/run", root);
    mkdir(p, 0700);
    setenv("XDG_RUNTIME_DIR", p, 1);
    setenv("WAYLAND_DISPLAY", "wayland-test", 1);
    snprintf(p, sizeof p, "%s/assets/webapps/icons", root);
    snprintf(q, sizeof q, "mkdir -p '%s'", p);
    CHECK(system(q) == 0, "assets folder");
    snprintf(p, sizeof p, "%s/assets/webapps/catalog.json", root);
    write_text(p, "{\"schema\":1,\"apps\":["
                  "{\"id\":\"chat\",\"name\":\"Chat\",\"startUrl\":\"https://web.chat.example/\",\"scope\":\"https://web.chat.example/\","
                  "\"origin\":\"https://web.chat.example\",\"icon\":\"chat.png\",\"themeColor\":\"#25d366\",\"background\":true},"
                  "{\"id\":\"mail\",\"name\":\"Mail\",\"startUrl\":\"https://mail.example/\",\"scope\":\"https://mail.example/\","
                  "\"origin\":\"https://mail.example\",\"icon\":\"mail.png\",\"themeColor\":\"#2255cc\",\"background\":false}]}");
    snprintf(p, sizeof p, "%s/assets", root);
    webmgr_init(p);
    webmgr_set_helper(fake);
    CHECK(webmgr_available(), "the stand-in is usable (%s)", fake);
    char app[WEBREG_ID_MAX + 1];
    const char *cat = "{\"source\":\"catalog\",\"id\":\"chat\",\"profile\":\"Personal\"}";
    CHECK(!webmgr_install(cat, strlen(cat), app, sizeof app), "chat installed");
    cat = "{\"source\":\"catalog\",\"id\":\"mail\",\"profile\":\"Personal\"}";
    CHECK(!webmgr_install(cat, strlen(cat), app, sizeof app), "mail installed");

    int fd = dbus_connect_session();
    CHECK(fd >= 0 && !dbus_open(&conn, fd, 5000), "lord-server's connection to the bus");
    if (fails) return 1;
    /* short limits: a ping every 300 ms, 400 ms to answer, 300 ms when urgent, two misses, 600 ms of grace */
    webalive_set_limits((WebWatchLimits){ .period = 300, .wait = 400, .urgent = 300, .grace = 600, .misses = 2 });
    webalive_attach(&conn);

    /* 1. one that answers is left alone */
    int raised = 0;
    CHECK(!webmgr_launch("chat", "personal", &raised) && !raised, "chat started");
    CHECK(!webmgr_launch("mail", "personal", &raised) && !raised, "mail started");
    pid_t chat = wait_started("chat", "personal", 5000), mail = wait_started("mail", "personal", 5000);
    CHECK(chat > 0 && mail > 0, "both on the bus: %d %d", (int)chat, (int)mail);
    pump(2500);
    CHECK(restarted_events == 0 && running_pid("chat", "personal") == chat && running_pid("mail", "personal") == mail,
          "answering: nothing restarted (%d events)", restarted_events);

    /* 2. it hangs: ended with the child that left its group, started again, and the shell is told */
    pid_t kid = child_of(chat);
    CHECK(kid > 0 && !gone(kid), "the stand-in's child runs: %d", (int)kid);
    hang(chat);
    double took = wait_restart(0, 5000);
    CHECK(took >= 0, "a hang: restarted");
    CHECK(took < 2500, "found within the misses' wait: %.0f ms", took);
    CHECK(!strcmp(last_event, "{\"app\":\"chat\",\"profile\":\"personal\"}"), "the event: %s", last_event);
    pid_t chat2 = wait_started("chat", "personal", 5000);
    CHECK(chat2 > 0 && chat2 != chat, "a new process: %d (was %d)", (int)chat2, (int)chat);
    CHECK(gone_soon(chat, 1000) && gone_soon(kid, 1000), "the hung one and its child are gone");
    CHECK(running_pid("mail", "personal") == mail && !gone(mail), "the other app untouched");

    /* 3. the person asks for it (a launch of a running app): one short wait is enough */
    pump(1000);                                     /* past the new one's grace */
    hang(chat2);
    pump(150);                                      /* the stand-in looks for its hang file every 50 ms */
    CHECK(!webmgr_launch("chat", "personal", &raised) && raised, "a launch of the running app only raises it");
    webalive_urgent("chat", "personal");
    took = wait_restart(1, 3000);
    /* the periodic way would take two misses: at least a period and two waits (1100 ms) */
    CHECK(took >= 0 && took < 800, "asked for: restarted after one short wait: %.0f ms", took);
    pid_t chat3 = wait_started("chat", "personal", 5000);
    CHECK(chat3 > 0 && chat3 != chat2 && gone(chat2), "started again: %d", (int)chat3);

    /* 4. an older lord-web (no Ping: UnknownMethod) is there */
    snprintf(p, sizeof p, "%s/old", root);
    write_text(p, "");
    pump(2000);
    CHECK(restarted_events == 2 && running_pid("chat", "personal") == chat3, "UnknownMethod is an answer: %d events", restarted_events);
    unlink(p);

    /* 5. a third restart, then left alone; the person asking for it still restarts it */
    pump(700);
    hang(chat3);
    CHECK(wait_restart(2, 5000) >= 0, "a third restart");
    pid_t chat4 = wait_started("chat", "personal", 5000);
    pump(700);
    hang(chat4);
    CHECK(wait_restart(3, 3000) < 0 && running_pid("chat", "personal") == chat4, "a fourth hang within 10 minutes: left alone");
    webalive_urgent("chat", "personal");
    CHECK(wait_restart(3, 3000) >= 0, "unless the person asks for it");
    pid_t chat5 = wait_started("chat", "personal", 5000);
    CHECK(chat5 > 0 && gone(chat4), "started again: %d", (int)chat5);

    /* 6. Force Quit, Restart, Restart all: our processes only */
    int was = 0;
    pid_t kid5 = child_of(chat5);
    CHECK(!webmgr_quit("chat", "personal", &was) && was == 1, "force quit");
    CHECK(gone_soon(chat5, 1000) && kid5 > 0 && gone_soon(kid5, 1000) && !running_pid("chat", "personal"), "it and its child are gone");
    CHECK(!webmgr_quit("chat", "personal", &was) && was == 0, "force quit of one that doesn't run: nothing");
    CHECK(webmgr_quit("../x", "personal", &was) && webmgr_restart("chat", "nope", &was) && webmgr_restart("nope", "personal", &was), "bad ids, a profile or app not in the registry");
    CHECK(!webmgr_restart("chat", "personal", &was) && was == 0, "restart of one that doesn't run: started");
    pid_t chat6 = wait_started("chat", "personal", 5000);
    CHECK(chat6 > 0, "running: %d", (int)chat6);
    CHECK(!webmgr_restart("chat", "personal", &was) && was == 1, "restart");
    pid_t chat7 = wait_started("chat", "personal", 5000);
    CHECK(chat7 > 0 && chat7 != chat6 && gone(chat6), "a new process: %d", (int)chat7);
    pid_t kid7 = child_of(chat7), kidm = child_of(mail);
    CHECK(webmgr_restart_all() == 2, "restart all: both");
    pid_t chat8 = wait_started("chat", "personal", 5000), mail2 = wait_started("mail", "personal", 5000);
    CHECK(chat8 > 0 && mail2 > 0 && chat8 != chat7 && mail2 != mail && gone_soon(chat7, 1000) && gone_soon(mail, 1000) && gone_soon(kid7, 1000) && gone_soon(kidm, 1000),
          "every one started again, the old ones and their children gone");
    CHECK(restarted_events == 4, "manual restarts send no hang event: %d", restarted_events);

    /* the end: nothing left running */
    CHECK(!webmgr_quit("chat", "personal", &was) && !webmgr_quit("mail", "personal", &was), "the last ones quit");
    pid_t k1 = child_of(chat8), k2 = child_of(mail2);
    CHECK(gone_soon(chat8, 1000) && gone_soon(mail2, 1000) && gone_soon(k1, 1000) && gone_soon(k2, 1000), "nothing left");
    dbus_close(&conn);
    snprintf(q, sizeof q, "rm -rf '%s'", root);
    if (system(q)) printf("could not remove %s\n", root);
    printf("webalive-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
