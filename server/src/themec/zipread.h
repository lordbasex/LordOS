/* zipread.h — a strict reader of a community theme's .zip (lord-themec; docs/THEMES-GUIDE.md "Community themes").
 * Untrusted data: only lord-themec, inside the helpers' sandbox, parses it.
 *
 * What it accepts: one folder at the top (the theme's id, [A-Za-z0-9_-], at most 26 characters) holding regular files
 * and folders, stored or deflated, no encryption, no Zip64, no data descriptors with sizes it can't check. What it
 * refuses (the whole archive): a name with "..", "\", a leading "/", a drive letter, a control character or empty
 * segment; a link or a device (the Unix mode in the external attributes); two entries with one name; more than
 * ZIP_FILES_MAX entries; a file over ZIP_FILE_MAX or ZIP_TOTAL_MAX in all, as its header says and as inflating gives;
 * a CRC-32 that doesn't match; local and central headers that disagree. Every refusal names why. */
#ifndef LORD_ZIPREAD_H
#define LORD_ZIPREAD_H

#include <stddef.h>
#include <stdint.h>

#define ZIP_FILES_MAX 410
#define ZIP_FILE_MAX (5u << 20)
#define ZIP_TOTAL_MAX (20u << 20)
#define ZIP_NAME_MAX 200

typedef struct {
    char name[ZIP_NAME_MAX + 1];        /* inside the theme's folder (the id and its "/" taken off) */
    uint8_t *data;                      /* malloc'd */
    size_t size;
} ZipFile;

typedef struct {
    char id[32];
    ZipFile *files;
    int n;
} ZipTheme;

/* 0, or -1 with why (a short English sentence); z freed with zip_free either way */
int zip_read_theme(const uint8_t *buf, size_t len, ZipTheme *z, char *why, size_t why_n);
void zip_free(ZipTheme *z);
uint32_t zip_crc32(const uint8_t *p, size_t n);

#endif
