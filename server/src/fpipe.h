/* fpipe.h — the frame pipeline's decisions (docs/GPU.md, "The frame pipeline"). Without it a display's next
 * frame is built (input, JS, layout, paint) only once its page flip is done, so building, the GPU's part
 * and the flip run one after another and a frame that takes more than a refresh period in all waits for the
 * second vblank. With it, while the flip of frame N is pending the CPU part of frame N+1 is done ahead
 * ("prepared": painted into the display's scene), and at N's flip only the upload, the GPU's draw and the
 * page flip of that frame remain. main.c keeps the per-display state; the rules are here, without I/O, so
 * tests/fpipe_test.c can check them. */
#ifndef LORD_FPIPE_H
#define LORD_FPIPE_H

/* LORD_PIPELINE, read once at startup: 0 off (every frame is built after the flip, as before 0.8.6),
 * 2 always, anything else (and unset) auto: on while frames are heavy (fpipe_note) */
enum { FPIPE_OFF, FPIPE_AUTO, FPIPE_ALWAYS };
typedef struct {
    int mode;
    int on;                             /* auto: engaged */
    int heavy, light;                   /* heavy frames in a row (to engage), light ones (to let go) */
} FPipe;
void fpipe_init(FPipe *p, const char *env);
/* after each frame the main display got: the CPU time it took (input excluded: JS frame callbacks,
 * layout, paint, the upload and the flip's submission) against the refresh period. Auto engages after
 * 2 frames in a row above 60 % of the period (where a frame plus the GPU's part stops fitting before
 * the next vblank) and lets go after 60 in a row below 40 %, like the paint threads (main.c). */
void fpipe_note(FPipe *p, double cpu_ms, double period_ms);
int  fpipe_engaged(const FPipe *p);

/* A display's prepared frame */
enum {
    FPIPE_NONE,                            /* nothing prepared */
    FPIPE_READY,                           /* painted into the scene, waiting for the pending flip to be submitted */
    FPIPE_EMPTY,                           /* tried during this pending flip and nothing was painted: the next
                                         * frame is built after the flip, as without the pipeline */
};
/* What a display does now. eligible: the pipeline applies to it (engaged, one display with a real page
 * flip: not headless, not paced in software); pending: its flip is pending; prep: FPIPE_*; needs: something
 * to show (damage, the cursor, a Linux app's new frame, a fullscreen app); paint: work the CPU can do
 * ahead (UI damage, layout or a JS frame callback waiting). */
enum {
    FPIPE_WAIT,                            /* nothing now (or wait for the flip) */
    FPIPE_BUILD,                           /* build and submit a frame at once (as without the pipeline) */
    FPIPE_SUBMIT,                          /* submit the prepared frame: only upload, GPU and flip are left */
    FPIPE_PREPARE,                         /* paint the next frame while the flip is pending */
};
int fpipe_step(int eligible, int pending, int prep, int needs, int paint);
/* Pointer input and LordBench's scripted step wait for this display's flip (they belong to the next
 * frame, and only one is prepared at a time): without the pipeline while its flip is pending, with it
 * only once a frame is prepared (or was tried) for that flip. */
int fpipe_blocked(int eligible, int pending, int prep);

#endif
