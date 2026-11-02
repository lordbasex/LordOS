/* clean_admin.h — Cleaner's closed system actions in the admin helper (approved by the maintainer on
 * 2026-10-30; docs/CLEANER-SECURITY.md). No argument is taken from anyone; every program and folder is fixed.
 *
 *   clean-status     read-only: "OK journal=<bytes> apt=<bytes> vartmp=<bytes>"                        anyone
 *   clean-journal    /usr/bin/journalctl --vacuum-size=200M                                            admin
 *   clean-apt-cache  regular files in /var/cache/apt/archives (*.deb) and its partial/, no links       admin
 *   clean-var-tmp    regular, root-owned, single-link files directly in /var/tmp, older than 30 days
 *                    by modification and change time, and open in no process                           admin
 */
#ifndef LORD_CLEAN_ADMIN_H
#define LORD_CLEAN_ADMIN_H

#include "../admin.h"
#include <sys/types.h>
#include <time.h>

#define CLEAN_VAR_TMP_DAYS 30

/* the rules, on an open folder (tests use throwaway ones): files removed, bytes in *freed */
long clean_purge_debs(int dirfd, double *freed);
long clean_purge_partial(int dirfd, double *freed);
long clean_purge_old(int dirfd, uid_t owner, time_t now, double *freed);
double clean_folder_bytes(int dirfd, int depth);

void clean_act_status(const AdminReq *r, const char *arg, size_t len);
void clean_act_journal(const AdminReq *r, const char *arg, size_t len);
void clean_act_apt(const AdminReq *r, const char *arg, size_t len);
void clean_act_var_tmp(const AdminReq *r, const char *arg, size_t len);

#define CLEAN_ADMIN_ACTIONS \
    { "clean-status", clean_act_status, A_ANYONE }, \
    { "clean-journal", clean_act_journal, A_FORK }, \
    { "clean-apt-cache", clean_act_apt, A_FORK }, \
    { "clean-var-tmp", clean_act_var_tmp, A_FORK },

#endif
