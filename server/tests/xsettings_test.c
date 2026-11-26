/* xsettings_test.c — the _XSETTINGS_SETTINGS property the X window manager serves to GTK 3 on X11
 * (src/wl/xsettings.h): decoded here the way GTK's own reader (gdk/x11/xsettings-client.c) does it, field by field,
 * with its checks: byte order, padding to 4, the number of settings, a string's length, nothing past the end. */
#include "wl/xsettings.h"

#include <stdio.h>
#include <string.h>

static int n, bad;
#define CHECK(name, cond) do { n++; if (!(cond)) { bad++; printf("✗ %s\n", name); } } while (0)

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct { char name[256], value[256]; uint32_t serial; } Got;

/* -1 when malformed, else the number of settings read into g */
static int decode(const uint8_t *b, size_t len, uint32_t *serial, Got *g, int max) {
    if (len < 12 || b[0] != 0) return -1;
    *serial = rd32(b + 4);
    uint32_t cnt = rd32(b + 8);
    size_t at = 12;
    for (uint32_t i = 0; i < cnt; i++) {
        if (at + 4 > len || (int)i >= max) return -1;
        if (b[at] != 1) return -1;                          /* only strings are served */
        size_t nl = b[at + 2] | (size_t)b[at + 3] << 8, np = (nl + 3) & ~(size_t)3;
        if (at + 4 + np + 8 > len) return -1;
        memcpy(g[i].name, b + at + 4, nl); g[i].name[nl] = 0;
        for (size_t k = nl; k < np; k++) if (b[at + 4 + k]) return -1;
        at += 4 + np;
        g[i].serial = rd32(b + at);
        size_t vl = rd32(b + at + 4), vp = (vl + 3) & ~(size_t)3;
        if (vl > 255 || at + 8 + vp > len) return -1;
        memcpy(g[i].value, b + at + 8, vl); g[i].value[vl] = 0;
        for (size_t k = vl; k < vp; k++) if (b[at + 8 + k]) return -1;
        at += 8 + vp;
    }
    return at == len ? (int)cnt : -1;
}

int main(void) {
    uint8_t buf[1024];
    Got g[8];
    uint32_t serial = 0;
    XsString v[] = { { "Net/ThemeName", "LordOS-dark" }, { "Net/IconThemeName", "Adwaita" },
                     { "Gtk/DecorationLayout", "close,minimize,maximize:" }, { "Gtk/FontName", "Inter 10" } };
    size_t len = xsettings_build(buf, sizeof buf, 7, v, 4);
    CHECK("built", len > 12);
    CHECK("a multiple of 4", len % 4 == 0);
    int k = decode(buf, len, &serial, g, 8);
    CHECK("four settings decoded", k == 4);
    CHECK("serial", serial == 7);
    CHECK("Net/ThemeName", k == 4 && !strcmp(g[0].name, "Net/ThemeName") && !strcmp(g[0].value, "LordOS-dark"));
    CHECK("Gtk/DecorationLayout", k == 4 && !strcmp(g[2].name, "Gtk/DecorationLayout") && !strcmp(g[2].value, "close,minimize,maximize:"));
    CHECK("Gtk/FontName", k == 4 && !strcmp(g[3].value, "Inter 10"));
    CHECK("last-change serial", k == 4 && g[1].serial == 7);

    /* light mode: a new serial, the light theme (GTK compares serials to see a change) */
    XsString l[] = { { "Net/ThemeName", "LordOS" } };
    len = xsettings_build(buf, sizeof buf, 8, l, 1);
    k = decode(buf, len, &serial, g, 8);
    CHECK("light: one setting, serial 8", k == 1 && serial == 8 && !strcmp(g[0].value, "LordOS"));

    /* every padding length, an empty value */
    const char *names[] = { "A", "Ab", "Abc", "Abcd", "Abcde" };
    for (int i = 0; i < 5; i++) {
        XsString one[] = { { names[i], names[4 - i] }, { "E", "" } };
        len = xsettings_build(buf, sizeof buf, 1, one, 2);
        k = decode(buf, len, &serial, g, 8);
        CHECK("padding", k == 2 && !strcmp(g[0].name, names[i]) && !strcmp(g[0].value, names[4 - i]) && g[1].value[0] == 0);
    }

    /* left out: NULL, empty names, values over 255 bytes */
    char big[300];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    XsString odd[] = { { NULL, "a" }, { "N", NULL }, { "", "a" }, { "Big", big }, { "Ok", "1" } };
    len = xsettings_build(buf, sizeof buf, 3, odd, 5);
    k = decode(buf, len, &serial, g, 8);
    CHECK("odd entries left out", k == 1 && !strcmp(g[0].name, "Ok"));

    /* too small a buffer: 0, never a partial property */
    CHECK("no room", xsettings_build(buf, 20, 1, v, 4) == 0);
    CHECK("no room for the header", xsettings_build(buf, 8, 1, v, 0) == 0);
    CHECK("no settings", xsettings_build(buf, sizeof buf, 1, v, 0) == 12 && rd32(buf + 8) == 0);

    printf(bad ? "xsettings: %d of %d failed\n" : "xsettings: %d/%d ok\n", bad ? bad : n, n);
    return bad ? 1 : 0;
}
