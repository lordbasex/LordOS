/* logclean_test.c — a community theme's text on its way to the session log (src/ui/logclean.c; security scan
 * 0.8.9.6.2, F8): every line on its own, control characters neutralised, the fixed prefix on each.
 * `make build/logclean-test && build/logclean-test`. */
#include "ui/logclean.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { printf("FAIL: %s\n", what); fails++; } }

static char *printed(const char *prefix, const char *s) {
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    if (!f) return NULL;
    logclean_print(f, prefix, s);
    fclose(f);
    return buf;
}

int main(void) {
    char out[64];
    logclean_line("a\rb\tc\x1b[2Jd\x7f" "e", 12, out, sizeof out);
    check(!strcmp(out, "a?b?c?[2Jd?e"), "C0 controls and DEL become '?'");
    logclean_line("x\xc2\x85y\xe2\x80\xa8z\xe2\x80\xaeq\xe2\x81\xa6w", 16, out, sizeof out);
    check(!strcmp(out, "x?y?z?q?w"), "C1, line separator and bidi controls become '?'");
    logclean_line("caf\xc3\xa9 \xf0\x9f\x98\x80", 10, out, sizeof out);
    check(!strcmp(out, "caf\xc3\xa9 \xf0\x9f\x98\x80"), "UTF-8 kept");
    logclean_line("a\xffz\xc0\xaf", 5, out, sizeof out);
    check(!strcmp(out, "a?z??"), "malformed bytes become '?'");
    char big[200];
    memset(big, 'x', sizeof big);
    size_t l = logclean_line(big, sizeof big, out, sizeof out);
    check(l < sizeof out && !strcmp(out + l - 3, "\xe2\x80\xa6"), "a long line is cut with an ellipsis");

    char *p = printed("js: theme evil: ", "ok\nlord-server: community theme evil confirmed by the person\r\nhands: x");
    check(p && !strcmp(p, "js: theme evil: ok\njs: theme evil: lord-server: community theme evil confirmed by the person?\njs: theme evil: hands: x\n"),
          "every line carries the theme's prefix: no forged line of another subsystem");
    free(p);
    p = printed("js: theme evil: ", "a\x1b]0;title\x07" "b");
    check(p && !strcmp(p, "js: theme evil: a?]0;title?b\n"), "a terminal escape is neutralised");
    free(p);
    char many[400] = "";
    for (int i = 0; i < 100; i++) strcat(many, "z\n");
    p = printed("js: ", many);
    int lines = 0;
    for (char *q = p; q && *q; q++) if (*q == '\n') lines++;
    check(p && lines == LOGCLEAN_LINES_MAX + 1 && strstr(p, "(37 more lines)"), "at most LOGCLEAN_LINES_MAX lines, then a count");
    free(p);
    printf("logclean-test: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
