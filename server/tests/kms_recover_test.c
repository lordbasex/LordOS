/* kms_recover_test.c — a page flip the kernel never completes (src/kms.c kms_flip_stuck, kms_flip_abandon; QA
 * 0.8.9.6.2, BUG-B: virtio-gpu after an s2idle suspend, the greeter never showed). The stuck flip is taken as done (the
 * GPU path's on_flip with its data, or the CPU path's buffers swapped), the next frame is a full modeset, and the
 * kernel's late event for it, if it ever comes, completes nothing. No card needed: the event comes through a pipe. */
#include "kms.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int flips;
static void *flipped;
static void on_flip(void *data) { flips++; flipped = data; }

int main(void) {
    const uint64_t s = 1000000000ull;
    Kms k;
    memset(&k, 0, sizeof k);
    k.fd = -1;
    k.crtc_id = 41;
    /* the GPU path: kms_flip_fb's flip, pending */
    int tag = 7;
    k.custom = 1;
    k.on_flip = on_flip;
    k.flip_data = &tag;
    k.flip_pending = 1;
    k.flip_submit_ns = 10 * s;
    CHECK(!kms_flip_stuck(&k, 11 * s, 2 * s) && !kms_flip_stuck(&k, 12 * s, 2 * s) && kms_flip_stuck(&k, 12 * s + 1, 2 * s));
    kms_flip_abandon(&k);
    CHECK(!k.flip_pending && k.modeset_pending && flips == 1 && flipped == &tag);
    CHECK(!kms_flip_stuck(&k, 100 * s, 2 * s));
    kms_flip_abandon(&k);                       /* nothing pending: nothing done */
    CHECK(flips == 1);

    /* the kernel's late event for it completes nothing */
    int p[2];
    CHECK(!pipe(p));
    struct drm_event_vblank ev;
    memset(&ev, 0, sizeof ev);
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof ev;
    ev.crtc_id = 41;
    CHECK(write(p[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    k.fd = p[0];
    Kms *heads[1] = { &k };
    kms_dispatch_heads(heads, 1);
    CHECK(flips == 1 && !k.flip_pending);
    /* a real flip pending afterwards still completes with its event */
    k.flip_pending = 1;
    CHECK(write(p[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    kms_dispatch_heads(heads, 1);
    CHECK(flips == 2 && !k.flip_pending);
    close(p[0]);
    close(p[1]);

    /* the CPU path: the dumb buffers swap as the event would */
    Kms c;
    memset(&c, 0, sizeof c);
    c.fd = -1;
    c.front = 0;
    c.flip_pending = 1;
    c.flip_submit_ns = 1;
    kms_flip_abandon(&c);
    CHECK(c.front == 1 && !c.flip_pending && c.modeset_pending);
    CHECK(!kms_flip_stuck(&c, 0, 0));
    /* a flip never queued (no submit time) is never "stuck" */
    c.flip_pending = 1;
    c.flip_submit_ns = 0;
    CHECK(!kms_flip_stuck(&c, 100 * s, 2 * s));

    printf("kms-recover-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
