/* forti.c — openfortivpn's output and the single sign-on reply, as data (see forti.h).
 *
 * openfortivpn (1.2x) writes "ERROR:  ", "WARN:   " or "INFO:   " and a message per line. When it
 * can't verify the gateway's certificate (for example a server that sends its certificate without
 * the intermediate one, so the chain doesn't reach a known authority) it prints, all as ERROR:
 *
 *   Gateway certificate validation failed, and the certificate digest is not in the local whitelist. …
 *       --trusted-cert <64 hex>
 *   …
 *   Gateway certificate:
 *       subject:
 *           C=XX
 *           CN=vpn.example.test
 *       issuer:
 *           CN=Example CA
 *       sha256 digest:
 *           <64 hex>
 *
 * forti_out_line collects that block and reports FO_CERT at the digest, so LordOS can ask the
 * person to trust that certificate instead of showing the digest as an error. */
#include "forti.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

int forti_hex_norm(const char *in, char out[65]) {
    size_t o = 0;
    if (!in) return -1;
    for (const char *p = in; *p; p++) {
        if (*p == ':' && o && o % 2 == 0 && p[1] && p[1] != ':') continue;
        if (!isxdigit((unsigned char)*p) || o >= 64) return -1;
        out[o++] = (char)tolower((unsigned char)*p);
    }
    if (o != 64) return -1;
    out[64] = 0;
    return 0;
}

static void append(char *dst, size_t n, const char *s) {
    size_t l = strlen(dst);
    if (l + 3 >= n) return;
    snprintf(dst + l, n - l, "%s%s", l ? ", " : "", s);
}

int forti_out_line(FortiOut *o, const char *line) {
    /* without colour codes (openfortivpn only uses them on a terminal, but just in case) */
    char clean[256];
    size_t c = 0;
    for (const char *p = line; *p && c + 1 < sizeof clean; p++) {
        if (*p == 0x1b) { const char *m = strchr(p, 'm'); if (!m) break; p = m; continue; }
        clean[c++] = *p;
    }
    clean[c] = 0;
    const char *l = clean;
    int err = 0;
    if (starts(l, "ERROR:")) { err = 1; l += 6; }
    else if (starts(l, "WARN:")) l += 5;
    else if (starts(l, "INFO:")) l += 5;
    else if (starts(l, "DEBUG:")) l += 6;
    while (*l == ' ' || *l == '\t') l++;
    char text[256];
    snprintf(text, sizeof text, "%s", l);
    size_t tl = strlen(text);
    while (tl && isspace((unsigned char)text[tl - 1])) text[--tl] = 0;

    /* the certificate block */
    if (err && starts(text, "Gateway certificate validation failed")) { o->block = 1; o->subject[0] = o->issuer[0] = 0; return FO_NONE; }
    if (err && starts(text, "--trusted-cert ")) { forti_hex_norm(text + 15, o->sha256); return FO_NONE; }
    if (err && (starts(text, "or add this line") || starts(text, "trusted-cert = "))) return FO_NONE;
    if (err && !strcmp(text, "Gateway certificate:")) { o->block = 1; o->subject[0] = o->issuer[0] = 0; return FO_NONE; }
    if (err && o->block) {
        if (!strcmp(text, "subject:")) { o->block = 2; return FO_NONE; }
        if (!strcmp(text, "issuer:")) { o->block = 3; return FO_NONE; }
        if (!strcmp(text, "sha256 digest:")) { o->block = 4; return FO_NONE; }
        if (o->block == 2) { append(o->subject, sizeof o->subject, text); return FO_NONE; }
        if (o->block == 3) { append(o->issuer, sizeof o->issuer, text); return FO_NONE; }
        if (o->block == 4) {
            o->block = 0;
            char h[65];
            if (!forti_hex_norm(text, h)) memcpy(o->sha256, h, sizeof h);
            return o->sha256[0] ? FO_CERT : FO_ERROR;
        }
    }
    o->block = 0;

    if (starts(text, "Connected to gateway")) return FO_GATEWAY_OK;
    if (strstr(text, "Tunnel is up and running")) return FO_TUNNEL_UP;
    if (starts(text, "Listening for SAML login on port")) return FO_SAML_LISTEN;
    if (!err) return FO_NONE;
    if (starts(text, "Could not authenticate") || strstr(text, "authentication failed") || strstr(text, "Invalid credentials")) return FO_AUTH_FAILED;
    if (starts(text, "VPN allocation request failed")) return FO_ALLOC_FAILED;
    if (starts(text, "Failed to bind socket to port")) return FO_PORT_BUSY;
    if (starts(text, "Finally failed to retrieve SAML") || starts(text, "Failed to retrieve SAML") || starts(text, "Failed to receive SAML")) return FO_SAML_FAILED;
    if (starts(text, "connect:")) return FO_UNREACHABLE;
    if (starts(text, "getaddrinfo:")) return FO_RESOLVE_FAILED;
    if (starts(text, "SSL_connect:")) return FO_TLS_FAILED;
    if (starts(text, "SSL_CTX_use_certificate_chain_file:") || starts(text, "SSL_CTX_use_PrivateKey_file:") || starts(text, "SSL_CTX_check_private_key:"))
        return FO_CLIENT_CERT;
    snprintf(o->error, sizeof o->error, "%.150s", text);
    return FO_ERROR;
}

