/* share_admin.h — the share-* admin actions (see share_admin.c, docs/FILES.md, "Network"). */
#ifndef LORD_LAN_SHARE_ADMIN_H
#define LORD_LAN_SHARE_ADMIN_H

#include "../admin.h"

void share_act_mount(const AdminReq *r, const char *arg, size_t len);
void share_act_unmount(const AdminReq *r, const char *arg, size_t len);

/* Both are A_ANYONE: the mount is the session user's own, under their own /media/<user>. */
#define SHARE_ADMIN_ACTIONS \
    { "share-mount", share_act_mount, A_FORK | A_ANYONE }, \
    { "share-unmount", share_act_unmount, A_FORK | A_ANYONE },

#endif
