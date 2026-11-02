/* cleanfs.h — what axia-server and axia-clean share to reach and judge a file (cleanfs.c). */
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

/* Build outputs of a code project. A folder's name proves nothing by itself: the kind is the one whose name it has and
 * whose evidence is there, next to it or inside it. parent is the folder that holds name. A kind or -1. */
#define CLEAN_PROJECT_KINDS 8
int clean_project_kind(int parent, const char *name);
int clean_project_named(int kind, const char *name);    /* name is one of that kind's folders */
int clean_project_output(const char *name);             /* left out when judging how recent a project is */
/* The newest change in the project in folder parent, its build outputs apart: 0 when nothing else is there, which
 * proves nothing. A folder's time says only that its entries changed, so the walk goes all the way down. left bounds
 * the entries read; tick, if any, runs for each. */
long long clean_project_newest(int parent, int max_depth, long *left, void (*tick)(void));
int clean_project_idle_days(int kind);                  /* offered only after this long without a change; 0 = any time */

/* The kind of file a name suggests (CLEAN_TYPE_*). Only for grouping: it decides nothing. */
int clean_type(const char *name);
const char *clean_type_name(int type);
/* path is ex or under it */
int clean_under(const char *path, const char *ex);
/* Two images whose difference hashes are within CLEAN_SIMILAR_BITS and whose proportions match. */
int clean_similar(uint64_t a, uint64_t b, unsigned aw, unsigned ah, unsigned bw, unsigned bh);

#endif
