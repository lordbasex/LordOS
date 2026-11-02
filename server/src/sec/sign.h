/* sign.h — signing of .lpk packages and the signed catalog (see docs/SIGNING.md).
 *
 * A signed .lpk carries, besides its files:
 *   contenido.json   { format: 1, id, version, signer: { name, key }, files: [{ path, size, sha256 }] }
 *   firma.json       { format: 1, alg: "ed25519", key, sig }   sig = Ed25519 over the bytes of contenido.json
 *
 * The catalog (catalog.json + catalog.sig, signed with an official key) carries the versions,
 * sizes and hashes of the packages, the keys of verified authors and the revocation list. */
#ifndef LORD_SEC_SIGN_H
#define LORD_SEC_SIGN_H

#include "../lpk.h"
#include <stddef.h>
#include <stdint.h>

#define SIG_CONTENTS "contenido.json"
#define SIG_SIGNATURE "firma.json"

typedef enum {
    LPK_UNSIGNED = 0,                   /* "Unsigned" */
    LPK_SIGNED = 1,                     /* "Signed by <name>" (unknown key) */
    LPK_VERIFIED = 2,                   /* "Verified author" (key from the catalog) */
    LPK_OFFICIAL = 3,                   /* "LordOS" (official key) */
} LpkTrust;

typedef struct {
    int has_contents, has_signature;
    LpkTrust trust;
    char key[65];                       /* public key in hex ("" if unsigned) */
    char signer[128];                   /* name: from the catalog if verified, otherwise the package's */
    char contents_sha[65];              /* SHA-256 of contenido.json ("" if none) */
    int revoked;
    char revoked_why[160];
} LpkSigInfo;

/* Verifies contenido.json and firma.json against pk's files (id and version from the manifest).
 * 0 = OK (also an unsigned package without contenido.json); -1 = the package was tampered with, is
 * badly signed or is missing files (err says why). */
int sig_verify_package(const Lpk *pk, const char *id, const char *version, LpkSigInfo *out, char *err, size_t errlen);

/* The same check on an unpacked folder, reading each file through one descriptor: nothing but root-owned
 * regular files and folders no one else can write, every file listed and none extra. keep (a listed path, or NULL):
 * *keep_fd gets the descriptor that was hashed, so what runs is what was checked. A tree must be signed. */
int sig_verify_tree(int dirfd, const char *id, const char *version, const char *keep, int *keep_fd, LpkSigInfo *out, char *err, size_t errlen);

/* Verifies a firma.json (alg, key, sig) over data. 0 on success; key (65) gets the key. */
int sig_verify_detached(const char *sig_json, size_t siglen, const void *data, size_t len, char key[65], char *err, size_t errlen);

/* Is it an official LordOS key? (keys.h) */
int sig_official(const char *keyhex);

/* firma.json text for data with seed `seed` (used by "Share my version"). */
int sig_make_detached(const uint8_t seed[32], const void *data, size_t len, char *out, size_t outlen);

/* ---- catalog ---- */

typedef struct { char key[65], name[96]; } CatAuthor;
typedef struct { char id[48], version[40], sha[65]; } CatRevoked;
typedef struct { char id[48], version[40], sha256[65], key[65]; double size; } CatPackage;

typedef struct {
    int ok;
    double serial;
    char date[32];
    char path[512];
    CatAuthor *authors;      int nauthors;
    char (*revoked_keys)[65]; int nrevoked_keys;
    CatRevoked *revoked;     int nrevoked;
    CatPackage *packages;    int npackages;
} Catalog;

/* Reads catalog.json with its catalog.sig (signed by an official key). 0 on success. */
int  catalog_parse(const char *json, size_t len, const char *sig, size_t siglen, Catalog *out, char *err, size_t errlen);
void catalog_free(Catalog *c);

/* The current catalog: the valid one with the highest serial between /usr/share/lordos/catalog.json and
 * ~/.local/share/lordos/catalog/catalog.json. Reread if the file changed. NULL if none. */
const Catalog *catalog_current(void);
const char *catalog_author(const char *keyhex);
/* Revoked? key, id, version and SHA-256 of contenido.json (any of them may be ""). */
int catalog_revoked(const char *key, const char *id, const char *version, const char *contents_sha, char *why, size_t n);

/* Level name for logs: "lordos", "verified", "signed", "unsigned". */
const char *sig_trust_name(LpkTrust t);

#endif
