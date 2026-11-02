/* lbvideo.h — LordBench's video measurement from lord-server's side (docs/BENCHMARK.md, "Workloads"). It
 * launches lord-media (the only binary that links FFmpeg) from lord-server's folder, as media.c does, with
 * no fds and one MK_VBENCH job, and checks every MM_BENCH record before passing it on with LordBench's own
 * ids (lbproto.h, LB_VID_*). Blocking: lbrun.c calls it on its own thread. */
#ifndef LORD_BENCH_LBVIDEO_H
#define LORD_BENCH_LBVIDEO_H

/* kind: LB_MSG_STEP, LB_MSG_RESULT (value, aux = spread) or LB_MSG_SKIP (aux = LB_SKIP_*) */
typedef void (*LbvOn)(void *ctx, int kind, int id, double value, double aux);
/* tick is called about every 250 ms: nonzero stops (lord-media is killed). 0 when lord-media finished,
 * -1 if it could not start, -2 stopped by tick, -3 a bad record or it died, -4 timeout */
int lbv_run(int full, LbvOn on, int (*tick)(void *ctx), void *ctx);

/* 1 if an MM_BENCH record is acceptable: a known measurement and kind, a rate in range, a known reason */
int lbv_record_valid(unsigned id, long long kind, long long fps_milli, long long spread_milli, long long why);

#endif
