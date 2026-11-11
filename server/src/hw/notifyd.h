/* notifyd.h — org.freedesktop.Notifications on the session bus, so notifications from Linux apps
 * (GTK, Qt, Electron, notify-send) appear as LordOS banners and follow Settings → Notifications
 * (per-app style, sound, Do Not Disturb). The banner is drawn by the theme (sdk/notify.js); this
 * only translates the protocol (spec 1.2): Notify, CloseNotification, GetCapabilities,
 * GetServerInformation, and the NotificationClosed / ActionInvoked signals. Body markup is reduced
 * to plain text. Uses our own D-Bus client (vault/dbus.c). Chromium and Electron use the service
 * only if GetCapabilities has "body" and "actions" and the spec version is 1.1 or later; otherwise
 * they draw their own popups (docs/WAYLAND.md, "Notifications of Linux apps"). Image hints
 * (image-data, image-path) are read past.
 *
 * The firewall's "no notifications" (docs/FIREWALL.md, "Linux apps"): before showing a Notify,
 * the bus is asked for the caller's PID (GetConnectionUnixProcessID, answered by the bus from the
 * socket's credentials, not by the app), the PID's cgroup gives its app scope and so its firewall
 * id; if that app is restricted the notification is dropped and the call still gets a valid id,
 * so the app carries on as if it had been shown. Only the bus's own answer counts; an error, a
 * process that can't be read or no answer within 3 s also drop it (fail closed). Applies to the
 * next notification. */
#ifndef LORD_HW_NOTIFYD_H
#define LORD_HW_NOTIFYD_H

#include <stddef.h>
#include <stdint.h>

typedef void (*NotifydEvent)(const char *kind, const char *json);   /* "notify", "notify-close" */

int  notifyd_start(NotifydEvent fn);    /* 0 if we own the name */
int  notifyd_fd(void);
int  notifyd_wants_write(void);
void notifyd_dispatch(void);
/* Called on every pass of the main loop: drops Notify calls the bus didn't answer in time. */
void notifyd_tick(void);
/* From the theme: the banner went away (reason: 1 expired, 2 dismissed, 3 closed by a call) or
 * an action was clicked. */
void notifyd_closed(uint32_t id, uint32_t reason);
void notifyd_action(uint32_t id, const char *key);
/* Tests only: replaces the PID → firewall id lookup (/proc/<pid>/cgroup). fn returns 0 with the
 * id, 1 if the PID is in no app's scope, -1 if it can't tell. */
void notifyd_set_app_of_pid(int (*fn)(uint32_t pid, char *out, size_t n));
/* Strips markup (<b>, <i>, <a href>, <img>...) and decodes &amp; &lt; &gt; &quot; &apos;. */
void notifyd_plain(const char *in, char *out, size_t n);

#endif
