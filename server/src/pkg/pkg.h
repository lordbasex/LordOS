/* pkg.h — LordOS's own Debian packages (see pkg.c and docs/PACKAGES.md).
 *
 * Everything that modifies the system is an action of the privileged helper (admin.c); lord-server
 * only reads the index (idx.h) and the dpkg database (status.h). */
#ifndef LORD_PKG_H
#define LORD_PKG_H

#include "../admin.h"

void pkg_act_update(const AdminReq *r, const char *arg, size_t len);
void pkg_act_install(const AdminReq *r, const char *arg, size_t len);
void pkg_act_remove(const AdminReq *r, const char *arg, size_t len);
void pkg_act_autoremove(const AdminReq *r, const char *arg, size_t len);
void pkg_act_upgrade(const AdminReq *r, const char *arg, size_t len);
void pkg_act_install_file(const AdminReq *r, const char *arg, size_t len);
/* (0.8.5) a vendor repository from LordOS's catalog: sources_admin.c */
void pkg_act_source_add(const AdminReq *r, const char *arg, size_t len);
void pkg_act_source_remove(const AdminReq *r, const char *arg, size_t len);
/* (0.8.8.9) i386 multiarch: dpkg --add-architecture i386, the only value, on amd64 only (pkg.c) */
void pkg_act_foreign_arch(const AdminReq *r, const char *arg, size_t len);
/* (0.8.8.9) A catalog entry whose vendor package ships its own source ("files": {"list", "keyring",
 * "ownedBy"}): once that package is installed with its own files, LordOS's bootstrap source and keyring for
 * it are removed (sources_admin.c). Run after dpkg changed something and before pkg-update reads the
 * sources. root: "" for the real system, or a test tree. */
void pkg_sources_handover(const char *root);
/* tests only (pkg_test.c): the catalog in <dir>/apt, files owned by pkg_sources_owner; returns how many
 * entries were handed over */
int pkg_sources_handover_in(const char *root, const char *dir);
extern uid_t pkg_sources_owner;

/* Privileged helper (admin.c) actions this module adds. */
#define PKG_ADMIN_ACTIONS \
    { "pkg-update", pkg_act_update, A_FORK }, \
    { "pkg-install", pkg_act_install, A_FORK }, \
    { "pkg-remove", pkg_act_remove, A_FORK }, \
    { "pkg-upgrade", pkg_act_upgrade, A_FORK }, \
    { "pkg-autoremove", pkg_act_autoremove, A_FORK }, \
    { "pkg-install-file", pkg_act_install_file, A_FORK }, \
    { "pkg-source-add", pkg_act_source_add, A_FORK }, \
    { "pkg-source-remove", pkg_act_source_remove, A_FORK }, \
    { "pkg-foreign-arch", pkg_act_foreign_arch, A_FORK },

#endif
