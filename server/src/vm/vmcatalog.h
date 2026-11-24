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
    /* LordOS's own releases only (VMCAT_SIG_LORDOS, a fixed list name): the same signed list also published on lordos.org
     * ("list": { "dir", "sums", "signature" }), read first because GitHub's release downloads sometimes answer 403 on
     * their signed redirect; dir's list stays the fallback. Both are checked with the same compiled-in release key, so
     * where the list comes from changes nothing about what is trusted. "" when the entry has none. */
    char list_dir[VMCAT_URL];     /* https://lordos.org/releases/<tag>/ */
    char list_sums[96];           /* the list's name there (SHA256SUMS.txt: the site needs an extension) */
    char list_sig[96];            /* its signature's name there */
    char keys[64];                /* the maker's key file, a plain name in the keys folder ("" for VMCAT_SIG_LORDOS) */
    char fprs[VMCAT_FPRS][41];    /* the primary keys of that file that may sign (40 hex digits, upper case) */
    int nfprs;
    /* An entry whose maker signs no list (Windows' evaluation, virtio-win, WinFsp; docs/VIRTUALIZATION.md 4.3, 6.7.1) names its files
     * itself, "pinned": each with its https URL, its SHA-256 (from the maker's page, checked against a download) and its
     * size. The catalog is part of LordOS's signed package; a pinned file is kept only with that hash. */
    int npins;
    struct { char name[96]; char url[VMCAT_URL]; char sha[65]; unsigned long long bytes; } pin[VMCAT_PINS];
    /* machine groups (docs/VIRTUALIZATION.md 19.6; approved by the maintainer on 2026-11-23): */
    int hash;                     /* VMCAT_SHA256 (the default) or VMCAT_SHA512 ("hash": "sha512"): the list's and the download's */
    char sig_suffix[16];          /* "signature": { "type": "detached", "suffix": ".asc" }: the list's own name plus this */
    /* "releases": the newest release folder under rel_dir whose list verifies, never older than rel_floor (the
     * maintainer's decision: a current Tor Browser after a Start Fresh, and no rollback below a release already used).
     * dir is then rel_dir + "<release>/" (vmcat_release_dir); "" when the entry pins its folder */
    char rel_dir[VMCAT_URL];
    char rel_re[VMCAT_RE];        /* a release folder's name */
    char rel_floor[32];
    /* "kind": "group": the download is an archive of a machine group's disks ("archive"), unpacked by lord-vmunpack */
    int group;
    char group_kind[16];          /* "whonix" */
    char edition[16];
    char member_re[2][VMCAT_RE];  /* [0] the gateway's member, [1] the workstation's: their names in the archive */
    unsigned long long max_apparent, max_data;   /* bytes: a member's real (sparse) size, and the data of all members */
    int member_vcpus[2], member_mem_mb[2];      /* "machine": each member's vCPUs and memory (defaults 1 and 2048) */
} VmCatEntry;
enum { VMCAT_SHA256 = 256, VMCAT_SHA512 = 512 };

/* the entry of `id` for `arch` ("amd64", "arm64"): 0, or -1 (not there, or the entry isn't well formed: a folder that
 * isn't https:// ending in '/', a pattern that isn't anchored or doesn't compile, a size out of range, no signature,
 * key file or fingerprint) */
int vmcat_find(const char *json, size_t n, const char *id, const char *arch, VmCatEntry *e);

/* where the list `name` (the entry's fixed list, or the name vm-image-list was given for a sumsPattern entry) and its
 * signature are fetched from, in the order to try them: source i (0, 1, …) into list (and sig, "" for a clear-signed
 * list), each of n bytes. 0, or -1 when there is no source i. Source 0 is the entry's "list" mirror when it has one and
 * name is its fixed list; the maker's folder (dir) is always the last. The first source whose list verifies is kept. */
int vmcat_list_source(const VmCatEntry *e, const char *name, int i, char *list, char *sig, size_t n);

/* 1 if name is a plain file name (letters, digits, . _ + ~ -, 1 to 127 bytes, not starting with '.' or '-') */
int vmcat_name_ok(const char *name);

/* 1 if name is a plain file name that matches the anchored extended regular expression re */
int vmcat_match(const char *re, const char *name);

/* 1 if a line of the maker's list (n bytes) gives exactly this name and SHA-256 (64 hex digits, any case): GNU form
 * "<hash>  <name>" or "<hash> *<name>", or BSD form "SHA256 (<name>) = <hash>". With 128 hex digits, the same for a
 * SHA-512 ("SHA512 (<name>) = <hash>" in BSD form) */
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
/* 1 if s is the entry's hash in hex: 64 digits (SHA-256) or 128 (an entry with "hash": "sha512") */
int vmcat_hash_ok(const VmCatEntry *e, const char *s);
/* a release folder's name for an entry with "releases": it matches rel_re and isn't older than rel_floor (numbers
 * compared one by one). 1 yes */
int vmcat_release_ok(const VmCatEntry *e, const char *rel);
/* -1, 0, 1: a release "18.2.4.3" compared to another, numerically, part by part */
int vmcat_release_cmp(const char *a, const char *b);
/* the release a file name of such an entry names (Whonix-LXQt-<release>.Intel_AMD64…: the first run of numbers joined by
 * dots, at least two of them, that matches rel_re), into out; 0, or -1 when it names none or more than one different.
 * A list's or an archive's name must name the very release whose folder it comes from (no older signed file paired with
 * a newer folder) */
int vmcat_release_in(const VmCatEntry *e, const char *name, char *out, size_t n);
/* e->dir and e->file_dir set to rel_dir + rel + "/" (after vmcat_release_ok); 0, or -1 */
int vmcat_release_dir(VmCatEntry *e, const char *rel);

/* a pinned entry's file of that name and SHA-256 (any case): its index, or -1 */
int vmcat_pin_find(const VmCatEntry *e, const char *name, const char *sha);

#endif
