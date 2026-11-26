/* lumo.c — the `lumo` command: ✨ AI Writing from the terminal (docs/AI.md "Writing from the terminal"). Installed as
 * /usr/lib/lordos/bin/lumo, /usr/bin/lumo a link to it. Awaiting the maintainer's approval (AGENTS.md).
 *
 *   lumo corregir "texto"        lumo correct "text"          (fix)
 *   lumo mejorar "texto"         lumo improve "text"
 *   lumo resumir < archivo       lumo summarize < file        (shorten, shorter, acortar)
 *   lumo traducir en "texto"     lumo translate pt "text"     (a code or a name: en, inglés, Portuguese…)
 *   echo texto | lumo corregir   the text from standard input when none follows the command
 *   lumo estado | status         Writing on or off, its model, where it runs
 *   lumo --help | ayuda
 *
 * It never reaches the model itself: it asks the person's running LordOS session over the session bus
 * (org.lordos.Writing1, lord-server's ui/writebus.c: only the same user), and lord-server runs the same job as the ✨ AI
 * panel (llm/writing.c's fixed prompts, the local engine in its sandbox; nothing goes online). The answer is streamed to
 * standard output as it comes, made safe for a terminal (llm/lumo.c lumo_out_feed: no control or bidi characters).
 * Writing's switch is the panel's: off, it says where to turn it on.
 *
 * Messages come from assets/locales/<lang>/lumo.json, by LC_ALL, LC_MESSAGES or LANG, English for anything missing.
 * Exit status: 0 done, 1 the model failed or took too long, 2 a wrong command, 3 Writing is off or has no model or
 * engine, 4 no LordOS session, 5 the text is empty, too long or not UTF-8, 130 interrupted. */
#include "../llm/lumo.h"
#include "../cli/mcli.h"
#include "../sec/json.h"
#include "../vault/dbus.h"
#include "../web/weblangid.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { EX_OK = 0, EX_FAILED = 1, EX_USAGE = 2, EX_OFF = 3, EX_SESSION = 4, EX_TEXT = 5, EX_INT = 130 };
#define POLL_MS 100
#define WAIT_MS (200 * 1000)        /* as the panel: the engine may have to start first */

/* ---- texts ---- */

static JDoc cat_lang, cat_en;
static char self_dir[PATH_MAX], lang[WEBLANG_ID_MAX];

static int load_json(const char *path, JDoc *d) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 20)) { close(fd); return -1; }
    char *b = malloc((size_t)st.st_size);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    int bad = r != st.st_size || json_parse(b, (size_t)r, d, NULL, 0) || !d->root || d->root->type != J_OBJ;
    free(b);
    if (bad) { json_free(d); memset(d, 0, sizeof *d); return -1; }
    return 0;
}

static void load_catalog(const char *l, JDoc *d) {
    /* installed: /usr/lib/lordos/bin → /usr/lib/lordos/assets; built: server/build → assets */
    const char *tries[] = { "%s/../assets/locales/%s/lumo.json", "%s/../../assets/locales/%s/lumo.json" };
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        char p[PATH_MAX + 96];
        snprintf(p, sizeof p, tries[i], self_dir, l);
        if (!load_json(p, d)) return;
    }
}

static const char *T(const char *key) {
    const char *s = cat_lang.root ? json_str(cat_lang.root, key) : NULL;
    if (!s || !s[0]) s = cat_en.root ? json_str(cat_en.root, key) : NULL;
    return s && s[0] ? s : key;
}

/* key's text with placeholders ("name", value, ..., NULL) */
static const char *say(const char *key, ...) {
    static char out[2][2048];
    static int slot;
    const char *kv[9];
    int n = 0;
    va_list ap;
    va_start(ap, key);
    while (n < 8) {
        const char *k = va_arg(ap, const char *);
        if (!k) break;
        kv[n++] = k;
        kv[n++] = va_arg(ap, const char *);
    }
    va_end(ap);
    kv[n] = NULL;
    char *o = out[slot++ & 1];
    mcli_fill(T(key), kv, o, sizeof out[0]);
    return o;
}
#define ERR(...) (fprintf(stderr, "lumo: %s\n", say(__VA_ARGS__)))

