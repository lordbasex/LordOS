/* wl_textinput.c — zwp_text_input_v3: a Linux app tells the compositor that a text field took focus
 * and where its caret is, and LordOS's input method (ime.c) composes into it. The on-screen keyboard
 * uses the same hint to open and keep the field in view (docs/ON-SCREEN-KEYBOARD.md).
 *
 * While the field is enabled and an input method is on, keys go through ime.c before the app
 * (ui/js.c): the composition reaches the app as preedit_string (cursor in bytes over the focused
 * segment, or at the end), the finished text as commit_string, each batch closed by done with the
 * number of commits the app has sent on that object, as the protocol counts. A done is sent only with
 * something in it: answering every commit would loop with apps that commit again on each done. The
 * candidate window is drawn by the shell at the app's cursor rectangle (wl_text_caret in wl.c). A
 * password or PIN field, or one marked hidden, sensitive or Latin, is never composed: keys go straight.
 * Disabling the field, enabling it again (another widget) or losing the keyboard discards the
 * composition. Everything the app sends is clamped (tiutf8.c): its text and offsets to valid UTF-8 and
 * character boundaries, its rectangle to sane coordinates. */
#include "wl.h"
#include "wl_input.h"
#include "tiutf8.h"
#include "ime.h"
#include "text-input-unstable-v3-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

enum { TEXT_MAX = 511 };                /* the most of a preedit or commit sent (ime.c composes less) */

typedef struct {
    struct wl_resource *res;
    struct wl_client *client;
    struct wl_list link;
    int entered;                        /* on the surface with the keyboard */
    int want, active;                   /* pending enable, enabled since the last commit */
    int fresh;                          /* enable was asked since the last commit: a new field */
    uint32_t commits;                   /* commit requests so far: the serial for done */
    int rect[4], p_rect[4];             /* cursor rectangle (surface coordinates), committed and pending */
    int has_rect, p_has_rect;
    uint32_t hint, purpose, p_hint, p_purpose;
    /* surrounding text: committed and pending, cursor and anchor in bytes; known = 0 without one */
    char sur[TIU_SURROUNDING + 1], p_sur[TIU_SURROUNDING + 1];
    int cur, anc, p_cur, p_anc, known, p_known;
    int composed;                       /* a preedit was sent and not yet cleared or committed */
} TextInput;

static struct wl_list inputs;
static struct wl_resource *focus_surface;
static struct wl_client *focus_client;
static void (*report)(int on, int x, int y, int w, int h);

static void notify(const TextInput *ti, int on) { if (report) report(on, ti->rect[0], ti->rect[1], ti->rect[2], ti->rect[3]); }

/* State the protocol resets on enable (and the pending copy of it). */
static void reset_pending(TextInput *ti) {
    memset(ti->p_rect, 0, sizeof ti->p_rect);
    ti->p_has_rect = 0;
    ti->p_hint = ti->p_purpose = 0;
    ti->p_sur[0] = 0;
    ti->p_cur = ti->p_anc = ti->p_known = 0;
}

/* The composition ends without reaching the app (the field went away): the input method forgets it. */
static void drop_composition(TextInput *ti) {
    if (ti->composed) ime_reset();
    ti->composed = 0;
}

/* A field that takes the input method: enabled, on the surface with the keyboard, not secret or Latin. */
static int composable(const TextInput *ti) {
    if (!ti->entered || !ti->active) return 0;
    if (ti->purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD || ti->purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN) return 0;
    return !(ti->hint & (ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT | ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA |
                         ZWP_TEXT_INPUT_V3_CONTENT_HINT_LATIN));
}

static void ti_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void ti_enable(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    ti->want = 1;
    ti->fresh = 1;
    reset_pending(ti);
}
static void ti_disable(struct wl_client *c, struct wl_resource *r) { (void)c; ((TextInput *)wl_resource_get_user_data(r))->want = 0; }
static void ti_surrounding(struct wl_client *c, struct wl_resource *r, const char *t, int32_t cursor, int32_t anchor) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    ti->p_known = tiu_surrounding(t, cursor, anchor, ti->p_sur, sizeof ti->p_sur, &ti->p_cur, &ti->p_anc);
}
static void ti_cause(struct wl_client *c, struct wl_resource *r, uint32_t v) { (void)c; (void)r; (void)v; }
static void ti_content(struct wl_client *c, struct wl_resource *r, uint32_t hint, uint32_t purpose) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    ti->p_hint = hint;
    ti->p_purpose = purpose;
}
static void ti_cursor(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    tiu_rect(x, y, w, h, ti->p_rect);
    ti->p_has_rect = 1;
}
static void ti_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    int was = ti->active, moved = ti->rect[0] != ti->p_rect[0] || ti->rect[1] != ti->p_rect[1] || ti->rect[3] != ti->p_rect[3];
    int other = ti->fresh && ti->want && was;              /* enabled again: another widget took over */
    ti->commits++;
    if (!ti->want || ti->fresh) drop_composition(ti);       /* disabled, or another field enabled */
    ti->fresh = 0;
    ti->active = ti->want;
    memcpy(ti->rect, ti->p_rect, sizeof ti->rect);
    ti->has_rect = ti->p_has_rect;
    ti->hint = ti->p_hint;
    ti->purpose = ti->p_purpose;
    memcpy(ti->sur, ti->p_sur, sizeof ti->sur);
    ti->cur = ti->p_cur; ti->anc = ti->p_anc; ti->known = ti->p_known;
    if (ti->composed && !composable(ti)) {                  /* became a password field mid-composition */
        drop_composition(ti);
        if (ti->entered && ti->active) zwp_text_input_v3_send_done(ti->res, ti->commits);   /* clears it */
    }
    if (ti->entered && (ti->active != was || other || (ti->active && moved))) notify(ti, ti->active);
}