const char *forti_out_code(int kind) {
    switch (kind) {
    case FO_SAML_FAILED: return "vpn.fortiSsoFailed";
    case FO_PORT_BUSY: return "vpn.fortiSsoPortBusy";
    case FO_UNREACHABLE: return "vpn.fortiUnreachable";
    case FO_RESOLVE_FAILED: return "vpn.serverUnresolved";
    case FO_TLS_FAILED: return "vpn.fortiTlsFailed";
    case FO_ALLOC_FAILED: return "vpn.fortiNoTunnel";
    case FO_CLIENT_CERT: return "vpn.fortiClientCert";
    default: return NULL;
    }
}

int forti_saml_id(const char *req, char *id, size_t n) {
    if (!starts(req, "GET ")) return -1;
    const char *p = req + 4;
    if (!starts(p, "/?id=")) {
        /* another path (the browser asks for /favicon.ico): not the reply */
        return *p == '/' ? 1 : -1;
    }
    p += 5;
    size_t k = 0;
    while (p[k] && (isalnum((unsigned char)p[k]) || p[k] == '-')) k++;
    if (!k || k >= n) return -1;
    if (p[k] != ' ' && p[k] != '&') return -1;
    memcpy(id, p, k);
    id[k] = 0;
    return 0;
}

/* ---- the connection's settings ---- */

static int host_ok(const char *h) {
    size_t n = strlen(h);
    if (!n || n > 253 || h[0] == '-' || h[0] == '.') return 0;
    for (const char *p = h; *p; p++) if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return 0;
    return 1;
}

static int port_val(const char *p) {
    if (!*p || strlen(p) > 5 || strspn(p, "0123456789") != strlen(p)) return -1;
    int v = atoi(p);
    return v >= 1 && v <= 65535 ? v : -1;
}

int forti_gw_parse(const char *s, int defport, FortiGw *g) {
    char t[300];
    if (!s || strlen(s) >= sizeof t) return -1;
    snprintf(t, sizeof t, "%s", s);
    char *c = strchr(t, ':');
    int port = defport;
    if (c) {
        *c = 0;
        if ((port = port_val(c + 1)) < 0) return -1;
    }
    if (!host_ok(t)) return -1;
    memcpy(g->host, t, strlen(t) + 1);              /* host_ok: at most 253 characters */
    g->port = port;
    return 0;
}

