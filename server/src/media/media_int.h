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

typedef struct MAudio MAudio;

enum { SK_NONE = 0, SK_PLAYER, SK_CAMERA, SK_IMAGE, SK_CAPTURE, SK_SCREENREC, SK_AUDIOREC, SK_STREAM };

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
    int node, box_w, box_h, sent_w, sent_h;
    uint32_t *cache;            /* scaled/rotated (when copying isn't enough) */
    int cache_w, cache_h;
    unsigned cache_seq, cache_gen;
    float cache_box[4];
    float shown_scale;          /* physical pixels per picture pixel at the last paint (the pointer follows it) */
    /* the source's own pointer (a Machines guest's): drawn instead of the host's over its node */
    McurImage cur;              /* as the source sent it, premultiplied and cut */
    int cur_mode;               /* 0 none (the host's arrow), 1 the image, 2 hidden */
    int cur_off;                /* the source said its pointer is hidden right now */
    McurImage cur_shown;        /* cur at cur_shown_f */
    float cur_shown_f;
    uint32_t *mip[8];           /* halvings (large photos) */
    int mip_w[8], mip_h[8], nmip;
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
    long rec_frames, rec_dropped;
    int64_t stats[6];
    int job;                    /* image: job that decodes it */
    MAudio *audio;              /* player: the PipeWire stream (lord-server's) */
    MAudio *mic;                /* recordings: the microphone (lord-server's, with permission) */
    /* camera of an isolated app (sec/brokercam.c): its helper's messages go to the app over the
     * broker instead of the session; bk_msg returns 1 when it consumed the message */
    void *bk;
    int (*bk_msg)(void *src, const MediaMsg *m, const char *payload, int *fds);
} MediaSrc;

/* media.c */
MediaSrc *media_src_new(int kind, const char *app);
void media_cursor_touch(void);          /* a source's pointer changed (or the scale it's drawn at) */
MediaSrc *media_src_get(int id);
MediaSrc *media_src_at(int i);          /* iterate: 0..MEDIA_MAX_SRC-1 (NULL if free) */
void media_src_close(MediaSrc *s);
void media_src_damage(MediaSrc *s);
int  media_spawn(int kind, const char *params, const int *fds, int nfds, pid_t *pid);
int  media_pulse_fd(void);              /* socket connected to PipeWire (the user's) or -1 */
int  media_render_fd(void);             /* render node for VA-API or -1 */
const char *media_hwaccel(void);        /* "vaapi" | "v4l2request" | "none" (LORD_MEDIA_HWACCEL) */
void media_event(int id, const char *type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void media_json_str(char *out, size_t n, const char *s);
const char *media_home(void);

/* worker jobs */
enum { JP_EVENT = 0, JP_THUMB, JP_IMAGE, JP_PIXELS, JP_FILE, JP_CLIP };
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

/* mview.c */
void media_view_reset(MediaSrc *s);

/* capture.c */
MediaSrc *media_screenshot(const char *app, int x, int y, int w, int h);
void media_capture_size(int *w, int *h);     /* recording / remote desktop frame size (docs/DISPLAYS.md) */
MediaSrc *media_crop(MediaSrc *src, int x, int y, int w, int h);
int  media_screenrec_start(MediaSrc *s, int fps);
void media_capture_src_closed(MediaSrc *s);
int  media_memfd_from_pixels(const uint32_t *px, int w, int h);

/* annot.c: annotations over a capture */
typedef struct {
    int type;                   /* 'a' arrow, 'b' box, 't' text, 'z' blur, 'l' line */
    float x1, y1, x2, y2, width, size;
    uint32_t color;
    char text[512];
} MediaOp;
void media_annotate(uint32_t *px, int w, int h, const MediaOp *ops, int n);

#endif
