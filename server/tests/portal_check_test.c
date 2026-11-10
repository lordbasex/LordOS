/* portal_check_test.c — the OpenURI portal's checks without a bus (src/ui/portalcheck.c): the web
 * addresses an app may hand to the browser (the same answers as sdk/portalopenmodel.js), the local files
 * it may open, and that a file swapped after it was checked is refused. */
#include "ui/portalcheck.h"
#include "fsops.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int ok, bad;
#define CHECK(c) do { if (c) ok++; else { bad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char H[256];

static void web(const char *url, const char *want) {
    char host[256];
    int r = portal_web_host(url, host, sizeof host);
    if (want ? (r == 0 && !strcmp(host, want)) : r != 0) ok++;
    else { bad++; printf("FAIL web %s: %s (wanted %s)\n", url, r ? "refused" : host, want ? want : "refused"); }
}

static void put(const char *rel, mode_t mode) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd >= 0) { if (write(fd, "x", 1) != 1) bad++; close(fd); }
    chmod(p, mode);
}
static void at(char *out, const char *rel) { snprintf(out, PATH_MAX, "%s/%s", H, rel); }

int main(void) {
    /* parser differentials: refused */
    const char *refused[] = {
        "https://bank.example@evil.example/", "https://user:pw@example.org/", "https://bank.example:443@evil.example",
        "https://evil.example\\@bank.example/", "https://bank.example\\.evil.example/", "https://example.org/a\\b",
        "https://ban\tk.example/", "https://example.org/\nx", "https://example.org/\x7f", "https://example.org/\xc2\x85",
        " https://example.org/", "https://example.org/\xc2\xa0", "https://b\xc3\xa4" "cker.example/", "https://example.org/caf\xc3\xa9",
        "https://evil%2Eexample/", "https://bank.example%40evil.example/", "http://2130706433/", "http://0x7f.1/",
        "http://0177.0.0.1/", "http://127.1/", "http://1.2.3.4./", "http://1.2.3.256/", "http://example.123/", "http://example.0x/",
        "https://example.org:/", "https://example.org:443x/", "https://example.org:65536/", "https://example.org:1:2/",
        "https://example..org/", "https://.example.org/", "http://[1:2:3]/", "http://[1::2::3]/", "http://[::1/",
        "http://[::1]evil.example/", "http://[1:2:3:4:5:6:7:8:9]/", "http://[::1:]/", "http://[:1::2]/", "https:/example.org/",
        "https:///example.org/", "blob:https://example.org/x", "data:text/html,x", "file:///etc/passwd", "mailto:a@example.org",
        "javascript:alert(1)", "steam://run/1", "https://", "", NULL,
    };
    for (int i = 0; refused[i]; i++) web(refused[i], NULL);
    web(NULL, NULL);
    char longurl[5000];
    snprintf(longurl, sizeof longurl, "https://example.org/");
    memset(longurl + 20, 'a', sizeof longurl - 21);
    longurl[sizeof longurl - 1] = 0;
    web(longurl, NULL);

    /* the host a browser goes to */
    web("https://example.org/a?b=1#c", "example.org");
    web("HTTPS://WWW.Example.ORG:8443/x", "www.example.org");
    web("http://example.org", "example.org");
    web("http://example.org.", "example.org.");
    web("http://example.org?q=@x", "example.org");
    web("http://example.org#@evil.example", "example.org");
    web("https://example.org/path@evil.example", "example.org");
    web("https://xn--bcker-gra.example/", "xn--bcker-gra.example");
    web("https://my_host.example/", "my_host.example");
    web("http://192.168.1.10:8080/", "192.168.1.10");
    web("http://[::1]:8080/", "[::1]");
    web("http://[::]/", "[::]");
    web("http://[2001:db8::1]/", "[2001:db8::1]");
    web("http://[1:2:3:4:5:6:7:8]/", "[1:2:3:4:5:6:7:8]");
    web("http://[::ffff:192.168.0.1]/", "[::ffff:192.168.0.1]");
    web("https://example.org/a%20b", "example.org");
    char small[4];
    CHECK(portal_web_host("https://example.org/", small, sizeof small) != 0);

    /* local files */
    snprintf(H, sizeof H, "/tmp/portal-check-test-%d", (int)getpid());
    mkdir(H, 0755);
    fsops_init(H, "nadie");
    char p[PATH_MAX], q[PATH_MAX], real[PATH_MAX];
    PortalSeen seen;
    put("a.txt", 0644);
    put("b.txt", 0644);
    put("run.sh", 0755);
    put("app.desktop", 0644);
    at(p, "Music");
    mkdir(p, 0755);

    at(p, "a.txt");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FILE && !strcmp(real, p));
    CHECK(portal_file_still(real, PORTAL_FILE, &seen));
    CHECK(portal_file_still(real, PORTAL_REVEAL, &seen));
    CHECK(portal_file_kind(p, PORTAL_REVEAL, real, &seen) == PORTAL_REVEAL);
    at(p, "Music");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FOLDER && portal_file_still(real, PORTAL_FOLDER, &seen));
    at(p, "run.sh");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) < 0);
    CHECK(portal_file_kind(p, PORTAL_REVEAL, real, &seen) == PORTAL_REVEAL);
    at(p, "app.desktop");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) < 0);
    CHECK(portal_file_kind("/etc/passwd", PORTAL_FILE, real, &seen) < 0);
    CHECK(portal_file_kind("a.txt", PORTAL_FILE, real, &seen) < 0);
    at(p, "missing");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) < 0);

    /* a link is followed to the real file when the app asks */
    at(p, "a.txt"); at(q, "link.txt");
    CHECK(symlink(p, q) == 0);
    CHECK(portal_file_kind(q, PORTAL_FILE, real, &seen) == PORTAL_FILE && !strcmp(real, p));

    /* swapped after the check, before the person answers: refused */
    at(p, "a.txt");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FILE);
    at(q, "b.txt");
    CHECK(rename(q, p) == 0);                                   /* another file in its place */
    CHECK(!portal_file_still(real, PORTAL_FILE, &seen));

    at(p, "a.txt");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FILE);
    unlink(p);
    CHECK(symlink("/etc/passwd", p) == 0);                      /* a link in its place */
    CHECK(!portal_file_still(real, PORTAL_FILE, &seen));
    unlink(p);
    put("a.txt", 0644);

    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FILE);
    chmod(p, 0755);                                             /* made executable */
    CHECK(!portal_file_still(real, PORTAL_FILE, &seen));
    chmod(p, 0644);
    CHECK(portal_file_still(real, PORTAL_FILE, &seen));
    unlink(p);                                                  /* gone */
    CHECK(!portal_file_still(real, PORTAL_FILE, &seen));
    put("a.txt", 0644);

    /* a folder on the way swapped for a link to another folder with the same names */
    at(p, "d1"); mkdir(p, 0755);
    at(p, "d2"); mkdir(p, 0755);
    put("d1/f.txt", 0644);
    put("d2/f.txt", 0644);
    at(p, "d1/f.txt");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FILE);
    at(p, "d1"); at(q, "d1-old");
    CHECK(rename(p, q) == 0);
    CHECK(symlink("d2", p) == 0);
    CHECK(!portal_file_still(real, PORTAL_FILE, &seen));

    /* a folder swapped for a file */
    at(p, "Music");
    CHECK(portal_file_kind(p, PORTAL_FILE, real, &seen) == PORTAL_FOLDER);
    rmdir(p);
    put("Music", 0644);
    CHECK(!portal_file_still(real, PORTAL_FOLDER, &seen));

    /* the file behind an fd */
    at(p, "b2.txt");
    put("b2.txt", 0644);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && portal_fd_kind(fd, PORTAL_FILE, real, &seen) == PORTAL_FILE && !strcmp(real, p));
    CHECK(portal_file_still(real, PORTAL_FILE, &seen));
    unlink(p);                                                  /* its name leads nowhere now */
    CHECK(portal_fd_kind(fd, PORTAL_FILE, real, &seen) < 0);
    close(fd);
    CHECK(!portal_file_still(NULL, PORTAL_FILE, &seen));
    CHECK(!portal_file_still(real, PORTAL_FILE, NULL));

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", H);
    if (system(cmd)) bad++;
    printf("portal-check-test: %d/%d checks\n", ok, ok + bad);
    return bad ? 1 : 0;
}
