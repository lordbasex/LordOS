/* deb.h — Debian formats: versions (like dpkg), deb822 fields and relationships between packages. */
#ifndef LORD_PKG_DEB_H
#define LORD_PKG_DEB_H

#include <stddef.h>

/* Compares versions with dpkg's algorithm (epoch:version-revision, ~ before anything). */
int ver_cmp(const char *a, const char *b);
int ver_cmp_n(const char *a, size_t an, const char *b, size_t bn);

enum { REL_NONE, REL_LT, REL_LE, REL_EQ, REL_GE, REL_GT };      /* <<  <=  =  >=  >> */
int ver_satisfies(const char *have, int op, const char *want, size_t wantlen);

/* Debian package name: [a-z0-9][a-z0-9+.-]+ */
int deb_valid_name(const char *s, size_t n);
int deb_valid_version(const char *s, size_t n);

/* (0.8.8.9, multiarch) A package as a request names it: a Debian name, or a name of the one foreign
 * architecture LordOS supports, "name:i386" (docs/PACKAGES.md, "Multiarch"). */
#define DEB_FOREIGN_ARCH "i386"
int deb_valid_pkgarg(const char *s, size_t n);
/* Length of the name without its ":arch" (the whole length when there is none). */
size_t deb_base_len(const char *s, size_t n);

/* deb822 fields: walks a paragraph (stanza). */
typedef struct {
    const char *p, *end;
} D822;

typedef struct {
    const char *name, *value;           /* value without the leading space; may contain \n + continuation */
    size_t nlen, vlen;
} D822Field;

/* Next paragraph of buf: [*start, *stop). 0 if there are no more. */
int d822_next_stanza(const char **pos, const char *end, const char **start, const char **stop);
int d822_next_field(D822 *it, D822Field *f);
int d822_is(const D822Field *f, const char *name);   /* case-insensitive */

/* A relationship: "name[:arch] (op version)". Walks "a (>= 1) | b, c" group by group. arch: the
 * qualifier after the colon ("any", "native", "i386"), alen 0 when there is none. */
typedef struct {
    const char *name, *ver;
    size_t nlen, vlen;
    int op;
    const char *arch;
    size_t alen;
} DebRel;

/* Reads the next group of alternatives (up to the comma). Returns the count (0 = end). */
int deb_next_group(const char **p, const char *end, DebRel *alts, int max);

/* Release notes of pkg at version from a ReleaseNotes file (deb822 Package, Version, Notes):
 * the Markdown text, malloc'd (the caller frees it), or NULL. See docs/UPDATES.md. */
char *deb_notes_find(const char *buf, size_t len, const char *pkg, const char *version, size_t *outlen);

#endif
