/* iconfind.h — the file for an icon name of a Linux app (a .desktop's Icon=, a tray item's IconName), in the
 * installed icon themes. apps.c uses it for the Dock, the launcher and the tray; build/iconfind-test checks the
 * order over a made-up tree.
 *
 * PNGs first, as before 0.8.8.8: hicolor (the user's, then the system's) at the sizes asked for, then
 * /usr/share/pixmaps. Then SVG, which lord-media draws (media/svgicon.c): scalable/ of every theme (hicolor, then
 * Adwaita, where most status icons only exist as SVG), /usr/share/pixmaps; an app's AppStream PNG; and last the
 * symbolic ones (symbolic/ and scalable/ "<name>-symbolic.svg"), which the bar recolors to its text color. */
#ifndef LORD_ICONFIND_H
#define LORD_ICONFIND_H

#include <limits.h>
#include <stddef.h>

#define ICONFIND_THEMES 3

typedef struct {
    char themes[ICONFIND_THEMES][PATH_MAX];     /* theme folders, in order */
    int nthemes, npng;                          /* the first npng are searched for PNGs too (hicolor) */
    char pixmaps[PATH_MAX];                     /* /usr/share/pixmaps */
    char appstream[PATH_MAX];                   /* the package engine's 64 px AppStream PNGs (<package>_<icon>.png) */
} IconRoots;

/* The usual folders: ~/.local/share/icons/hicolor (if home), /usr/share/icons/hicolor and /usr/share/icons/Adwaita,
 * each under prefix ("" outside tests). */
void iconfind_roots(IconRoots *r, const char *home, const char *prefix);

/* A .desktop's Icon=: a name (no "..") or an absolute .png/.svg path. A tray's IconName: a plain name of
 * [A-Za-z0-9._+-], at most 128 characters, looked for in status/ before apps/ at the bar's sizes. 1 if found
 * (out = the file), else 0 and out = "". */
int iconfind_app(const IconRoots *r, const char *name, char *out, size_t n);
int iconfind_status(const IconRoots *r, const char *name, char *out, size_t n);

/* Is the file (already through realpath) one lord-server may hand lord-media as an SVG icon? A .svg under the
 * system's or the user's icon folders, /usr/share, /usr/local/share or /opt (absolute Icon= paths). */
int iconfind_svg_allowed(const char *real, const char *home);

#endif
