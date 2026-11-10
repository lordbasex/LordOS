/* urilist_test.c — file paths as text/uri-list and back (src/wl/urilist.c), for drag and drop. */
#include "../src/wl/urilist.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    char out[1024], dec[1024];

    /* encoding: file:// URIs, percent-encoded, each ended by CRLF */
    const char *two[] = { "/home/ana/Report 2026.pdf", "/home/ana/ñandú#1?.txt" };
    size_t n = uril_encode(two, 2, out, sizeof out);
    CHECK(n == strlen(out));
    CHECK(!strcmp(out, "file:///home/ana/Report%202026.pdf\r\nfile:///home/ana/%C3%B1and%C3%BA%231%3F.txt\r\n"));
    const char *plain[] = { "/a/b c", "/d" };
    CHECK(uril_plain(plain, 2, out, sizeof out) == 9 && !strcmp(out, "/a/b c\n/d"));

    /* what a drag may not carry: relative, a newline, empty, too many, and a list that does not fit */
    const char *rel[] = { "home/ana" }, *nl[] = { "/home/a\nb" }, *cr[] = { "/x\ry" }, *empty[] = { "" };
    CHECK(!uril_encode(rel, 1, out, sizeof out));
    CHECK(!uril_encode(nl, 1, out, sizeof out));
    CHECK(!uril_encode(cr, 1, out, sizeof out));
    CHECK(!uril_encode(empty, 1, out, sizeof out));
    CHECK(!uril_plain(nl, 1, out, sizeof out));
    CHECK(!uril_encode(two, 0, out, sizeof out));
    CHECK(!uril_encode(two, URIL_MAX_PATHS + 1, out, sizeof out));
    CHECK(!uril_encode(two, 2, out, 20));
    CHECK(!uril_plain(plain, 2, out, 9));                      /* one byte short of the NUL */
    CHECK(uril_plain(plain, 2, out, 10) == 9);
    CHECK(uril_path_ok("/a", 2) && !uril_path_ok("/a\0b", 4) && !uril_path_ok(NULL, 0) && !uril_path_ok("/a", 0));
    static char longp[PATH_MAX + 2];
    memset(longp, 'a', PATH_MAX);
    longp[0] = '/';
    CHECK(!uril_path_ok(longp, strlen(longp)));

    /* decoding: back to the same paths */
    n = uril_encode(two, 2, out, sizeof out);
    CHECK(uril_decode(out, n, dec, sizeof dec, 8) == 2);
    CHECK(!strcmp(dec, two[0]) && !strcmp(dec + strlen(dec) + 1, two[1]));

    /* what apps write: localhost, LF only, comments, other hosts and schemes, a query, no final newline */
    const char *mixed = "# a comment\nfile://localhost/etc/hosts\n\nhttps://example.org/x\nfile://server/share/x\r\n"
                        "file:///tmp/a%2Fb?query\r\nfile:///no/end";
    CHECK(uril_decode(mixed, strlen(mixed), dec, sizeof dec, 8) == 3);
    const char *p = dec;
    CHECK(!strcmp(p, "/etc/hosts"));
    p += strlen(p) + 1;
    CHECK(!strcmp(p, "/tmp/a/b"));
    p += strlen(p) + 1;
    CHECK(!strcmp(p, "/no/end"));

    /* dropped lines: %00, an encoded newline, a bad escape, a relative path, nothing after the host */
    const char *bad = "file:///a%00b\nfile:///a%0Ab\nfile:///a%0db\nfile:///a%zz\nfile:///a%4\nfile://relative\nfile://\nfile:///ok\n";
    CHECK(uril_decode(bad, strlen(bad), dec, sizeof dec, 8) == 1 && !strcmp(dec, "/ok"));
    /* a NUL inside the data drops its line too */
    const char nul[] = "file:///a\0b\nfile:///c\n";
    CHECK(uril_decode(nul, sizeof nul - 1, dec, sizeof dec, 8) == 1 && !strcmp(dec, "/c"));

    /* at most max paths, and -1 if they do not fit */
    const char *three = "file:///1\nfile:///2\nfile:///3\n";
    CHECK(uril_decode(three, strlen(three), dec, sizeof dec, 2) == 2);
    CHECK(uril_decode(three, strlen(three), dec, 5, 8) == -1);
    CHECK(uril_decode(three, strlen(three), dec, 6, 8) == -1);
    CHECK(uril_decode(three, strlen(three), dec, 9, 8) == 3);
    CHECK(uril_decode("", 0, dec, sizeof dec, 8) == 0);

    if (fails) { printf("urilist-test: %d failure(s)\n", fails); return 1; }
    printf("urilist-test: ok\n");
    return 0;
}
