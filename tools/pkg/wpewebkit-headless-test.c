/* wpewebkit-headless-test.c — the smoke test of tools/pkg/build-wpewebkit.sh (docs/WPEWEBKIT.md): a WebKitWebView on
 * WPEPlatform's headless display loads a page with a known colour, and the frame WebKit hands to the view
 * ("buffer-rendered") is read back to pixels and checked. It proves that the backport's WPEPlatform works end to end
 * (UI process, web process in its sandbox, rendering, buffer to the platform) without a compositor or a GPU.
 *
 *   wpe-headless-render <out.ppm>      exit 0 when the centre pixel is the page's colour; writes the frame as a PPM
 *
 * Built in the test chroot with: pkg-config wpe-webkit-2.0 wpe-platform-2.0 wpe-platform-headless-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>

#define WIDTH 320
#define HEIGHT 200

static GMainLoop *loop;
static const char *out_path;
static gboolean loaded;
static int result = 1;

static gboolean on_timeout(gpointer data)
{
    (void)data;
    fprintf(stderr, "headless: no frame with the page's colour within the time limit\n");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

/* the frame's pixels: WPE's SHM and imported buffers are 32-bit, little-endian ARGB/XRGB (B, G, R, A in memory) */
static void on_buffer_rendered(WPEView *view, WPEBuffer *buffer, gpointer data)
{
    (void)view; (void)data;
    if (!loaded)
        return;
    GError *error = NULL;
    GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &error);
    if (!bytes) {
        fprintf(stderr, "headless: wpe_buffer_import_to_pixels: %s\n", error ? error->message : "?");
        g_clear_error(&error);
        return;
    }
    int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
    gsize size = 0;
    const guint8 *px = g_bytes_get_data(bytes, &size);
    if (w <= 0 || h <= 0 || size < (gsize)w * h * 4) {
        fprintf(stderr, "headless: frame %dx%d with %zu bytes\n", w, h, (size_t)size);
        return;
    }
    gsize stride = size / h;
    const guint8 *c = px + (gsize)(h / 2) * stride + (gsize)(w / 2) * 4;
    printf("headless: frame %dx%d (%s), centre pixel r=%u g=%u b=%u\n", w, h, G_OBJECT_TYPE_NAME(buffer), c[2], c[1], c[0]);
    if (c[2] == 0x20 && c[1] == 0x80 && c[0] == 0xc0) {
        FILE *f = fopen(out_path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const guint8 *p = px + (gsize)y * stride + (gsize)x * 4;
                    guint8 rgb[3] = { p[2], p[1], p[0] };
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
        printf("headless: OK, the page was rendered into the view's buffer (%s)\n", out_path);
        result = 0;
        g_main_loop_quit(loop);
    }
    g_bytes_unref(bytes);
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event, gpointer data)
{
    (void)web_view; (void)data;
    if (event == WEBKIT_LOAD_FINISHED) {
        printf("headless: page loaded\n");
        loaded = TRUE;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out.ppm>\n", argv[0]);
        return 2;
    }
    out_path = argv[1];
    printf("headless: WPE WebKit %u.%u.%u, WPEPlatform %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(),
           webkit_get_micro_version(), wpe_platform_get_major_version(), wpe_platform_get_minor_version(), wpe_platform_get_micro_version());

    WPEDisplay *display = wpe_display_headless_new();
    GError *error = NULL;
    if (!wpe_display_connect(display, &error)) {
        fprintf(stderr, "headless: wpe_display_connect: %s\n", error ? error->message : "?");
        return 1;
    }
    loop = g_main_loop_new(NULL, FALSE);
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, NULL));
    WPEView *view = webkit_web_view_get_wpe_view(web_view);
    if (!view) {
        fprintf(stderr, "headless: the web view has no WPEView (WPEPlatform not in use)\n");
        return 1;
    }
    WPEToplevel *toplevel = wpe_view_get_toplevel(view);
    if (toplevel)
        wpe_toplevel_resize(toplevel, WIDTH, HEIGHT);
    g_signal_connect(view, "buffer-rendered", G_CALLBACK(on_buffer_rendered), NULL);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    webkit_web_view_load_html(web_view,
        "<!doctype html><html><body style='margin:0;background:#2080c0'><p>LordOS</p></body></html>", "about:blank");
    g_timeout_add_seconds(90, on_timeout, NULL);
    g_main_loop_run(loop);
    return result;
}
