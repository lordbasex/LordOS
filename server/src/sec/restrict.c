/* restrict.c — the firewall's non-network restrictions (see restrict.h). */
#include "restrict.h"
#include "broker.h"
#include "../fw/rules.h"

#include <stdio.h>
#include <string.h>

static const FwStore *store;
static RestrictLocalFn local_hook;

void restrict_set_store(const struct FwStore *s) { store = s; }
void restrict_set_local_hook(RestrictLocalFn fn) { local_hook = fn; }

int restrict_blocks(const char *fwid, uint32_t flag) {
    return store && fwid && flag && fw_blocks(store, fwid, flag);
}

int restrict_blocks_pkg(const char *id, uint32_t flag) {
    char fwid[FW_APP_MAX];
    if (!id || !id[0] || (size_t)snprintf(fwid, sizeof fwid, "lpk:%s", id) >= sizeof fwid) return 0;
    return restrict_blocks(fwid, flag);
}

uint32_t restrict_flag_of(Perm p) {
    switch (p) {
    case PERM_CAMERA:        return FW_R_NO_CAMERA;
    case PERM_MICROPHONE:    return FW_R_NO_MIC;
    case PERM_SCREEN:        return FW_R_NO_SCREEN;
    case PERM_USB:           return FW_R_NO_USB;
    case PERM_FILES:         return FW_R_NO_FILES;
    case PERM_CLIPBOARD:     return FW_R_NO_CLIPBOARD;
    case PERM_BLUETOOTH:     return FW_R_NO_BLUETOOTH;
    case PERM_NOTIFICATIONS: return FW_R_NO_NOTIFICATIONS;
    default:                 return 0;
    }
}

const char *restrict_code(uint32_t flag) {
    switch (flag) {
    case FW_R_NO_CAMERA:        return "fw.restricted.camera";
    case FW_R_NO_MIC:           return "fw.restricted.microphone";
    case FW_R_NO_SCREEN:        return "fw.restricted.screen";
    case FW_R_NO_USB:           return "fw.restricted.usb";
    case FW_R_NO_FILES:         return "fw.restricted.files";
    case FW_R_NO_CLIPBOARD:     return "fw.restricted.clipboard";
    case FW_R_NO_BLUETOOTH:     return "fw.restricted.bluetooth";
    case FW_R_NO_NOTIFICATIONS: return "fw.restricted.notifications";
    default:                    return NULL;
    }
}

PermState restrict_perm_pkg(const char *id, Perm p) {
    uint32_t f = restrict_flag_of(p);
    if (f && restrict_blocks_pkg(id, f)) return PERM_DENY;
    return perm_get(id, p);
}

void restrict_changed(const char *fwid, uint32_t before, uint32_t after) {
    uint32_t added = after & ~before;
    if (!fwid || !added) return;
    if (!strncmp(fwid, "lpk:", 4) && fwid[4]) {
        /* the same path as revoking the permission in Privacy: the app is told, the camera's
         * axia-media is killed and the microphone stream is closed */
        if (added & FW_R_NO_CAMERA) brokersrv_revoke(fwid + 4, "camera");
        if (added & FW_R_NO_MIC) brokersrv_revoke(fwid + 4, "microphone");
    } else if (!strcmp(fwid, "lordos") && local_hook) local_hook(added);
}