int forti_gw_list(const char *host, const char *port, const char *alt, FortiGw *g, int max) {
    int n = 0, p = 443;
    if (port && *port && (p = port_val(port)) < 0) return -1;
    if (!host || !host_ok(host) || max < 1) return -1;
    snprintf(g[0].host, sizeof g[0].host, "%s", host);
    g[0].port = p;
    n = 1;
    if (!alt || !*alt) return n;
    char copy[512];
    if (strlen(alt) >= sizeof copy) return -1;
    snprintf(copy, sizeof copy, "%s", alt);
    char *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ') t++;
        size_t l = strlen(t);
        while (l && t[l - 1] == ' ') t[--l] = 0;
        if (!*t) continue;
        if (n >= max || forti_gw_parse(t, 443, &g[n])) return -1;
        n++;
    }
    return n;
}

/* one token: "hex" or "host@hex" */
static int cert_token(const char *tok, char *host, size_t hn, char hex[65]) {
    const char *at = strchr(tok, '@');
    host[0] = 0;
    if (at) {
        size_t k = (size_t)(at - tok);
        if (!k || k >= hn) return -1;
        memcpy(host, tok, k);
        host[k] = 0;
        if (!host_ok(host)) return -1;
        tok = at + 1;
    }
    return forti_hex_norm(tok, hex);
}

int forti_certs_norm(const char *val, char *out, size_t n) {
    char copy[512];
    if (!val || strlen(val) >= sizeof copy) return -1;
    snprintf(copy, sizeof copy, "%s", val);
    out[0] = 0;
    int k = 0;
    char *save = NULL;
    for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        char host[254], hex[65];
        if (cert_token(t, host, sizeof host, hex)) return -1;
        size_t need = strlen(out) + strlen(host) + 1 + 64 + 2;
        if (need > n) return -1;
        snprintf(out + strlen(out), n - strlen(out), "%s%s%s%s", k++ ? "," : "", host, host[0] ? "@" : "", hex);
    }
    return k ? 0 : -1;
}

int forti_certs_valid(const char *val) {
    char tmp[1024];
    return forti_certs_norm(val, tmp, sizeof tmp);
}

int forti_certs_for(const char *const *vals, int n, const char *host, char *out, size_t on) {
    int k = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        char copy[512];
        snprintf(copy, sizeof copy, "%s", vals[i]);
        char *save = NULL;
        for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
            char h[254], hex[65];
            if (cert_token(t, h, sizeof h, hex)) continue;
            if (h[0] && strcasecmp(h, host)) continue;
            if (strstr(out, hex) || strlen(out) + 66 > on) continue;
            snprintf(out + strlen(out), on - strlen(out), "%s%s", k++ ? "," : "", hex);
        }
    }
    return k;
}

static int line_ok(const char *v) { return !v || !strpbrk(v, "\r\n"); }

int forti_conf_text(const FortiConf *c, char *out, size_t n) {
    if (!line_ok(c->host) || !line_ok(c->user) || !line_ok(c->password) || !line_ok(c->certs) || !line_ok(c->user_cert)
        || !line_ok(c->user_key) || !line_ok(c->pem_pass) || !c->host || !*c->host)
        return -1;
    size_t o = 0;
#define PUT(...) do { int w_ = snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__); if (w_ < 0 || o + (size_t)w_ >= n) return -1; o += (size_t)w_; } while (0)
    PUT("# Generated by LordOS (do not edit)\nhost = %s\nport = %d\n", c->host, c->port);
    if (c->user && *c->user) PUT("username = %s\n", c->user);
    if (c->password && *c->password) PUT("password = %s\n", c->password);
    if (c->certs && *c->certs) {
        char copy[1024];
        snprintf(copy, sizeof copy, "%s", c->certs);
        char *save = NULL;
        for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) PUT("trusted-cert = %s\n", t);
    }
    if (c->user_cert && *c->user_cert) PUT("user-cert = %s\n", c->user_cert);
    if (c->user_key && *c->user_key) PUT("user-key = %s\n", c->user_key);
    if (c->pem_pass && *c->pem_pass) PUT("pem-passphrase = %s\n", c->pem_pass);
    /* routes and DNS are LordOS's (openfortivpn's set-routes would change the default route) */
    PUT("set-routes = 0\nset-dns = 0\npppd-use-peerdns = 0\n");
#undef PUT
    return (int)o;
}
