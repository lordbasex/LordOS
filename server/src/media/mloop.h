/* mloop.h — the rules of the moving wallpapers' GPU loop cache and of their pause (docs/WALLPAPERS.md, "The loop cache";
 * docs/MEDIA.md). A moving wallpaper is a 6–10 s loop played forever: after one whole pass lord-server can keep its
 * frames as GPU textures (NV12 planes at the shown size) and play the loop from them, with lord-media paused (no decode,
 * no copy). These are the decisions only, with no GPU and no media source, so build/mloop-test checks them alone; the
 * textures and the clock are mambient.c's.
 *
 * Every size is in bytes and every time in ms. */
#ifndef LORD_MEDIA_MLOOP_H
#define LORD_MEDIA_MLOOP_H

#include <stdint.h>

#define MLOOP_MAX_FRAMES 1200           /* 10 s at 120 fps: a clip longer than that is never cached */
#define MLOOP_MAX_MS     15000          /* nor one whose loop lasts longer */
#define MLOOP_HIDDEN_STOP_MS 10000      /* no view visible this long: the cache is freed and lord-media stopped */

/* What the machine's GPU memory allows. vram_total/vram_used: the card's (amdgpu mem_info_vram_*), -1 unknown;
 * integrated: shared memory (no VRAM of its own, or no PCIe link: an APU, Intel, the Raspberry Pi's v3d); mem_total:
 * the system's RAM. setting_mb: Settings' motionCacheMb (0 off). The budget, in bytes, for every cache together:
 *   - discrete: at most the setting, half the card's VRAM, and what is free beyond a headroom (a tenth of it, at least
 *     256 MB) for everything else;
 *   - integrated (or VRAM unknown): at most the setting and a sixteenth of the RAM, never more than 1 GB. */
int64_t mloop_budget(int64_t setting_mb, int64_t vram_total, int64_t vram_used, int64_t mem_total, int integrated);

/* Memory pressure: a cache must go now. Discrete: free VRAM below the headroom; integrated: MemAvailable below a tenth
 * of the RAM. Unknown numbers: no pressure. */
int mloop_pressure(int64_t vram_total, int64_t vram_used, int64_t mem_avail, int64_t mem_total, int integrated);

/* One frame's bytes (NV12, w × h × 1.5) and a whole loop's estimate (dur / interval frames, one more each end). */
int64_t mloop_frame_bytes(int w, int h);
int64_t mloop_estimate(int w, int h, int64_t dur_ms, int64_t interval_ms);

/* Recording the first whole pass. The frames as lord-media presents them, with their pts (ms from the clip's start). */
typedef struct {
    int on;                             /* recording */
    int w, h;                           /* the size every frame must keep */
    int n;                              /* frames kept */
    int64_t pts[MLOOP_MAX_FRAMES];
    int64_t last;                       /* the last pts seen (also before recording, to find a loop's start) */
    int64_t interval;                   /* the median gap of the frames kept */
    int seen;                           /* frames seen since the player started (to know the interval before starting) */
} MLoopRec;

enum {
    MLR_SKIP = 0,                       /* not recording (waiting for a loop's start): show it, keep nothing */
    MLR_KEEP,                           /* keep it as frame number (n - 1) */
    MLR_DONE,                           /* the pass is whole: this frame is the next loop's first; keep nothing more */
    MLR_ABORT,                          /* the pass can't be cached (another size, a gap, too long): forget it */
};
void mloop_rec_reset(MLoopRec *r);
/* One frame presented, pts ms, w × h. start: recording may begin at a loop's start (a pts no further than one interval
 * from 0, after at least one frame with a known gap, or right after the pts went back). */
int  mloop_rec_frame(MLoopRec *r, int64_t pts, int w, int h, int start);
/* The pass recorded is good enough to play forever: at least 2 frames, gaps no larger than 3 intervals, and it reaches
 * the clip's end (its last frame within 3 intervals of dur_ms, when dur_ms is known). *period: the loop's length. */
int  mloop_rec_whole(const MLoopRec *r, int64_t dur_ms, int64_t *period);

/* Playback from the cache: the frame to show at t ms since the loop's start (0 ≤ t, wrapped by period), the last
 * frame whose pts ≤ t. With a cap (frames a second, 0 none) a frame sooner than 1000/cap after the shown one (shown
 * pts) is left out: the shown one stays. Returns the index; *next: ms of t when the answer can change next. */
int  mloop_pick(const int64_t *pts, int n, int64_t period, int64_t t, int cap, int shown, int64_t *next);

/* What a moving wallpaper's player does now (mambient.c): views visible (any of its NODE_MEDIA views shows a pixel: not
 * hidden behind opaque windows, a fullscreen app or a dark screen), JS paused it (Reduce motion, idle, battery…), and how
 * long no view has been visible. RUN: decode (or play the cache); PAUSE: lord-media paused, the cache's clock stopped,
 * the last frame stays; STOP: no view seen for MLOOP_HIDDEN_STOP_MS: the cache freed and lord-media ended (made again,
 * at the same place, when a view shows again). */
enum { MLH_RUN = 0, MLH_PAUSE, MLH_STOP };
int  mloop_hold(int any_visible, int js_paused, double hidden_ms);

/* The frame-rate cap of a player, from Settings (cap: 0 the video's own, 30, 15) and the battery rule (lower: 15 while
 * discharging). 0 none. */
int  mloop_cap(int setting, int battery_lower);

#endif
