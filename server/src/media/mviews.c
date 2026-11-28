/* mviews.c — the nodes that show a media source: which ones, the damage a new frame makes in each, the
 * present of a GPU picture, and the opaque video box that lets paint.c start from it (media_cover_node).
 *
 * One source can be shown by several nodes at once:
 *   - plain nodes (no Node.crop) that show all of it: one moving wallpaper player shared by every display
 *     (sdk/motion.jsx: one decoder per file, whatever the number of displays). Each one is in view_nodes.
 *   - crop nodes that show a part of it (Machines' apps mode, one guest window each): crop_nodes.
 * Up to 0.8.9.6.2 only the plain node painted last was remembered (MediaSrc.node): a new frame damaged that
 * display's copy alone, the others stayed on an old frame, and where something else repainted them (a clock, a
 * hover) a patch of the newer frame showed through, the picture «spilling» over the old one.
 *
 * Only UI pieces are used here (node_get, damage_add), so engine-test links it with its sources table stubbed. */
#include "media_int.h"
#include "../ui/ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* a plain node in view_nodes still shows all of this source? (pruned when it died, shows another one or a part) */
static Node *plain_node(MediaSrc *s, int i) {
    Node *n = s->view_nodes[i] ? node_get(s->view_nodes[i]) : NULL;
    if (n && n->type == NODE_MEDIA && n->term == s->id && !n->crop) return n;
    s->view_nodes[i] = 0;
    return NULL;
}

void media_src_view_note(MediaSrc *s, int node, int bw, int bh) {
    if (!s || node <= 0) return;
    int free_at = -1;
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) {
        if (s->view_nodes[i] == node) {
            if (bw > 0 && bh > 0) { s->view_w[i] = bw; s->view_h[i] = bh; }
            return;
        }
        if (free_at < 0 && !plain_node(s, i)) free_at = i;
    }
    if (free_at < 0) { s->views_over = 1; return; }
    s->view_nodes[free_at] = node;
    s->view_w[free_at] = bw > 0 ? bw : 0;
    s->view_h[free_at] = bh > 0 ? bh : 0;
}

void media_src_view_drop(MediaSrc *s, int node) {
    if (!s) return;
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) if (s->view_nodes[i] == node) s->view_nodes[i] = 0;
    if (s->node == node) s->node = 0;
}

int media_src_view_count(MediaSrc *s) {
    int k = 0;
    if (s) for (int i = 0; i < MEDIA_VIEW_NODES; i++) if (plain_node(s, i)) k++;
    return k;
}

int media_src_view_box(MediaSrc *s, int *bw, int *bh) {
    long best = 0;
    *bw = *bh = 0;
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) {
        if (!plain_node(s, i) || s->view_w[i] <= 0 || s->view_h[i] <= 0) continue;
        long a = (long)s->view_w[i] * s->view_h[i];
        if (a > best) { best = a; *bw = s->view_w[i]; *bh = s->view_h[i]; }
    }
    return best > 0;
}

/* a node in crop_nodes still shows a part of this source? */
static Node *crop_node(MediaSrc *s, int i) {
    Node *n = s->crop_nodes[i] ? node_get(s->crop_nodes[i]) : NULL;
    if (n && n->type == NODE_MEDIA && n->term == s->id && n->crop) return n;
    s->crop_nodes[i] = 0;
    return NULL;
}

int media_src_crop_count(MediaSrc *s) {
    int k = 0;
    for (int i = 0; i < MEDIA_CROP_NODES; i++) if (crop_node(s, i)) k++;
    return k;
}

/* The part of a node that shows the picture's pixels x, y, w × h (where its last paint put them, media_paint), in the
 * node's logical coordinates, one pixel wider on each side for the scaled picture's filter; the whole node when that
 * paint is unknown, rotated or mirrored, or was of another size. 0 when nothing of it is in the box. */
