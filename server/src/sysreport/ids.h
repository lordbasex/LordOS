/* ids.h — vendor, device and class names from the hwdata lists (`pci.ids` and `usb.ids`, the Debian
 * packages of the same names, /usr/share/misc). The file is read once into memory (at most 8 MB) and
 * searched in place; names are copied out as printable ASCII. A missing file only means no names. */
#ifndef LORD_SYSREPORT_IDS_H
#define LORD_SYSREPORT_IDS_H

#include <stddef.h>

typedef struct { char *text; size_t len; } IdsDb;

int  ids_open(IdsDb *db, const char *path);         /* 0, or -1 (db stays empty and lookups find nothing) */
void ids_close(IdsDb *db);

/* vendor and device are lowercase or uppercase hex ("10de", "2684"); device may be NULL for the
 * vendor only. Each out gets the name, or "" when it isn't listed. */
void ids_lookup(const IdsDb *db, const char *vendor, const char *device, char *vname, size_t vsize, char *dname, size_t dsize);

/* PCI class: "030000" → class "Display controller", subclass "VGA compatible controller"
 * (pci.ids "C" lines). Either out may be NULL. */
void ids_pci_class(const IdsDb *db, const char *cls, char *cname, size_t csize, char *sname, size_t ssize);

#endif
