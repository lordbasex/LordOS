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
    CHECK(forti_out_line(&x, "ERROR:  SSL_CTX_use_PrivateKey_file: error:1C800064:Provider routines::bad decrypt") == FO_CLIENT_CERT);
    CHECK(forti_out_line(&x, "ERROR:  SSL_CTX_check_private_key: error:05800074:x509 certificate routines::key values mismatch") == FO_CLIENT_CERT);
    CHECK(!strcmp(forti_out_code(FO_CLIENT_CERT), "vpn.fortiClientCert"));
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


    /* servers: "host[:port]", the main one first */
    FortiGw g[FORTI_MAX_GW];
    CHECK(forti_gw_parse("vpn.example.test", 443, &g[0]) == 0 && !strcmp(g[0].host, "vpn.example.test") && g[0].port == 443);
    CHECK(forti_gw_parse("vpn2.example.test:4443", 443, &g[0]) == 0 && !strcmp(g[0].host, "vpn2.example.test") && g[0].port == 4443);
    CHECK(forti_gw_parse("192.0.2.10:8443", 443, &g[0]) == 0 && g[0].port == 8443);
    CHECK(forti_gw_parse("vpn.example.test:0", 443, &g[0]) == -1);
    CHECK(forti_gw_parse("vpn.example.test:70000", 443, &g[0]) == -1);
    CHECK(forti_gw_parse("vpn.example.test:", 443, &g[0]) == -1);
    CHECK(forti_gw_parse("-x.example.test", 443, &g[0]) == -1);
    CHECK(forti_gw_parse("vpn example", 443, &g[0]) == -1);
    CHECK(forti_gw_parse("vpn.example.test;id", 443, &g[0]) == -1);
    CHECK(forti_gw_parse("", 443, &g[0]) == -1);
    CHECK(forti_gw_list("vpn.example.test", "4443", NULL, g, FORTI_MAX_GW) == 1 && g[0].port == 4443);
    CHECK(forti_gw_list("vpn.example.test", NULL, "", g, FORTI_MAX_GW) == 1 && g[0].port == 443);
    CHECK(forti_gw_list("vpn.example.test", "4443", "b.example.test, c.example.test:10001", g, FORTI_MAX_GW) == 3
          && !strcmp(g[1].host, "b.example.test") && g[1].port == 443 && !strcmp(g[2].host, "c.example.test") && g[2].port == 10001);
    CHECK(forti_gw_list("vpn.example.test", NULL, "a.test,b.test,c.test,d.test", g, FORTI_MAX_GW) == 5);
    CHECK(forti_gw_list("vpn.example.test", NULL, "a.test,b.test,c.test,d.test,e.test", g, FORTI_MAX_GW) == -1);   /* too many */
    CHECK(forti_gw_list("vpn.example.test", NULL, "a.test,bad host", g, FORTI_MAX_GW) == -1);
    CHECK(forti_gw_list("vpn.example.test", "x", NULL, g, FORTI_MAX_GW) == -1);
    CHECK(forti_gw_list(NULL, NULL, NULL, g, FORTI_MAX_GW) == -1);

    /* trusted certificates: untagged (any server) and per server */
    char out[1024];
    CHECK(!forti_certs_norm(FPC, out, sizeof out) && !strcmp(out, FP));
    CHECK(!forti_certs_norm("vpn.example.test@" FPC, out, sizeof out) && !strcmp(out, "vpn.example.test@" FP));
    CHECK(!forti_certs_norm(FP ", b.example.test@" FP, out, sizeof out) && !strcmp(out, FP ",b.example.test@" FP));
    CHECK(forti_certs_norm("@" FP, out, sizeof out) == -1);
    CHECK(forti_certs_norm("bad host@" FP, out, sizeof out) == -1);
    CHECK(forti_certs_norm("vpn.example.test@123", out, sizeof out) == -1);
    CHECK(forti_certs_norm("", out, sizeof out) == -1);
    CHECK(forti_certs_norm(FP, out, 10) == -1);                                    /* doesn't fit */
    CHECK(forti_certs_valid("a.example.test@" FP) == 0 && forti_certs_valid("nope") == -1);
#define FP2 "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"
#define FP3 "1111111111111111111111111111111111111111111111111111111111111111"
    const char *vals[] = { FP, "a.example.test@" FP2, "B.Example.Test@" FP3 ",a.example.test@" FP2 };
    CHECK(forti_certs_for(vals, 3, "a.example.test", out, sizeof out) == 2 && !strcmp(out, FP "," FP2));
    CHECK(forti_certs_for(vals, 3, "b.example.test", out, sizeof out) == 2 && !strcmp(out, FP "," FP3));   /* case of the name ignored */
    CHECK(forti_certs_for(vals, 3, "c.example.test", out, sizeof out) == 1 && !strcmp(out, FP));
    CHECK(forti_certs_for(vals + 1, 1, "c.example.test", out, sizeof out) == 0 && !out[0]);

    /* openfortivpn's config file */
    char conf[2048];
    FortiConf fc = { .host = "vpn.example.test", .port = 4443, .user = "user1", .password = "pa ss", .certs = FP "," FP2 };
    int len = forti_conf_text(&fc, conf, sizeof conf);
    CHECK(len > 0 && (size_t)len == strlen(conf));
    CHECK(strstr(conf, "host = vpn.example.test\nport = 4443\n") && strstr(conf, "username = user1\npassword = pa ss\n"));
    CHECK(strstr(conf, "trusted-cert = " FP "\ntrusted-cert = " FP2 "\n"));
    CHECK(strstr(conf, "set-routes = 0\nset-dns = 0\npppd-use-peerdns = 0\n") && !strstr(conf, "user-cert") && !strstr(conf, "pem-passphrase"));
    FortiConf fs = { .host = "vpn.example.test", .port = 443, .user_cert = "/etc/x/t.cert.pem", .user_key = "/etc/x/t.key.pem", .pem_pass = "k3y" };
    CHECK(forti_conf_text(&fs, conf, sizeof conf) > 0 && !strstr(conf, "username") && !strstr(conf, "password =") && !strstr(conf, "trusted-cert")
          && strstr(conf, "user-cert = /etc/x/t.cert.pem\nuser-key = /etc/x/t.key.pem\npem-passphrase = k3y\n"));
    FortiConf fe = { .host = "vpn.example.test", .port = 443, .user = "u", .password = "x\nset-routes = 1" };
    CHECK(forti_conf_text(&fe, conf, sizeof conf) == -1);                            /* no line can be injected */
    FortiConf fk = { .host = "vpn.example.test", .port = 443, .pem_pass = "a\rb" };
    CHECK(forti_conf_text(&fk, conf, sizeof conf) == -1);
    FortiConf fn = { .host = "", .port = 443 };
    CHECK(forti_conf_text(&fn, conf, sizeof conf) == -1);
    CHECK(forti_conf_text(&fc, conf, 40) == -1);                                     /* doesn't fit */

    printf("forti: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
