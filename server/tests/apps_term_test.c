/* apps_term_test.c — terminal apps (.desktop with Terminal=true) in the Linux app launcher (src/apps.c) and
 * their one-shot slots (src/term/termapp.c): which entries are listed with terminal = 1, that apps_launch
 * never runs them, the Exec line → argv → the shell-quoted line lord-server types, and the tokens (single
 * use, 10 s, unknown ones refused, at most 4 pending; a fake clock).
 *
 * Uses a temporary HOME with .desktop files in ~/.local/share/applications. Nothing here starts a shell. */
#include "apps.h"
#include "fsops.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"
#include "sec/restrict.h"
#include "term/termapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

static int n, bad;
#define CHECK(name, cond) do { n++; if (!(cond)) { bad++; printf("✗ %s\n", name); } } while (0)

static char home[64];

static void desktop(const char *id, const char *body) {
    char path[512];
    snprintf(path, sizeof path, "%s/.local/share/applications/%s.desktop", home, id);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "[Desktop Entry]\nType=Application\n%s", body);
    fclose(f);
}

static const DesktopApp *listed(const char *id) {
    const DesktopApp *list;
    int na = apps_list("en", &list);
    for (int i = 0; i < na; i++) if (!strcmp(list[i].id, id)) return &list[i];
    return NULL;
}

/* The line a terminal app's slot gives, as lord.term.create would take it. */
static int line_of(const char *id, int fish, const char *rule, char *line, size_t sz) {
    char tk[TERMAPP_TOKEN_HEX + 1];
    if (apps_launch_terminal(id, tk, sizeof tk)) return -1;
    return termapp_take(tk, termapp_now(), fish, rule, line, sz);
}

static void test_quote(void) {
    char out[256];
    char *a1[] = { "htop" };
    CHECK("one word", !termapp_quote(a1, 1, 0, out, sizeof out) && !strcmp(out, "'htop'"));
    char *a2[] = { "/usr/bin/vim", "a b", "it's", "$HOME", "`id`", "back\\slash", ";", "&&", "\"q\"", "ñandú", "" };
    CHECK("specials quoted", !termapp_quote(a2, 11, 0, out, sizeof out) &&
          !strcmp(out, "'/usr/bin/vim' 'a b' 'it'\\''s' '$HOME' '`id`' 'back\\slash' ';' '&&' '\"q\"' 'ñandú' ''"));
    char *a3[] = { "a\\b", "c'd" };
    CHECK("fish doubles backslashes", !termapp_quote(a3, 2, 1, out, sizeof out) && !strcmp(out, "'a\\\\b' 'c'\\''d'"));
    char *nl[] = { "vim", "a\nb" };
    CHECK("newline refused", termapp_quote(nl, 2, 0, out, sizeof out) == -1 && !out[0]);
    char *cr[] = { "a\rb" }, *tab[] = { "a\tb" }, *esc[] = { "a\033[2Jb" }, *del[] = { "a\177" };
    CHECK("CR refused", termapp_quote(cr, 1, 0, out, sizeof out) == -1);
    CHECK("tab refused", termapp_quote(tab, 1, 0, out, sizeof out) == -1);
    CHECK("ESC refused", termapp_quote(esc, 1, 0, out, sizeof out) == -1);
    CHECK("DEL refused", termapp_quote(del, 1, 0, out, sizeof out) == -1);
    char *c1[] = { "a\xc2\x9b" "2J" }, *bad8[] = { "a\xff" }, *over[] = { "\xc0\xaf" }, *sep[] = { "a\xe2\x80\xa8" };
    CHECK("C1 CSI refused", termapp_quote(c1, 1, 0, out, sizeof out) == -1);
    CHECK("invalid UTF-8 refused", termapp_quote(bad8, 1, 0, out, sizeof out) == -1);
    CHECK("overlong UTF-8 refused", termapp_quote(over, 1, 0, out, sizeof out) == -1);
    CHECK("line separator refused", termapp_quote(sep, 1, 0, out, sizeof out) == -1);
    char *a4[] = { "abcdef" };
    CHECK("too small a buffer: refused, empty", termapp_quote(a4, 1, 0, out, 6) == -1 && !out[0]);
    CHECK("exit rules", !strcmp(termapp_exit_suffix("close"), "; exit") && !strcmp(termapp_exit_suffix("closeIfOk"), " && exit") &&
          !strcmp(termapp_exit_suffix("keep"), "") && !strcmp(termapp_exit_suffix(NULL), "; exit") &&
          !strcmp(termapp_exit_suffix("rm -rf"), "; exit"));
}

