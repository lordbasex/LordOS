/* sgrant_admin.h — the share-grant action: the root-owned store of the screen-sharing API (sharegrant.h). */
#ifndef LORD_REMOTE_SGRANT_ADMIN_H
#define LORD_REMOTE_SGRANT_ADMIN_H

#include "../admin.h"

void sgrant_act(const AdminReq *r, const char *arg, size_t len);
void sgrant_act_forget(const AdminReq *r, const char *arg, size_t len);

/* A_ANYONE: it acts only on the session user's own file. The helper is reachable only from lord-server,
 * and only the shell's context has lord.admin: an isolated app can't call it, nor can a Linux app. */
#define SGRANT_ADMIN_ACTIONS \
    { "share-grant", sgrant_act, A_ANYONE }, \
    { "share-forget", sgrant_act_forget, A_ANYONE },

#endif
