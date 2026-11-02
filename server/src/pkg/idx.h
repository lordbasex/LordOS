/* idx.h — LordOS's compact package index (/var/lib/lordos/pkg/index.bin).
 *
 * The root helper builds it in pkg-update from the already verified Packages, Translation-en and
 * DEP-11; lord-server (no root) opens it with mmap: nothing is copied into memory until it's
 * used. Everything is 32-bit little-endian integers and offsets into the string block
 * (string 0 is ""). Packages are sorted by name and, within a name, from the newest version
 * to the oldest. Long descriptions go in their own block at the end, in the same file (it's
 * replaced whole with rename: there's never half an index). */
#ifndef LORD_PKG_IDX_H
#define LORD_PKG_IDX_H

#include <stddef.h>
#include <stdint.h>

#define IDX_MAGIC "LPKIDX2"
#define PKG_DIR "/var/lib/lordos/pkg"
#define PKG_CACHE "/var/cache/lordos/pkg"

enum { IF_ESSENTIAL = 1, IF_PROTECTED = 2, IF_MA_SAME = 4, IF_MA_FOREIGN = 8, IF_MA_ALLOWED = 16 };
enum { PRIO_UNKNOWN, PRIO_REQUIRED, PRIO_IMPORTANT, PRIO_STANDARD, PRIO_OPTIONAL, PRIO_EXTRA };

typedef struct {
    uint32_t name, version, arch, section, summary, filename, homepage, source;
    uint32_t depends, predepends, recommends, suggests, provides, conflicts, breaks, replaces;
    uint32_t size, isize;               /* bytes of the .deb, installed KB */
    uint32_t desc, desclen;             /* in the descriptions block */
    uint32_t app;                       /* index into apps or UINT32_MAX */
    uint16_t src;                       /* index into srcs */
    uint8_t prio, flags;
    uint8_t sha256[32];
} IdxPkg;                               /* 120 bytes */

typedef struct { uint32_t name, version, pkg; } IdxProv;      /* name is provided by package pkg */

typedef struct {
    uint32_t id, package, name, summary, name_es, summary_es, name_pt, summary_pt, icon, categories, type;
    uint32_t desc;                      /* in the descriptions block, 0-terminated; UINT32_MAX: none */
} IdxApp;

typedef struct {
    uint32_t uri, suite, component, codename, origin, label;
    int64_t date, valid_until;
    uint32_t prio, npkgs;               /* prio: 500 normal, 100 backports, 1 experimental */
} IdxSrc;

typedef struct {
    char magic[8];
    uint32_t npkg, nprov, napp, nsrc;
    uint32_t off_pkg, off_prov, off_app, off_src, off_str, len_str;
    int64_t built;
    uint32_t off_desc, len_desc;
    char arch[16];                      /* native architecture it was built with */
} IdxHeader;

/* ---- reading (lord-server and the helper) ---- */

typedef struct {
    const IdxHeader *h;
    const IdxPkg *pkg;
    const IdxProv *prov;
    const IdxApp *app;
    const IdxSrc *src;
    const char *str, *desc;
    size_t str_len, desc_len, map_len;
    void *map;
    long mtime, ino;
    int16_t *prio;                      /* each record's priority after apt's preferences (pin.c) */
    unsigned long pin_stamp;
} Idx;

int  idx_open(Idx *x, const char *dir);          /* 0 ok */
void idx_close(Idx *x);
int  idx_refresh(Idx *x, const char *dir);       /* reopens if it changed on disk; 1 if it changed */
static inline const char *idx_s(const Idx *x, uint32_t off) { return off < x->str_len ? x->str + off : ""; }
/* A record's priority (apt's preferences applied; the source's own priority without them). */
static inline int idx_prio(const Idx *x, long rec) { return x->prio ? x->prio[rec] : (int)x->src[x->pkg[rec].src].prio; }
/* First package with that name (or -1); the next ones with the same name follow it. */
long idx_find(const Idx *x, const char *name, size_t n);
/* Range [*lo, *hi) of those that provide name. */
void idx_providers(const Idx *x, const char *name, size_t n, long *lo, long *hi);

/* ---- building (root helper) ---- */

typedef struct IdxBuilder IdxBuilder;
IdxBuilder *idxb_new(void);
int  idxb_source(IdxBuilder *b, const char *uri, const char *suite, const char *component, const char *codename,
                 const char *origin, const char *label, int64_t date, int64_t valid_until, int prio);
/* Already decompressed Packages of source src (the index returned by idxb_source). */
int  idxb_packages(IdxBuilder *b, int src, const char *buf, size_t len, const char *arch);
/* Translation-en: long descriptions by Description-md5. */
int  idxb_translation(IdxBuilder *b, const char *buf, size_t len);
/* DEP-11 Components (already decompressed YAML). */
int  idxb_appstream(IdxBuilder *b, const char *buf, size_t len);
/* Does any app use this icon (file name in icons-64x64.tar)? */
int  idxb_icon_wanted(IdxBuilder *b, const char *name);
int  idxb_write(IdxBuilder *b, const char *dir, const char *arch, char *err, size_t errlen);
void idxb_free(IdxBuilder *b);
uint32_t idxb_count(const IdxBuilder *b);

#endif
