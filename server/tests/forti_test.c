/* forti_test.c — openfortivpn's output → what LordOS does (src/net/forti.c). All names and
 * fingerprints here are invented. */
#include "../src/net/forti.h"

#include <stdio.h>
#include <string.h>

static int bad;
#define CHECK(x) do { if (!(x)) { printf("FAIL forti:%d: %s\n", __LINE__, #x); bad++; } } while (0)

#define FP  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define FPC "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF"

static int feed(FortiOut *o, const char *const *lines, int *last_kind) {
    int cert = 0;
    for (int i = 0; lines[i]; i++) {
        int k = forti_out_line(o, lines[i]);
        if (k == FO_CERT) cert++;
        if (k != FO_NONE) *last_kind = k;
    }
    return cert;
}

int main(void) {
    char h[65];
    /* fingerprints */
    CHECK(!forti_hex_norm(FP, h) && !strcmp(h, FP));
    CHECK(!forti_hex_norm(FPC, h) && !strcmp(h, FP));
    CHECK(!forti_hex_norm("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF", h) && !strcmp(h, FP));
    CHECK(forti_hex_norm("0123", h) == -1);
    CHECK(forti_hex_norm(FP "00", h) == -1);
    CHECK(forti_hex_norm("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", h) == -1);
    CHECK(forti_hex_norm("01::23", h) == -1);
    CHECK(forti_hex_norm(":" FP, h) == -1);
    CHECK(forti_hex_norm(NULL, h) == -1);

    /* the certificate block of openfortivpn 1.2x (a server without its intermediate certificate) */
    static const char *const cert[] = {
        "INFO:   Resolving gateway host ip",
        "ERROR:  Gateway certificate validation failed, and the certificate digest is not in the local whitelist. If you trust it, rerun with:",
        "ERROR:      --trusted-cert " FP,
        "ERROR:  or add this line to your configuration file:",
        "ERROR:      trusted-cert = " FP,
        "ERROR:  Gateway certificate:",
        "ERROR:      subject:",
        "ERROR:          C=XX",
        "ERROR:          O=Example Org",
        "ERROR:          CN=*.example.test",
        "ERROR:      issuer:",
        "ERROR:          C=XX",
        "ERROR:          CN=Example Server CA R1",
        "ERROR:      sha256 digest:",
        "ERROR:          " FP,
        NULL,
    };
    FortiOut o = {0};
    int last = FO_NONE;
    CHECK(feed(&o, cert, &last) == 1 && last == FO_CERT);
    CHECK(!strcmp(o.sha256, FP));
    CHECK(!strcmp(o.subject, "C=XX, O=Example Org, CN=*.example.test"));
    CHECK(!strcmp(o.issuer, "C=XX, CN=Example Server CA R1"));
    CHECK(o.block == 0);
    /* after the block, an ordinary error is an error again */
    CHECK(forti_out_line(&o, "ERROR:  Something else went wrong") == FO_ERROR && !strcmp(o.error, "Something else went wrong"));

    /* the same with colour codes (a terminal) */
    FortiOut oc = {0};
    CHECK(forti_out_line(&oc, "\033[0;31mERROR:  Gateway certificate:\033[0m") == FO_NONE && oc.block == 1);
    CHECK(forti_out_line(&oc, "\033[0;31mERROR:      sha256 digest:\033[0m") == FO_NONE && oc.block == 4);
    CHECK(forti_out_line(&oc, "\033[0;31mERROR:          " FP "\033[0m") == FO_CERT && !strcmp(oc.sha256, FP));

    /* the other lines */
    FortiOut x = {0};
    CHECK(forti_out_line(&x, "INFO:   Connected to gateway.") == FO_GATEWAY_OK);
    CHECK(forti_out_line(&x, "INFO:   Tunnel is up and running.") == FO_TUNNEL_UP);
    CHECK(forti_out_line(&x, "INFO:   Listening for SAML login on port 8020") == FO_SAML_LISTEN);
    CHECK(forti_out_line(&x, "INFO:   Authenticate at 'https://vpn.example.test:4443/remote/saml/start?redirect=1'") == FO_NONE);
    CHECK(forti_out_line(&x, "ERROR:  Could not authenticate to gateway. Please check the password, client certificate, etc.") == FO_AUTH_FAILED);
    CHECK(forti_out_line(&x, "ERROR:  VPN allocation request failed (Permission denied).") == FO_ALLOC_FAILED);
    CHECK(forti_out_line(&x, "ERROR:  Failed to bind socket to port 8020") == FO_PORT_BUSY);
    CHECK(forti_out_line(&x, "ERROR:  Finally failed to retrieve SAML authentication token") == FO_SAML_FAILED);
    CHECK(forti_out_line(&x, "ERROR:  Failed to retrieve SAML cookie from HTTP") == FO_SAML_FAILED);
    CHECK(forti_out_line(&x, "ERROR:  connect: Connection refused") == FO_UNREACHABLE);
    CHECK(forti_out_line(&x, "ERROR:  connect: Connection timed out") == FO_UNREACHABLE);
    CHECK(forti_out_line(&x, "ERROR:  getaddrinfo: Name or service not known") == FO_RESOLVE_FAILED);
    CHECK(forti_out_line(&x, "ERROR:  SSL_connect: error:0A000410:SSL routines::sslv3 alert handshake failure") == FO_TLS_FAILED);
    CHECK(forti_out_line(&x, "WARN:   Bad port in config file: \"x\".") == FO_NONE);
    CHECK(forti_out_line(&x, "DEBUG:  whatever") == FO_NONE);
    CHECK(forti_out_line(&x, "") == FO_NONE);
    CHECK(!strcmp(forti_out_code(FO_UNREACHABLE), "vpn.fortiUnreachable"));
    CHECK(!strcmp(forti_out_code(FO_SAML_FAILED), "vpn.fortiSsoFailed"));
    CHECK(!strcmp(forti_out_code(FO_PORT_BUSY), "vpn.fortiSsoPortBusy"));
    CHECK(!strcmp(forti_out_code(FO_TLS_FAILED), "vpn.fortiTlsFailed"));
    CHECK(!strcmp(forti_out_code(FO_ALLOC_FAILED), "vpn.fortiNoTunnel"));
    CHECK(forti_out_code(FO_TUNNEL_UP) == NULL && forti_out_code(FO_CERT) == NULL && forti_out_code(FO_ERROR) == NULL);

    /* a digest line that isn't a fingerprint: an error, not a certificate */
    FortiOut y = {0};
    forti_out_line(&y, "ERROR:  Gateway certificate:");
    forti_out_line(&y, "ERROR:      sha256 digest:");
    CHECK(forti_out_line(&y, "ERROR:          not-a-digest") == FO_ERROR);

    /* the browser's reply to the local sign-in listener */
    char id[64];
    CHECK(forti_saml_id("GET /?id=abc-123DEF HTTP/1.1\r\nHost: 127.0.0.1:8020\r\n\r\n", id, sizeof id) == 0 && !strcmp(id, "abc-123DEF"));
    CHECK(forti_saml_id("GET /?id=abc123&x=1 HTTP/1.1\r\n", id, sizeof id) == 0 && !strcmp(id, "abc123"));
    CHECK(forti_saml_id("GET /favicon.ico HTTP/1.1\r\n", id, sizeof id) == 1);
    CHECK(forti_saml_id("GET /?id= HTTP/1.1\r\n", id, sizeof id) == -1);
    CHECK(forti_saml_id("GET /?id=abc;rm HTTP/1.1\r\n", id, sizeof id) == -1);
    CHECK(forti_saml_id("GET /?id=abc%20 HTTP/1.1\r\n", id, sizeof id) == -1);
    CHECK(forti_saml_id("GET /?id=abc", id, sizeof id) == -1);                     /* cut short */
    CHECK(forti_saml_id("POST /?id=abc HTTP/1.1\r\n", id, sizeof id) == -1);
    CHECK(forti_saml_id("GET http://x/?id=abc HTTP/1.1\r\n", id, sizeof id) == -1);
    char small[4];
    CHECK(forti_saml_id("GET /?id=abcdef HTTP/1.1\r\n", small, sizeof small) == -1);   /* too long for the buffer */

    printf("forti: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
