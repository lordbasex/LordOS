/* pgp.h — what's needed from OpenPGP (RFC 4880) to verify a repository's InRelease:
 * binary or armored keyrings (.gpg/.pgp/.asc), v4 RSA or EdDSA (Ed25519) signatures with SHA-256 or
 * SHA-512, and cleartext-signed messages. Our own code (no gpgv or sqv). */
#ifndef LORD_PKG_PGP_H
#define LORD_PKG_PGP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { PGP_RSA = 1, PGP_EDDSA = 22 };

typedef struct {
    uint8_t fpr[20];                    /* v4 fingerprint (SHA-1 of the key packet) */
    int alg;
    const uint8_t *n, *e;               /* RSA, point into the keyring buffer */
    size_t nlen, elen;
    uint8_t ed[32];                     /* EdDSA */
    int64_t created, expires;           /* expires 0: never */
    int revoked, can_sign, primary;
} PgpKey;

typedef struct {
    PgpKey *keys;
    int n, cap;
    uint8_t **bufs;                     /* data of each loaded keyring */
    int nbufs;
} PgpKeyring;

int  pgp_keyring_add_file(PgpKeyring *kr, const char *path);      /* 0 ok */
int  pgp_keyring_add_mem(PgpKeyring *kr, const char *data, size_t len);   /* binary or armored */
void pgp_keyring_free(PgpKeyring *kr);

/* Verifies a cleartext-signed message (InRelease). Returns the number of valid signatures from
 * keyring keys (> 0 = ok; 0 or less = bad, with the reason in err). An invalid signature from
 * a known key makes everything fail. *body receives the signed text (with \n, malloc). */
int  pgp_verify_clearsigned(const PgpKeyring *kr, const char *msg, size_t len, int64_t now,
                            char **body, size_t *body_len, char *err, size_t errlen);

/* An armored block ("-----BEGIN …") as binary packets in *out (malloc); returns its length or -1. */
long pgp_dearmor(const char *in, size_t len, uint8_t **out);

/* Base64 of the armor; returns bytes written or -1. */
long pgp_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap);

#endif