static int node_part(const MediaSrc *s, const Node *n, int x, int y, int w, int h, Box *out) {
    const float *pd = node_padding(n);
    float bl = node_border(n, EDGE_L), bt = node_border(n, EDGE_T), br = node_border(n, EDGE_R), bb = node_border(n, EDGE_B);
    float cx = n->ax + pd[EDGE_L] + bl, cy = n->ay + pd[EDGE_T] + bt;
    float cw = n->w - pd[EDGE_L] - pd[EDGE_R] - bl - br, ch = n->h - pd[EDGE_T] - pd[EDGE_B] - bt - bb;
    *out = (Box){ floorf(n->ax), floorf(n->ay), ceilf(n->ax + n->w), ceilf(n->ay + n->h) };
    if (!s->map_ok || s->map_w != s->w || s->map_h != s->h || cw <= 0 || ch <= 0) return 1;
    float x0 = cx + cw * (s->map_fx + (float)(x - 1) * s->map_kx), x1 = cx + cw * (s->map_fx + (float)(x + w + 1) * s->map_kx);
    float y0 = cy + ch * (s->map_fy + (float)(y - 1) * s->map_ky), y1 = cy + ch * (s->map_fy + (float)(y + h + 1) * s->map_ky);
    x0 = fmaxf(floorf(x0) - 1, cx); y0 = fmaxf(floorf(y0) - 1, cy);
    x1 = fminf(ceilf(x1) + 1, cx + cw); y1 = fminf(ceilf(y1) + 1, cy + ch);
    if (x1 <= x0 || y1 <= y0) return 0;
    *out = (Box){ floorf(x0), floorf(y0), ceilf(x1), ceilf(y1) };
    return 1;
}

/* the whole box of a node (not the shadow margin): that way a new frame falls inside it (media_cover_node) */
static Box node_box(const Node *n) { return (Box){ floorf(n->ax), floorf(n->ay), ceilf(n->ax + n->w), ceilf(n->ay + n->h) }; }

/* MediaSrc.node (its pointer, Machines' checks) stays one of the live plain nodes, 0 when none is left */
static void node_follow(MediaSrc *s) {
    int first = 0;
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) {
        if (!plain_node(s, i)) continue;
        if (s->view_nodes[i] == s->node) return;
        if (!first) first = s->view_nodes[i];
    }
    s->node = first;
}

/* a node found no slot (more than MEDIA_VIEW_NODES of them): a new frame repaints everything, and that paint notes
 * every node again (media_paint), so the flag comes back only while there are still too many */
static void damage_over(MediaSrc *s) {
    s->views_over = 0;
    damage_add((Box){ 0, 0, 16384, 16384 });
}

/* the crop nodes: each one only where its part of the picture changed (x, y, w × h) */
static int damage_crops(MediaSrc *s, int x, int y, int w, int h) {
    int k = 0;
    for (int i = 0; i < MEDIA_CROP_NODES; i++) {
        Node *c = crop_node(s, i);
        if (!c || c->prev_box.x1 <= c->prev_box.x0 || c->w <= 0 || c->h <= 0) continue;
        const float *r = c->crop;
        /* the changed area inside this node's part, in the node's coordinates (the part is stretched over the box) */
        float x0 = fmaxf((float)x, r[0]), y0 = fmaxf((float)y, r[1]), x1 = fminf((float)(x + w), r[0] + r[2]), y1 = fminf((float)(y + h), r[1] + r[3]);
        if (x1 <= x0 || y1 <= y0) continue;
        float sx = c->w / r[2], sy = c->h / r[3];
        damage_add((Box){ floorf(c->ax + (x0 - r[0]) * sx) - 1, floorf(c->ay + (y0 - r[1]) * sy) - 1,
                          ceilf(c->ax + (x1 - r[0]) * sx) + 1, ceilf(c->ay + (y1 - r[1]) * sy) + 1 });
        k++;
    }
    return k;
}

int media_src_damage_rect(MediaSrc *s, int x, int y, int w, int h) {
    int k = 0, nplain = media_src_view_count(s);
    media_pd_note(s, x, y, w, h);
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) {
        Node *n = plain_node(s, i);
        if (!n || n->prev_box.x1 <= n->prev_box.x0) continue;
        /* only where the picture changed (Machines: a pointer, a clock), as the crop nodes: the rest of the node
         * keeps its pixels (docs/MACHINES-GPU.md, Part 1 step 3). The picture's place (map_*) is the last paint's,
         * one for the source: with several nodes, each one whole */
        Box b = node_box(n);
        if (nplain == 1 && !node_part(s, n, x, y, w, h, &b)) continue;
        damage_add(b);
        k++;
    }
    if (s->views_over) { damage_over(s); k++; }
    node_follow(s);
    k += damage_crops(s, x, y, w, h);
    ui.dirty = 1;
    return k;
}

