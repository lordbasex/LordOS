/* vt_keys_test.c — keys with modifiers in the protocol the program asks for (vt_key_encode): the kitty
 * keyboard protocol's flag 1 and xterm's modifyOtherKeys, as Claude Code asks for them at start
 * (CSI ? u, CSI > 1 u, CSI > 4 ; 2 m), so Shift+Enter is not a plain Enter. */
#include "term/term.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

void term_set_clipboard(const char *s, size_t n) { (void)s; (void)n; }

static char replied[64];
static void respond(Term *t, const char *s, size_t n) { (void)t; snprintf(replied, sizeof replied, "%.*s", (int)n, s); }

static int fails, checks;
static void feed(Term *t, const char *s) { vt_write(t, (const uint8_t *)s, strlen(s)); }
static void key(Term *t, const char *k, unsigned cp, int m, const char *want, const char *what) {
    char out[32];
    int n = vt_key_encode(t, k, cp, m, out, sizeof out);
    out[n > 0 ? n : 0] = 0;
    checks++;
    if (strcmp(out, want)) { fails++; printf("FAIL %s: want \"%s\", got \"%s\"\n", what, want, out); }
}
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

int main(void) {
    setlocale(LC_CTYPE, "C.UTF-8");
    TermProfile p;
    memset(&p, 0, sizeof p);
    Term t;
    if (vt_init(&t, 40, 5, 10, &p)) return 1;
    t.respond = respond;

    /* nothing asked: the legacy encoding (0 = the caller's own) */
    key(&t, "Enter", 0, 2, "", "Shift+Enter without a protocol");
    feed(&t, "\033[?u");
    check(!strcmp(replied, "\033[?0u"), "the query answers flags 0");

    /* kitty, flag 1 (what is pushed beyond flag 1 is not kept: the query says what is supported) */
    feed(&t, "\033[>5u\033[?u");
    check(!strcmp(replied, "\033[?1u"), "the query answers flag 1 after a push of 5");
    key(&t, "Enter", 0, 2, "\033[13;2u", "Shift+Enter");
    key(&t, "Enter", 0, 3, "\033[13;3u", "Alt+Enter");
    key(&t, "Enter", 0, 1, "", "Enter alone stays CR");
    key(&t, "Escape", 0, 1, "\033[27u", "Escape alone");
    key(&t, "Tab", 0, 5, "\033[9;5u", "Ctrl+Tab");
    key(&t, "Backspace", 0, 3, "\033[127;3u", "Alt+Backspace");
    key(&t, NULL, 'c', 5, "\033[99;5u", "Ctrl+C");
    key(&t, NULL, 'A', 6, "\033[97;6u", "Ctrl+Shift+A: the key's own character");
    key(&t, NULL, 'a', 2, "", "Shift+A is text");
    key(&t, "ArrowUp", 0, 2, "", "arrows keep their CSI form");

    /* the alternate screen has its own stack */
    feed(&t, "\033[?1049h");
    key(&t, "Enter", 0, 2, "", "the alternate screen starts empty");
    feed(&t, "\033[>1u");
    key(&t, "Enter", 0, 2, "\033[13;2u", "pushed on the alternate screen");
    feed(&t, "\033[<u\033[?1049l");
    key(&t, "Enter", 0, 2, "\033[13;2u", "the main screen's stack is kept");

    /* pop, set */
    feed(&t, "\033[<u");
    key(&t, "Enter", 0, 2, "", "popped");
    feed(&t, "\033[=1;1u");
    key(&t, "Enter", 0, 2, "\033[13;2u", "set");
    feed(&t, "\033[=1;3u");
    key(&t, "Enter", 0, 2, "", "and not");
    feed(&t, "\033[<10u");

    /* modifyOtherKeys 2 */
    feed(&t, "\033[>4;2m");
    key(&t, "Enter", 0, 2, "\033[27;2;13~", "Shift+Enter with modifyOtherKeys 2");
    key(&t, NULL, 'c', 5, "\033[27;5;99~", "Ctrl+C with modifyOtherKeys 2");
    key(&t, "Tab", 0, 2, "", "Shift+Tab keeps CSI Z");
    feed(&t, "\033[>4m");
    key(&t, "Enter", 0, 2, "", "modifyOtherKeys reset");

    /* a reset forgets everything */
    feed(&t, "\033[>1u\033[>4;2m\033c");
    key(&t, "Enter", 0, 2, "", "after RIS");

    vt_free(&t);
    printf("vt-keys-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
