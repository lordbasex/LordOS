/* toolkitenv.h — the toolkit variables the session sets for the Linux apps it starts (wl.c, wl_init),
 * inherited by apps opened from the launcher and the LordOS terminal (docs/WAYLAND.md, "How to open an app").
 *
 * Wayland first, with X11 (Xwayland) as the fallback for a program that can't use Wayland:
 * - GTK: "wayland,x11". With "wayland" alone GTK skips x11, and a program that allows only x11
 *   (gdk_set_allowed_backends: VMware's vmware-user agent) can't open any display (GitHub #76).
 * - Qt: "wayland;xcb". With "wayland" alone a Qt app whose Wayland platform plugin is not installed
 *   (Debian's qt6-wayland / qtwayland5, which no Qt app depends on) stops at start: MusicBrainz Picard
 *   from the Store opened nothing (GitHub #96). Qt tries the list in order. */
#ifndef LORD_WL_TOOLKITENV_H
#define LORD_WL_TOOLKITENV_H

#include <stdlib.h>

#define TOOLKITENV_GDK "wayland,x11"
#define TOOLKITENV_QT "wayland;xcb"

static inline void toolkit_env(void) {
    setenv("GDK_BACKEND", TOOLKITENV_GDK, 1);
    setenv("QT_QPA_PLATFORM", TOOLKITENV_QT, 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    /* GTK 4.18 doesn't pick its text-input-v3 module by itself here (no input-method framework running), and the
     * keys then reach the app raw: LordOS's input methods compose only through it. Not overwritten when the person
     * set another one (an IBus or Fcitx user) */
    setenv("GTK_IM_MODULE", "wayland", 0);
}

#endif
