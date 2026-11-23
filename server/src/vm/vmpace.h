/* vmpace.h — how often a machine's picture is taken (docs/MACHINES-GPU.md, "Pacing a machine's picture"; VD_PACE in
 * vmproto.h). Pure logic, shared by lord-server (js_vm.c) and lord-vmdisplay (vmdisplay.c), tested by vmpace-test.
 *
 *   - At most one picture per refresh of the display the window is on (VD_RATE), on every path.
 *   - Idle: an Intel vGPU's vfio display hands over the whole picture on every GUI refresh (~33 a second), changed or
 *     not. After VMPACE_IDLE_AFTER of them in a row changed nothing, the picture is taken VMPACE_IDLE_MS apart (and
 *     QEMU, whose console waits for the answer, isn't asked for more meanwhile). The first changed frame, or the first
 *     key, button, wheel or motion sent to the guest, brings the full rate back at once (the helper ends it itself on
 *     input, so a keystroke's picture never waits for lord-server).
 *   - Hidden: no window shows the picture (minimized, on another desktop, wholly under an opaque window): 2D and vGPU
 *     pictures are taken VMPACE_HIDDEN_MS apart and lord-server copies none of them; showing a window again brings the
 *     full rate back and the next picture is taken whole.
 *   - A Linux 3D (virgl) picture is never paced in the helper: QEMU holds the guest's GPU work while an UpdateDMABUF
 *     waits, so pacing it would slow the guest itself. Hidden, lord-server only stops reading it (the answers go back at
 *     once). Its frames only come when the guest draws, so it never turns idle. */
#ifndef LORD_VM_PACE_H
#define LORD_VM_PACE_H

#include "vmproto.h"

#define VMPACE_IDLE_AFTER 30        /* unchanged frames in a row before idle (about a second at a vfio display's 33) */
#define VMPACE_IDLE_MS    100       /* idle: 10 pictures a second, so a change the guest makes alone waits ≤ 100 ms */
#define VMPACE_HIDDEN_MS  500       /* hidden: 2 a second (the library's card, a recording keep their own reads) */

typedef struct {
    int same_run;                   /* frames in a row that changed nothing */
    int kind;                       /* the VDP_* the helper was last told (or ended itself: VDP_IDLE on input) */
} VmPace;

/* a frame was read: changed or not */
static inline void vmpace_frame(VmPace *p, int changed) {
    if (changed) p->same_run = 0;
    else if (p->same_run < (1 << 20)) p->same_run++;
}

/* input went to the guest: the helper ended an idle pace by itself, so this side's record follows */
static inline void vmpace_input(VmPace *p) {
    p->same_run = 0;
    if (p->kind == VDP_IDLE) p->kind = VDP_NONE;
}

/* What the helper should be told now. hidden: no window shows the picture; virgl: a Linux 3D picture (never paced in
 * the helper). 1 and *ms, *kind (the VD_PACE to send) when it differs from what was last told; 0 nothing to send. */
static inline int vmpace_step(VmPace *p, int hidden, int virgl, int *ms, int *kind) {
    int k = virgl ? VDP_NONE : hidden ? VDP_HIDDEN : p->same_run >= VMPACE_IDLE_AFTER ? VDP_IDLE : VDP_NONE;
    if (k == p->kind) return 0;
    p->kind = k;
    *kind = k;
    *ms = k == VDP_HIDDEN ? VMPACE_HIDDEN_MS : k == VDP_IDLE ? VMPACE_IDLE_MS : 0;
    return 1;
}

/* the helper: the least time between two pictures, for the display's rate (VD_RATE, 1..240; 0 = 60) and a pace */
static inline int vmpace_interval(int rate, int pace_ms) {
    int r = rate > 0 ? 1000 / rate : 1000 / 60;
    return pace_ms > r ? pace_ms : r;
}

/* the helper: how long QEMU's UpdateDMABUF may be held at most (a stalled lord-server never stalls the guest long;
 * paced, the hold is the pace plus that, still well inside QEMU's 1 s) */
static inline int vmpace_hold_ms(int pace_ms, int ack_wait_ms) {
    int h = pace_ms > 0 ? pace_ms + ack_wait_ms : ack_wait_ms;
    return h > VD_PACE_MAX_MS ? VD_PACE_MAX_MS : h;
}

#endif
