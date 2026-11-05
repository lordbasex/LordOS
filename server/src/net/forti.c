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
#include <string.h>

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
