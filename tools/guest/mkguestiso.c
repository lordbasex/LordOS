/* mkguestiso — the "LordOS Guest Tools" disc (docs/VIRTUALIZATION.md 6.17, "Install Agent without qemu-guest-agent"):
 * a small read-only ISO 9660 image made at build time (server/mk/vm.mk), never at run time and never on the host from
 * anything a guest, a person or JS gave. It holds lordos-guest for this build's architecture, install.sh and a README;
 * lord-vm attaches it as the machine's CD only when its SHA-256 is the one compiled into lord-vm (build/gen/guesttools.h).
 *
 *   mkguestiso OUT LABEL NAME=FILE...
 *
 * LABEL: the volume id, 1..32 of A-Z 0-9 _. NAME: a plain ISO 9660 level 1 name (1..8 of A-Z 0-9 _, then optionally a
 * dot and 1..3 more), at most 16 files, each at most 32 MiB. No Rock Ridge or Joliet: Linux shows these names in lower
 * case without the ";1" (isofs's default map=normal), which is all install.sh needs. The output is the same bytes for the
 * same inputs (fixed dates, names sorted), so the hash only changes when a file does. Only the build runs it. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SEC 2048u
#define MAX_FILES 16
#define MAX_FILE (32u << 20)

typedef struct { char id[16]; unsigned idn; uint8_t *data; uint32_t n, lba; } File;

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void both32(uint8_t *p, uint32_t v) { le32(p, v); be32(p + 4, v); }
static void both16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void pad(uint8_t *p, size_t n, const char *s) { size_t l = strlen(s); memset(p, ' ', n); memcpy(p, s, l < n ? l : n); }

static int dchar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* NAME or NAME.EXT, 8.3 of d-characters → "NAME.EXT;1" (a name without an extension keeps its dot: "LGUEST.;1") */
static int iso_name(const char *s, size_t len, char out[16], unsigned *outn) {
    size_t base = 0, ext = 0;
    const char *dot = memchr(s, '.', len);
    base = dot ? (size_t)(dot - s) : len;
    ext = dot ? len - base - 1 : 0;
    if (!base || base > 8 || (dot && (!ext || ext > 3))) return -1;
    for (size_t i = 0; i < len; i++) if (!(dchar(s[i]) || (dot && s + i == dot))) return -1;
    int n = snprintf(out, 16, "%.*s.%.*s;1", (int)base, s, (int)ext, dot ? dot + 1 : "");
    if (n <= 0 || n >= 16) return -1;
    *outn = (unsigned)n;
    return 0;
}

/* 7-byte recording date: 2026-01-01 00:00:00 UTC, fixed so the image is reproducible */
static void rec_date(uint8_t *p) { p[0] = 126; p[1] = 1; p[2] = 1; p[3] = 0; p[4] = 0; p[5] = 0; p[6] = 0; }
static void vol_date(uint8_t *p) { memcpy(p, "2026010100000000", 16); p[16] = 0; }

/* one directory record at p; returns its length (even) */
static size_t dir_rec(uint8_t *p, const char *id, unsigned idn, uint32_t lba, uint32_t size, int dir) {
    size_t len = 33 + idn + ((33 + idn) & 1);
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    rec_date(p + 18);
    p[25] = dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = (uint8_t)idn;
    memcpy(p + 33, id, idn);
    return len;
}

static int by_id(const void *a, const void *b) { return strcmp(((const File *)a)->id, ((const File *)b)->id); }

