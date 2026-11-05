/* forti.h — reading openfortivpn's output and the single sign-on reply (pure functions, no I/O;
 * tests/forti_test.c). vpnproc.c drives openfortivpn with them. */
#ifndef LORD_NET_FORTI_H
#define LORD_NET_FORTI_H

#include <stddef.h>

/* What one line of openfortivpn's output means. */
enum {
    FO_NONE,            /* nothing to act on (or a line inside the certificate block) */
    FO_GATEWAY_OK,      /* "Connected to gateway.": TLS is up and the certificate was accepted */
    FO_TUNNEL_UP,       /* "Tunnel is up and running." */
    FO_AUTH_FAILED,     /* the user name / password (or the sign-in) was refused */
    FO_CERT,            /* the certificate block is complete: o->sha256, o->subject, o->issuer */
    FO_SAML_LISTEN,     /* "Listening for SAML login on port N" */
    FO_SAML_FAILED,     /* openfortivpn gave up waiting for the sign-in reply */
    FO_PORT_BUSY,       /* it couldn't listen on the sign-in port */
    FO_UNREACHABLE,     /* TCP connect to the gateway failed */
    FO_RESOLVE_FAILED,  /* the gateway's name didn't resolve */
    FO_TLS_FAILED,      /* the TLS handshake failed */
    FO_ALLOC_FAILED,    /* signed in, but the gateway gave no tunnel (no VPN for this account) */
    FO_CLIENT_CERT,     /* the client certificate or its key couldn't be loaded (a wrong pass phrase…) */
    FO_ERROR,           /* another "ERROR:" line: o->error has its text */
};

typedef struct {
    int block;          /* inside "Gateway certificate:": 1 header, 2 subject, 3 issuer, 4 digest */
    char sha256[65];    /* lowercase hex */
    char subject[200], issuer[200];
    char error[160];    /* FO_ERROR: the text after "ERROR:" */
} FortiOut;

int forti_out_line(FortiOut *o, const char *line);

/* The code line (errcode.h) for a kind that ends the attempt, or NULL. */
const char *forti_out_code(int kind);

/* A sha256 fingerprint as 64 hex digits, with or without ':' between the bytes, any case →
 * 64 lowercase digits (what openfortivpn compares). 0 ok, -1 invalid. */
int forti_hex_norm(const char *in, char out[65]);

/* The browser's request to the local sign-in listener: "GET /?id=<id>[&…] HTTP/1.x".
 * 0 = id copied (letters, digits and '-', like openfortivpn accepts), 1 = another request
 * (favicon…), -1 = malformed or an invalid id. */
int forti_saml_id(const char *req, char *id, size_t n);

/* ---- the connection's settings (forti_import / forti_run) ---- */

#define FORTI_MAX_GW 5                  /* the main server and up to 4 more */

typedef struct { char host[254]; int port; } FortiGw;

/* "host" or "host:port" (a name or IPv4 address; port 1..65535, defport if absent). 0 ok, -1 invalid. */
int forti_gw_parse(const char *s, int defport, FortiGw *g);

/* The servers to try, in order: host/port (port NULL = 443), then the comma-separated alt list
 * (NULL = none; spaces around each entry are ignored). Number of servers, or -1 if one is invalid or there are more than max. */
int forti_gw_list(const char *host, const char *port, const char *alt, FortiGw *g, int max);

/* One trusted-cert value: fingerprints separated by ',' or ' ', each "hex" (any server) or
 * "host@hex" (only that server; what LordOS saves when the person trusts a certificate).
 * Valid: 0; -1 otherwise. */
int forti_certs_valid(const char *val);

/* The same value with every fingerprint as 64 lowercase hex digits. 0 ok, -1 invalid or too long. */
int forti_certs_norm(const char *val, char *out, size_t n);

/* The fingerprints that apply to host from the n trusted-cert values (untagged ones, and those
 * tagged with that host, ignoring case), as a comma-separated list in out. Returns how many. */
int forti_certs_for(const char *const *vals, int n, const char *host, char *out, size_t on);

/* openfortivpn's config file. NULL/empty fields are left out; a value with a line break makes it
 * fail (-1). Returns the length written, or -1 if it doesn't fit. */
typedef struct {
    const char *host; int port;
    const char *user, *password;        /* NULL with single sign-on */
    const char *certs;                  /* comma-separated fingerprints (forti_certs_for) */
    const char *user_cert, *user_key, *pem_pass;    /* client certificate: file paths and the key's pass phrase */
} FortiConf;
int forti_conf_text(const FortiConf *c, char *out, size_t n);

#endif
