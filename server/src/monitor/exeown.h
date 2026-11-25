/* exeown.h — which installed package owns an executable (Monitor → Processes, the origin badges; docs/MONITOR.md,
 * "Origin badges"). dpkg's own file lists answer it (<info>/<package>[:<arch>].list, one absolute path per line,
 * world-readable): never a process's name. No privileges, no program run.
 *
 * exeown_lookup is cheap and never blocks on the disk: it answers from a cache, and an executable not in it yet is
 * queued for a worker thread, which reads every list once for all the queued paths (about 0.1 s for 2000 packages)
 * and caches each answer. When dpkg changes a list (the info folder's modification time), every answer is looked up
 * again. Bounded: EXEOWN_MAX paths cached, lines up to 4 KiB, EXEOWN_MAX_LISTS lists. */
#ifndef LORD_MONITOR_EXEOWN_H
#define LORD_MONITOR_EXEOWN_H

#include <stddef.h>

#define EXEOWN_MAX 1024
#define EXEOWN_MAX_LISTS 50000
#define EXEOWN_PKG_MAX 128

/* 1: pkg gets the owning package (its name without the architecture); 0: no package owns it; -1: not known yet
 * (queued), or not a path that can be asked about (not absolute, too long, a control character). A trailing
 * " (deleted)" (the file was replaced while the process runs) is left out. */
int exeown_lookup(const char *path, char *pkg, size_t n);

/* The work itself, for the thread and the tests: for each of the n paths (sorted, distinct), out[i] gets the package
 * whose list in dir names it, or stays "" when none does. Returns the number of lists read, -1 if dir can't be read. */
int exeown_scan(const char *dir, const char *const *paths, int n, char (*out)[EXEOWN_PKG_MAX]);

/* The folder the lists are read from (DPKG_INFO; tests point it elsewhere). */
extern const char *exeown_info_dir;

#endif
