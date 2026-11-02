/* codesig.h — verified system code (0.8.2, "System apps immutable", phase 1; see docs/SIGNING.md,
 * "Verified system code").
 *
 * The JavaScript that LordOS runs as its own code — the theme bundles (the shell, the SDK, the
 * runtime and every native app), the login screen, the app host of isolated apps and the system
 * apps in assets/sysapps — is listed with its size and SHA-256 in <root>/code-manifest.json, and
 * that file is signed with the LordOS release key (Ed25519) in <root>/code-manifest.sig (base64 of
 * the 64-byte signature over the exact bytes of the manifest). <root> is the folder that holds
 * assets/ and themes/ (/usr/lib/lordos when installed).
 *
 * axia-server checks the signature once per process, with the public key compiled in, and the hash
 * of every file before it runs it: codesig_load() reads the file from one descriptor (opened under
 * <root>, never following a link out of it), hashes exactly the bytes it read and returns those same
 * bytes, so what is checked is what runs (no TOCTOU).
 *
 * States:
 *   VERIFIED     signed manifest, signature good: only listed files with the right hash load.
 *   DEVELOPMENT  unsigned manifest (or none) in a development build: hashes are still checked when
 *                there is a manifest; About shows "Development build". A release build
 *                (make RELEASE=1, LORD_CODE_RELEASE) never accepts this state.
 *   INVALID      bad signature, broken manifest, or an unsigned/missing one in a release build:
 *                nothing loads and the session shows the recovery screen. */
#ifndef LORD_SEC_CODESIG_H
#define LORD_SEC_CODESIG_H

#include <stddef.h>
#include <stdint.h>

#define CODE_MANIFEST     "code-manifest.json"
#define CODE_SIGNATURE    "code-manifest.sig"
#define CODE_KIND         "lordos-system-code"
#define CODE_MANIFEST_MAX (1u << 20)        /* bytes */
#define CODE_FILES_MAX    4096
#define CODE_FILE_MAX     (64u << 20)       /* one bundle */
#define CODE_PATH_MAX     200

typedef enum { CODE_UNCHECKED, CODE_VERIFIED, CODE_DEVELOPMENT, CODE_INVALID } CodeState;

typedef struct {
    char path[CODE_PATH_MAX + 1];
    uint64_t size;
    uint8_t sha256[32];
} CodeFile;

typedef struct {
    CodeFile *files;            /* sorted by path (the manifest must already be sorted) */
    int nfiles;
    char version[32];
    int signed_ok;              /* 1 if the signature was checked and is good */
} CodeManifest;

/* ---- pure functions (server/tests/codesig_test.c) ---- */

/* Parses and checks a manifest. sig/siglen: the text of code-manifest.sig, or NULL when there is none
 * (then signed_ok = 0). With a signature, it must verify with pub over json[0..len). 0 on success;
 * err gets the reason. Strict: format 1, kind "lordos-system-code", relative paths of
 * [A-Za-z0-9._+-/] without "." or ".." parts, strictly sorted, sizes ≤ CODE_FILE_MAX, 64 lowercase
 * hex digits, at most CODE_FILES_MAX files. */
int  codesig_parse(const char *json, size_t len, const char *sig, size_t siglen, const uint8_t pub[32],
                   CodeManifest *out, char *err, size_t errlen);
void codesig_free(CodeManifest *m);
const CodeFile *codesig_find(const CodeManifest *m, const char *rel);
/* 0 if buf is exactly the listed file rel; otherwise why ("not listed", "size", "hash"). */
int  codesig_check_buf(const CodeManifest *m, const char *rel, const void *buf, size_t len, char *why, size_t n);
/* Base64 (standard alphabet, padding, optional trailing newline) of exactly 64 bytes. 0 ok. */
int  codesig_sig_decode(const char *text, size_t len, uint8_t out[64]);

/* ---- the process's state ---- */

/* Loads <assets_dir>/../code-manifest.json once (later calls do nothing). Returns the state. */
CodeState codesig_init(const char *assets_dir);
/* For tests and --check-code: the same with an explicit root, key and policy (release = 1 refuses
 * DEVELOPMENT). Replaces any previous state. */
CodeState codesig_init_root(const char *root, const uint8_t pub[32], int release);
CodeState codesig_state(void);
const char *codesig_state_name(CodeState s);     /* "verified", "development", "invalid", "unchecked" */
const char *codesig_root(void);
const char *codesig_reason(void);                /* why INVALID (or a DEVELOPMENT detail) */
const CodeManifest *codesig_manifest(void);      /* NULL without a manifest */
int codesig_release_build(void);                 /* 1 if built with RELEASE=1 */
double codesig_ms(void);                         /* time spent verifying in this process */

/* Reads one file of LordOS's code. On success returns a malloc'd, NUL-terminated buffer with exactly
 * the bytes that were hashed (len set). On refusal returns NULL, records the refusal (see below),
 * logs it and fills why. max: largest size accepted. */
char *codesig_load(const char *path, size_t max, size_t *len, char *why, size_t whylen);

/* Refusals of this process, for the "This LordOS component was modified" notification. */
typedef struct { char path[CODE_PATH_MAX + 1]; char reason[96]; int reported; } CodeRefusal;
int codesig_refused_count(void);
CodeRefusal *codesig_refused(int i);

/* The LordOS release key (tools/release/lordos-release.pub.pem), raw 32 bytes. */
extern const uint8_t codesig_release_key[32];
/* The key this build trusts: the release key, or a test build's CODE_PUBKEY. */
const uint8_t *codesig_key(void);

#endif
