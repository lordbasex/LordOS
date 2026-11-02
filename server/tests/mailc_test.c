/* mailc_test.c — axia-server's network policy for axia-maild (docs/MAIL.md): which endpoints the
 * connector accepts for configured accounts, and the public-address check that keeps remote
 * images (hosts chosen by whoever sent the mail) away from loopback, LAN and link-local. */
#include "mail/mailc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
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
    printf("mailc-test: %d/%d checks ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
