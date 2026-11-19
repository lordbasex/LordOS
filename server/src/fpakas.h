/* fpakas.h — Flathub's AppStream catalog to a compact index (docs/FLATPAK.md, "The catalog"; AGENTS.md E7). The parser
 * runs only in the sandboxed helper lord-appstream (fpakasmain.c), on the file lord-server opened; lord-server checks
 * every line of the index again (fpakas_line) before JS sees it. */
#ifndef LORD_FPAKAS_H
#define LORD_FPAKAS_H

#include <stddef.h>

#define FPAKAS_NAME_MAX 128
#define FPAKAS_SUMMARY_MAX 256
#define FPAKAS_ICON_MAX 128
#define FPAKAS_CATS_MAX 256
#define FPAKAS_LICENSE_MAX 128
#define FPAKAS_DEV_MAX 128

typedef struct {
    char id[64], branch[64];
    char name[FPAKAS_NAME_MAX + 1], summary[FPAKAS_SUMMARY_MAX + 1], icon[FPAKAS_ICON_MAX + 1];
    char categories[FPAKAS_CATS_MAX + 1], license[FPAKAS_LICENSE_MAX + 1], developer[FPAKAS_DEV_MAX + 1];
    int verified;
} FpakasApp;

/* The index of an AppStream XML text (len bytes, need not end in 0): one line per desktop application whose bundle is
 * "app/<id>/<arch>/<branch>" for this arch, its fields tab-separated in FpakasApp's order (id, branch, name, summary,
 * icon, categories, license, developer, verified "0"/"1"); name and summary in lang ("es-AR": es_AR, then es, then the
 * untranslated one). Calls out for each line (0 to go on). → the number of apps, or -1 on input that isn't AppStream. */
int fpakas_index(const char *xml, size_t len, const char *lang, const char *arch,
                 int (*out)(void *ud, const char *line, size_t n), void *ud);

/* One line of the index, checked again in lord-server: every field of the right shape (fpak_id_ok, fpak_branch_ok, an
 * icon file name, printable UTF-8 text, "0"/"1"). 0 and *a filled, or -1. */
int fpakas_line(const char *line, size_t n, FpakasApp *a);

#endif
