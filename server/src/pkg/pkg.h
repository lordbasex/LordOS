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

/* Privileged helper (admin.c) actions this module adds. */
#define PKG_ADMIN_ACTIONS \
    { "pkg-update", pkg_act_update, A_FORK }, \
    { "pkg-install", pkg_act_install, A_FORK }, \
    { "pkg-remove", pkg_act_remove, A_FORK }, \
    { "pkg-upgrade", pkg_act_upgrade, A_FORK }, \
    { "pkg-autoremove", pkg_act_autoremove, A_FORK }, \
    { "pkg-install-file", pkg_act_install_file, A_FORK }, \
    { "pkg-source-add", pkg_act_source_add, A_FORK }, \
    { "pkg-source-remove", pkg_act_source_remove, A_FORK },

#endif