static const struct zwp_text_input_v3_interface ti_impl = {
    .destroy = ti_destroy, .enable = ti_enable, .disable = ti_disable, .set_surrounding_text = ti_surrounding,
    .set_text_change_cause = ti_cause, .set_content_type = ti_content, .set_cursor_rectangle = ti_cursor, .commit = ti_commit,
};

static void ti_gone(struct wl_resource *r) {
    TextInput *ti = wl_resource_get_user_data(r);
    if (ti->entered && ti->active) notify(ti, 0);
    drop_composition(ti);
    wl_list_remove(&ti->link);
    free(ti);
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void mgr_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat;
    TextInput *ti = calloc(1, sizeof *ti);
    struct wl_resource *res = ti ? wl_resource_create(c, &zwp_text_input_v3_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(ti); wl_client_post_no_memory(c); return; }
    ti->res = res;
    ti->client = c;
    wl_resource_set_implementation(res, &ti_impl, ti, ti_gone);
    wl_list_insert(&inputs, &ti->link);
    if (focus_client == c) { ti->entered = 1; zwp_text_input_v3_send_enter(res, focus_surface); }
}
static const struct zwp_text_input_manager_v3_interface mgr_impl = { .destroy = mgr_destroy, .get_text_input = mgr_get };
static void mgr_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, (int)ver, id);
    if (r) wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

/* The field keys compose into: the first composable one (an app with two seats' worth of objects). */
static TextInput *target(void) {
    TextInput *ti;
    if (!inputs.next) return NULL;                          /* no compositor (an isolated app's process) */
    wl_list_for_each(ti, &inputs, link) if (composable(ti)) return ti;
    return NULL;
}

int wl_textinput_composing(void) { return target() != NULL; }

void wl_textinput_send(const char *preedit, int fb, int fe, const char *commit) {
    TextInput *ti = target();
    if (!ti) return;
    size_t clen = commit ? tiu_valid_prefix(commit, TEXT_MAX) : 0;
    size_t plen;
    int b, e;
    tiu_preedit(preedit ? preedit : "", TEXT_MAX, fb, fe, &plen, &b, &e);
    if (!clen && !plen && !ti->composed) return;           /* nothing shown, nothing to clear */
    if (clen) {
        char text[TEXT_MAX + 1];
        memcpy(text, commit, clen);
        text[clen] = 0;
        zwp_text_input_v3_send_commit_string(ti->res, text);
    }
    if (plen) {
        char text[TEXT_MAX + 1];
        memcpy(text, preedit, plen);
        text[plen] = 0;
        zwp_text_input_v3_send_preedit_string(ti->res, text, b, e);
    }
    /* no preedit_string: the one shown goes away at done (it resets to empty) */
    ti->composed = plen > 0;
    zwp_text_input_v3_send_done(ti->res, ti->commits);
}

void wl_textinput_delete(uint32_t before, uint32_t after) {
    TextInput *ti = target();
    if (!ti || !ti->known) return;                          /* without its text there is nothing to clamp to */
    uint32_t b, a;
    tiu_delete(ti->sur, strlen(ti->sur), ti->cur, before, after, &b, &a);
    if (!b && !a) return;
    zwp_text_input_v3_send_delete_surrounding_text(ti->res, b, a);
    zwp_text_input_v3_send_done(ti->res, ti->commits);
}

int wl_textinput_rect(int out[4]) {
    TextInput *ti = target();
    if (!ti) return 0;
    memcpy(out, ti->rect, sizeof ti->rect);
    return ti->has_rect ? 1 : 2;
}

void wl_textinput_focus(struct wl_client *client, struct wl_resource *surface) {
    TextInput *ti;
    wl_list_for_each(ti, &inputs, link) {
        if (!ti->entered) continue;
        ti->entered = 0;
        if (ti->active) notify(ti, 0);
        drop_composition(ti);
        ti->active = ti->want = 0;
        if (focus_surface) zwp_text_input_v3_send_leave(ti->res, focus_surface);
    }
    focus_client = client;
    focus_surface = surface;
    if (!client) return;
    wl_list_for_each(ti, &inputs, link) {
        if (ti->client != client) continue;
        ti->entered = 1;
        zwp_text_input_v3_send_enter(ti->res, surface);
    }
}

/* Its resource is going away: no leave event, the client knows. */
void wl_textinput_gone(void) {
    TextInput *ti;
    wl_list_for_each(ti, &inputs, link) {
        if (ti->entered) drop_composition(ti);
        ti->entered = ti->active = ti->want = 0;
    }
    focus_client = NULL;
    focus_surface = NULL;
}

void wl_textinput_globals(struct wl_display *dpy, void (*fn)(int on, int x, int y, int w, int h)) {
    wl_list_init(&inputs);
    report = fn;
    wl_global_create(dpy, &zwp_text_input_manager_v3_interface, 1, NULL, mgr_bind);
}
