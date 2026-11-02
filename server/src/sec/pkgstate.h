/* pkgstate.h — state of installed .lpk apps: the original package kept at install time,
 * "Modified by you" (comparing with the original's hashes), "Restore original",
 * "Share my version" and the update rule (same key). See docs/SIGNING.md. */
#ifndef LORD_SEC_PKGSTATE_H
#define LORD_SEC_PKGSTATE_H

#include "sign.h"
#include <stddef.h>
#include <stdint.h>

#define PKG_MAX_CHANGED 64

typedef struct {
    int installed, has_original, modified, nchanged;
    LpkSigInfo sig;                     /* of the saved original */
    char version[40];
    char changed[PKG_MAX_CHANGED][224];
    char change_kind[PKG_MAX_CHANGED];  /* 'm' modified, 'a' added, 'r' deleted */
    char error[160];                    /* the original doesn't verify (shouldn't happen) */
} PkgStatus;

/* ~/.local/share/axia/apps/.orig/<id>.lpk */
int pkg_original_path(const char *id, char *out, size_t n);
/* Copies the freshly installed .lpk as the original (atomically). */
int pkg_keep_original(const char *id, const char *lpk_path);
void pkg_forget(const char *id);        /* on uninstall */

/* State of an installed app (cached: recomputed if any file changed). */
int pkg_status(const char *id, PkgStatus *st);

/* Update rule (like Android): if the installed version is signed, the new one
 * has to be signed with the same key. 0 = can be installed. */
int pkg_check_update(const char *id, const LpkSigInfo *next, char *err, size_t n);

/* Reinstalls exactly the original. */
int pkg_restore(const char *id, char *err, size_t n);

/* Builds a new .lpk with the installed files (the user's version) and the given manifest
 * (with basedOn), signed with the user's own key, into out_path (doesn't overwrite). */
int pkg_share(const char *id, const char *manifest, size_t mlen, const char *out_path, char *err, size_t n);

/* The user's own key (~/.local/share/axia/keys/user.json, 0600): created the first time. */
int pkg_user_key(uint8_t seed[32], char pubhex[65], char name[128]);

#endif
