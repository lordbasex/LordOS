/* cleanfs.h — what lord-server and lord-clean share to reach and judge a file (cleanfs.c). */
#ifndef LORD_CLEAN_FS_H
#define LORD_CLEAN_FS_H

#include <stdint.h>

/* openat2 beneath dirfd: no symlink, no mount crossed, no `..` above dirfd. An fd or -1 with errno. */
int clean_open(int dirfd, const char *rel, int flags);
/* A file name that can be shown and carried in JSON: valid UTF-8, no control characters, not
 * empty, `.`, `..` and no slash. */
int clean_name_ok(const char *name);
/* A path relative to a root made only of such names, at most depth levels and CLEAN_PATH_MAX bytes. */
int clean_relpath_ok(const char *rel, int depth);
/* The rule tables' bounded pattern: literal text with at most one `*`. Linear. */
int clean_glob(const char *pattern, const char *name);

/* The kind of file a name suggests (CLEAN_TYPE_*). Only for grouping: it decides nothing. */
int clean_type(const char *name);
const char *clean_type_name(int type);
/* path is ex or under it */
int clean_under(const char *path, const char *ex);
/* Two images whose difference hashes are within CLEAN_SIMILAR_BITS and whose proportions match. */
int clean_similar(uint64_t a, uint64_t b, unsigned aw, unsigned ah, unsigned bw, unsigned bh);

#endif
