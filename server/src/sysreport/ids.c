/* ids.c — pci.ids / usb.ids lookups (see ids.h). The format: a vendor line "vvvv  Name" at the start
 * of a line, its devices on the lines below as "\tdddd  Name" (subsystems "\t\t…" are skipped), and,
 * in pci.ids, classes as "C cc  Name" with subclasses "\tss  Name". Comments start with '#'. */
#include "ids.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IDS_MAX (8u << 20)

int ids_open(IdsDb *db, const char *path) {
    db->text = NULL;
    db->len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (unsigned long long)st.st_size > IDS_MAX) { close(fd); return -1; }
    char *p = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (p && got < (size_t)st.st_size) {
        ssize_t n = read(fd, p + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!p || !got) { free(p); return -1; }
    p[got] = 0;
    db->text = p;
    db->len = got;
    return 0;
}

void ids_close(IdsDb *db) { free(db->text); db->text = NULL; db->len = 0; }

/* The name after "<hex>  " up to the end of the line, printable ASCII only. */
static void copy_name(const char *s, const char *end, char *out, size_t size) {
    if (!out || !size) return;
    while (s < end && *s == ' ') s++;
    size_t o = 0;
    for (; s < end && *s != '\n' && o + 1 < size; s++) out[o++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* The line [s, e) starts (after `tabs` tabs) with the hex id followed by a space. */
static int id_line(const char *s, const char *e, int tabs, const char *id, size_t idlen, const char **name) {
    for (int i = 0; i < tabs; i++) { if (s >= e || *s != '\t') return 0; s++; }
    if (s < e && *s == '\t') return 0;
    if ((size_t)(e - s) <= idlen) return 0;
    for (size_t i = 0; i < idlen; i++) if (tolower((unsigned char)s[i]) != tolower((unsigned char)id[i])) return 0;
    if (s[idlen] != ' ') return 0;
    *name = s + idlen;
    return 1;
}

static size_t hexlen(const char *s) {
    size_t n = 0;
    while (s && isxdigit((unsigned char)s[n])) n++;
    return s && !s[n] ? n : 0;
}

void ids_lookup(const IdsDb *db, const char *vendor, const char *device, char *vname, size_t vsize, char *dname, size_t dsize) {
    if (vname && vsize) vname[0] = 0;
    if (dname && dsize) dname[0] = 0;
    size_t vl = hexlen(vendor), dl = hexlen(device);
    if (!db->text || vl != 4) return;
    const char *p = db->text, *end = db->text + db->len;
    int in_vendor = 0;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        const char *name;
        if (*p != '#' && *p != '\t' && p < e) {
            if (in_vendor) return;          /* past the vendor's devices */
            if (*p == 'C' && p[1] == ' ') return;   /* the class section follows the vendors */
            if (id_line(p, e, 0, vendor, 4, &name)) {
                copy_name(name, e, vname, vsize);
                if (dl != 4) return;
                in_vendor = 1;
            }
        } else if (in_vendor && id_line(p, e, 1, device, 4, &name)) {
            copy_name(name, e, dname, dsize);
            return;
        }
        p = e + 1;
    }
}

void ids_pci_class(const IdsDb *db, const char *cls, char *cname, size_t csize, char *sname, size_t ssize) {
    if (cname && csize) cname[0] = 0;
    if (sname && ssize) sname[0] = 0;
    if (!db->text || hexlen(cls) < 4) return;
    const char *p = db->text, *end = db->text + db->len;
    int in_class = 0;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        const char *name;
        if (p + 2 < e && p[0] == 'C' && p[1] == ' ') {
            if (in_class) return;
            if (id_line(p + 2, e, 0, cls, 2, &name)) { copy_name(name, e, cname, csize); in_class = 1; }
        } else if (in_class && *p != '#' && *p != '\t' && p < e) {
            return;
        } else if (in_class && id_line(p, e, 1, cls + 2, 2, &name)) {
            copy_name(name, e, sname, ssize);
            return;
        }
        p = e + 1;
    }
}
