/* urilist_test.c — file lists on the clipboard: text/uri-list and x-special/gnome-copied-files (src/wl/urilist.c). */
#include "../src/wl/urilist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

/* parses and gives the paths joined with '|' ("" when refused) */
static const char *parsed(const char *s, size_t n, int gnome, int *cut, int *count) {
    static char out[8192];
    size_t len;
    char *p = uril_parse(s, n, gnome, cut, &len, count);
    out[0] = 0;
    if (!p) return out;
    size_t o = 0;
    for (size_t off = 0; off < len && o + 2 < sizeof out;) {
        size_t k = strlen(p + off);
        if (o) out[o++] = '|';
        if (o + k + 1 >= sizeof out) break;
        memcpy(out + o, p + off, k);
        o += k;
        off += k + 1;
    }
    out[o] = 0;
    free(p);
    return out;
}
#define PARSE(s, g) parsed(s, strlen(s), g, &cut, &count)

int main(void) {
    int cut, count;
    size_t n;
    char *t;

    /* encoding: reserved bytes, spaces and non-ASCII are %XX; slashes and unreserved stay */
    static const char two[] = "/home/u/a b.txt\0/home/u/año/100%#?.png";
    t = uril_encode(two, sizeof two, URIL_URI_LIST, 0, &n);
    CHECK(t && !strcmp(t, "file:///home/u/a%20b.txt\r\nfile:///home/u/a%C3%B1o/100%25%23%3F.png\r\n") && n == strlen(t));
    free(t);
    t = uril_encode(two, sizeof two, URIL_GNOME, 0, &n);
    CHECK(t && !strcmp(t, "copy\nfile:///home/u/a%20b.txt\nfile:///home/u/a%C3%B1o/100%25%23%3F.png"));
    free(t);
    t = uril_encode(two, sizeof two, URIL_GNOME, 1, &n);
    CHECK(t && !strncmp(t, "cut\nfile:///home/u/a%20b.txt\n", 29));
    free(t);
    t = uril_encode(two, sizeof two, URIL_PLAIN, 0, &n);
    CHECK(t && !strcmp(t, "/home/u/a b.txt\n/home/u/año/100%#?.png"));
    free(t);

    /* bad paths are refused, the whole list with them */
    CHECK(!uril_encode("relative\0", 9, URIL_URI_LIST, 0, &n));
    CHECK(!uril_encode("/a\nb\0", 5, URIL_URI_LIST, 0, &n));
    CHECK(!uril_encode("/a\rb\0", 5, URIL_URI_LIST, 0, &n));
    CHECK(!uril_encode("/ok\0\0", 5, URIL_URI_LIST, 0, &n));             /* an empty path */
    CHECK(!uril_encode("/no-nul", 7, URIL_URI_LIST, 0, &n));             /* not NUL-terminated */
    CHECK(!uril_encode("/bad\xff\0", 6, URIL_URI_LIST, 0, &n));          /* not UTF-8 */
    CHECK(!uril_encode("/a\xc0\xaf\0", 5, URIL_URI_LIST, 0, &n));        /* overlong "/" */
    CHECK(!uril_encode("", 0, URIL_URI_LIST, 0, &n));
    CHECK(!uril_path_ok("/a\0b", 4));                                    /* a NUL inside */
    CHECK(uril_path_ok("/", 1));

    /* round trip */
    t = uril_encode(two, sizeof two, URIL_URI_LIST, 0, &n);
    CHECK(!strcmp(parsed(t, n, 0, &cut, &count), "/home/u/a b.txt|/home/u/año/100%#?.png") && count == 2 && !cut);
    free(t);
    t = uril_encode(two, sizeof two, URIL_GNOME, 1, &n);
    CHECK(!strcmp(parsed(t, n, 1, &cut, &count), "/home/u/a b.txt|/home/u/año/100%#?.png") && count == 2 && cut);
    free(t);

    /* uri-list: comments, blank lines, LF or CRLF, localhost, any letter case in the scheme and host */
    CHECK(!strcmp(PARSE("# a comment\r\n\r\nfile:///tmp/x\nFILE://LocalHost/tmp/y\r\n", 0), "/tmp/x|/tmp/y") && count == 2);
    /* other hosts, other schemes and broken escapes are left out; the rest stays */
    CHECK(!strcmp(PARSE("file://server/share/a\r\nsmb://h/s\r\nhttps://x.example/a\r\nfile:///ok\r\n", 0), "/ok") && count == 1);
    CHECK(!strcmp(PARSE("file:///a%2\r\nfile:///a%zz\r\nfile:///a%\r\nfile:///good%41\r\n", 0), "/goodA"));
    CHECK(!strcmp(PARSE("file:///a?x=1\r\nfile:///a#frag\r\nfile:///a b\r\nfile:relative\r\nfile://\r\nfile:///\r\n", 0), "/"));
    /* decodes to something that isn't a good path: NUL, a line break, not UTF-8 */
    CHECK(!*PARSE("file:///a%00b\r\nfile:///a%0Ab\r\nfile:///a%0Db\r\nfile:///a%FF\r\n", 0));
    CHECK(!*PARSE("file://localhost.evil/etc/passwd\r\nfile://localhostx/a\r\nfile://user@localhost/a\r\n", 0));
    CHECK(!*PARSE("", 0) && !count);
    CHECK(!*PARSE("\r\n\r\n# nothing\r\n", 0));

    /* gnome-copied-files: the first line says copy or cut, nothing else is accepted */
    CHECK(!strcmp(PARSE("copy\nfile:///a\nfile:///b", 1), "/a|/b") && !cut && count == 2);
    CHECK(!strcmp(PARSE("cut\r\nfile:///a\r\n", 1), "/a") && cut);
    CHECK(!*PARSE("move\nfile:///a", 1) && !cut);
    CHECK(!*PARSE("file:///a\nfile:///b", 1));                            /* no copy/cut line */
    CHECK(!*PARSE("x-special/some-clipboard\ncopy\nfile:///a", 1));
    CHECK(!*PARSE("cut\n", 1) && !cut);                                   /* cut of nothing */
    CHECK(!*PARSE("Copy\nfile:///a", 1));

    /* a path as long as allowed, and one byte too long */
    {
        static char big[URIL_PATH_MAX + 64];
        strcpy(big, "file:///");
        memset(big + 8, 'a', URIL_PATH_MAX - 2);
        big[8 + URIL_PATH_MAX - 2] = 0;                                  /* "/" + 4094 = 4095 bytes */
        CHECK(*PARSE(big, 0) && count == 1);
        strcat(big, "a");
        CHECK(!*PARSE(big, 0));
    }

    /* huge input: refused or bounded, never read past */
    {
        size_t hn = URIL_MAX_BYTES + 1;
        char *h = malloc(hn);
        if (h) {
            memset(h, 'x', hn);
            size_t len;
            CHECK(!uril_parse(h, hn, 0, &cut, &len, &count));             /* longer than the clipboard reads */
            CHECK(!uril_parse(h, URIL_MAX_BYTES, 0, &cut, &len, &count)); /* one 16 MiB line, not a URI */
            /* more files than allowed: the list is refused whole */
            size_t o = 0;
            for (int i = 0; i <= URIL_MAX_FILES; i++) o += (size_t)sprintf(h + o, "file:///f%d\n", i);
            CHECK(!uril_parse(h, o, 0, &cut, &len, &count) && !count);
            /* exactly as many as allowed */
            o = 0;
            for (int i = 0; i < URIL_MAX_FILES; i++) o += (size_t)sprintf(h + o, "file:///f%d\n", i);
            char *p = uril_parse(h, o, 0, &cut, &len, &count);
            CHECK(p && count == URIL_MAX_FILES && len < o);
            free(p);
            /* only escapes: three bytes of text per byte of path, and a path's limit still holds */
            for (int k = 0; k < 2; k++) {
                int esc = k ? URIL_PATH_MAX : URIL_PATH_MAX - 2;
                o = (size_t)sprintf(h, "file:///");
                for (int i = 0; i < esc; i++) o += (size_t)sprintf(h + o, "%%41");
                p = uril_parse(h, o, 0, &cut, &len, &count);
                CHECK(k ? !p : p && len == (size_t)esc + 2 && p[1] == 'A');
                free(p);
            }
            free(h);
        }
    }

    if (fails) { printf("urilist-test: %d failures\n", fails); return 1; }
    printf("urilist-test: ok\n");
    return 0;
}
