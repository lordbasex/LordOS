/* outpick.h — which display a Linux window belongs to, for its frame callbacks, its presentation feedback and the
 * displays its new content has to reach (wl.c; docs/WAYLAND.md, "Frames, titles and damage"). Pure, header only:
 * tests/outpick_test.c checks it.
 *
 * Displays are the compositor's output slots, each with a logical rect and a key: main.c's own index of that display,
 * the bit its frames are reported with. A window's main display is the one with most of its box; frame callbacks go
 * with that display's frames, and its presentation feedback with that display's flips. */
#ifndef LORD_WL_OUTPICK_H
#define LORD_WL_OUTPICK_H

#define OUTPICK_ALL 0xffffffffu         /* every display (no monitor, or not known) */

typedef struct { int used, key, x, y, w, h; } OutPickSlot;

/* The slot with most of the box (x0, y0)–(x1, y1); the first on a tie (mirrored displays); -1 if it is on none. */
static inline int outpick_main(const OutPickSlot *s, int n, float x0, float y0, float x1, float y1) {
    int best = -1;
    float area = 0;
    for (int i = 0; i < n; i++) {
        if (!s[i].used) continue;
        float l = x0 > (float)s[i].x ? x0 : (float)s[i].x, r = x1 < (float)(s[i].x + s[i].w) ? x1 : (float)(s[i].x + s[i].w);
        float t = y0 > (float)s[i].y ? y0 : (float)s[i].y, b = y1 < (float)(s[i].y + s[i].h) ? y1 : (float)(s[i].y + s[i].h);
        if (r <= l || b <= t) continue;
        if ((r - l) * (b - t) > area) { area = (r - l) * (b - t); best = i; }
    }
    return best;
}

/* The keys of the slots the box touches (its new content must reach those displays); OUTPICK_ALL for an empty box. */
static inline unsigned outpick_keys(const OutPickSlot *s, int n, float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return OUTPICK_ALL;
    unsigned m = 0;
    for (int i = 0; i < n; i++)
        if (s[i].used && s[i].key >= 0 && s[i].key < 32 && x0 < (float)(s[i].x + s[i].w) && x1 > (float)s[i].x &&
            y0 < (float)(s[i].y + s[i].h) && y1 > (float)s[i].y)
            m |= 1u << s[i].key;
    return m ? m : OUTPICK_ALL;
}

/* The key of a slot, -1 for none (slot < 0, gone, or a key past 31). */
static inline int outpick_key(const OutPickSlot *s, int n, int slot) {
    return slot >= 0 && slot < n && s[slot].used && s[slot].key >= 0 && s[slot].key < 32 ? s[slot].key : -1;
}

/* Are a window's frame callbacks due now? keys: the displays that just showed a frame or have none waiting for a flip.
 * A window on no known display follows any frame, as before. */
static inline int outpick_due(int key, unsigned keys) { return key < 0 || (keys >> key & 1u); }

/* Does a flip of display `key` (-1: every display, no monitor) answer a window's presentation feedback? Its own
 * display's does; a window on no known display follows the main display's (main_key). */
static inline int outpick_presents(int win_key, int key, int main_key) {
    return key < 0 || win_key == key || (win_key < 0 && key == main_key);
}

#endif
