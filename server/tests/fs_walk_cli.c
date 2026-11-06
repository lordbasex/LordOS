/* fs_walk_cli.c — lord.fs's Recents and search walks (src/fsops.c) on their own, for tests/cloud_integration.sh: they must
 * come back at once even with a cloud mount under the home folder whose server doesn't answer.
 *   build/fs-walk recent | search NEEDLE      (HOME is the home folder) → one line per hit, then "hits N ms M"
 *   build/fs-walk rename FROM TO              lord.fs's rename (fsops_rename): "ok" or the error code line */
#include "../src/fsops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void hit(void *ud, const char *path, int dir, double size, double mtime) { (void)ud; (void)dir; (void)size; (void)mtime; printf("%s\n", path); }

int main(int argc, char **argv) {
    if (argc < 2 || !getenv("HOME")) { fprintf(stderr, "usage: fs-walk recent | search NEEDLE\n"); return 2; }
    fsops_init(getenv("HOME"), getenv("USER") ? getenv("USER") : "user");
    if (!strcmp(argv[1], "rename") && argc > 3) {
        char err[256] = "";
        int r = fsops_rename(argv[2], argv[3], err, sizeof err);
        printf("%s\n", r ? err : "ok");
        return r ? 1 : 0;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int n = !strcmp(argv[1], "recent") ? fs_recent(200, 30, hit, NULL) : argc > 2 ? fs_search(fsops_home(), argv[2], 200, 8, hit, NULL) : -1;
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("hits %d ms %ld\n", n, (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000));
    return n < 0;
}
