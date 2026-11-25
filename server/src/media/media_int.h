/* media_int.h — internal parts of lord-server's media (media.c, mview.c, capture.c,
 * annot.c and ui/js_media.c). */
#ifndef LORD_MEDIA_INT_H
#define LORD_MEDIA_INT_H

#include "media.h"
#include "mcursor.h"
#include "proto.h"

#include <stddef.h>
#include <sys/types.h>

#define MEDIA_MAX_SRC 48
#define MEDIA_APP_MAX 48
#define MEDIA_CROP_NODES 64       /* LAG_WIN_MAX: a machine's windows in apps mode */
#define MEDIA_VIEW_NODES 16       /* plain nodes showing all of one source (a moving wallpaper on every display) */

typedef struct MAudio MAudio;

enum { SK_NONE = 0, SK_PLAYER, SK_CAMERA, SK_IMAGE, SK_CAPTURE, SK_SCREENREC, SK_AUDIOREC, SK_STREAM, SK_M3U };   /* SK_M3U: lord-stream's channel list */

/* How a source is shown in its node. */
typedef struct {
    int fit;                    /* 0 fit (contain), 1 fill (cover) */
    float zoom;                 /* 0 = per fit; 1 = 100 % (one image pixel per pixel) */
    float cx, cy;               /* visible center, 0..1 of the image (with zoom) */
    int rot;                    /* 0, 90, 180, 270 */
    int mirror;                 /* horizontal mirror (camera) */
    uint32_t bg;                /* background of the bands (ARGB; 0 transparent) */
    int bright, contrast;       /* Photos preview: -100..100 */
    unsigned gen;               /* changes with each setView */
} MediaView;

