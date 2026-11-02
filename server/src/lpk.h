/* lpk.h — LordOS native app packages (.lpk): read, install, list and remove (see lpk.c). */
#ifndef LORD_LPK_H
#define LORD_LPK_H

#include <stddef.h>

#define LPK_MAX_FILES 2000
#define LPK_MAX_TOTAL (64u * 1024 * 1024)     /* decompressed content */

typedef struct {
    char path[224];                     /* relative, already validated: "app.js", "wasm/vida.wasm" */
    const unsigned char *data;
    size_t size;
} LpkFile;

typedef struct {
    unsigned char *raw;                 /* the tar in memory (already decompressed) */
    LpkFile *files;
    int nfiles;
} Lpk;

/* Reads an .lpk (tar or tar.gz). Rejects links, devices, paths with "..", absolute ones or with
 * odd characters, and those that exceed the limits. err receives a text for the user. */
int  lpk_open(const char *path, Lpk *out, char *err, size_t errlen);
const LpkFile *lpk_find(const Lpk *p, const char *path);
void lpk_free(Lpk *p);

/* Installed apps folder: ~/.local/share/axia/apps (created if missing). */
const char *lpk_root(void);
/* Valid package id: [a-z][a-z0-9-]{1,39} (it is also its text namespace). */
int  lpk_valid_id(const char *id);
/* Valid relative path inside a package. */
int  lpk_valid_path(const char *rel);
/* Installs (or replaces) the package in lpk_root()/<id>, atomically. */
int  lpk_install(const Lpk *p, const char *id, char *err, size_t errlen);
int  lpk_remove(const char *id);

#endif
