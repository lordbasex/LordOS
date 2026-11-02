/* lbinput.h — LordBench's synthetic pointer, for the input latency of the desktop test (docs/BENCHMARK.md,
 * "Desktop impact").
 *
 * While the scene's window moves (grab_script_begin, ui/grab.c), a thread of lord-server plays a mouse: 125
 * events a second along the scene's figure of eight, each one stamped with CLOCK_MONOTONIC when it is made,
 * put in a ring and announced on an eventfd that the main loop polls like an input device. The main loop
 * takes the newest position once per frame (as it does with a real mouse's motion, main.c) and moves the
 * window to it; each event's latency is the time until the first frame showing it, or a later position,
 * was presented (bench/lbframes.c). It measures lord-server's part (waking up, the frame, the flip), not the
 * device's. The positions only ever move LordBench's own window: nothing reaches the theme's JS, an app or
 * the real pointer. */
#ifndef LORD_BENCH_LBINPUT_H
#define LORD_BENCH_LBINPUT_H

#define LBI_HZ 125                          /* a common mouse's report rate */

int  lbi_start(float rx, float ry, float period_ms);    /* 0 if the thread runs; restarts a running one */
void lbi_stop(void);
int  lbi_fd(void);                          /* the eventfd while running, -1 otherwise */
void lbi_drain(void);                       /* main loop: lbi_fd() is readable */
/* main loop, once per frame: the newest position (offsets from where the window started) of the events
 * since the last call, each one handed to lbf_input(); 0 when none came */
int  lbi_take(float *dx, float *dy);

/* the path, also used by grab.c without the thread: x = rx sin(k), y = ry sin(2k) / 2, k = 2π t / period */
void lbi_path(float rx, float ry, float period_ms, double t_ms, float *dx, float *dy);

#endif