static uint8_t *slurp(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    uint8_t *b = NULL;
    if (!fstat(fileno(f), &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= MAX_FILE && (b = malloc((size_t)st.st_size))) {
        if (fread(b, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free(b); b = NULL; }
        else *n = (uint32_t)st.st_size;
    }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: mkguestiso OUT LABEL NAME=FILE...\n"); return 2; }
    const char *out = argv[1], *label = argv[2];
    size_t ll = strlen(label);
    if (!ll || ll > 32) { fprintf(stderr, "mkguestiso: bad label\n"); return 2; }
    for (size_t i = 0; i < ll; i++) if (!dchar(label[i])) { fprintf(stderr, "mkguestiso: bad label\n"); return 2; }
    static File files[MAX_FILES];
    int nf = 0;
    for (int i = 3; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq || nf == MAX_FILES) { fprintf(stderr, "mkguestiso: bad file argument %s\n", argv[i]); return 2; }
        File *f = &files[nf];
        if (iso_name(argv[i], (size_t)(eq - argv[i]), f->id, &f->idn)) { fprintf(stderr, "mkguestiso: %.*s isn't an 8.3 name of A-Z 0-9 _\n", (int)(eq - argv[i]), argv[i]); return 2; }
        for (int j = 0; j < nf; j++) if (!strcmp(files[j].id, f->id)) { fprintf(stderr, "mkguestiso: %s twice\n", f->id); return 2; }
        if (!(f->data = slurp(eq + 1, &f->n))) { fprintf(stderr, "mkguestiso: can't read %s: %s\n", eq + 1, strerror(errno ? errno : EINVAL)); return 1; }
        nf++;
    }
    qsort(files, (size_t)nf, sizeof *files, by_id);
    /* sectors: 0-15 system area, 16 PVD, 17 terminator, 18 L path table, 19 M path table, 20 root, 21… the files */
    uint32_t lba = 21;
    for (int i = 0; i < nf; i++) { files[i].lba = lba; lba += (files[i].n + SEC - 1) / SEC; }
    uint32_t total = lba;
    uint8_t *img = calloc(total, SEC);
    if (!img) return 1;
    /* the root directory: ".", "..", then each file (one sector holds 16 records of these sizes) */
    uint8_t *root = img + 20 * SEC;
    size_t rp = 0;
    rp += dir_rec(root + rp, "\0", 1, 20, SEC, 1);
    rp += dir_rec(root + rp, "\1", 1, 20, SEC, 1);
    for (int i = 0; i < nf; i++) {
        if (rp + 33 + files[i].idn + 1 > SEC) { fprintf(stderr, "mkguestiso: too many files\n"); return 2; }
        rp += dir_rec(root + rp, files[i].id, files[i].idn, files[i].lba, files[i].n, 0);
        memcpy(img + (size_t)files[i].lba * SEC, files[i].data, files[i].n);
    }
    /* path tables: the root alone (10 bytes) */
    uint8_t *lp = img + 18 * SEC, *mp = img + 19 * SEC;
    lp[0] = 1; le32(lp + 2, 20); lp[6] = 1; lp[7] = 0;
    mp[0] = 1; be32(mp + 2, 20); mp[6] = 0; mp[7] = 1;
    /* the primary volume descriptor */
    uint8_t *v = img + 16 * SEC;
    v[0] = 1; memcpy(v + 1, "CD001", 5); v[6] = 1;
    pad(v + 8, 32, "LINUX");
    pad(v + 40, 32, label);
    both32(v + 80, total);
    both16(v + 120, 1);
    both16(v + 124, 1);
    both16(v + 128, SEC);
    both32(v + 132, 10);
    le32(v + 140, 18);
    be32(v + 148, 19);
    dir_rec(v + 156, "\0", 1, 20, SEC, 1);
    pad(v + 190, 128, "LORDOS");
    pad(v + 318, 128, "LORDOS");
    pad(v + 446, 128, "LORDOS");
    pad(v + 574, 128, "LORDOS GUEST TOOLS");
    pad(v + 702, 37, ""); pad(v + 739, 37, ""); pad(v + 776, 37, "");
    vol_date(v + 813); vol_date(v + 830);
    memcpy(v + 847, "0000000000000000", 16); v[863] = 0;
    memcpy(v + 864, "0000000000000000", 16); v[880] = 0;
    v[881] = 1;
    /* the set terminator */
    uint8_t *t = img + 17 * SEC;
    t[0] = 255; memcpy(t + 1, "CD001", 5); t[6] = 1;
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", out) >= (int)sizeof tmp) return 2;
    FILE *o = fopen(tmp, "wb");
    if (!o || fwrite(img, SEC, total, o) != total || fclose(o)) { fprintf(stderr, "mkguestiso: can't write %s\n", tmp); remove(tmp); return 1; }
    if (rename(tmp, out)) { remove(tmp); return 1; }
    return 0;
}
