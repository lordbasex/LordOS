/* mailc_test.c — lord-server's network policy for lord-maild (docs/MAIL.md): which endpoints the
 * connector accepts for configured accounts, and the public-address check that keeps remote
 * images (hosts chosen by whoever sent the mail) away from loopback, LAN and link-local. */
#include "mail/mailc.h"
#include "mail/html.h"
#include "doc/proto.h"
#include "sec/json.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int pub(const char *ip) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    if (strchr(ip, ':')) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, ip, &s6->sin6_addr) != 1) return -1;
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        if (inet_pton(AF_INET, ip, &s4->sin_addr) != 1) return -1;
    }
    return mail_addr_public((struct sockaddr *)&ss);
}

/* The event queue (issue #18): lord-maild's replies are re-validated before JS sees them. A message
 * view nests as deep as the HTML tree (HTML_JSON_DEPTH), broken JSON becomes an error the waiter
 * sees, and a reply that would overflow the queue becomes "too-big" without spoiling the queue. */
static void test_events(void) {
    JBuf q;
    jb_init(&q);
    /* nested like a newsletter: {"html":{"blocks":[{"t":"tbl","c":[ … ]}]}} */
    JBuf deep;
    jb_init(&deep);
    jb_raw(&deep, "{\"subject\":\"x\",\"html\":{\"v\":1,\"blocks\":[");
    int frames = HTML_MAX_DEPTH - 1;
    for (int i = 0; i < frames; i++) jb_raw(&deep, "{\"t\":\"blk\",\"s\":{\"_\":0,\"p\":[1,2,3,4]},\"c\":[");
    jb_raw(&deep, "{\"t\":\"x\",\"x\":\"deep\",\"s\":{\"_\":0,\"b\":1}}");
    for (int i = 0; i < frames; i++) jb_raw(&deep, "]}");
    jb_raw(&deep, "]}}");
    mail_ev_append(&q, 7, "done", deep.p, deep.n);
    CHECK(strstr(q.p, "\"error\"") == NULL && strstr(q.p, "deep") != NULL);
    mail_ev_append(&q, 8, "done", "{\"a\":1", 6);                 /* broken */
    mail_ev_append(&q, 9, "done", "{\"a\":\"\\u0000\"}", 14);      /* refused by json_parse */
    mail_ev_append(&q, 10, "done", "{\"a\":\"b\0c\"}", 11);        /* a raw NUL */
    mail_ev_append(&q, 0, "evt", "{\"ev\":\"ready\"}", 14);
    /* the whole queue, as JS gets it, is valid JSON (QuickJS has no 32-level limit; ours does) */
    char *arr = malloc(q.n + 3);
    arr[0] = '[';
    memcpy(arr + 1, q.p, q.n);
    arr[q.n + 1] = ']';
    arr[q.n + 2] = 0;
    JDoc d;
    CHECK(!json_parse_depth(arr, q.n + 2, &d, NULL, 0, HTML_JSON_DEPTH + 16));
    if (d.root) {
        CHECK(d.root->len == 5);
        int bad = 0;
        for (const JVal *e = d.root->kid; e; e = e->next) {
            const char *err = json_str(json_get(e, "data"), "error");
            if (err && !strcmp(err, "bad-json")) bad++;
        }
        CHECK(bad == 3);
        json_free(&d);
    }
    free(arr);

    /* too big for the queue: one small error event in its place, and the queue keeps working */
    size_t before = q.n;
    size_t big = (size_t)DOC_JSON_MAX + 64;
    char *huge = malloc(big);
    if (huge) {
        memset(huge, ' ', big);
        huge[0] = '"';
        huge[big - 1] = '"';
        mail_ev_append(&q, 11, "done", huge, big);
        CHECK(!q.oom);
        CHECK(strstr(q.p + before, "{\"id\":11,\"type\":\"done\",\"data\":{\"error\":\"too-big\"}}") != NULL);
        CHECK(q.n < before + 200);
        mail_ev_append(&q, 12, "done", "{\"ok\":true}", 11);
        CHECK(strstr(q.p, "\"id\":12") != NULL);
        free(huge);
    }
    jb_free(&deep);
    jb_free(&q);
}

