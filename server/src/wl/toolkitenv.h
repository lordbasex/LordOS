/* toolkitenv.h — the toolkit variables the session sets for the Linux apps it starts (wl.c, wl_init),
 * inherited by apps opened from the launcher and the LordOS terminal (docs/WAYLAND.md, "How to open an app").
 *
 * Wayland first, with X11 (Xwayland) as the fallback for a program that can't use Wayland:
 * - GTK: "wayland,x11". With "wayland" alone GTK skips x11, and a program that allows only x11
 *   (gdk_set_allowed_backends: VMware's vmware-user agent) can't open any display (GitHub #76).
 * - Qt: "wayland;xcb". With "wayland" alone a Qt app whose Wayland platform plugin is not installed
 *   (Debian's qt6-wayland / qtwayland5, which no Qt app depends on) stops at start: MusicBrainz Picard
 *   from the Store opened nothing (GitHub #96). Qt tries the list in order.
 * - Qt's platform theme: "gnome", QGnomePlatform (Debian's qgnomeplatform-qt5 and qgnomeplatform-qt6, Recommends of
 *   the lordos package). Without a platform theme Qt 5 and Qt 6 drew light on a dark desktop: Qt 5 has no colour
 *   scheme at all, and Qt 6.8's own "gnome" theme and generic theme don't read the portal's. QGnomePlatform reads
 *   LordOS's dark mode from the portal and GSettings (ui/portal.c) and gives both an Adwaita palette that follows it,
 *   live. Where it isn't installed Qt falls back to its own "gnome" theme, as before. The person's own value (qt5ct,
 *   kde, ...) is kept. docs/TOOLKIT-AUDIT.md. */
#ifndef LORD_WL_TOOLKITENV_H
#define LORD_WL_TOOLKITENV_H

#include <stdlib.h>

#define TOOLKITENV_GDK "wayland,x11"
#define TOOLKITENV_QT "wayland;xcb"
#define TOOLKITENV_QT_THEME "gnome"

static inline void toolkit_env(void) {
    setenv("GDK_BACKEND", TOOLKITENV_GDK, 1);
    setenv("QT_QPA_PLATFORM", TOOLKITENV_QT, 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("QT_QPA_PLATFORMTHEME", TOOLKITENV_QT_THEME, 0);
    /* No GTK_IM_MODULE here: every GTK reads it, also GTK 3 running on X11 inside Electron and Chromium, which then
     * loaded the Wayland input-method module on an X11 display and crashed (GitHub #150). GTK 4 on Wayland gets
     * "wayland" from the Settings portal's gtk-im-module instead (ui/portal.c), and an IBus or Fcitx user's own
     * GTK_IM_MODULE passes through untouched. */
}

#endif