/* ---- the session bus ---- */

static DConn bus;
static int bus_up;

static int bus_open(void) {
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open(&bus, fd, 3000)) { if (fd >= 0) close(fd); return -1; }
    bus_up = 1;
    return 0;
}

/* a call to lord-server's Writing1 and its reply: 1 reply (in m), 0 a D-Bus error (in m, its name in m->error), -1 none */
static int call(const char *member, const char *sig, const DBuf *body, DMsg *m) {
    DHdr h = { .type = DBUS_CALL, .path = LUMO_BUS_PATH, .iface = LUMO_BUS_IFACE, .member = member, .dest = LUMO_BUS_NAME,
               .sig = sig, .flags = DBUS_NO_AUTOSTART };
    uint32_t s = dbus_send(&bus, &h, body);
    if (!s) return -1;
    memset(m, 0, sizeof *m);
    return dbus_wait_reply(&bus, s, m, 30000);
}

/* a D-Bus error's exit status and message */
static int bus_error(const DMsg *m) {
    const char *e = m->error ? m->error : "";
    if (!strcmp(e, LUMO_ERR_OFF)) { ERR("err.off"); return EX_OFF; }
    if (!strcmp(e, LUMO_ERR_NO_MODEL)) { ERR("err.noModel"); return EX_OFF; }
    if (!strcmp(e, LUMO_ERR_NO_ENGINE)) { ERR("err.noEngine"); return EX_OFF; }
    if (!strcmp(e, LUMO_ERR_BUSY)) { ERR("err.busy"); return EX_FAILED; }
    if (!strcmp(e, LUMO_ERR_DENIED)) { ERR("err.denied"); return EX_SESSION; }
    if (!strcmp(e, "org.freedesktop.DBus.Error.ServiceUnknown") || !strcmp(e, "org.freedesktop.DBus.Error.NameHasNoOwner") ||
        !strcmp(e, "org.freedesktop.DBus.Error.UnknownObject") || !strcmp(e, "org.freedesktop.DBus.Error.UnknownMethod")) {
        ERR("err.noSession");
        return EX_SESSION;
    }
    ERR("err.failed");
    return EX_FAILED;
}

/* ---- commands ---- */

static int cmd_help(FILE *f) {
    /* the command words: Spanish first in Spanish, English first otherwise; both always work */
    int es = !strcmp(lang, "es");
    static const char *const ROWS[][3] = {
        { "correct, fix", "corregir", "help.fix" },
        { "improve", "mejorar", "help.improve" },
        { "summarize, shorten", "resumir, acortar", "help.summarize" },
        { "translate LANG", "traducir IDIOMA", "help.translate" },
        { "status", "estado", "help.status" },
        { "--help", "--ayuda", "help.help" },
    };
    fprintf(f, "%s\n%s\n\n", T("help.usage"), T("help.intro"));
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        char words[64];
        snprintf(words, sizeof words, "%s (%s)", ROWS[i][es ? 1 : 0], ROWS[i][es ? 0 : 1]);
        fprintf(f, "  %-36s %s\n", words, T(ROWS[i][2]));
    }
    fprintf(f, "\n%s\n%s\n\n%s\n", T("help.stdin"), T("help.langs"), T("help.examples"));
    fprintf(f, "  lumo %s \"hola como estas? todo bien\"\n", es ? "corregir" : "correct");
    fprintf(f, "  lumo %s en \"Mañana paso a buscar los libros.\"\n", es ? "traducir" : "translate");
    fprintf(f, "  echo \"...\" | lumo %s\n  lumo %s < %s\n", es ? "mejorar" : "improve", es ? "resumir" : "summarize", es ? "archivo.txt" : "notes.txt");
    fprintf(f, "\n%s\n%s\n", T("help.local"), T("help.exit"));
    return EX_OK;
}

