/* xsettings.h — the _XSETTINGS_SETTINGS property LordOS's X window manager serves (xwm.c), so GTK 3 running on X11
 * (Xwayland) follows LordOS's light or dark mode, title bar buttons, font and icons as it does on Wayland.
 *
 * GTK 3 on X11 reads none of the portal or GSettings: only XSETTINGS (the freedesktop.org XSETTINGS specification,
 * 0.5). Without an owner of _XSETTINGS_S0 it kept its built-in Adwaita (light), whatever LordOS's mode was, and the
 * toolkit audit's gtk3-x11 theme check failed (docs/TOOLKIT-AUDIT.md). The values are the ones ui/portal.c gives
 * GTK through the portal and the GSettings keyfile (portal_gtk_values).
 *
 * The property, in the byte order of the property itself (LSBFirst here):
 *   CARD8 byte order, 3 unused, CARD32 serial, CARD32 number of settings, then each setting:
 *   CARD8 type (0 integer, 1 string, 2 colour), 1 unused, CARD16 name length, the name padded to 4,
 *   CARD32 last-change serial, then a string's CARD32 length and bytes padded to 4 (an integer: INT32).
 * Pure and bounded: the test (tests/xsettings_test.c) decodes what it builds. */
#ifndef LORD_WL_XSETTINGS_H
#define LORD_WL_XSETTINGS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { const char *name, *value; } XsString;

/* Writes the property for n string settings (names and values at most 255 bytes; a longer or NULL one is left
 * out) into out. Returns its length, or 0 when it doesn't fit in cap. */
static inline size_t xsettings_build(uint8_t *out, size_t cap, uint32_t serial, const XsString *s, int n) {
    size_t at = 12;
    uint32_t count = 0;
    if (cap < 12) return 0;
    for (int i = 0; i < n; i++) {
        if (!s[i].name || !s[i].value) continue;
        size_t nl = strlen(s[i].name), vl = strlen(s[i].value);
        if (nl == 0 || nl > 255 || vl > 255) continue;
        size_t need = 4 + ((nl + 3) & ~(size_t)3) + 4 + 4 + ((vl + 3) & ~(size_t)3);
        if (at + need > cap) return 0;
        uint8_t *p = out + at;
        memset(p, 0, need);
        p[0] = 1;                                           /* XSettingsTypeString */
        p[2] = (uint8_t)nl; p[3] = (uint8_t)(nl >> 8);
        memcpy(p + 4, s[i].name, nl);
        p += 4 + ((nl + 3) & ~(size_t)3);
        for (int b = 0; b < 4; b++) p[b] = (uint8_t)(serial >> (8 * b));       /* last change */
        for (int b = 0; b < 4; b++) p[4 + b] = (uint8_t)(vl >> (8 * b));
        memcpy(p + 8, s[i].value, vl);
        at += need;
        count++;
    }
    memset(out, 0, 12);
    out[0] = 0;                                             /* LSBFirst */
    for (int b = 0; b < 4; b++) out[4 + b] = (uint8_t)(serial >> (8 * b));
    for (int b = 0; b < 4; b++) out[8 + b] = (uint8_t)(count >> (8 * b));
    return at;
}

#endif
