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
#define FPAKAS_DESC_MAX 4096            /* a description's bytes, its "…" included */
#define FPAKAS_DESC_LINE_MAX 8448       /* "#d", the id and every byte of it escaped */

typedef struct {
    char id[64], branch[64];
    char name[FPAKAS_NAME_MAX + 1], summary[FPAKAS_SUMMARY_MAX + 1], icon[FPAKAS_ICON_MAX + 1];
    char categories[FPAKAS_CATS_MAX + 1], license[FPAKAS_LICENSE_MAX + 1], developer[FPAKAS_DEV_MAX + 1];
    int verified;
    long long updated;                  /* the newest <release timestamp> (seconds since 1970), 0 when none */
} FpakasApp;

/* The index of an AppStream XML text (len bytes, need not end in 0): one line per desktop application whose bundle is
 * "app/<id>/<arch>/<branch>" for this arch, its fields tab-separated in FpakasApp's order (id, branch, name, summary,
 * icon, categories, license, developer, verified "0"/"1", updated: the newest release's timestamp in decimal, "0" when
 * it has none); name and summary in lang ("es-AR": es_AR, then es, then the
 * untranslated one). An app with a <description> has a second line right after its own: "#d", its id and the text, tab
 * separated, the text in lang like the name, flattened to plain text (paragraphs apart by an empty line, list items
 * after "• " or their number, at most FPAKAS_DESC_MAX bytes, cut at a word with "…") and its line breaks and
 * backslashes written "\n" and "\\". Calls out for each line (0 to go on). → the number of components, or -1 on input
 * that isn't AppStream. */
int fpakas_index(const char *xml, size_t len, const char *lang, const char *arch,
                 int (*out)(void *ud, const char *line, size_t n), void *ud);

/* A description line of the index, checked again in lord-server: the id (fpak_id_ok) into id, the text decoded into
 * text (FPAKAS_DESC_MAX + 1 bytes): not empty, valid UTF-8 whose only control character is a line break, no more than
 * one empty line in a row. → its length, or -1. */
int fpakas_desc_line(const char *line, size_t n, char id[64], char *text);

/* One line of the index, checked again in lord-server: every field of the right shape (fpak_id_ok, fpak_branch_ok, an
 * icon file name, printable UTF-8 text, "0"/"1", a timestamp of 1 to 11 digits without a leading zero). 0 and *a filled, or -1. */
int fpakas_line(const char *line, size_t n, FpakasApp *a);

#endif
