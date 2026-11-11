/* toolkitenv_test.c — the toolkit variables the session gives Linux apps (src/wl/toolkitenv.h):
 * Wayland first for GTK and Qt, each with X11 as a fallback, so a Qt app without Qt's Wayland
 * plugin opens through Xwayland instead of stopping at start (GitHub #96), and a GTK program that
 * allows only x11 still opens (GitHub #76); GTK_IM_MODULE is kept when the person set one. */
#include "wl/toolkitenv.h"

#include <stdio.h>
#include <string.h>

static int n, bad;
#define CHECK(name, cond) do { n++; if (!(cond)) { bad++; printf("✗ %s\n", name); } } while (0)

/* Is item one of the entries of a sep-separated list? (Qt splits QT_QPA_PLATFORM on ';', GTK GDK_BACKEND on ',') */
static int has(const char *list, char sep, const char *item) {
    size_t l = strlen(item);
    for (const char *p = list; p && *p; ) {
        const char *e = strchr(p, sep);
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == l && !strncmp(p, item, l)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

int main(void) {
    setenv("QT_QPA_PLATFORM", "eglfs", 1);
    unsetenv("GTK_IM_MODULE");
    toolkit_env();
    const char *qt = getenv("QT_QPA_PLATFORM"), *gdk = getenv("GDK_BACKEND");
    CHECK("Qt: Wayland first", qt && !strncmp(qt, "wayland;", 8));
    CHECK("Qt: xcb as the fallback (#96)", qt && has(qt, ';', "xcb"));
    CHECK("Qt: no ',' (Qt reads ';' only)", qt && !strchr(qt, ','));
    CHECK("GTK: Wayland first", gdk && !strncmp(gdk, "wayland,", 8));
    CHECK("GTK: x11 as the fallback (#76)", gdk && has(gdk, ',', "x11"));
    CHECK("Firefox on Wayland", getenv("MOZ_ENABLE_WAYLAND") && !strcmp(getenv("MOZ_ENABLE_WAYLAND"), "1"));
    CHECK("GTK_IM_MODULE set when unset", getenv("GTK_IM_MODULE") && !strcmp(getenv("GTK_IM_MODULE"), "wayland"));
    setenv("GTK_IM_MODULE", "ibus", 1);
    toolkit_env();
    CHECK("GTK_IM_MODULE kept when the person set one", !strcmp(getenv("GTK_IM_MODULE"), "ibus"));
    printf(bad ? "toolkitenv: %d of %d failed\n" : "toolkitenv: %d/%d ok\n", bad ? bad : n, n);
    return bad ? 1 : 0;
}
