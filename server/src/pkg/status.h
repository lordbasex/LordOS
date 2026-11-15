/* status.h — installed packages, read from /var/lib/dpkg/status (read-only: the dpkg
 * database is written only by dpkg). */
#ifndef LORD_PKG_STATUS_H
#define LORD_PKG_STATUS_H

#include <stddef.h>
#include <stdint.h>

#define DPKG_STATUS "/var/lib/dpkg/status"

typedef struct {
    const char *name, *version, *arch, *summary, *section, *source;
    const char *key;                    /* (0.8.8.9) the name, or "name:<arch>" for another architecture's (libc6:i386) */
    const char *depends, *predepends, *provides, *conflicts, *breaks, *replaces, *recommends;
    uint32_t isize;
    uint8_t flags;                      /* IF_ESSENTIAL, IF_PROTECTED, IF_MA_* from idx.h */
    uint8_t prio;
    uint8_t native;                     /* native architecture or all */
    uint8_t config_only;                /* removed but with its configuration ("config-files") */
} StPkg;

typedef struct {
    StPkg *p;                           /* sorted by key */
    int n;
    char *buf;
    char **keys;                        /* the "name:<arch>" keys of other architectures' packages */
    int nkeys;
    long mtime, size;
} Status;

/* Loads (or reloads if it changed). arch: the native one ("amd64"). Only counts what has its
 * files on the system (installed, unpacked, half-configured, triggers-*). 0 ok. */
int  status_load(Status *s, const char *path, const char *arch);
void status_free(Status *s);
/* Installed package with that key, or NULL: "name" is the native (or all) one, "name:i386" the i386 one. */
const StPkg *status_find(const Status *s, const char *name, size_t n);

/* The installed package that owns a launcher's .desktop file, from dpkg's file lists
 * (<info>/<package>[:<arch>].list, one absolute path per line; read-only). Only paths directly in
 * /usr/share/applications or /usr/local/share/applications are answered (the only ones Store →
 * Installed asks about; never a home folder). One pass over the lists keeps just the lines in those two
 * folders, cached until the info folder's modification time changes (dpkg replaces a .list by renaming
 * it there); bounded: DPKG_OWNER_MAX_LISTS lists, DPKG_OWNER_MAX_ENTRIES entries, lines up to 4 KiB.
 * out gets the package name without its architecture. 0 if found, -1 otherwise. */
#define DPKG_INFO "/var/lib/dpkg/info"
#define DPKG_OWNER_MAX_LISTS 50000
#define DPKG_OWNER_MAX_ENTRIES 20000
int dpkg_owner(const char *info, const char *path, char *out, size_t n);

/* dpkg's native architecture (amd64, arm64…), according to the machine. */
const char *pkg_native_arch(void);

/* (0.8.8.9, multiarch) dpkg's list of architectures (/var/lib/dpkg/arch, written only by dpkg; tests point
 * pkg_dpkg_arch_file elsewhere). pkg_i386_enabled: i386 is in it on an amd64 system, the only foreign
 * architecture the engine fetches and installs (docs/PACKAGES.md, "Multiarch"). */
#define DPKG_ARCH_FILE "/var/lib/dpkg/arch"
extern const char *pkg_dpkg_arch_file;
int pkg_arch_listed(const char *native, const char *arch);
int pkg_i386_enabled(void);

#endif