static void test_tokens(void) {
    termapp_reset();
    char *argv[] = { "htop" };
    char tk[5][TERMAPP_TOKEN_HEX + 1], line[256];
    double t0 = 1000000;
    CHECK("put", !termapp_put(argv, 1, t0, tk[0]) && strlen(tk[0]) == TERMAPP_TOKEN_HEX);
    int hex = 1;
    for (const char *p = tk[0]; *p; p++) if (!strchr("0123456789abcdef", *p)) hex = 0;
    CHECK("token is 128 bits of hex", hex);
    CHECK("take", !termapp_take(tk[0], t0 + 10, 0, "close", line, sizeof line) && !strcmp(line, "'htop'; exit"));
    CHECK("used token refused", termapp_take(tk[0], t0 + 20, 0, "close", line, sizeof line) == -1 && !line[0]);
    CHECK("unknown token refused", termapp_take("0123456789abcdef0123456789abcdef", t0, 0, "close", line, sizeof line) == -1 && !line[0]);
    CHECK("malformed tokens refused", termapp_take("", t0, 0, "close", line, sizeof line) == -1 &&
          termapp_take(NULL, t0, 0, "close", line, sizeof line) == -1 &&
          termapp_take("0123456789ABCDEF0123456789ABCDEF", t0, 0, "close", line, sizeof line) == -1);

    CHECK("put again", !termapp_put(argv, 1, t0, tk[1]));
    CHECK("not expired at 9.99 s", termapp_pending(t0 + TERMAPP_TTL_MS - 10) == 1);
    CHECK("expired at 10 s", termapp_take(tk[1], t0 + TERMAPP_TTL_MS, 0, "keep", line, sizeof line) == -1 && !line[0]);
    CHECK("expired slot gone", termapp_pending(t0 + TERMAPP_TTL_MS) == 0);
    CHECK("put", !termapp_put(argv, 1, t0, tk[1]));
    CHECK("a clock that goes back expires it", termapp_take(tk[1], t0 - 1, 0, "keep", line, sizeof line) == -1);

    termapp_reset();
    for (int i = 0; i < 5; i++) CHECK("put five", !termapp_put(argv, 1, t0 + i, tk[i]));
    CHECK("at most 4 pending", termapp_pending(t0 + 5) == TERMAPP_MAX_PENDING);
    CHECK("the oldest went", termapp_take(tk[0], t0 + 5, 0, "keep", line, sizeof line) == -1);
    for (int i = 1; i < 5; i++) CHECK("the newer four work", !termapp_take(tk[i], t0 + 5, 0, "keep", line, sizeof line) && !strcmp(line, "'htop'"));
    CHECK("none left", termapp_pending(t0 + 5) == 0);
    CHECK("distinct tokens", strcmp(tk[1], tk[2]) && strcmp(tk[2], tk[3]));

    char *nl[] = { "vim", "a\nb" };
    CHECK("an argv with a newline is never stored", termapp_put(nl, 2, t0, tk[0]) == -1 && !tk[0][0] && termapp_pending(t0) == 0);
    CHECK("an empty argv is never stored", termapp_put(argv, 0, t0, tk[0]) == -1);
    termapp_put(argv, 1, t0, tk[0]);
    CHECK("closeIfOk", !termapp_take(tk[0], t0, 0, "closeIfOk", line, sizeof line) && !strcmp(line, "'htop' && exit"));
    termapp_put(argv, 1, t0, tk[0]);
    CHECK("a line that doesn't fit: refused and spent", termapp_take(tk[0], t0, 0, "close", line, 8) == -1 && !line[0] &&
          termapp_pending(t0) == 0);
}