static int cmd_status(void) {
    if (bus_open()) { ERR("err.noSession"); return EX_SESSION; }
    DMsg m;
    int r = call("Status", NULL, NULL, &m);
    if (r < 0) { ERR("err.noSession"); return EX_SESSION; }
    if (r == 0) { int e = bus_error(&m); dbus_msg_free(&m); return e; }
    DRd d;
    dr_init(&d, &m);
    const char *state = dr_str(&d, NULL), *model = dr_str(&d, NULL), *engine = dr_str(&d, NULL);
    if (d.err || !m.sig || strcmp(m.sig, "sss")) { dbus_msg_free(&m); ERR("err.failed"); return EX_FAILED; }
    int on = strcmp(state, "off") != 0;
    puts(T(on ? "status.on" : "status.off"));
    if (!strcmp(state, "no-engine")) puts(T("status.noEngine"));
    else {
        char k[32];
        snprintf(k, sizeof k, "engine.%s", !strcmp(engine, "starting") || !strcmp(engine, "ready") || !strcmp(engine, "busy") ? engine : "off");
        puts(say("status.engine", "state", T(k), NULL));
    }
    puts(model[0] ? say("status.model", "model", model, NULL) : T("status.noModel"));
    puts(T("status.where"));
    int rc = !strcmp(state, "ready") ? EX_OK : EX_OFF;
    if (!strcmp(state, "off")) puts(T("err.off"));
    else if (!strcmp(state, "no-model")) puts(T("err.noModel"));
    dbus_msg_free(&m);
    return rc;
}

static volatile sig_atomic_t interrupted;
static void on_int(int s) { (void)s; interrupted = 1; }

static char *read_stdin(size_t *n) {
    size_t cap = LUMO_TEXT_MAX + 2, len = 0;
    char *b = malloc(cap + 1);
    if (!b) return NULL;
    for (;;) {
        ssize_t r = read(0, b + len, cap - len);
        if (r < 0 && errno == EINTR) { if (interrupted) break; continue; }
        if (r <= 0) break;
        len += (size_t)r;
        if (len >= cap) break;                  /* too long: lumo_text_ok says so */
    }
    b[len] = 0;
    *n = len;
    return b;
}

