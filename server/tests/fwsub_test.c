/* fwsub_test.c — blocklist subscriptions, lord-server's side (src/fw/fwsub.c) and the lord-fetch
 * helper's request checks (0.8.5, docs/FIREWALL.md "Blocklists"). No network: the downloads are
 * seeded in the cache folder; lord-fetch is only asked things it must refuse.
 *   build/fwsub-test [path to build/lord-fetch] */
#include "fw/fwsub.h"
#include "fw/fwlists.h"
#include "doc/wire.h"
#include "fetch/fetchproto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(s, f);
    fclose(f);
}

static char *slurp(const char *p) {
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    static char b[1 << 16];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static void test_urls(void) {
    char h[256], p[512];
    int port;
    CHECK(!fws_split_url("https://lists.example.org/a/b.txt", h, sizeof h, &port, p, sizeof p) && !strcmp(h, "lists.example.org") && port == 443 && !strcmp(p, "/a/b.txt"));
    CHECK(!fws_split_url("https://Lists.Example.org:8443/x?y=1", h, sizeof h, &port, p, sizeof p) && !strcmp(h, "lists.example.org") && port == 8443 && !strcmp(p, "/x?y=1"));
    CHECK(!fws_split_url("https://example.org", h, sizeof h, &port, p, sizeof p) && !strcmp(p, "/"));
    const char *bad[] = { "http://example.org/x", "https://", "https:///x", "https://exa mple.org/", "https://example.org:0/", "https://example.org:70000/",
                          "https://user@example.org/", "https://localhost/x", "https://.example.org/", "https://example..org/", "https://example.org/a b",
                          "https://example.org/\x01", "https://example.org?x", "file:///etc/passwd", "https://[::1]/x", "", NULL };
    for (int i = 0; bad[i]; i++) CHECK(fws_split_url(bad[i], h, sizeof h, &port, p, sizeof p) == -1);
}

static int wait_job(void) {
    for (int i = 0; i < 300; i++) {
        if (fws_poll()) return 1;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void test_subscriptions(const char *root) {
    char p[1024], assets[256];
    snprintf(assets, sizeof assets, "%s/assets", root);
    snprintf(p, sizeof p, "%s/fw", assets);
    mkdir(assets, 0700);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/fw/blocklists.json", assets);
    put(p, "{\"lists\":[{\"id\":\"ads\",\"url\":\"https://lists.example.org/ads.txt\",\"format\":\"domains\",\"sub\":0},"
           "{\"id\":\"bad id\",\"url\":\"https://x.example/\",\"format\":\"domains\"},"
           "{\"id\":\"ips\",\"url\":\"http://plain.example/ips\",\"format\":\"ips\"},"
           "{\"id\":\"nets\",\"url\":\"https://lists.example.org/nets.txt\",\"format\":\"ips\"}]}");
    CHECK(!fws_init(root, assets, "/nonexistent"));
    /* seeded downloads (what lord-fetch would have written) */
    snprintf(p, sizeof p, "%s/.cache/lordos/fwlists/ads.txt", root);
    put(p, "d tracker.example\ns ads.example\n");
    snprintf(p, sizeof p, "%s/.cache/lordos/fwlists/nets.txt", root);
    put(p, "n 203.0.113.0/24\n");
    char *j = fws_json();
    CHECK(j && strstr(j, "\"id\":\"ads\"") && strstr(j, "\"id\":\"nets\"") && !strstr(j, "bad id") && !strstr(j, "plain.example"));
    free(j);
    CHECK(fws_set("nope", 1) == -1);
    CHECK(!fws_set("ads", 1));
    CHECK(wait_job());
    const char *n1[] = { "x.ads.example" }, *n2[] = { "site.example" };
    CHECK(fws_match(n1, 1, 0, NULL) && !strcmp(fws_match(n1, 1, 0, NULL), "ads"));
    CHECK(!fws_match(n2, 1, 0, NULL));
    uint8_t a[16] = { 0 };
    inet_pton(AF_INET, "203.0.113.7", a);
    CHECK(!fws_match(NULL, 0, 4, a));                               /* nets is off */
    CHECK(!fws_set("nets", 1) && wait_job());
    CHECK(fws_match(NULL, 0, 4, a) && !strcmp(fws_match(NULL, 0, 4, a), "nets"));
    /* the compiled file lord-netperm reads */
    snprintf(p, sizeof p, "%s/.config/lordos/fwlists.txt", root);
    char *t = slurp(p);
    char err[64];
    FwLists *l = t ? fwl_load_text(t, strlen(t), err, sizeof err) : NULL;
    CHECK(l && fwl_domains(l) == 2 && fwl_nets(l) == 1);
    fwl_free(l);
    struct stat st;
    CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600);
    /* the config: survives, and a custom list */
    char id[32], e[64];
    CHECK(fws_add_custom("http://x.example/l", "domains", id, e, sizeof e) == -1 && !strcmp(e, "fw.list.badUrl"));
    CHECK(fws_add_custom("https://x.example/l", "csv", id, e, sizeof e) == -1 && !strcmp(e, "fw.list.badFormat"));
    CHECK(fws_add_custom("https://lists.example.org/ads.txt", "domains", id, e, sizeof e) == -1 && !strcmp(e, "fw.list.exists"));
    CHECK(!fws_add_custom("https://own.example/mine.txt", "hosts", id, e, sizeof e) && !strcmp(id, "custom-1"));
    wait_job();                         /* it tries to download it (no lord-fetch here: an error, no copy) */
    snprintf(p, sizeof p, "%s/.config/lordos/fwlists.json", root);
    t = slurp(p);
    CHECK(t && strstr(t, "\"enabled\":[\"ads\",\"nets\",\"custom-1\"]") && strstr(t, "own.example"));
    j = fws_json();
    CHECK(j && strstr(j, "\"error\":\"helper\""));
    free(j);
    CHECK(fws_remove_custom("ads") == -1);                          /* only custom ones */
    CHECK(!fws_remove_custom("custom-1") && wait_job());
    t = slurp(p);
    CHECK(t && !strstr(t, "own.example"));
    /* turning a list off removes it from the compiled set */
    CHECK(!fws_set("ads", 0) && wait_job());
    CHECK(!fws_match(n1, 1, 0, NULL));
}

/* lord-fetch refuses what it must, without any network (it never opens a socket itself) */
static void test_helper(const char *bin) {
    if (access(bin, X_OK)) { printf("fwsub-test: lord-fetch skipped: needs %s\n", bin); return; }
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    pid_t p = fork();
    if (p == 0) {
        dup2(sv[1], 3);
        char *argv[] = { "lord-fetch", NULL }, *envp[] = { NULL };
        execve(bin, argv, envp);
        _exit(127);
    }
    close(sv[1]);
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);              /* a stand-in for the connected socket */
    const char *reqs[] = { "{\"host\":\"../etc\",\"path\":\"/x\",\"format\":\"domains\"}", "{\"host\":\"a.example\",\"path\":\"x\",\"format\":\"domains\"}",
                           "{\"host\":\"a.example\",\"path\":\"/x\",\"format\":\"csv\"}", "not json", NULL };
    for (int i = 0; reqs[i]; i++) {
        int fd = dup(pair[0]);
        CHECK(!doc_send(sv[0], FQ_GET, (uint32_t)i + 1, reqs[i], strlen(reqs[i]), &fd, 1));
        close(fd);
        DocMsg m;
        struct pollfd pf = { sv[0], POLLIN, 0 };
        CHECK(poll(&pf, 1, 5000) == 1 && !doc_recv(sv[0], &m));
        CHECK(m.h.op == FR_ERR && m.h.id == (uint32_t)i + 1 && m.json && strstr(m.json, "bad-request"));
        doc_msg_free(&m);
    }
    /* a peer that isn't TLS: the handshake fails, it says so */
    int fd = dup(pair[0]);
    const char *ok = "{\"host\":\"a.example\",\"path\":\"/x\",\"format\":\"domains\"}";
    CHECK(!doc_send(sv[0], FQ_GET, 9, ok, strlen(ok), &fd, 1));
    close(fd);
    close(pair[0]);
    (void)!write(pair[1], "HTTP/1.1 200 OK\r\n\r\nnot tls\n", 27);
    close(pair[1]);
    DocMsg m;
    struct pollfd pf = { sv[0], POLLIN, 0 };
    CHECK(poll(&pf, 1, 35000) == 1 && !doc_recv(sv[0], &m));
    CHECK(m.h.op == FR_ERR && m.json && strstr(m.json, "\"tls\""));
    doc_msg_free(&m);
    doc_send(sv[0], FQ_END, 0, "{}", 2, NULL, 0);
    int st;
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(sv[0]);
    printf("fwsub-test: lord-fetch refuses bad requests and a peer without TLS: ok\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    char root[] = "/tmp/fwsub-test.XXXXXX";
    if (!mkdtemp(root)) return 2;
    unsetenv("LORDOS_SESSIOND");
    test_urls();
    test_subscriptions(root);
    test_helper(argc > 1 ? argv[1] : "build/lord-fetch");
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    (void)!system(cmd);
    printf("fwsub-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