int main(void) {
    snprintf(home, sizeof home, "/tmp/lord-termapps-XXXXXX");
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    unsetenv("LORDOS_SESSIOND");
    char path[512];
    const char *dirs[] = { "%s/.local", "%s/.local/share", "%s/.local/share/applications", "%s/.local/bin", NULL };
    for (int i = 0; dirs[i]; i++) { snprintf(path, sizeof path, dirs[i], home); mkdir(path, 0700); }
    FwStore *fw = fw_store_new();
    restrict_set_store(fw);

    test_quote();
    test_tokens();

    /* ---- the listing ---- */
    char body[1024];
    snprintf(body, sizeof body, "Name=Top\nTerminal=true\nExec=/usr/bin/touch %s/ran-top\n", home);
    desktop("lord-top", body);
    desktop("lord-missing", "Name=Missing\nTerminal=true\nExec=lord-no-such-program\n");
    desktop("lord-tryexec", "Name=TryExec\nTerminal=true\nTryExec=lord-no-such-program\nExec=/usr/bin/printf x\n");
    desktop("lord-hidden", "Name=Hidden\nTerminal=true\nNoDisplay=true\nExec=/usr/bin/printf x\n");
    desktop("lord-relative", "Name=Relative\nTerminal=true\nExec=./printf x\n");
    desktop("lord-onlyshow", "Name=Only\nTerminal=true\nOnlyShowIn=GNOME;\nExec=/usr/bin/printf x\n");
    desktop("lord-gui", "Name=Gui\nExec=/usr/bin/printf x\n");
    desktop("lord-quote", "Name=Quote\nTerminal=true\nExec=printf \"a b\" it's $HOME `id` \\back ; && %f %U %u --name=%c\n");
    desktop("lord-newline", "Name=Bad\\nName\nTerminal=true\nExec=printf %c\n");
    desktop("lord-unclosed", "Name=Unclosed\nTerminal=true\nExec=printf \"abc\n");
    desktop("lord-icon", "Name=Icon\nIcon=lord-icon\nTerminal=true\nExec=printf %i %k\n");
    desktop("lord-acts", "Name=Acts\nTerminal=true\nExec=printf x\nActions=one;\n\n[Desktop Action one]\nName=One\nExec=printf one\n");
    snprintf(path, sizeof path, "%s/.local/bin/lord-mytool", home);
    FILE *f = fopen(path, "w");
    if (f) { fputs("#!/bin/sh\n", f); fclose(f); chmod(path, 0755); }
    desktop("lord-mytool", "Name=Mine\nTerminal=true\nExec=lord-mytool --flag\n");

    const DesktopApp *a = listed("lord-top");
    CHECK("a terminal app is listed", a != NULL);
    CHECK("with terminal = 1", a && a->terminal == 1);
    a = listed("lord-gui");
    CHECK("a normal app has terminal = 0", a && a->terminal == 0);
    CHECK("missing program: not listed", !listed("lord-missing"));
    CHECK("missing TryExec: not listed", !listed("lord-tryexec"));
    CHECK("NoDisplay: not listed", !listed("lord-hidden"));
    CHECK("relative program: not listed", !listed("lord-relative"));
    CHECK("OnlyShowIn another desktop: not listed", !listed("lord-onlyshow"));
    CHECK("unclosed quotes: not listed", !listed("lord-unclosed"));
    CHECK("the user's ~/.local/bin counts for their own .desktop", listed("lord-mytool") != NULL);
    CHECK("apps_is_terminal", apps_is_terminal("lord-top") && !apps_is_terminal("lord-gui") && !apps_is_terminal("lord-nope"));

    /* ---- never run by the launcher ---- */
    CHECK("apps_launch refuses a terminal app", apps_launch("lord-top") != 0);
    CHECK("apps_launch_with refuses it", apps_launch_with("lord-top", home) != 0);
    CHECK("its actions are not listed", apps_actions("lord-acts", "en", (AppAction[APPS_MAX_ACTIONS]){ 0 }, APPS_MAX_ACTIONS) == 0);
    CHECK("its actions are not launched", apps_launch_action("lord-acts", "one") != 0);
    char tk[TERMAPP_TOKEN_HEX + 1], line[TERMAPP_LINE_MAX];
    termapp_reset();
    CHECK("apps_launch_terminal stores a slot", !apps_launch_terminal("lord-top", tk, sizeof tk) && strlen(tk) == TERMAPP_TOKEN_HEX);
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    snprintf(path, sizeof path, "%s/ran-top", home);
    CHECK("nothing ran", access(path, F_OK) != 0);
    CHECK("its line", !termapp_take(tk, termapp_now(), 0, "close", line, sizeof line));
    char want[600];
    snprintf(want, sizeof want, "'/usr/bin/touch' '%s/ran-top'; exit", home);
    CHECK("is the Exec line, quoted, with the exit rule", !strcmp(line, want));
    CHECK("a normal app gets no slot", apps_launch_terminal("lord-gui", tk, sizeof tk) == -1 && !tk[0]);
    CHECK("an unknown id gets no slot", apps_launch_terminal("lord-nope", tk, sizeof tk) == -1);
    CHECK("a path id gets no slot", apps_launch_terminal("../lord-top", tk, sizeof tk) == -1);
    CHECK("a short token buffer is refused", apps_launch_terminal("lord-top", tk, 8) == -1);
    CHECK("no slot was left behind", termapp_pending(termapp_now()) == 0);

    /* ---- Exec → argv → line ---- */
    CHECK("quoting app", !line_of("lord-quote", 0, "keep", line, sizeof line));
    CHECK("specials quoted, file codes removed, %c is the name",
          !strcmp(line, "'printf' 'a b' 'it'\\''s' '$HOME' '`id`' '\\back' ';' '&&' '--name=Quote'"));
    CHECK("a newline (from Name through %c) is refused", line_of("lord-newline", 0, "keep", line, sizeof line) == -1);
    CHECK("%i and %k", !line_of("lord-icon", 0, "closeIfOk", line, sizeof line));
    snprintf(want, sizeof want, "'printf' '--icon' 'lord-icon' '%s/.local/share/applications/lord-icon.desktop' && exit", home);
    CHECK("as --icon <name> and the .desktop file", !strcmp(line, want));
    CHECK("~/.local/bin program", !line_of("lord-mytool", 0, "close", line, sizeof line) && !strcmp(line, "'lord-mytool' '--flag'; exit"));

    /* ---- an app with restrictions would run without them: refused ---- */
    fw_set_restricted(fw, "desktop:lord-top", FW_R_NO_INTERNET);
    CHECK("a restricted terminal app is refused", apps_launch_terminal("lord-top", tk, sizeof tk) == -1);
    CHECK("with its reason", !strcmp(apps_last_error(), "apps.restrictTerminal"));
    fw_set_restricted(fw, "desktop:lord-top", 0);
    CHECK("without restrictions again", !apps_launch_terminal("lord-top", tk, sizeof tk));

    /* ---- the file is read again at launch ---- */
    desktop("lord-top", "Name=Top\nExec=/usr/bin/printf x\n");
    CHECK("no longer a terminal app: no slot", apps_launch_terminal("lord-top", tk, sizeof tk) == -1);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
    printf(bad ? "apps-term: %d of %d failed\n" : "apps-term: %d/%d ok\n", bad ? bad : n, n);
    return bad ? 1 : 0;
}
