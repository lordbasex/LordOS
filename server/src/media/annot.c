/* annot.c — the markup editor's annotations over the image's pixels (at full resolution):
 * arrow, line, box, text, blur and the two redactions (solid fill, pixelate). The same thing is seen
 * in the preview and saved. Pixels are premultiplied ARGB; a picture with transparency keeps it. */
#include "media_int.h"
#include "imgops.h"
#include "../ui/ui.h"

#include <math.h>
#include <string.h>

/* A straight-alpha color over a premultiplied pixel ("over"); an opaque pixel stays opaque. */
static inline uint32_t blend(uint32_t d, uint32_t argb, float cov) {
    uint32_t a = (uint32_t)((argb >> 24) * cov + 0.5f);
    if (!a) return d;
    uint32_t ia = 255 - a;
    uint32_t oa = a + ((d >> 24) * ia + 127) / 255;
    uint32_t r = (((argb >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
    uint32_t g = (((argb >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
    uint32_t b = ((argb & 255) * a + (d & 255) * ia) / 255;
    if (oa > 255) oa = 255;
    return oa << 24 | r << 16 | g << 8 | b;
}

/* Thick segment with soft edges (distance to the segment). */
static void thick_line(uint32_t *px, int w, int h, float x1, float y1, float x2, float y2, float width, uint32_t color) {
    float hw = width / 2;
    int bx0 = (int)floorf(fminf(x1, x2) - hw - 1), by0 = (int)floorf(fminf(y1, y2) - hw - 1);
    int bx1 = (int)ceilf(fmaxf(x1, x2) + hw + 1), by1 = (int)ceilf(fmaxf(y1, y2) + hw + 1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > w) bx1 = w;
    if (by1 > h) by1 = h;
    float dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px_ = x + 0.5f, py = y + 0.5f;
            float t = len2 > 0 ? ((px_ - x1) * dx + (py - y1) * dy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float ex = px_ - (x1 + t * dx), ey = py - (y1 + t * dy);
            float d = sqrtf(ex * ex + ey * ey);
            float cov = hw + 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            px[(size_t)y * w + x] = blend(px[(size_t)y * w + x], color, cov);
        }
}

/* Filled triangle with soft edges (for the arrow head). */
static void triangle(uint32_t *px, int w, int h, const float *vx, const float *vy, uint32_t color) {
    int bx0 = (int)floorf(fminf(vx[0], fminf(vx[1], vx[2]))) - 1, by0 = (int)floorf(fminf(vy[0], fminf(vy[1], vy[2]))) - 1;
    int bx1 = (int)ceilf(fmaxf(vx[0], fmaxf(vx[1], vx[2]))) + 1, by1 = (int)ceilf(fmaxf(vy[0], fmaxf(vy[1], vy[2]))) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > w) bx1 = w;
    if (by1 > h) by1 = h;
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
    float sgn = area < 0 ? -1.0f : 1.0f;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float cx = x + 0.5f, cy = y + 0.5f, cov = 1;
            for (int e = 0; e < 3; e++) {
                int a = e, b = (e + 1) % 3;
                float ex = vx[b] - vx[a], ey = vy[b] - vy[a], el = sqrtf(ex * ex + ey * ey);
                if (el <= 0) continue;
                float d = sgn * ((cx - vx[a]) * ey - (cy - vy[a]) * ex) / el;   /* >0 outside */
                float c = 0.5f - d;
                if (c < cov) cov = c;
            }
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            px[(size_t)y * w + x] = blend(px[(size_t)y * w + x], color, cov);
        }
}

void media_annotate(uint32_t *px, int w, int h, const MediaOp *ops, int n, int opaque) {
    Canvas c;
    canvas_init(&c, px, w, h, w);
    for (int i = 0; i < n; i++) {
        const MediaOp *o = &ops[i];
        float x0 = fminf(o->x1, o->x2), y0 = fminf(o->y1, o->y2), bw = fabsf(o->x2 - o->x1), bh = fabsf(o->y2 - o->y1);
        float width = o->width > 0 ? o->width : 4;
        switch (o->type) {
        case 'b':
            canvas_rrect_stroke(&c, x0, y0, bw, bh, width, width, o->color);
            break;
        case 'l':
            thick_line(px, w, h, o->x1, o->y1, o->x2, o->y2, width, o->color);
            break;
        case 'a': {
            float dx = o->x2 - o->x1, dy = o->y2 - o->y1, len = sqrtf(dx * dx + dy * dy);
            if (len < 1) break;
            float ux = dx / len, uy = dy / len, head = fminf(len * 0.6f, width * 4.5f);
            float bx = o->x2 - ux * head, by = o->y2 - uy * head;
            thick_line(px, w, h, o->x1, o->y1, bx + ux * 1.5f, by + uy * 1.5f, width, o->color);
            float vx[3] = { o->x2, bx - uy * head * 0.55f, bx + uy * head * 0.55f };
            float vy[3] = { o->y2, by + ux * head * 0.55f, by - ux * head * 0.55f };
            triangle(px, w, h, vx, vy, o->color);
            break;
        }
        case 't': {
            if (!o->text[0]) break;
            float size = o->size > 6 ? o->size : 24;
            /* dark border so it reads over any background */
            for (int k = 0; k < 4; k++) {
                float ox = (k & 1) ? 1.5f : -1.5f, oy = (k & 2) ? 1.5f : -1.5f;
                text_draw(&c, o->text, 700, size, 0, o->x1 + ox, o->y1 + oy, (float)w - o->x1, TEXT_LEFT, 0, 0x99000000u);
            }
            text_draw(&c, o->text, 700, size, 0, o->x1, o->y1, (float)w - o->x1, TEXT_LEFT, 0, o->color);
            break;
        }
        case 'z': {
            /* strong blur (three passes): for hiding data, it can't be «unblurred» by eye */
            int r = (int)fmaxf(8, width * 3);
            for (int k = 0; k < 3; k++) canvas_backdrop_blur(&c, x0, y0, bw, bh, 0, r);
            break;
        }
        case 'f':
            /* redact: an opaque fill, nothing of what was below is left */
            img_fill(px, w, h, o->x1, o->y1, o->x2, o->y2, o->color);
            break;
        case 'p':
            /* redact: blocks of their average color (the block size comes as `size`) */
            img_pixelate(px, w, h, o->x1, o->y1, o->x2, o->y2, (int)o->size);
            break;
        default: break;
        }
    }
    if (opaque) for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xff000000u;
}
