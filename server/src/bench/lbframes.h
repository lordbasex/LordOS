/* lbframes.h — LordBench's compositor frame recorder (docs/BENCHMARK.md, "Frame times against the refresh
 * budget"). While a recording is on, lord-server's main loop (main.c) reports the work of every frame it
 * builds (layout, paint and present, ms) and every frame the main display shows (the page flip's time, or
 * the end of the frame without a monitor); LordBench's thread (lbrun.c) starts and stops the recording
 * and gets the statistics. Off, each call is one atomic read. */
#ifndef LORD_BENCH_LBFRAMES_H
#define LORD_BENCH_LBFRAMES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int frames;                         /* frames built */
    int shown;                          /* frames shown on the main display */
    double span_s;                      /* from the first shown frame to the last */
    double fps;                         /* shown frames per second */
    double work_p50, work_p99, work_max;/* ms to build a frame */
    double budget_ms;                   /* the main display's refresh period */
    double missed_pct;                  /* intervals between shown frames longer than 1.5 periods, % of them */
    double gap_max;                     /* the longest interval, ms */
    int inputs;                         /* synthetic pointer events shown (bench/lbinput.h) */
    double in_p50, in_p99;              /* ms from such an event to the presentation of the first frame showing it */
} LbFrameStats;

/* main.c */
void lbf_frame(double work_ms);
void lbf_presented(uint64_t ns, uint32_t period_ns);
/* the synthetic pointer: an event (stamped ev_ns) was applied to the scene, before the next frame is built
 * (lbinput.c); then main.c says, after building a frame, whether the main display got it: the events
 * applied so far are in that frame and their latency ends at its presentation */
void lbf_input(uint64_t ev_ns);
void lbf_built(int main_drawn);
/* the frame pipeline (fpipe.h): a frame of the main display painted ahead, while the previous one's flip is
 * pending (the events applied so far are in it), then submitted at that flip (they end at its presentation).
 * lbf_built is both at once; events of a prepared frame that is dropped go with the next frame submitted. */
void lbf_painted(void);
void lbf_submitted(void);

/* lbrun.c: begin clears and starts; end stops and fills st (0, or -1 with fewer than 2 shown frames) */
void lbf_begin(void);
int  lbf_end(LbFrameStats *st);

/* ---- where a frame's time goes (docs/BENCHMARK.md, "Frame phases") ----
 * main.c fills one LbPhase for each frame the main display gets, when it is submitted (lbf_phase_built: at
 * the end of render(), or for a frame the pipeline prepared, right after its submission), and completes it at
 * that frame's presentation (lbf_phase_shown). Times are CLOCK_MONOTONIC ns, 0 =
 * unknown. All measured by lord-server itself; nothing comes from a helper. Two recorders take them:
 * LordBench's (lbf_begin/lbf_end, lbf_phases) and the scripted drag's (lbf_drag_begin/lbf_drag_end), for
 * its "drag (scripted):" line. Off, each call is one atomic read. */
#define LBF_G0R 8                       /* G0R_N (ui/ui.h), checked in main.c */
#define LBF_G0L 16                      /* G0L_N */
typedef struct {
    uint64_t prev_flip;                 /* the main display's presentation before this frame */
    uint64_t wake;                      /* poll() returned, before this frame was started */
    int from_flip;                      /* that wake handled the main display's flip (else input or a timer) */
    uint64_t render0, built;            /* render() started and ended */
    double paint_ms;                    /* layout and paint */
    double up_ms, g0_ms, swap_ms, submit_ms;   /* the UI texture's upload, the G0 textures', eglSwapBuffers,
                                                * the buffer's framebuffer and the page flip ioctl */
    long up_px, g0_px;
    int g0_full;                        /* a G0 texture was sent whole in this frame */
    double feed_ms;                     /* G0's textures catching up after the flip was queued */
    long feed_px;
    uint64_t submit;                    /* the page flip was queued (0: none, a modeset or no monitor) */
    uint32_t period;                    /* the refresh period, ns */
    int par;                            /* the paint threads were on */
    int g0;                             /* 1 G0 composed it, 0 refused (g0_why, g0_sub), -1 not tried */
    int g0_why, g0_sub;
    int unshown;                        /* gpu_frame_ex's -2: nothing shown, a vblank lost */
    /* the frame pipeline (fpipe.h): piped = painted ahead while the previous flip was pending, then submitted
     * at that flip. Then wake/from_flip/render0/built are its prepare's (render0..built: JS frame callbacks,
     * layout and paint), pre_end is when that render() started (the end of "before render"), swake and
     * sfrom_flip are the wake whose flip let it be submitted, sub0..sub1 its submission (upload, swap, flip
     * ioctl), and prev_flip is the presentation just before that submission */
    int piped, sfrom_flip;
    uint64_t pre_end, swake, sub0, sub1;
} LbPhase;
typedef struct { double p50, p99; } LbPct;
typedef struct {
    int n;                              /* frames with their phases */
    LbPct wake;                         /* the flip to lord-server running (frames started by a flip) */
    LbPct pre;                          /* that wake to render(): input, LordBench's scripted step, JS */
    LbPct paint, up, g0up, swap, submit_call;
    LbPct build;                        /* render() */
    LbPct gpu;                          /* the flip queued to the GPU done (the fence's signal time) */
    LbPct wait;                         /* the later of the two to the flip: the vblank */
    LbPct chain;                        /* the wake to the flip */
    LbPct feed;
    LbPct held;                         /* piped frames: prepared until submitted (the previous flip's wait) */
    double up_mpx, g0_mpx, feed_mpx;    /* pixels sent per frame, average, millions */
    int on_time;                        /* shown by the first vblank after the frame started (+ half a period) */
    int cpu_late;                       /* late, and queued after that vblank */
    int gpu_late;                       /* late, queued before it but the GPU finished after it */
    int other_late;                     /* late with both before it (or the GPU time unknown) */
    int gpu_known;                      /* frames with the GPU's finish time */
    int par;                            /* frames with the paint threads on */
    int g0_tried, g0_taken, g0_refused[LBF_G0R], g0_layer[LBF_G0L], g0_full;
    long g0_full_px;
    int unshown;
    int piped;                          /* frames prepared ahead (fpipe.h) */
} LbPhaseStats;
void lbf_phase_built(const LbPhase *p);
void lbf_phase_shown(uint64_t flip_ns, uint64_t gpu_done_ns);
int  lbf_recording(void);               /* either recorder is on (main.c takes the GPU's fences only then) */
void lbf_phases(LbPhaseStats *st);      /* LordBench's, of the recording lbf_end just stopped */
void lbf_drag_begin(void);
void lbf_drag_end(LbPhaseStats *st);
/* names of G0R_* and G0L_* (ui/ui.h's order) and the phases as text for a log line */
const char *lbf_g0r_name(int i);
const char *lbf_g0l_name(int i);
void lbf_phases_text(const LbPhaseStats *st, char *buf, size_t n);

#endif
