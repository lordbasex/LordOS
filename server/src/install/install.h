/* install.h — LordOS installer to disk, only from the live ISO (see install.c and docs/ISO.md). */
#ifndef LORD_INSTALL_H
#define LORD_INSTALL_H

#include "../admin.h"

void install_act_disks(const AdminReq *r, const char *arg, size_t len);
void install_act_run(const AdminReq *r, const char *arg, size_t len);
void install_act_account(const AdminReq *r, const char *arg, size_t len);
void install_act_reboot(const AdminReq *r, const char *arg, size_t len);

/* The unattended install (autoinstall.c): lets install-run take passwordHash, sshKey and autologin. */
void install_set_unattended(int on);
int  install_hash_ok(const char *hash);          /* a $y$ or $6$ crypt(3) hash crypt accepts */
int  install_ssh_key_ok(const char *line);       /* one plain authorized_keys line */

/* Privileged helper actions (admin.c). They all answer ERR outside live mode, except install-account,
 * which needs the OVA's first boot. */
#define INSTALL_ADMIN_ACTIONS \
    { "install-disks", install_act_disks, 0 }, \
    { "install-run", install_act_run, A_FORK }, \
    { "install-account", install_act_account, A_FORK }, \
    { "install-reboot", install_act_reboot, 0 },

#endif
