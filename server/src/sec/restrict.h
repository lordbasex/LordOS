/* restrict.h — the firewall's non-network restrictions, enforced where the resource is handed out
 * (docs/FIREWALL.md, "Non-network restrictions", and docs/PRIVACY.md).
 *
 * Settings → Privacy → "Apps' network" lets the owner restrict an app: "no camera", "no
 * microphone"… (FW_R_* in fw/rules.h, stored in firewall.json by lord.fw). A restriction is
 * stronger than any permission: a restricted app is denied without a prompt, even if the user
 * allowed the permission before. The network ones are applied in the kernel by axia-netperm; the
 * others are checked here, by the code that hands out the resource:
 *
 *   isolated apps (.lpk and Passwords), firewall id "lpk:<id>": the broker (brokersrv.c,
 *     brokercam.c) for camera, microphone, screen, USB (the FIDO2 key of Passwords), the file
 *     picker (powerbox) and reading the clipboard;
 *   built-in apps inside axia-server, firewall id "lordos": js_media.c (camera, microphone,
 *     screen capture and recording).
 *
 * Linux apps (desktop:<id>) are not sandboxed: they reach devices directly, so only the network
 * restrictions apply to them. Bluetooth and notifications have no app API yet.
 *
 * The store is lord.fw's (ui/js_fw.c, the one Settings edits), so a change is seen at once. */
#ifndef LORD_SEC_RESTRICT_H
#define LORD_SEC_RESTRICT_H

#include <stdint.h>
#include "perms.h"

struct FwStore;

/* The rule store to consult (js_fw.c sets it after loading firewall.json). NULL = no restrictions. */
void restrict_set_store(const struct FwStore *s);

/* Is the app with firewall id `fwid` ("lpk:<id>", "lordos"…) restricted in `flag` (FW_R_*)? */
int restrict_blocks(const char *fwid, uint32_t flag);
/* The same for an isolated app by its broker id (the verified package id, "lpk:" is added). */
int restrict_blocks_pkg(const char *id, uint32_t flag);

/* The restriction that covers permission p (FW_R_NO_CAMERA for PERM_CAMERA…), or 0. */
uint32_t restrict_flag_of(Perm p);
/* Error code of a denial by restriction ("fw.restricted.camera"…), or NULL for an unknown flag. */
const char *restrict_code(uint32_t flag);

/* The permission an isolated app effectively has: PERM_DENY when a restriction covers it,
 * otherwise the saved decision (perm_get). Every broker check uses this, never perm_get. */
PermState restrict_perm_pkg(const char *id, Perm p);

/* The restrictions of `fwid` changed from `before` to `after`: what was just restricted is taken
 * away from a running app, like revoking the permission (the camera stream and the microphone stop
 * now). Isolated apps through brokersrv_revoke; built-in apps through the hook js_media.c sets. */
void restrict_changed(const char *fwid, uint32_t before, uint32_t after);
typedef void (*RestrictLocalFn)(uint32_t added);
void restrict_set_local_hook(RestrictLocalFn fn);

#endif
