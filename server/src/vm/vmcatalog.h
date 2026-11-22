/* vmcatalog.h — the systems the New Machine assistant downloads (assets/machines/systems.json; docs/VIRTUALIZATION.md
 * 4.1 and 3.9, "The Linux download cards"), as the admin helper reads them: pure functions over bytes, apart for their
 * test (tests/vmcatalog_test.c). No network and no files here.
 *
 * The helper never takes a URL from JS: an entry is named by its id and an architecture, and every URL is the entry's
 * folder plus a plain file name that must match the entry's pattern. A downloaded image is kept only when its SHA-256
 * is the one the maker's list (fetched by the helper itself) gives for that very name. */
#ifndef LORD_VM_CATALOG_H
#define LORD_VM_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#define VMCAT_URL 400
#define VMCAT_RE  160
#define VMCAT_FPRS 4
#define VMCAT_PINS 48

/* VMCAT_SIG_LORDOS: LordOS's own releases ("signature": { "type": "lordos-release", "name": "SHA256SUMS.sig" }, no "keys"
 * or "fingerprints"): the detached SHA256SUMS.sig of docs/SIGNING.md, "Release files", checked with the LordOS release key
 * compiled into LordOS (codesig_release_key) by the LordOS Imager's own check (imager/imgverify.h, img_sig_verify) */
enum { VMCAT_SIG_DETACHED = 1, VMCAT_SIG_CLEAR = 2, VMCAT_SIG_LORDOS = 3 };

typedef struct {
    char dir[VMCAT_URL];          /* https://…/ : the maker's folder for the release (its list, its listing) */
    char file_dir[VMCAT_URL];     /* where the image itself comes from (dir unless the entry says fileDir) */
    char sums[96];                /* the list's fixed name, or "" when it is found in dir's listing */
    char sums_re[VMCAT_RE];       /* that name's pattern ("" with a fixed name) */
    char file_re[VMCAT_RE];       /* the image's name, an extended regular expression */
    unsigned long long max_bytes; /* the most a download of this entry may be */
    /* how the maker signs the list (the system's "signature", "keys", "fingerprints") */
    int sig_type;                 /* VMCAT_SIG_DETACHED (a file in dir), VMCAT_SIG_CLEAR (the list itself) or VMCAT_SIG_LORDOS */
    char sig_name[96];            /* the detached signature's file name ("" for a clear-signed list) */
    char keys[64];                /* the maker's key file, a plain name in the keys folder ("" for VMCAT_SIG_LORDOS) */
    char fprs[VMCAT_FPRS][41];    /* the primary keys of that file that may sign (40 hex digits, upper case) */
    int nfprs;
    /* An entry whose maker signs no list (Windows' evaluation, virtio-win, WinFsp; docs/VIRTUALIZATION.md 4.3, 6.7.1) names its files
     * itself, "pinned": each with its https URL, its SHA-256 (from the maker's page, checked against a download) and its
     * size. The catalog is part of LordOS's signed package; a pinned file is kept only with that hash. */
    int npins;
    struct { char name[96]; char url[VMCAT_URL]; char sha[65]; unsigned long long bytes; } pin[VMCAT_PINS];
} VmCatEntry;

/* the entry of `id` for `arch` ("amd64", "arm64"): 0, or -1 (not there, or the entry isn't well formed: a folder that
 * isn't https:// ending in '/', a pattern that isn't anchored or doesn't compile, a size out of range, no signature,
 * key file or fingerprint) */
int vmcat_find(const char *json, size_t n, const char *id, const char *arch, VmCatEntry *e);

/* 1 if name is a plain file name (letters, digits, . _ + ~ -, 1 to 127 bytes, not starting with '.' or '-') */
int vmcat_name_ok(const char *name);

/* 1 if name is a plain file name that matches the anchored extended regular expression re */
int vmcat_match(const char *re, const char *name);

/* 1 if a line of the maker's list (n bytes) gives exactly this name and SHA-256 (64 hex digits, any case): GNU form
 * "<hash>  <name>" or "<hash> *<name>", or BSD form "SHA256 (<name>) = <hash>" */
int vmcat_sums_has(const char *sums, size_t n, const char *name, const char *sha_hex);

/* The maker's list checked with the entry's pinned keys before any hash in it is trusted. keys: the key file's bytes;
 * sig: the detached signature's bytes (NULL when the maker published none; ignored for a clear-signed list). Returns 1
 * with *body (malloc) the text that was signed (the list itself, or a clear-signed list's text without its armour),
 * 0 when the list isn't signed (no signature file, or a list that should be clear-signed and isn't): vm.listUnsigned,
 * -1 when the signature doesn't verify with the pinned keys (a changed list, another key, an expired or revoked key,
 * or a key file without a pinned fingerprint): vm.listBadSignature, with pgp.c's reason in err.
 * VMCAT_SIG_LORDOS: keys is the 32-byte Ed25519 release key itself (the helper passes codesig_release_key) and sig the
 * base64 SHA256SUMS.sig; a signature that doesn't verify, or any other key size: -1 with imager.badSignature in err. */
int vmcat_verify_list(const VmCatEntry *e, const char *keys, size_t keys_len, const char *list, size_t list_len,
                      const char *sig, size_t sig_len, int64_t now, char **body, size_t *body_len, char *err, size_t errlen);

/* 1 if s is 64 hex digits */
int vmcat_sha_ok(const char *s);

/* a pinned entry's file of that name and SHA-256 (any case): its index, or -1 */
int vmcat_pin_find(const VmCatEntry *e, const char *name, const char *sha);

#endif
