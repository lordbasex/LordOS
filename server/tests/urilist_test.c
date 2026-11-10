/* urilist_test.c — file lists on the clipboard and in drag and drop: text/uri-list and
 * x-special/gnome-copied-files (src/wl/urilist.c). */
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

    /* drag and drop: the paths Files drags, joined and offered as text/uri-list and plain text */
    {
        const char *dtwo[] = { "/home/ana/Report 2026.pdf", "/home/ana/ñandú#1?.txt" };
        size_t pl;
        char *pls = uril_join(dtwo, 2, &pl);
        CHECK(pls && pl == strlen(dtwo[0]) + strlen(dtwo[1]) + 2 && !strcmp(pls, dtwo[0]) && !strcmp(pls + strlen(pls) + 1, dtwo[1]));
        t = pls ? uril_encode(pls, pl, URIL_URI_LIST, 0, &n) : NULL;
        CHECK(t && n == strlen(t) && !strcmp(t, "file:///home/ana/Report%202026.pdf\r\nfile:///home/ana/%C3%B1and%C3%BA%231%3F.txt\r\n"));
        /* and a drop of that list reads back the same paths */
        CHECK(t && !strcmp(parsed(t, n, 0, &cut, &count), "/home/ana/Report 2026.pdf|/home/ana/ñandú#1?.txt") && count == 2 && !cut);
        free(t);
        free(pls);
        const char *dplain[] = { "/a/b c", "/d" };
        pls = uril_join(dplain, 2, &pl);
        t = pls ? uril_encode(pls, pl, URIL_PLAIN, 0, &n) : NULL;
        CHECK(t && n == 9 && !strcmp(t, "/a/b c\n/d"));
        free(t);
        free(pls);
        /* what a drag may not carry: relative, a line break, empty, none at all; one bad path refuses them all */
        const char *rel[] = { "home/ana" }, *nl[] = { "/home/a\nb" }, *cr[] = { "/x\ry" }, *empty[] = { "" }, *nul[] = { NULL };
        const char *mix[] = { "/ok", "relative" };
        CHECK(!uril_join(rel, 1, &pl) && !pl);
        CHECK(!uril_join(nl, 1, &pl));
        CHECK(!uril_join(cr, 1, &pl));
        CHECK(!uril_join(empty, 1, &pl));
        CHECK(!uril_join(nul, 1, &pl));
        CHECK(!uril_join(mix, 2, &pl));
        CHECK(!uril_join(dtwo, 0, &pl));
        CHECK(!uril_join(NULL, 1, &pl));
        CHECK(uril_path_ok("/a", 2) && !uril_path_ok(NULL, 0) && !uril_path_ok("/a", 0));
        static char longp[URIL_PATH_MAX + 2];
        memset(longp, 'a', URIL_PATH_MAX);
        longp[0] = '/';
        CHECK(!uril_path_ok(longp, strlen(longp)));

        /* what apps drop: localhost, LF only, comments, other hosts and schemes, no final newline; a query is
         * not a file's path, so its line is left out */
        const char *mixed = "# a comment\nfile://localhost/etc/hosts\n\nhttps://example.org/x\nfile://server/share/x\r\n"
                            "file:///tmp/a%2Fb?query\r\nfile:///tmp/a%2Fb\r\nfile:///no/end";
        CHECK(!strcmp(PARSE(mixed, 0), "/etc/hosts|/tmp/a/b|/no/end") && count == 3);
        /* dropped lines: %00, an encoded line break, a bad escape, a relative path, nothing after the host */
        CHECK(!strcmp(PARSE("file:///a%00b\nfile:///a%0Ab\nfile:///a%0db\nfile:///a%zz\nfile:///a%4\nfile://relative\nfile://\nfile:///ok\n", 0), "/ok"));
        /* a NUL inside the data drops its line too */
        static const char dnul[] = "file:///a\0b\nfile:///c\n";
        CHECK(!strcmp(parsed(dnul, sizeof dnul - 1, 0, &cut, &count), "/c") && count == 1);
    }

    /* the text a person sees must name exactly the paths that get copied */
    {
        static const char ab[] = "/home/u/a b\0/home/u/c";
        size_t al = sizeof ab;
#define NAMES(s) uril_text_names(s, strlen(s), ab, al)
        CHECK(NAMES("/home/u/a b\n/home/u/c"));
        CHECK(NAMES("/home/u/a b\r\n/home/u/c\r\n"));                    /* CRLF, one line end after the last */
        CHECK(NAMES("/home/u/a b\n/home/u/c\n"));
        CHECK(NAMES("file:///home/u/a%20b\nfile://localhost/home/u/c"));    /* the same files as URIs */
        CHECK(NAMES("/home/u/a b\nfile:///home/u/c"));
        CHECK(!NAMES("/home/u/c\n/home/u/a b"));                            /* another order */
        CHECK(!NAMES("/home/u/a b"));                                       /* fewer */
        CHECK(!NAMES("/home/u/a b\n/home/u/c\n/home/u/.ssh/id_ed25519"));  /* more */
        CHECK(!NAMES("/home/u/a b\n/home/u/c\n\n"));                        /* a blank line more */
        CHECK(!NAMES("/home/u/a b\n\n/home/u/c"));
        CHECK(!NAMES("holiday photos"));                                    /* a text that isn't them */
        CHECK(!NAMES(" /home/u/a b\n/home/u/c"));
        CHECK(!NAMES("/home/u/a%20b\n/home/u/c"));                          /* escapes only in a URI */
        CHECK(!NAMES("file://otherhost/home/u/a%20b\n/home/u/c"));
        CHECK(!NAMES("") && !NAMES("\n"));
        CHECK(!uril_text_names("/home/u/a b\0/home/u/c", sizeof ab - 1, ab, al));   /* a NUL is not a line end */
        CHECK(!uril_text_names("/home/u/a b\n/home/u/c", 22, ab, al - 1));          /* a list without its last NUL */
#undef NAMES
        /* the decision: by where the list came from, and its text */
        const char *txt = "/home/u/a b\n/home/u/c", *other = "/home/u/.ssh/id_ed25519";
        CHECK(uril_accept(URIL_FROM_APP, txt, strlen(txt), ab, al));
        CHECK(uril_accept(URIL_FROM_APP, NULL, 0, ab, al));                 /* files only: their text will be the paths */
        CHECK(!uril_accept(URIL_FROM_APP, other, strlen(other), ab, al));
        CHECK(!uril_accept(URIL_FROM_ISOLATED, txt, strlen(txt), ab, al));
        CHECK(!uril_accept(URIL_FROM_ISOLATED, NULL, 0, ab, al));
        CHECK(!uril_accept(URIL_FROM_FLATPAK, txt, strlen(txt), ab, al));
        CHECK(!uril_accept(URIL_FROM_FLATPAK, NULL, 0, ab, al));
        CHECK(!uril_accept(URIL_FROM_UNKNOWN, txt, strlen(txt), ab, al));
        CHECK(!uril_accept(42, txt, strlen(txt), ab, al));
        CHECK(!uril_accept(URIL_FROM_APP, txt, strlen(txt), NULL, 0));
    }

    if (fails) { printf("urilist-test: %d failures\n", fails); return 1; }
    printf("urilist-test: ok\n");
    return 0;
}
