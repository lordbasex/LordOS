/* drmlease.h — wp_drm_lease_device_v1 (drm-lease-v1): a VR headset's connector leased to an app the person allowed
 * (docs/WAYLAND.md, "DRM leases"; approved by the maintainer 2026-11-22). The decisions are drmlcore.h's; drmlease.c
 * speaks the protocol and makes the leases with the kernel's ioctls on lord-server's card. */
#ifndef LORD_WL_DRMLEASE_H
#define LORD_WL_DRMLEASE_H

struct wl_display;
struct wl_client;

/* What only wl.c knows of a client */
typedef struct {
    /* 1 if the client may ever lease: not sandboxed (Flatpak, a security context), not an isolated LordOS app, not
     * Xwayland (it speaks for every X11 app), not lord-server itself, and not on the login screen */
    int (*trusted)(struct wl_client *c);
    /* who lord-server checked the client is ("program:<path>"; "" when it can't be told) */
    const char *(*identity)(struct wl_client *c);
    /* the shell asks the person (sdk/wayland.js, "drmlease"): app's identity and the headset's name */
    void (*ask)(const char *app, const char *headset);
} DrmlHooks;

void drmlease_init(struct wl_display *dpy, const DrmlHooks *h);     /* wl_start */
/* main.c: the card lord-server is DRM master of, after it is attached and on every hotplug (outputs_apply); -1 when
 * the session leaves the monitor (display_detach), before the card is closed. The global exists only while that
 * card has a non-desktop connector plugged in. */
void drmlease_card(int fd);
/* lord.wayland.leaseAnswer: the person's answer ("once", "always" or "deny") to a prompt lord-server is waiting on for
 * app. 0 when no request of app waits (the answer is ignored and nothing is saved). */
int  drmlease_answer(const char *app, const char *how);
void drmlease_perm_changed(void);           /* a "vr" decision changed (lord.perms.set) */

#endif
