/* pkg_https_test.c — the package engine's downloads over HTTPS (src/pkg/http.c + lord-tls), against
 * real servers: run as root on a machine with network, lord-tls next to this binary (root's).
 *
 *   sudo build/pkg-https-test        (not in make test: it needs the network) */
#include "../src/pkg/http.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int n, bad;
#define CHECK(name, cond, it) do { n++; if (!(cond)) { bad++; printf("✗ %s (status %d, %s)\n", name, (it)->status, (it)->err); } else printf("✓ %s\n", name); } while (0)

static FetchItem one(const char *url) {
    FetchItem it;
    memset(&it, 0, sizeof it);
    snprintf(it.url, sizeof it.url, "%s", url);
    snprintf(it.dest, sizeof it.dest, "/tmp/pkg-https-test.out");
    it.max = 64u << 20;
    fetch_batch(&it, 1, NULL, NULL);
    unlink(it.dest);
    return it;
}

int main(void) {
    FetchItem a = one("https://downloads.claude.ai/claude-code/apt/stable/dists/stable/InRelease");
    CHECK("an HTTPS-only vendor repository", a.status == 200 && a.size > 100, &a);
    FetchItem b = one("https://lordos.org/apt/dists/stable/InRelease");
    CHECK("LordOS's repository over HTTPS", b.status == 200 && b.size > 100, &b);
    FetchItem c = one("http://lordos.org/apt/dists/stable/InRelease");
    CHECK("the same over plain HTTP still works", c.status == 200 && c.size == b.size, &c);
    FetchItem d = one("http://packages.microsoft.com/repos/code/dists/stable/InRelease");
    CHECK("an http:// URL redirected to https:// is followed", d.status == 200 && d.size > 100, &d);
    FetchItem e = one("https://self-signed.badssl.com/");
    CHECK("a self-signed certificate is refused", e.status != 200 && strstr(e.err, "tlsFailed"), &e);
    FetchItem f = one("https://wrong.host.badssl.com/");
    CHECK("a certificate for another host is refused", f.status != 200 && strstr(f.err, "tlsFailed"), &f);
    FetchItem g = one("https://expired.badssl.com/");
    CHECK("an expired certificate is refused", g.status != 200 && strstr(g.err, "tlsFailed"), &g);
    printf("pkg-https-test: %d/%d ok\n", n - bad, n);
    return bad ? 1 : 0;
}