typedef struct {
    int used, id, kind;
    int studio;                 /* a LordOS Studio camera (lord-studio, mstudio.c): it gets the effects' settings */
    char app[MEDIA_APP_MAX];
    /* helper (player, camera, recordings) */
    int sock;
    pid_t pid;
    int state;                  /* MS_* */
    int closing;                /* closing was requested: wait for the end (recordings) */
    /* pixels: helper slots, image (memfd) or capture (own) */
    uint8_t *map;
    size_t map_len;
    int ring_gen, ring_w, ring_h, ring_slots, cur_slot;
    uint32_t *px;               /* capture or image: premultiplied ARGB */
    int w, h, opaque;
    int xrgb;                   /* the 4th byte means nothing (QEMU's x8r8g8b8): drawn with alpha 255 */
    unsigned frame_seq;         /* changes with each new frame */
    unsigned painted_seq;       /* the frame_seq the last paint read (Machines acknowledges frames with it) */
    /* drawing */
    MediaView view;
    int node, box_w, box_h, sent_w, sent_h;     /* node: the plain node painted last (its pointer, Machines) */
    /* every plain node (no Node.crop) that shows all of it, with the box (physical px) its last paint had: one player
     * shared by the displays (sdk/motion.jsx) damages and presents each one, not only the one painted last (mviews.c) */
    int view_nodes[MEDIA_VIEW_NODES], view_w[MEDIA_VIEW_NODES], view_h[MEDIA_VIEW_NODES];
    int views_over;                     /* a node found no slot: a new frame damages the whole screen */
    int crop_nodes[MEDIA_CROP_NODES];   /* nodes that show a part of it (Node.crop): each one damaged for its part */
    uint32_t *cache;            /* scaled/rotated (when copying isn't enough) */
    int cache_w, cache_h;
    unsigned cache_seq, cache_gen;
    float cache_box[4];
    float shown_scale;          /* physical pixels per picture pixel at the last paint (the pointer follows it) */
    /* where the last paint of its node put the picture, as fractions of the node's content box (so the logical and
     * physical boxes agree): picture pixel (u, v) at (map_fx + u·map_kx, map_fy + v·map_ky). map_ok: neither rotated
     * nor mirrored, and map_w × map_h is the picture's size then. A new frame damages only where it changed
     * (media_src_damage_rect) */
    int map_ok, map_w, map_h;
    float map_fx, map_fy, map_kx, map_ky;
    /* the source's own pointer (a Machines guest's): drawn instead of the host's over its node */
    McurImage cur;              /* as the source sent it, premultiplied and cut */
    int cur_mode;               /* 0 none (the host's arrow), 1 the image, 2 hidden */
    int cur_off;                /* the source said its pointer is hidden right now */
    int guest_draws;            /* the guest draws its pointer into the picture itself (Windows' display drivers): no
                                 * host arrow over it until it hands one over (a second arrow beside its own) */
    McurImage cur_shown;        /* cur at cur_shown_f */
    float cur_shown_f;
    uint32_t *mip[8];           /* halvings (large photos) */
    int mip_w[8], mip_h[8], nmip;
    /* a picture that is a GPU texture (a Machines 3D or vGPU picture; docs/MACHINES-GPU.md, "A vGPU's picture without
     * a readback"): with the GPU composing it's drawn straight from gtex below the UI (wl_paint_media). map/px then
     * hold an older frame (cpu_stale) until something needs the pixels (a capture, a recording, a rotation, the CPU
     * path): media_src_need_cpu asks the owner (cpu_read) to read the picture back into them */
    struct GpuTex *gtex;
    int gtex_flip;                      /* its first row is the picture's bottom */
    int cpu_stale;
    int gpu_drawn;                      /* the last paint of its node(s) drew gtex: a new frame needs only a present */
    int (*cpu_read)(int id);            /* 0 when map holds the shown frame now */
    /* a picture whose own pixels (px) the GPU shows (a Machines 2D picture; MACHINES-GPU.md Part 1 step 5): its owner sets
     * tex_px while the GPU may draw it; the texture is ptex (gtex points at it while shown), made whole at a paint when it
     * isn't current (a new picture, a new GPU context), then only the changed rectangles go up (media_src_tex_rect) */
    int tex_px;
    struct GpuTex *ptex;
    /* what changed since the scaled copy (cache) was made: pd_n frames from cache_seq on, inside pd_* (picture pixels);
     * a paint of a scaled picture remakes only that part of the cache (Part 1 step 5, the CPU path) */
    unsigned pd_base, pd_last;
    int pd_n, pd_x0, pd_y0, pd_x1, pd_y1;
    unsigned pd_band, pd_full;          /* copies remade in part, whole (mview-test) */
    /* player state */
    int64_t pos, dur;
    int playing;
    /* recordings: temporary file → final one when it ends well */
    char tmp[4096], dst[4096];
    /* screen recording: input slots (lord-server writes) */
    int in_fd, in_slots, in_w, in_h, in_busy[4];
    uint8_t *in_map;
    size_t in_len;
    double rec_start, rec_last;
    int rec_fps;
    int rec_x, rec_y, rec_fw, rec_fh;   /* screen recording of an area: its place in the full frame (rec_fw × rec_fh) */
    int rec_cursor;                     /* screen recording: the pointer is drawn into it */
    long rec_frames, rec_dropped;
    int64_t stats[6];
    int job;                    /* image: job that decodes it */
    MAudio *audio;              /* player: the PipeWire stream (lord-server's) */
    MAudio *mic;                /* recordings: the microphone (lord-server's, with permission) */
    /* camera of an isolated app (sec/brokercam.c): its helper's messages go to the app over the
     * broker instead of the session; bk_msg returns 1 when it consumed the message */
    void *bk;
    int (*bk_msg)(void *src, const MediaMsg *m, const char *payload, int *fds);
    /* its picture belongs to a protected window (shown in a NODE_MEDIA inside a protected node, or marked by its owner:
     * lord.vm.protect): never copied out of lord-server (media_src_exportable). Set once, never cleared. */
    int protect;
} MediaSrc;

/* media.c */
MediaSrc *media_src_new(int kind, const char *app);
void media_cursor_touch(void);          /* a source's pointer changed (or the scale it's drawn at) */
MediaSrc *media_src_get(int id);
MediaSrc *media_src_at(int i);          /* iterate: 0..MEDIA_MAX_SRC-1 (NULL if free) */
void media_src_close(MediaSrc *s);
void media_src_damage(MediaSrc *s);
/* A new frame changed (x, y, w, h) of the picture: the node that shows all of it is damaged whole (as
 * media_src_damage), each node showing a part only where its part changed. Returns how many nodes were damaged. */
int  media_src_damage_rect(MediaSrc *s, int x, int y, int w, int h);
/* for the scaled copy (mview.c): (x, y, w, h) of the picture changed in the frame frame_seq is now */
static inline void media_pd_note(MediaSrc *s, int x, int y, int w, int h) {
    if (s->pd_base != s->cache_seq || s->pd_n <= 0) { s->pd_base = s->cache_seq; s->pd_n = 0; s->pd_x0 = x; s->pd_y0 = y; s->pd_x1 = x + w; s->pd_y1 = y + h; }
    else {
        if (x < s->pd_x0) s->pd_x0 = x;
        if (y < s->pd_y0) s->pd_y0 = y;
        if (x + w > s->pd_x1) s->pd_x1 = x + w;
        if (y + h > s->pd_y1) s->pd_y1 = y + h;
    }
    if (!s->pd_n || s->pd_last != s->frame_seq) { s->pd_n++; s->pd_last = s->frame_seq; }
}
/* the GPU picture made of px (tex_px): (x, y, w, h) of px changed. 1 when the shown texture has it now (a present is
 * enough), 0 when it isn't shown as a texture (the next paint makes it whole or draws px) */
