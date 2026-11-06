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

#define VMCAT_URL 400
#define VMCAT_RE  160

typedef struct {
    char dir[VMCAT_URL];          /* https://…/ : the maker's folder for the release (its list, its listing) */
    char file_dir[VMCAT_URL];     /* where the image itself comes from (dir unless the entry says fileDir) */
    char sums[96];                /* the list's fixed name, or "" when it is found in dir's listing */
    char sums_re[VMCAT_RE];       /* that name's pattern ("" with a fixed name) */
    char file_re[VMCAT_RE];       /* the image's name, an extended regular expression */
    unsigned long long max_bytes; /* the most a download of this entry may be */
} VmCatEntry;

/* the entry of `id` for `arch` ("amd64", "arm64"): 0, or -1 (not there, or the entry isn't well formed: a folder that
 * isn't https:// ending in '/', a pattern that isn't anchored or doesn't compile, a size out of range) */
int vmcat_find(const char *json, size_t n, const char *id, const char *arch, VmCatEntry *e);

/* 1 if name is a plain file name (letters, digits, . _ + ~ -, 1 to 127 bytes, not starting with '.' or '-') */
int vmcat_name_ok(const char *name);

/* 1 if name is a plain file name that matches the anchored extended regular expression re */
int vmcat_match(const char *re, const char *name);

/* 1 if a line of the maker's list (n bytes) gives exactly this name and SHA-256 (64 hex digits, any case): GNU form
 * "<hash>  <name>" or "<hash> *<name>", or BSD form "SHA256 (<name>) = <hash>" */
int vmcat_sums_has(const char *sums, size_t n, const char *name, const char *sha_hex);

/* 1 if s is 64 hex digits */
int vmcat_sha_ok(const char *s);

#endif
