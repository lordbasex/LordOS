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

#endif