int  media_src_tex_rect(MediaSrc *s, int x, int y, int w, int h);
/* tex_px off: the CPU's pixels are drawn again */
void media_src_tex_off(MediaSrc *s);
/* at a paint: ptex made whole when tex_px is on and it isn't the shown, current texture (mview.c) */
void media_src_tex_fresh(MediaSrc *s);
/* how many nodes show a part of it now */
int  media_src_crop_count(MediaSrc *s);
/* mviews.c: a plain node shows all of s (media_paint, js_media_node), at bw × bh physical px (0: not painted yet); a node
 * dropped (it shows a part now). media_src_view_count: the live plain nodes. media_src_view_box: the box the helper
 * scales to, the largest one shown (0 if none) */
void media_src_view_note(MediaSrc *s, int node, int bw, int bh);
void media_src_view_drop(MediaSrc *s, int node);
int  media_src_view_count(MediaSrc *s);
int  media_src_view_box(MediaSrc *s, int *bw, int *bh);
int  media_spawn(int kind, const char *params, const int *fds, int nfds, pid_t *pid);
/* the same with lord-stream (0.8.8.10: network players and M3U lists, src/media/stream; streamurl.h) */
int  media_spawn_stream(int kind, const char *params, const int *fds, int nfds, pid_t *pid);
/* lord-studio: LordOS Studio's camera (mstudio.c) */
int  media_spawn_studio(const char *params, const int *fds, int nfds, pid_t *pid);
int  media_has_studio(void);
int  media_pulse_fd(void);              /* socket connected to PipeWire (the user's) or -1 */
int  media_render_fd(void);             /* render node for VA-API or -1 */
int  media_render_fds(int *out, int max);  /* the compositor's render node first, then the others; how many */
const char *media_hwaccel(void);        /* "vaapi" | "v4l2request" | "none" (LORD_MEDIA_HWACCEL) */
void media_event(int id, const char *type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void media_json_str(char *out, size_t n, const char *s);
const char *media_home(void);

/* worker jobs */
enum { JP_EVENT = 0, JP_THUMB, JP_IMAGE, JP_PIXELS, JP_FILE, JP_CLIP, JP_SVG, JP_PORTAL };    /* JP_SVG: svgicon.c (params = its key) */
/* JP_PORTAL: a picture for the Screenshot portal (ui/portalshot.c): when the file is in place (or not), this is called
 * with the job's src (the portal's request) and the final path (NULL when it failed) */
extern void (*media_portal_saved)(int tag, int ok, const char *path);
int  media_job(int bg, int kind, int purpose, const int *fds, int nfds, int64_t a, int64_t b, const char *params,
               const char *tmp, const char *dst, int src);
/* Pixels of a ready poster (lord.media.take): returns RGBA in *out (malloc) or -1. */
int  media_take_pixels(int job, uint8_t **out, int *w, int *h);

/* Paths: input in the home folder or mounts (realpath) and temporary + final output. */
int  media_open_input(const char *path, char *real, size_t n);
int  media_open_output(const char *path, char *tmp, size_t tn, char *dst, size_t dn);
int  media_finish_output(const char *tmp, const char *dst, int ok);
/* Thumbnail: the path in ~/.cache/lordos/thumbs if it's already there; otherwise it requests it and returns NULL. */
const char *media_thumb(const char *path, int size, char *out, size_t n);
int  media_cameras(char *json, size_t n);
/* the same list on an fsasync worker: a request id, answered as __lordFsReply(req, { cameras: [...] }); -1 + err */
int  media_cameras_async(char *err, size_t errlen);

/* maudio.c: the media's PipeWire streams (lord-media doesn't have the socket) */
MAudio *maudio_open(int rec, const char *app, const char *title, const char *role_or_source, int *ring_fd);
void maudio_close(MAudio *a);
void maudio_service(MAudio *a);
int  maudio_pollfd(const MAudio *a, short *events);
/* Recording in "broadcast" mode (the consumer reads with its own cursor and doesn't touch rpos): the server
 * frees space by advancing rpos up to wpos, so the ring never jams. See docs/PRIVACY.md. */
void maudio_mic_drain(MAudio *a);
void maudio_playing(MAudio *a, int on);
void maudio_volume(MAudio *a, double v);
int  maudio_overruns(const MAudio *a);
int  maudio_failed(const MAudio *a);
int  maudio_retry(MAudio *a);             /* a failed stream connects again, same ring (0), or -1 it hadn't failed */
int  maudio_target(MAudio *a, const char *node);   /* the output or input it uses (node.name, "" the default); moves a live stream */
void maudio_latency(MAudio *a, int ms);   /* playback: a lower target buffer (ms), before its first service */
uint64_t maudio_bytes(const MAudio *a);   /* bytes taken from the ring (playback) or put in it (recording) */
int  maudio_ring_fd(const MAudio *a);     /* the ring's memfd (still owned by a), to hand a helper again */

/* mview.c */
void media_view_reset(MediaSrc *s);

/* capture.c */
/* display: a display's connector id (lord.screen.displays[].id): that display at its physical resolution; 0 for
 * Settings' choice ("Screenshots and sharing": the main display, or every display at 100 %). */
/* until: 0, or the node of a window's frame: the screen painted up to that window and no further, so the
 * window is whole even where other windows covered it (paint_capture_until; NULL if it isn't reached).
 * also: nalso nodes painted over that window (its sheets in a shell layer), the rest still left out.
 * refused: set to 1 when until is a protected window (never captured alone: media.protectedWindow). */
/* May src's pixels leave the compositor as a picture of their own (a file, the clipboard, OCR, a crop, a card picture, a
 * recording)? 0 when they are a protected window's: src is shown in a NODE_MEDIA inside a protected node (hidden or
 * minimized too), or was before, or its owner marked it (media_src_protect). The one gate for every such path. */
int  media_src_exportable(MediaSrc *src);
void media_src_protect(MediaSrc *src);
MediaSrc *media_screenshot(const char *app, int x, int y, int w, int h, uint32_t display, int until, const int *also, int nalso, int *refused);
void media_capture_size(int *w, int *h);     /* recording / remote desktop frame size (docs/DISPLAYS.md) */
MediaSrc *media_crop(MediaSrc *src, int x, int y, int w, int h);
MediaSrc *media_resize(MediaSrc *src, int w, int h);          /* new capture source, w × h */
MediaSrc *media_rotate(MediaSrc *src, int turns);             /* new capture source, quarter turns clockwise */
/* An area of the recording's frame (lx, ly, lw, lh logical, in the global space) → its physical rectangle in the
 * frame, even-sized: 0 ok, -1 when nothing of at least 16 × 16 is left on the recorded display. */
int  media_capture_region(double lx, double ly, double lw, double lh, int *x, int *y, int *w, int *h);
/* region: NULL (the whole frame) or { x, y, w, h } from media_capture_region; cursor: draw the pointer into it */
int  media_screenrec_start(MediaSrc *s, int fps, const int *region, int cursor);
/* A recording of one picture instead of the screen (a Machines guest, 6.20): the same input ring and encoder, w × h
 * fixed at start; the caller feeds it (a picture of another size is drawn into its top left, the rest black). It never
 * becomes the screen recording, so the screen keeps its own. 0 ok. */
int  media_framerec_start(MediaSrc *s, int w, int h, int fps);
/* one frame, at most at the recording's rate (sooner ones are skipped); 1 sent, 0 skipped */
int  media_framerec_feed(MediaSrc *s, const uint32_t *px, int w, int h);
void media_capture_src_closed(MediaSrc *s);
int  media_memfd_from_pixels(const uint32_t *px, int w, int h);

/* annot.c: annotations over a capture */
typedef struct {
    int type;                   /* 'a' arrow, 'b' box, 't' text, 'z' blur, 'l' line, 'f' fill, 'p' pixelate */
    float x1, y1, x2, y2, width, size;
    uint32_t color;
    char text[512];
} MediaOp;
void media_annotate(uint32_t *px, int w, int h, const MediaOp *ops, int n, int opaque);

/* map/px brought up to date with the frame gtex shows (a readback), when cpu_stale; mview.c and the users of px */
void media_src_need_cpu(MediaSrc *s);
/* a new frame of a picture drawn by the GPU (gpu_drawn): no UI damage, its node(s) presented again (main.c) */
void media_src_present(MediaSrc *s);

#endif