static int cmd_write(int verb, const char *target, int argc, char **argv) {
    char *text = NULL;
    size_t n = 0;
    if (argc > 0 && !(argc == 1 && !strcmp(argv[0], "-"))) {
        for (int i = 0; i < argc; i++) n += strlen(argv[i]) + 1;
        text = malloc(n + 1);
        if (!text) return EX_FAILED;
        text[0] = 0;
        for (int i = 0; i < argc; i++) { if (i) strcat(text, " "); strcat(text, argv[i]); }
        n = strlen(text);
    } else {
        if (isatty(0)) { ERR("err.noText"); return EX_USAGE; }
        text = read_stdin(&n);
        if (!text) return EX_FAILED;
        /* the trailing line break `echo` adds is not part of the text */
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) text[--n] = 0;
    }
    int ok = lumo_text_ok(text, n);
    if (ok) { ERR(ok == -1 ? "err.noText" : ok == -2 ? "err.tooLong" : "err.notUtf8"); free(text); return EX_TEXT; }
    if (bus_open()) { free(text); ERR("err.noSession"); return EX_SESSION; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, lumo_action(verb));
    dw_str(&b, target ? target : "");
    dw_str(&b, text);
    DMsg m;
    int r = call("Start", "sss", &b, &m);
    dw_free(&b);
    memset(text, 0, n);
    free(text);
    if (r < 0) { ERR("err.noSession"); return EX_SESSION; }
    if (r == 0) { int e = bus_error(&m); dbus_msg_free(&m); return e; }
    DRd d;
    dr_init(&d, &m);
    uint32_t id = dr_u32(&d);
    int bad = d.err || !m.sig || strcmp(m.sig, "u");
    dbus_msg_free(&m);
    if (bad) { ERR("err.failed"); return EX_FAILED; }
    struct sigaction sa = { .sa_handler = on_int };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    LumoOut o;
    lumo_out_init(&o);
    static char out[LUMO_TEXT_MAX * 4 + 64];
    int rc = EX_FAILED, wrote = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (interrupted) { rc = EX_INT; break; }
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 > WAIT_MS) { ERR("err.timeout"); break; }
        DBuf q;
        dw_init(&q);
        dw_u32(&q, id);
        r = call("Read", "u", &q, &m);
        dw_free(&q);
        if (r < 0 && interrupted) { rc = EX_INT; break; }
        if (r < 0) { ERR("err.noSession"); rc = EX_SESSION; break; }
        if (r == 0) { rc = bus_error(&m); dbus_msg_free(&m); break; }
        dr_init(&d, &m);
        const char *st = dr_str(&d, NULL), *tx = dr_str(&d, NULL);
        if (d.err) { dbus_msg_free(&m); ERR("err.failed"); break; }
        int done = !strcmp(st, "done");
        if (!strcmp(st, "failed")) {
            if (!strcmp(tx, "llm.timeout")) ERR("err.timeout");
            else if (!strcmp(tx, "llm.busy")) ERR("err.busy");
            else ERR("err.failed");
            dbus_msg_free(&m);
            break;
        }
        if (done || !strcmp(st, "writing")) {
            size_t k = lumo_out_feed(&o, tx, done, out, sizeof out);
            if (k) { fwrite(out, 1, k, stdout); fflush(stdout); wrote = 1; }
        }
        dbus_msg_free(&m);
        if (done) { rc = wrote ? EX_OK : EX_FAILED; if (!wrote) ERR("err.failed"); break; }
        struct timespec ts = { 0, POLL_MS * 1000000L };
        nanosleep(&ts, NULL);
    }
    if (wrote) { fputc('\n', stdout); fflush(stdout); }
    if (rc == EX_INT) {
        DBuf q;
        dw_init(&q);
        dw_u32(&q, id);
        if (call("Cancel", "u", &q, &m) >= 0) dbus_msg_free(&m);
        dw_free(&q);
    }
    return rc;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    char exe[PATH_MAX];
    if (realpath("/proc/self/exe", exe)) { char *s = strrchr(exe, '/'); if (s) { *s = 0; snprintf(self_dir, sizeof self_dir, "%s", exe); } }
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    weblang_id(l, lang);
    load_catalog("en", &cat_en);
    if (strcmp(lang, "en")) load_catalog(lang, &cat_lang);
    if (argc < 2) { cmd_help(stderr); return EX_USAGE; }
    int verb = lumo_verb(argv[1]);
    int rc;
    if (verb == LUMO_HELP) rc = cmd_help(stdout);
    else if (verb == LUMO_VERSION) { puts("lumo (LordOS)"); rc = EX_OK; }
    else if (verb == LUMO_STATUS) rc = argc > 2 ? (ERR("err.usage", "word", argv[2], NULL), EX_USAGE) : cmd_status();
    else if (verb == LUMO_TRANSLATE) {
        char code[16];
        if (argc < 3) { ERR("err.langMissing"); rc = EX_USAGE; }
        else if (lumo_lang(argv[2], code)) { ERR("err.lang", "word", argv[2], NULL); rc = EX_USAGE; }
        else rc = cmd_write(verb, code, argc - 3, argv + 3);
    } else if (verb != LUMO_NONE) rc = cmd_write(verb, NULL, argc - 2, argv + 2);
    else { ERR("err.usage", "word", argv[1], NULL); rc = EX_USAGE; }
    if (bus_up) dbus_close(&bus);
    json_free(&cat_en);
    json_free(&cat_lang);
    return rc;
}
