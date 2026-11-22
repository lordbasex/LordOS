/* tltag.c — xdg-toplevel-tag-v1: an app names each kind of window it opens (docs/WAYLAND.md, "Window tags").
 *
 * set_toplevel_tag gives a window an untranslated tag ("main window", "preferences", "e-mail composer"),
 * set_toplevel_description a translated description of it. LordOS keeps both per window and the shell uses the tag
 * to key where it remembers that window (sdk/windowmemory.js: an app's windows with different tags each reopen in
 * their own place, instead of every window of the app taking the last one's), and the description as the window's
 * name while it has no title. Both are the app's own words: kept only as text, cut to a whole character
 * (TAG_MAX, DESC_MAX bytes of valid UTF-8, control characters out), never matched against anything but that same
 * app's windows (the shell prefixes the app's Dock id). Set before the first frame they arrive with the window
 * (the shell reads them at "map", tltag_get); changed later, a "tag" event says so. */
#include "wlshell.h"
#include "tiutf8.h"
#include "xdg-toplevel-tag-v1-protocol.h"
#include "xdg-shell-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#define TAG_MAX 127
#define DESC_MAX 255

typedef struct {
    struct wl_resource *top;
    struct wl_listener gone;
    int id;
    char tag[TAG_MAX + 1], desc[DESC_MAX + 1];
    struct wl_list link;
} Tag;

static struct wl_list tags;

/* s cut to at most max bytes of valid UTF-8 on a character boundary, control characters as spaces, ends trimmed */
static void clean(char *out, size_t max, const char *s) {
    size_t n = tiu_valid_prefix(s ? s : "", max);
    memcpy(out, s ? s : "", n);
    out[n] = 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)out[i] < 0x20 || out[i] == 0x7f) out[i] = ' ';
    while (n && out[n - 1] == ' ') out[--n] = 0;
    size_t lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead) memmove(out, out + lead, n - lead + 1);
}

static void on_gone(struct wl_listener *l, void *data) {
    (void)data;
    Tag *t = wl_container_of(l, t, gone);
    wl_list_remove(&t->link);
    free(t);
}

static Tag *of(int id) {
    Tag *t;
    wl_list_for_each(t, &tags, link) if (t->id == id) return t;
    return NULL;
}

static Tag *for_toplevel(struct wl_resource *top) {
    int id = wl__toplevel_id(top);
    if (!id) return NULL;               /* inert: its surface is gone */
    Tag *t;
    wl_list_for_each(t, &tags, link) if (t->top == top) return t;
    t = calloc(1, sizeof *t);
    if (!t) { wl_client_post_no_memory(wl_resource_get_client(top)); return NULL; }
    t->top = top;
    t->id = id;
    t->gone.notify = on_gone;
    wl_resource_add_destroy_listener(top, &t->gone);
    wl_list_insert(&tags, &t->link);
    return t;
}

/* A change on a mapped window is told ("tag"); before the map the shell reads it with the window (tltag_get). An event
 * carries at most 600 bytes: a description that doesn't fit beside the tag (quotes escaped) goes as "". */
static void announce(Tag *t) {
    int m = 0, c = 0, top = 0;
    if (!wl__surface_state(t->id, &m, &c, &top) || !m) return;
    char a[TAG_MAX * 2 + 8], b[DESC_MAX * 2 + 8], j[600];
    wl__json_str(a, sizeof a, t->tag);
    wl__json_str(b, sizeof b, t->desc);
    if (snprintf(j, sizeof j, "{\"tag\":\"%s\",\"description\":\"%s\"}", a, b) >= (int)sizeof j)
        snprintf(j, sizeof j, "{\"tag\":\"%s\",\"description\":\"\"}", a);
    wl__emit(t->id, "tag", j);
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void mgr_tag(struct wl_client *c, struct wl_resource *r, struct wl_resource *toplevel, const char *tag) {
    (void)c; (void)r;
    Tag *t = for_toplevel(toplevel);
    if (!t) return;
    char v[TAG_MAX + 1];
    clean(v, TAG_MAX, tag);
    if (!strcmp(v, t->tag)) return;
    memcpy(t->tag, v, sizeof v);
    announce(t);
}
static void mgr_desc(struct wl_client *c, struct wl_resource *r, struct wl_resource *toplevel, const char *desc) {
    (void)c; (void)r;
    Tag *t = for_toplevel(toplevel);
    if (!t) return;
    char v[DESC_MAX + 1];
    clean(v, DESC_MAX, desc);
    if (!strcmp(v, t->desc)) return;
    memcpy(t->desc, v, sizeof v);
    announce(t);
}
static const struct xdg_toplevel_tag_manager_v1_interface mgr_impl = {
    .destroy = mgr_destroy, .set_toplevel_tag = mgr_tag, .set_toplevel_description = mgr_desc,
};
static void bind_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_toplevel_tag_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

void tltag_globals(struct wl_display *dpy) {
    wl_list_init(&tags);
    wl_global_create(dpy, &xdg_toplevel_tag_manager_v1_interface, 1, NULL, bind_mgr);
}

void tltag_mapped(int id) { (void)id; }   /* what it had comes with the map: the shell asks (tltag_get) */

int tltag_get(int id, const char **tag, const char **description) {
    Tag *t = of(id);
    if (!t || (!t->tag[0] && !t->desc[0])) return 0;
    *tag = t->tag;
    *description = t->desc;
    return 1;
}