void media_src_damage(MediaSrc *s) {
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) {
        Node *n = plain_node(s, i);
        if (n && n->prev_box.x1 > n->prev_box.x0) damage_add(node_box(n));
    }
    if (s->views_over) damage_over(s);
    node_follow(s);
    if (s->w > 0 && s->h > 0 && media_src_crop_count(s)) {
        media_pd_note(s, 0, 0, s->w, s->h);
        damage_crops(s, 0, 0, s->w, s->h);
    }
    ui.dirty = 1;
}

/* ---- a GPU picture's new frame: its nodes presented again (main.c) ----
 * One box per node shown (not one box around them all): three displays side by side with one view hidden (covered,
 * a fullscreen game) compose only the two that show it. */
#define PRESENT_MAX 32
static Box present_box[PRESENT_MAX];
static int present_n, present_over;

static void present_add(Box c) {
    for (int i = 0; i < present_n; i++) {
        Box *b = &present_box[i];
        if (c.x0 >= b->x0 && c.y0 >= b->y0 && c.x1 <= b->x1 && c.y1 <= b->y1) return;
    }
    if (present_n < PRESENT_MAX) present_box[present_n++] = c;
    else present_over = 1;
}

void media_src_present(MediaSrc *s) {
    int k = 0;
    for (int i = 0; i < MEDIA_VIEW_NODES; i++) {
        Node *n = plain_node(s, i);
        if (n && n->prev_box.x1 > n->prev_box.x0 && !s->view_hidden[i]) { present_add(n->prev_box); k++; }
    }
    if (s->views_over) { present_add((Box){ 0, 0, 16384, 16384 }); k++; }
    for (int i = 0; i < MEDIA_CROP_NODES; i++) {
        Node *c = crop_node(s, i);
        if (c && c->prev_box.x1 > c->prev_box.x0) { present_add(c->prev_box); k++; }
    }
    /* the frame counts as read: the present that follows draws the texture it is in (js_vm_painted acknowledges it);
     * not shown anywhere: nothing to present */
    (void)k;
    s->painted_seq = s->frame_seq;
}

int media_present_pending(void) { return present_n > 0 || present_over; }

int media_present_take(float (*b)[4], int max) {
    int n = 0;
    if (present_over && max > 0) { b[0][0] = 0; b[0][1] = 0; b[0][2] = 16384; b[0][3] = 16384; n = 1; }
    else for (int i = 0; i < present_n && n < max; i++) { b[n][0] = present_box[i].x0; b[n][1] = present_box[i].y0; b[n][2] = present_box[i].x1; b[n][3] = present_box[i].y1; n++; }
    present_n = 0;
    present_over = 0;
    return n;
}

/* ---- NV12 sources (mambient.c): the views and the blurred strips over them ---- */

Node *media_src_view_at(MediaSrc *s, int i) { return s && i >= 0 && i < MEDIA_VIEW_NODES ? plain_node(s, i) : NULL; }

void media_src_blur_note(MediaSrc *s, int node, const void *boxes, int n) {
    const Box *b = boxes;
    int k = 0;
    for (int i = 0; i < s->nblur; i++)               /* this node's strips go, the others' stay (other views) */
        if (s->blur_node[i] != node && node_get(s->blur_node[i])) {
            memcpy(s->blur_box[k], s->blur_box[i], sizeof s->blur_box[0]);
            s->blur_node[k++] = s->blur_node[i];
        }
    for (int i = 0; i < n && k < MEDIA_BLUR_MAX * 2; i++) {
        s->blur_box[k][0] = b[i].x0; s->blur_box[k][1] = b[i].y0; s->blur_box[k][2] = b[i].x1; s->blur_box[k][3] = b[i].y1;
        s->blur_node[k++] = node;
    }
    s->nblur = k;
}

