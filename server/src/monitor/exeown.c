/* exeown.c — which installed package owns an executable (exeown.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exeown.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

const char *exeown_info_dir = "/var/lib/dpkg/info";

typedef struct { char *path; char pkg[EXEOWN_PKG_MAX]; int state; } Ent;     /* state: -1 queued, 0 none, 1 owned */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Ent ents[EXEOWN_MAX];
static int nents, queued, started;
static long long seen_mtime = -1;               /* the info folder's, when the answers were found */
static time_t checked;                          /* when the folder's time was last looked at */

static long long dir_mtime(const char *dir) {
    struct stat st;
    return stat(dir, &st) ? -1 : (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

int exeown_scan(const char *dir, const char *const *paths, int n, char (*out)[EXEOWN_PKG_MAX]) {
    for (int i = 0; i < n; i++) out[i][0] = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int lists = 0, left = n;
    char line[4096 + 2], file[4096];
    struct dirent *e;
    while (left > 0 && lists < EXEOWN_MAX_LISTS && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l <= 5 || strcmp(e->d_name + l - 5, ".list") || e->d_name[0] == '.') continue;
        char pkg[EXEOWN_PKG_MAX];
        size_t pl = strcspn(e->d_name, ":");
        if (pl > l - 5) pl = l - 5;
        if (!pl || pl >= sizeof pkg) continue;
        memcpy(pkg, e->d_name, pl);
        pkg[pl] = 0;
        if ((size_t)snprintf(file, sizeof file, "%s/%s", dir, e->d_name) >= sizeof file) continue;
        FILE *f = fopen(file, "re");
        if (!f) continue;
        lists++;
        while (left > 0 && fgets(line, sizeof line, f)) {
            size_t ll = strlen(line);
            if (ll && line[ll - 1] == '\n') line[--ll] = 0;
            else if (ll >= sizeof line - 1) {           /* longer than 4 KiB: skip the rest of it */
                int ch;
                while ((ch = fgetc(f)) != EOF && ch != '\n') {}
                continue;
            }
            if (line[0] != '/') continue;
            const char *key = line;
            const char *const *hit = bsearch(&key, paths, (size_t)n, sizeof *paths, cmp_str);
            if (!hit) continue;
            int i = (int)(hit - paths);
            if (!out[i][0]) { snprintf(out[i], EXEOWN_PKG_MAX, "%s", pkg); left--; }
        }
        fclose(f);
    }
    closedir(d);
    return lists;
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!queued) pthread_cond_wait(&cv, &mu);
        int n = 0;
        static const char *paths[EXEOWN_MAX];
        static char *copies[EXEOWN_MAX];
        for (int i = 0; i < nents; i++) if (ents[i].state < 0 && (copies[n] = strdup(ents[i].path))) n++;
        queued = 0;
        pthread_mutex_unlock(&mu);
        for (int i = 0; i < n; i++) paths[i] = copies[i];
        qsort(paths, (size_t)n, sizeof *paths, cmp_str);
        static char out[EXEOWN_MAX][EXEOWN_PKG_MAX];
        long long mt = dir_mtime(exeown_info_dir);
        int ok = exeown_scan(exeown_info_dir, paths, n, out) >= 0;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < n; i++)
            for (int k = 0; k < nents; k++)
                if (ents[k].state < 0 && !strcmp(ents[k].path, paths[i])) {
                    if (ok) memcpy(ents[k].pkg, out[i], sizeof ents[k].pkg); else ents[k].pkg[0] = 0;
                    ents[k].state = ok && out[i][0] ? 1 : 0;
                }
        seen_mtime = mt;
        pthread_mutex_unlock(&mu);
        for (int i = 0; i < n; i++) free((char *)paths[i]);
    }
    return NULL;
}

static int path_ok(const char *p, size_t l) {
    if (!l || l >= 4096 || p[0] != '/') return 0;
    for (size_t i = 0; i < l; i++) if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f) return 0;
    return 1;
}

int exeown_lookup(const char *path, char *pkg, size_t n) {
    if (pkg && n) pkg[0] = 0;
    if (!path) return -1;
    size_t l = strlen(path);
    static const char del[] = " (deleted)";
    if (l > sizeof del - 1 && !strcmp(path + l - (sizeof del - 1), del)) l -= sizeof del - 1;
    if (!path_ok(path, l)) return -1;
    pthread_mutex_lock(&mu);
    /* dpkg changed a list: every answer is looked up again (at most one look at the folder every 5 s) */
    time_t now = time(NULL);
    if (now - checked >= 5) {
        checked = now;
        long long mt = dir_mtime(exeown_info_dir);
        if (seen_mtime != -1 && mt != seen_mtime) {
            for (int i = 0; i < nents; i++) ents[i].state = -1;
            queued = nents > 0;
            seen_mtime = mt;
        }
    }
    int r = -1;
    Ent *e = NULL;
    for (int i = 0; i < nents && !e; i++) if (strlen(ents[i].path) == l && !memcmp(ents[i].path, path, l)) e = &ents[i];
    if (e && e->state >= 0) {
        r = e->state;
        if (r && pkg && n) snprintf(pkg, n, "%s", e->pkg);
    } else if (!e) {
        if (nents == EXEOWN_MAX) {                  /* full: forget the answers (the next lookups ask again) */
            for (int i = 0; i < nents; i++) free(ents[i].path);
            nents = 0;
        }
        char *copy = strndup(path, l);
        if (copy) {
            ents[nents].path = copy;
            ents[nents].pkg[0] = 0;
            ents[nents].state = -1;
            nents++;
            queued = 1;
        }
    }
    if (queued) {
        if (!started) {
            pthread_t th;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (!pthread_create(&th, &at, worker, NULL)) started = 1;
            pthread_attr_destroy(&at);
        }
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
    return r;
}
