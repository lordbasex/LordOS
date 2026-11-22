/* mok_admin.h — guided Secure Boot enrolment of DKMS's signing key (docs/NVIDIA-PLAN.md A.5, where it was proposed as
 * gpu-mok-enroll; docs/CAMERA-STUDIO-PLAN.md 11.4; AGENTS.md "dkms-mok-enroll"). One closed action of the admin helper,
 * shared by the Store cards whose packages DKMS builds (nvidiaDriver, studioCamera):
 *
 *   dkms-mok-enroll status   read-only: "OK dkms.mokState state=<unsupported|off|nokey|enrolled|pending|needed>"
 *   dkms-mok-enroll import   only when Secure Boot is on and the key isn't enrolled (needed, or pending: the pending
 *                            request is withdrawn first so the new password is the one that counts): makes an
 *                            8-digit one-time password from getrandom, runs the fixed
 *                            "/usr/bin/mokutil --import /var/lib/dkms/mok.pub" with the password twice on standard
 *                            input (never in argv, a file or a log; mokutil's output is discarded), checks the key is
 *                            now in shim's MokNew request and answers "OK dkms.mokPassword password=<digits>" once.
 *                            The person types those digits in MokManager's blue screen at the next restart.
 *   dkms-mok-enroll cancel   only when the key's request is pending: the fixed "/usr/bin/mokutil --revoke-import"
 *
 * Administrators only. No other argument, path, key or program. The key must be DKMS's own pair at its fixed place
 * (/var/lib/dkms/mok.pub and mok.key: regular files, not links, owned by root, not writable by group or others, in a
 * root-owned folder only root can write), and mok.pub a single DER certificate; mokutil must be a root-owned regular
 * file. Whether the key is enrolled or pending is read from the firmware variables (MokListRT*, MokNew under
 * /sys/firmware/efi/efivars) by searching for the certificate's exact bytes; SecureBoot from its EFI variable. */
#ifndef LORD_HW_MOK_ADMIN_H
#define LORD_HW_MOK_ADMIN_H

#include "../admin.h"
#include <stddef.h>
#include <stdint.h>

#ifndef MOK_EFIVARS
#define MOK_EFIVARS "/sys/firmware/efi/efivars"
#endif
#ifndef MOK_DKMS_DIR
#define MOK_DKMS_DIR "/var/lib/dkms"
#endif
#ifndef MOK_UTIL
#define MOK_UTIL "/usr/bin/mokutil"
#endif

enum { MOK_UNSUPPORTED = 0, MOK_OFF, MOK_NOKEY, MOK_ENROLLED, MOK_PENDING, MOK_NEEDED };
#define MOK_KEY_MAX 8192                        /* a DER certificate; DKMS's is under 1 KB */
#define MOK_PASSWORD_DIGITS 8

const char *mok_state_name(int state);
/* 1 Secure Boot on, 0 off, -1 no EFI variables (a BIOS boot, or efivarfs not mounted) */
int mok_secure_boot(const char *efivars);
/* DKMS's public key from dir (mok.pub, with mok.key beside it), checked as above; the DER bytes into out. 0 ok. */
int mok_key_load(const char *dir, uint8_t *out, size_t cap, size_t *n);
/* 1 when the certificate's bytes are in one of the shim variables named <prefix>[digits]-605dab50-…: "MokListRT" (the
 * enrolled keys, split as MokListRT, MokListRT1… when long) or "MokNew" (the request waiting for MokManager) */
int mok_var_has(const char *efivars, const char *prefix, const uint8_t *der, size_t n);
/* the state from the variables and the key, as "status" reports it */
int mok_state(const char *efivars, const char *dkms_dir);
/* 8 random digits and a NUL into pw (getrandom, rejection-sampled); 0 ok */
int mok_password(char pw[MOK_PASSWORD_DIGITS + 1]);

void hw_act_dkms_mok_enroll(const AdminReq *r, const char *arg, size_t len);

#define MOK_ADMIN_ACTIONS \
    { "dkms-mok-enroll", hw_act_dkms_mok_enroll, 0 },

#endif
