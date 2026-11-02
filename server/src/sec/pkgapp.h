/* pkgapp.h — packaged apps: the third, read-only app root, /usr/lib/lordos-apps/<id>/ (docs/GO-LINK.md, 3.2).
 * Installed by a .deb, with the layout of an unpacked .lpk. Never system code: it isn't in code-manifest.json and it
 * runs in the usual sandbox. A folder there counts only if root owns it and no one else can write it, and it loads
 * only signed by a key LordOS's signed catalog lists as a Verified author, at every load. Only here does the
 * manifest's "companion" mean anything; the same manifest in a home .lpk is ignored. */
#ifndef LORD_SEC_PKGAPP_H
#define LORD_SEC_PKGAPP_H

#include "sign.h"
#include <stddef.h>

#define PKGAPP_ROOT "/usr/lib/lordos-apps"
#define PKGAPP_MANIFEST_MAX (256 * 1024)

typedef struct {
    char version[40];
    char companion[224];                /* the binary, a path inside the package; "" if none */
    int network, screen, input;         /* declared: network, screen-share, remote-input */
} PkgManifest;

const char *pkgapp_root(void);
/* <root>/<id>, if it is a safe folder with a manifest. 0, or -1. Takes no time worth measuring: main loop. */
int pkgapp_dir(const char *id, char *out, size_t n);
/* Reads and checks the manifest of the package in dir: well formed, this id. 0, or -1. Not its signature. */
int pkgapp_manifest(const char *dir, const char *id, PkgManifest *m);
/* The check at load: the folder as above, every file against contenido.json, the signature, a Verified author's
 * key, not revoked. It hashes every file, so it never runs on the main loop. bin_fd (or NULL): the companion, opened
 * once, the descriptor that was hashed. 0, or -1 with a code in err. */
int pkgapp_verify(const char *id, PkgManifest *m, LpkSigInfo *si, int *bin_fd, char *err, size_t n);

#endif