int main(void) {
    static const char *priv[] = {
        "0.0.0.0", "0.1.2.3", "10.0.0.1", "10.255.255.255", "127.0.0.1", "127.8.9.10", "100.64.0.1", "100.127.255.254",
        "169.254.169.254", "169.254.0.1", "172.16.0.1", "172.31.255.255", "192.168.0.1", "192.168.255.255", "192.0.0.8",
        "198.18.0.1", "224.0.0.1", "239.255.255.250", "240.0.0.1", "255.255.255.255",
        "::", "::1", "fe80::1", "febf::1", "fec0::1", "fc00::1", "fd12:3456::1", "ff02::1", "ff05::2",
        "::ffff:127.0.0.1", "::ffff:10.1.2.3", "::ffff:169.254.169.254", "::ffff:192.168.1.1", "::127.0.0.1",
        "64:ff9b::a00:1", "64:ff9b::7f00:1", "64:ff9b:1::1", "2001:db8::1", "2002:c0a8:0101::1", "2002:7f00:1::1",
    };
    static const char *pubs[] = {
        "1.1.1.1", "8.8.8.8", "9.255.255.255", "11.0.0.1", "100.63.255.255", "100.128.0.1", "172.15.255.255", "172.32.0.1",
        "169.253.1.1", "192.167.1.1", "192.169.1.1", "223.255.255.254", "93.184.216.34",
        "2606:4700::1111", "2a00:1450:4001::200e", "::ffff:8.8.8.8", "64:ff9b::808:808", "2002:0808:0808::1",
    };
    for (size_t i = 0; i < sizeof priv / sizeof priv[0]; i++) { int r = pub(priv[i]); if (r != 0) fprintf(stderr, "  %s → %d\n", priv[i], r); CHECK(r == 0); }
    for (size_t i = 0; i < sizeof pubs / sizeof pubs[0]; i++) { int r = pub(pubs[i]); if (r != 1) fprintf(stderr, "  %s → %d\n", pubs[i], r); CHECK(r == 1); }

    const char *cfg = "{\"accounts\":[{\"id\":\"a\",\"kind\":\"mail\",\"imap\":{\"host\":\"IMAP.Example.com\",\"port\":993},"
                      "\"smtp\":{\"host\":\"smtp.example.com\",\"port\":587}},"
                      "{\"id\":\"d\",\"kind\":\"dav\",\"url\":\"https://caldav.icloud.com/\"}]}";
    size_t n = strlen(cfg);
    CHECK(mail_endpoint_allowed(cfg, n, "imap.example.com", 993));
    CHECK(!mail_endpoint_allowed(cfg, n, "imap.example.com", 143));
    CHECK(mail_endpoint_allowed(cfg, n, "smtp.example.com", 587));
    CHECK(!mail_endpoint_allowed(cfg, n, "evil.example.com", 993));
    CHECK(!mail_endpoint_allowed(cfg, n, "example.com", 993));
    CHECK(mail_endpoint_allowed(cfg, n, "caldav.icloud.com", 443));
    CHECK(mail_endpoint_allowed(cfg, n, "p42-caldav.icloud.com", 443));     /* discovery redirect */
    CHECK(!mail_endpoint_allowed(cfg, n, "p42-caldav.icloud.com", 80));
    CHECK(!mail_endpoint_allowed(cfg, n, "icloud.com.evil.net", 443));
    CHECK(!mail_endpoint_allowed(cfg, n, "evilicloud.com", 443));
    CHECK(!mail_endpoint_allowed("not json", 8, "imap.example.com", 993));
    test_events();
    printf("mailc-test: %d/%d checks ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
