/* apphost_bc.h — the app host compiled to QuickJS bytecode once per session (see apphost_bc.c). */
#ifndef LORD_APPHOST_BC_H
#define LORD_APPHOST_BC_H

#include "quickjs.h"

#define APPHOST_BC_ENV "LORD_APPHOST_BC_FD"

/* axia-server: read <bundle_path> once through the verified-code check (codesig_load, sec/codesig.c)
 * and compile exactly those bytes on a thread of its own, a few seconds from now. Nothing is
 * compiled if the check refuses the file. */
void apphost_bc_start(const char *bundle_path);
/* axia-server: a read-only fd of the bytecode for a new app process, or -1 if not ready yet. */
int  apphost_bc_dup_ro(void);
/* The app process: run the bytecode in fd if it was compiled from exactly src[0..len) (the bytes the
 * app process itself loaded and verified: same size and SHA-256).
 * -1: not usable (parse the source instead); 0: ran (*ok = 1); 1: ran and threw (*ok = 0, the
 * exception is pending in ctx). */
int  apphost_bc_eval(JSContext *ctx, const char *src, size_t len, int fd, int *ok);

#endif