void media_src_blur_damage(MediaSrc *s) {
    for (int i = 0; i < s->nblur; i++) {
        int hidden = 0, live = 0;
        for (int v = 0; v < MEDIA_VIEW_NODES; v++)
            if (s->view_nodes[v] == s->blur_node[i] && plain_node(s, v)) { live = 1; hidden = s->view_hidden[v]; }
        if (!live || hidden) continue;
        damage_add((Box){ s->blur_box[i][0], s->blur_box[i][1], s->blur_box[i][2], s->blur_box[i][3] });
        ui.dirty = 1;
    }
}

int media_blur_interval_ms(void) {
    static int ms = -2;
    if (ms == -2) {
        const char *e = getenv("LORD_MOTION_BLUR_MS");
        char *end = NULL;
        long v = e && *e ? strtol(e, &end, 10) : -1;
        ms = e && *e && end && !*end && v >= 0 && v <= 2000 ? (int)v : MEDIA_BLUR_MS;
    }
    return ms;
}

int media_src_blur_frame(MediaSrc *s, double now) {
    int every = media_blur_interval_ms();
    if (every > 0 && s->blur_at > 0 && now - s->blur_at < every && now >= s->blur_at) { s->blur_pending = 1; return 0; }
    s->blur_at = now;
    s->blur_pending = 0;
    media_src_blur_damage(s);
    return 1;
}

double media_src_blur_due(MediaSrc *s, double now) {
    if (!s->blur_pending) return -1;
    double left = s->blur_at + media_blur_interval_ms() - now;
    if (left <= 0 || now < s->blur_at) { media_src_blur_frame(s, now); return -1; }
    return left < 1 ? 1 : left;
}

/* A moving wallpaper's view drawn by the GPU this paint (an NV12 source with its texture, gpu_drawn) whose picture covers
 * the logical box x0…y1 on node, opaque and unrounded: its source id, else 0. ui/paint.c then lets the GPU blur it under
 * a frosted node there (wl_paint_frost). */
int media_frost_source(int node, float x0, float y0, float x1, float y1) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s || s->kind != SK_PLAYER || !s->nv || !s->gtex || !s->gpu_drawn || !s->map_ok || s->map_w <= 0 || s->map_h <= 0) continue;
        for (int v = 0; v < MEDIA_VIEW_NODES; v++) {
            Node *n = plain_node(s, v);
            if (!n || n->id != node || n->s.radius > 0 || n->crop) continue;
            for (Node *p = n; p; p = p->parent) if (p->s.opacity < 0.999f || p->s.display_none) return 0;
            float px0 = n->ax + s->map_fx * n->w, py0 = n->ay + s->map_fy * n->h;
            float px1 = px0 + s->map_w * s->map_kx * n->w, py1 = py0 + s->map_h * s->map_ky * n->h;
            return x0 >= px0 - 0.5f && y0 >= py0 - 0.5f && x1 <= px1 + 0.5f && y1 <= py1 + 0.5f ? s->id : 0;
        }
    }
    return 0;
}

/* The frame of a video or of the camera covers its whole box (the bands are painted opaque): if everything
 * that changed is inside, what's beneath that node isn't visible. Any of the source's plain nodes: each one is a
 * whole copy of the picture (one per display, a moving wallpaper), and paint.c asks with one display's damage. */
int media_cover_node(float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return 0;
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s) continue;
        if ((s->kind != SK_PLAYER && s->kind != SK_CAMERA) || (s->view.bg >> 24) != 255 || !s->map) continue;
        for (int v = 0; v < MEDIA_VIEW_NODES; v++) {
            Node *n = plain_node(s, v);
            if (!n || n->s.opacity < 1 || n->s.radius > 0) continue;
            /* the box must be visible and without transparency in the ancestors */
            int ok = 1;
            for (Node *p = n; p && ok; p = p->parent) if (p->s.display_none || p->s.opacity < 1) ok = 0;
            if (!ok) continue;
            Box b = node_box(n);
            if (x0 >= b.x0 && y0 >= b.y0 && x1 <= b.x1 && y1 <= b.y1) return n->id;
        }
    }
    return 0;
}
