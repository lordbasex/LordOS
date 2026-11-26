/* lumo_test.c — the `lumo` command (src/cli/lumo.c) and lord-server's org.lordos.Writing1 (src/ui/writebus.c),
 * docs/AI.md "Writing from the terminal":
 *   - pure: the verbs in English and Spanish, translation targets by code or name, the text's checks, the stream made
 *     safe for a terminal (control and bidi characters, markers, a <think> block, split UTF-8), the model picked, Read's
 *     states;
 *   - on a private dbus-daemon: a stand-in lord-server (writebus.c with a fake engine that streams its answer) and the
 *     real build/lumo: status, a correction streamed to stdout, a translation, text from stdin, Writing off (exit 3 and
 *     where to turn it on), no session (exit 4), a wrong command (2), a text too long (5), Spanish messages by LANG,
 *     and a job nobody else can read.
 * Run by make test (built under ASan and UBSan). Never touches the session's bus. */
#include "llm/lumo.h"
#include "llm/writing.h"
#include "ui/writebus.h"
#include "vault/dbus.h"

#include <errno.h>
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

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- a fake engine for writebus.c ---- */

static int fake_on_engine = 1, next_id, job_id, job_step;
static char job_text[256];
int llm_available(void) { return fake_on_engine; }
char *llm_models_json(void) { return strdup("[{\"id\":\"qwen3-1.7b\",\"bytes\":1},{\"id\":\"qwen3-4b\",\"bytes\":2}]"); }
int llm_model_id_ok(const char *id) { return id && id[0] && strlen(id) <= 48 && !strstr(id, ".."); }
int llm_submit_write(const char *model, const char *action, const char *target, const char *text) {
    (void)model;
    job_id = ++next_id;
    job_step = 0;
    if (!strcmp(action, "translate")) snprintf(job_text, sizeof job_text, "[%s] %s", target, text);
    else snprintf(job_text, sizeof job_text, "\x1b]52;c;x\x07¿Hola, cómo estás? \xe2\x80\xae" "(%s)", action);
    return job_id;
}
char *llm_peek(int id) {
    if (id != job_id) return NULL;
    char *o = malloc(600);
    size_t k = (size_t)job_step * 6 < strlen(job_text) ? (size_t)job_step * 6 : strlen(job_text);
    job_step++;
    char part[300];
    snprintf(part, sizeof part, "%.*s", (int)k, job_text);
    /* JSON-quote the part by hand: the fake's texts have no quotes or backslashes, only controls */
    size_t n = (size_t)snprintf(o, 600, "{\"state\":\"writing\",\"text\":\"");
    for (size_t i = 0; part[i]; i++) {
        unsigned char c = (unsigned char)part[i];
        if (c < 0x20) n += (size_t)snprintf(o + n, 600 - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    snprintf(o + n, 600 - n, "\"}");
    return o;
}
char *llm_take(int id) {
    if (id != job_id || (size_t)job_step * 6 < strlen(job_text) + 6) return NULL;
    char *o = malloc(700);
    size_t n = (size_t)snprintf(o, 700, "{\"ok\":true,\"content\":\"");
    for (size_t i = 0; job_text[i]; i++) {
        unsigned char c = (unsigned char)job_text[i];
        if (c < 0x20) n += (size_t)snprintf(o + n, 700 - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    snprintf(o + n, 700 - n, "\"}");
    job_id = 0;
    return o;
}
void llm_cancel(int id) { if (id == job_id) job_id = 0; }
char *llm_state_json(void) { return strdup("{\"state\":\"ready\",\"model\":\"qwen3-1.7b\",\"pid\":1,\"idleMs\":0,\"queued\":0}"); }

/* ---- pure ---- */

static void pure(void) {
    CHECK(lumo_verb("corregir") == LUMO_FIX && lumo_verb("Correct") == LUMO_FIX && lumo_verb("fix") == LUMO_FIX, "fix verbs");
    CHECK(lumo_verb("mejorar") == LUMO_IMPROVE && lumo_verb("IMPROVE") == LUMO_IMPROVE, "improve verbs");
    CHECK(lumo_verb("resumir") == LUMO_SHORTER && lumo_verb("summarize") == LUMO_SHORTER && lumo_verb("acortar") == LUMO_SHORTER, "shorter verbs");
    CHECK(lumo_verb("traducir") == LUMO_TRANSLATE && lumo_verb("translate") == LUMO_TRANSLATE, "translate verbs");
    CHECK(lumo_verb("estado") == LUMO_STATUS && lumo_verb("status") == LUMO_STATUS && lumo_verb("--help") == LUMO_HELP && lumo_verb("ayuda") == LUMO_HELP, "others");
    CHECK(lumo_verb("rm") == LUMO_NONE && lumo_verb("") == LUMO_NONE && lumo_verb(NULL) == LUMO_NONE && lumo_verb("corregirrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr") == LUMO_NONE, "unknown verbs");
    CHECK(!strcmp(lumo_action(LUMO_SHORTER), "shorter") && !lumo_action(LUMO_STATUS), "bus actions");
    char c[16];
    CHECK(!lumo_lang("en", c) && !strcmp(c, "en"), "en");
    CHECK(!lumo_lang("Inglés", c) && !strcmp(c, "en"), "Inglés");
    CHECK(!lumo_lang("portugues", c) && !strcmp(c, "pt") && !lumo_lang("pt_BR", c) && !strcmp(c, "pt") && !lumo_lang("en-US", c) && !strcmp(c, "en"), "pt, regions");
    CHECK(!lumo_lang("zh-hant", c) && !strcmp(c, "zh-Hant") && !lumo_lang("chino", c) && !strcmp(c, "zh-Hans") && !lumo_lang("fil", c) && !strcmp(c, "fil"), "zh, fil");
    CHECK(lumo_lang("klingon", c) && lumo_lang("xx", c) && lumo_lang("", c) && lumo_lang("../en", c), "unknown languages");
    CHECK(!lumo_text_ok("hola", 4) && lumo_text_ok("  \n ", 4) == -1 && lumo_text_ok("caf\xe9", 4) == -3 && lumo_text_ok("a\0b", 3) == -3, "text checks");
    static char big[LUMO_CHARS_MAX + 2];
    memset(big, 'a', LUMO_CHARS_MAX + 1);
    CHECK(lumo_text_ok(big, LUMO_CHARS_MAX + 1) == -2 && lumo_text_ok(big + 1, LUMO_CHARS_MAX) == 0, "4,000 characters at most");

    /* the stream, safe for a terminal */
    LumoOut o;
    char out[512], all[1024] = "";
    const char *steps[] = { "  \n", "  \nHola", "  \nHola, ¿c\xc3", "  \nHola, ¿cómo\x1b[2J estás?\xe2\x80\xae" "\n", "  \nHola, ¿cómo\x1b[2J estás?\xe2\x80\xae" "\n<<<END-TEXT 0123",
                            "  \nHola, ¿cómo\x1b[2J estás?\xe2\x80\xae" "\n<<<END-TEXT 0123456789abcdef>>>\n\n" };
    lumo_out_init(&o);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) { lumo_out_feed(&o, steps[i], 0, out, sizeof out); strcat(all, out); }
    lumo_out_feed(&o, steps[5], 1, out, sizeof out);
    strcat(all, out);
    CHECK(!strcmp(all, "Hola, ¿cómo[2J estás?"), "streamed safe: [%s]", all);
    lumo_out_init(&o);
    lumo_out_feed(&o, "<think>\nhmm</think>\n\nLine one\n\nLine two\n", 1, out, sizeof out);
    CHECK(!strcmp(out, "Line one\n\nLine two"), "think dropped, inner blank lines kept: [%s]", out);
    lumo_out_init(&o);
    lumo_out_feed(&o, "<b>bold</b> stays", 0, out, sizeof out);
    CHECK(!strcmp(out, "") , "a short line starting with < waits");
    lumo_out_feed(&o, "<b>bold</b> stays", 1, out, sizeof out);
    CHECK(!strcmp(out, "<b>bold</b> stays"), "and is written at the end: [%s]", out);

    /* the model picked, Read's states */
    char m[64];
    CHECK(!writebus_pick_model("[{\"id\":\"qwen3-1.7b\"},{\"id\":\"qwen3-4b\"}]", "qwen3-4b", m) && !strcmp(m, "qwen3-4b"), "the chosen model");
    CHECK(!writebus_pick_model("[{\"id\":\"qwen3-1.7b\"}]", "qwen3-4b", m) && !strcmp(m, "qwen3-1.7b"), "else the first");
    CHECK(writebus_pick_model("[]", NULL, m) && writebus_pick_model("nope", NULL, m) && writebus_pick_model("[{\"id\":\"../x\"}]", NULL, m), "none");
    char *t;
    CHECK(!strcmp(writebus_state("{\"ok\":true,\"content\":\"Hi\"}", NULL, &t), "done") && !strcmp(t, "Hi"), "done");
    free(t);
    CHECK(!strcmp(writebus_state("{\"ok\":false,\"error\":\"llm.timeout\"}", NULL, &t), "failed") && !strcmp(t, "llm.timeout"), "failed");
    free(t);
    CHECK(!strcmp(writebus_state(NULL, "{\"state\":\"writing\",\"text\":\"H\"}", &t), "writing") && !strcmp(t, "H"), "writing");
    free(t);
    CHECK(!strcmp(writebus_state(NULL, "{\"state\":\"starting\"}", &t), "starting"), "starting");
    free(t);
    CHECK(!strcmp(writebus_state(NULL, NULL, &t), "queued"), "queued");
    free(t);
}

/* ---- on a private bus ---- */

static void stand_in(const char *addr, int ready) {
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    DConn c;
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open(&c, fd, 3000)) _exit(1);
    DBuf b;
    dw_init(&b);
    dw_str(&b, LUMO_BUS_NAME);
    dw_u32(&b, 4);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "RequestName",
               .dest = "org.freedesktop.DBus", .sig = "su" };
    uint32_t s = dbus_send(&c, &h, &b);
    dw_free(&b);
    DMsg m;
    if (dbus_wait_reply(&c, s, &m, 3000) != 1) _exit(1);
    dbus_msg_free(&m);
    if (write(ready, "r", 1) != 1) _exit(1);
    /* Writing's switch follows a file the test writes: "on" or "off" */
    for (;;) {
        char sw[8] = "";
        FILE *f = fopen(getenv("LUMO_TEST_SWITCH"), "r");
        if (f) { if (!fgets(sw, sizeof sw, f)) sw[0] = 0; fclose(f); }
        writebus_serve(!strncmp(sw, "on", 2), "qwen3-4b");
        struct pollfd p = { c.fd, POLLIN, 0 };
        poll(&p, 1, 50);
        int r;
        while ((r = dbus_next(&c, &m)) == 1) {
            if ((m.type == DBUS_RETURN || m.type == DBUS_ERROR) && m.reply_serial) writebus_reply(&c, &m);
            else if (m.type == DBUS_CALL && m.path && !strcmp(m.path, LUMO_BUS_PATH)) writebus_call(&c, &m);
            dbus_msg_free(&m);
        }
        if (r < 0 || dbus_flush(&c) < 0) _exit(0);
    }
}

/* runs build/lumo with args (and stdin text when given); its stdout and stderr in out, its exit status */
static int lumo(const char *addr, const char *lang, const char *in, char *out, size_t cap, const char *const *args) {
    int po[2], pi[2];
    if (pipe(po) || pipe(pi)) return -1;
    pid_t p = fork();
    if (!p) {
        dup2(pi[0], 0);
        dup2(po[1], 1);
        dup2(po[1], 2);
        close(pi[1]);
        close(po[0]);
        if (addr) setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
        else setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/lumo-test-bus", 1);
        unsetenv("LC_ALL");
        unsetenv("LC_MESSAGES");
        setenv("LANG", lang, 1);
        char *argv[12] = { "lumo" };
        int n = 1;
        for (; args[n - 1] && n < 11; n++) argv[n] = (char *)args[n - 1];
        argv[n] = NULL;
        execv("build/lumo", argv);
        _exit(127);
    }
    close(pi[0]);
    close(po[1]);
    if (in && write(pi[1], in, strlen(in)) < 0) { /* the command may not read it */ }
    close(pi[1]);
    size_t n = 0;
    ssize_t r;
    while ((r = read(po[0], out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(po[0]);
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void on_bus(void) {
    if (access("/usr/bin/dbus-daemon", X_OK) || access("build/lumo", X_OK)) { printf("lumo-test: bus part skipped: needs dbus-daemon and build/lumo\n"); return; }
    char dir[] = "/tmp/lumo-test-XXXXXX";
    if (!mkdtemp(dir)) return;
    char conf[PATH_MAX], sock[PATH_MAX], addr[PATH_MAX + 16], sw[PATH_MAX];
    snprintf(conf, sizeof conf, "%s/bus.conf", dir);
    snprintf(sock, sizeof sock, "%s/bus", dir);
    snprintf(sw, sizeof sw, "%s/switch", dir);
    snprintf(addr, sizeof addr, "unix:path=%s", sock);
    FILE *f = fopen(conf, "w");
    if (!f) return;
    fprintf(f, "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
               "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
               "<busconfig><type>session</type><listen>%s</listen><auth>EXTERNAL</auth>"
               "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/><allow own=\"*\"/><allow user=\"*\"/></policy></busconfig>\n", addr);
    fclose(f);
    f = fopen(sw, "w");
    fputs("on\n", f);
    fclose(f);
    setenv("LUMO_TEST_SWITCH", sw, 1);
    pid_t daemon = fork();
    if (!daemon) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1);
        char arg[PATH_MAX + 16];
        snprintf(arg, sizeof arg, "--config-file=%s", conf);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", arg, "--nofork", "--nopidfile", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100 && access(sock, F_OK); i++) { struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
    int rp[2];
    if (pipe(rp)) return;
    pid_t srv = fork();
    if (!srv) { close(rp[0]); stand_in(addr, rp[1]); _exit(0); }
    close(rp[1]);
    char ch;
    CHECK(read(rp[0], &ch, 1) == 1, "the stand-in lord-server owns " LUMO_BUS_NAME);
    close(rp[0]);

    static char out[8192];
    int rc = lumo(addr, "en_US.UTF-8", NULL, out, sizeof out, (const char *const[]){ "status", NULL });
    CHECK(rc == 0 && strstr(out, "on") && strstr(out, "qwen3-4b") && strstr(out, "Nothing goes online"), "status: rc %d [%s]", rc, out);
    rc = lumo(addr, "es_AR.UTF-8", NULL, out, sizeof out, (const char *const[]){ "corregir", "hola", "como", "estas", NULL });
    CHECK(rc == 0 && !strcmp(out, "]52;c;x¿Hola, cómo estás? (fix)\n"), "corregir streamed, safe: rc %d [%s]", rc, out);
    rc = lumo(addr, "en_US.UTF-8", NULL, out, sizeof out, (const char *const[]){ "translate", "Portuguese", "hello", NULL });
    CHECK(rc == 0 && !strcmp(out, "[pt] hello\n"), "translate by name: rc %d [%s]", rc, out);
    rc = lumo(addr, "en_US.UTF-8", "some text from a pipe\n", out, sizeof out, (const char *const[]){ "traducir", "zh-hant", NULL });
    CHECK(rc == 0 && !strcmp(out, "[zh-Hant] some text from a pipe\n"), "stdin, the trailing line break dropped: rc %d [%s]", rc, out);
    rc = lumo(addr, "en_US.UTF-8", NULL, out, sizeof out, (const char *const[]){ "translate", "klingon", "hi", NULL });
    CHECK(rc == 2 && strstr(out, "klingon"), "an unknown language: rc %d [%s]", rc, out);
    rc = lumo(addr, "en_US.UTF-8", NULL, out, sizeof out, (const char *const[]){ "delete", "everything", NULL });
    CHECK(rc == 2, "a wrong command: rc %d", rc);
    static char big[LUMO_CHARS_MAX + 16];
    memset(big, 'a', LUMO_CHARS_MAX + 10);
    rc = lumo(addr, "en_US.UTF-8", big, out, sizeof out, (const char *const[]){ "fix", NULL });
    CHECK(rc == 5 && strstr(out, "4,000"), "too long: rc %d [%s]", rc, out);
    f = fopen(sw, "w");
    fputs("off\n", f);
    fclose(f);
    struct timespec ts = { 0, 200000000 };
    nanosleep(&ts, NULL);
    rc = lumo(addr, "es_AR.UTF-8", NULL, out, sizeof out, (const char *const[]){ "mejorar", "hola", NULL });
    CHECK(rc == 3 && strstr(out, "Configuración"), "Writing off, in Spanish, says where: rc %d [%s]", rc, out);
    rc = lumo(NULL, "en_US.UTF-8", NULL, out, sizeof out, (const char *const[]){ "fix", "hello", NULL });
    CHECK(rc == 4, "no session: rc %d [%s]", rc, out);
    rc = lumo(addr, "en_US.UTF-8", NULL, out, sizeof out, (const char *const[]){ "--help", NULL });
    CHECK(rc == 0 && strstr(out, "corregir") && strstr(out, "Exit status"), "help: rc %d", rc);

    /* a job is its caller's: another connection can't read or cancel it */
    f = fopen(sw, "w");
    fputs("on\n", f);
    fclose(f);
    nanosleep(&ts, NULL);
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    DConn a, b2;
    int fa = dbus_connect_session(), fb = dbus_connect_session();
    if (fa >= 0 && fb >= 0 && !dbus_open(&a, fa, 3000) && !dbus_open(&b2, fb, 3000)) {
        DBuf q;
        dw_init(&q);
        dw_str(&q, "fix");
        dw_str(&q, "");
        dw_str(&q, "hola");
        DHdr h = { .type = DBUS_CALL, .path = LUMO_BUS_PATH, .iface = LUMO_BUS_IFACE, .member = "Start", .dest = LUMO_BUS_NAME, .sig = "sss" };
        DMsg m;
        int r = dbus_wait_reply(&a, dbus_send(&a, &h, &q), &m, 3000);
        dw_free(&q);
        uint32_t id = 0;
        if (r == 1) { DRd d; dr_init(&d, &m); id = dr_u32(&d); }
        if (r >= 0) dbus_msg_free(&m);
        CHECK(r == 1 && id > 0, "Start answers an id");
        dw_init(&q);
        dw_u32(&q, id);
        DHdr h2 = { .type = DBUS_CALL, .path = LUMO_BUS_PATH, .iface = LUMO_BUS_IFACE, .member = "Read", .dest = LUMO_BUS_NAME, .sig = "u" };
        r = dbus_wait_reply(&b2, dbus_send(&b2, &h2, &q), &m, 3000);
        CHECK(r == 0 && m.error && !strcmp(m.error, "org.freedesktop.DBus.Error.UnknownObject"), "another caller can't read it");
        if (r >= 0) dbus_msg_free(&m);
        DHdr h3 = { .type = DBUS_CALL, .path = LUMO_BUS_PATH, .iface = LUMO_BUS_IFACE, .member = "Start", .dest = LUMO_BUS_NAME, .sig = "sss" };
        DBuf q3;
        dw_init(&q3);
        dw_str(&q3, "rm -rf");
        dw_str(&q3, "");
        dw_str(&q3, "hola");
        r = dbus_wait_reply(&b2, dbus_send(&b2, &h3, &q3), &m, 3000);
        CHECK(r == 0 && m.error && !strcmp(m.error, "org.freedesktop.DBus.Error.InvalidArgs"), "an action outside the list refused");
        if (r >= 0) dbus_msg_free(&m);
        dw_free(&q3);
        dw_free(&q);
        dbus_close(&a);
        dbus_close(&b2);
    } else CHECK(0, "two connections to the private bus");
    kill(srv, SIGTERM);
    kill(daemon, SIGTERM);
    waitpid(srv, NULL, 0);
    waitpid(daemon, NULL, 0);
    unlink(conf);
    unlink(sock);
    unlink(sw);
    rmdir(dir);
}

int main(void) {
    pure();
    on_bus();
    printf("lumo-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
