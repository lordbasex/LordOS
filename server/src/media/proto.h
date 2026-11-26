/* proto.h — protocol between lord-server and its helper lord-media (see docs/MEDIA.md).
 *
 * One SOCK_SEQPACKET socket per helper: each packet is a MediaMsg plus an optional payload
 * ("key=value" text per line or short JSON) and up to MP_MAX_FDS descriptors (SCM_RIGHTS).
 * lord-media never opens paths: every file, device or socket reaches it already open through here.
 * Video frames travel over shared memory (memfd sealed against shrinking), not over
 * the socket. */
#ifndef LORD_MEDIA_PROTO_H
#define LORD_MEDIA_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define MP_MAX_FDS     4
#define MP_PAYLOAD_MAX 60000
#define MP_RING_SLOTS  3              /* triple frame buffer */
#define MP_MAX_DIM     8192           /* maximum side of a frame or an image */

typedef struct {
    uint32_t type;                    /* MM_* */
    uint32_t id;                      /* job, frame (gen << 8 | slot) or track */
    int64_t  v[6];                    /* numeric arguments */
    uint32_t len;                     /* payload bytes that follow */
    uint32_t pad;
} MediaMsg;

/* Helper types (first message, MM_JOB with v[0] = MK_*). */
enum {
    MK_PLAY = 1,        /* player: fds = file, audio ring (aring.h, or -1), render node (or -1) */
    MK_WORKER,          /* standalone queued jobs (MW_*), one per MM_JOB message */
    MK_SCREENREC,       /* screen recording: fds = mp4 output, microphone ring (or -1), render node */
    MK_CAMERA,          /* V4L2 camera: fds = /dev/videoN, render node. lord-studio (LM_STUDIO): fds = /dev/videoN, render
                           node (when render=1), the segmenter's model; with probe=1 no camera (a pipeline self-test) */
    MK_AUDIOREC,        /* audio recorder: fds = wav output, microphone ring */
    MK_HWPROBE,         /* which video codecs VA-API decodes: fds = render node → one MM_RESULT
                           with JSON {"vaapi":bool,"vendor":"…","vp9":bool,"av1":bool,"h264":bool,"hevc":bool} */
    MK_STREAM,          /* remote desktop live encoder (docs/REMOTE.md, "Video (WebRTC)"): fds = audio ring
                           (or -1), render node (or -1); params codec=h264|vp8, width, height, fps, kbps */
    MK_VBENCH,          /* LordBench's video measurement (lm_vbench.c, docs/BENCHMARK.md): no fds; params full=0|1.
                           Clips generated in memory; answers MM_BENCH records, then MM_STATE MS_DONE */
    MK_LIVE,            /* a packaged app's live stream (lm_live.c, live.h): fds = link to its companion, audio ring (or
                           none). Encoded pictures and Opus in; frame slots, sound and, with MM_RECORD, the microphone out */
    /* lord-stream only (0.8.8.10, src/media/stream, streamurl.h): MK_PLAY with no file (params url=…; fds = audio ring,
       render node), and the channels of an M3U list: */
    MK_M3U,             /* params url=http(s)://… → MM_RESULT JSON chunks [{name,group,logo,url}], v[1] = 1 while more follow */
};

/* Jobs of the MK_WORKER helper (MM_JOB with v[0] = MW_*). */
enum {
    MW_PROBE = 1,       /* fds = file → JSON: duration, tracks, tags */
    MW_THUMB,           /* fds = file, png output; v[1] = maximum side */
    MW_IMAGE,           /* fds = file → premultiplied BGRA memfd (v: width, height) */
    MW_EDIT,            /* fds = file, output; params: crop, rotation, brightness, contrast, format */
    MW_ENCODE,          /* fds = BGRA memfd, output; v[1] = width, v[2] = height; params: format */
    MW_TRANSCODE,       /* fds = file, output; params: format (opus|mp3|wav), from, to */
    MW_WAVEFORM,        /* fds = file; v[1] = number of bars → JSON with peaks 0..1000 */
    MW_SELFTEST,        /* tests the sandbox (open, network, processes): everything has to fail */
    MW_SVG,             /* an SVG icon (helper/lm_svg.h): fds = file (at most 1 MB); v[1] = side 16..256, v[2] = 0xRRGGBB
                           to recolor (a -symbolic icon) or -1 → premultiplied BGRA memfd, JSON {"width","height"} */
};

enum {
    /* lord-server → lord-media */
    MM_JOB = 1,         /* v[0] = MK_* or MW_*; payload = key=value parameters */
    MM_SIZE,            /* v[0], v[1]: size of the box where it's shown (px) */
    MM_PLAY,
    MM_PAUSE,
    MM_SEEK,            /* v[0] = ms */
    MM_VOLUME,          /* v[0] = 0..150 (%) */
    MM_TRACK,           /* v[0] = 'a' audio | 's' subtitles, v[1] = track index (-1 none) */
    MM_STOP,            /* finish (recordings: close the file properly) */
    MM_ACK,             /* id = frame that became shown: the other slots are free */
    MM_INRING,          /* fd = memfd of input frames; v: slots, width, height */
    MM_INFRAME,         /* id = slot with a new frame; v[0] = ms since the start */
    MM_PHOTO,           /* camera: fd = jpeg output; v[0] = mirrored */
    MM_RECORD,          /* camera: fds = mp4 output, microphone ring (or no fd); no fds: finish.
                           live: fd = microphone ring; no fd: the microphone stops */
    MM_LOOP,            /* v[0] = 1: start over when it ends */
    MM_KEYFRAME,        /* live encoder: the next frame is a keyframe */
    MM_RATE,            /* live encoder: v[0] = target kbit/s */
    MM_LINK,            /* camera: fd = live link (live.h); v[0] = 1 VP8, 0 H.264; v[1] = kbit/s. Its pictures also go there */
    /* lord-studio only (LordOS Studio, src/media/studio, docs/CAMERA-STUDIO-PLAN.md); lord-media ignores them */
    MM_STUDIO,          /* v[0] = mode (ST_MODE_*: 0 none, 1 blur, 2 picture), v[1] = blur 0..100, v[2] = mirror */
    MM_STUDIO_BG,       /* fd = the background picture (a file lord-server opened and checked); no fd: none */
    MM_STUDIO_CAM,      /* lord-studio with loopback=1: v[0] = what goes to the loopback (0 the idle card, 1 the camera, 2 the
                           off card); fd = the real camera when v[0] = 1 and it changes (none: keep the open one); v[1] = 1
                           the preview's frames are wanted in the ring too */
    MM_FORMAT,          /* player: v[0] = the frame format wanted from the next frame on (MF_BGRA, MF_NV12); lord-server asks
                           NV12 only while the GPU composes (docs/MEDIA.md, "Video frames"). Anything else: BGRA */
    MM_FPSCAP,          /* player: v[0] = at most this many frames a second are sent (0: the video's own rate); the others are
                           decoded (they are references) but never read back, converted or sent */

    /* lord-media → lord-server */
    MM_INFO = 64,       /* payload = JSON with the file's information */
    MM_RING,            /* fd = frames memfd; v[0] = gen, v[1] = width, v[2] = height, v[3] = slots, v[4] = MF_* (0 BGRA),
                           v[5] = the colours of an NV12 ring (MC_*). Slot layout: mp_ring_layout, never the helper's word */
    MM_FRAME,           /* id = gen << 8 | slot; v[0] = pts ms */
    MM_POS,             /* v[0] = position ms, v[1] = duration ms, v[2] = playing */
    MM_STATE,           /* v[0] = MS_* */
    MM_SUB,             /* v[0] = from ms, v[1] = to ms; payload = text */
    MM_LEVEL,           /* v[0] = level 0..1000, v[1] = ms recorded */
    MM_STATS,           /* v: decoded, shown, dropped, by hardware, decoding ms (x100) */
    MM_RESULT,          /* id = job; v[0] = 0 ok / <0 error; payload = JSON or message; optional fd */
    MM_SLOTFREE,        /* id = input slot already processed */
    MM_ERROR,           /* payload = message */
    MM_AUDIO,           /* there's new PCM in the audio ring (or a seek: discard_to/serial) */
    MM_PACKET,          /* live encoder: id = 0 video, 1 audio; v[0] = total bytes, v[1] = offset of this
                           chunk, v[2] = pts (ms for video, 48 kHz samples for audio), v[3] = 1 keyframe */
    MM_BENCH,           /* MK_VBENCH: id = measurement (VB_*); v[0] = VBR_*; v[1] = frames/s ×1000, v[2] = spread ×1000,
                           v[3] = why skipped (VBS_*) */
    MM_STUDIO_STATS,    /* lord-studio, once a second: v[0] = frames done, v[1] = segmenter µs, v[2] = matte and composite µs,
                           v[3] = 1 when the GPU composites */
    MM_DMAFRAME,        /* zero-copy (docs/MOVING-WALLPAPERS.md): id = frame number; v[0] = pts ms; payload = MpDma; fds = its
                           1..2 dmabuf objects (the VA-API surface, exported read-only). lord-server checks it (mp_dma_check),
                           imports it, answers MM_ACK with the number; a frame refused sends the player back to MF_NV12 */
    MM_STUDIO_READERS,  /* lord-studio with loopback=1, on every change: v[0] = readers streaming from the loopback (its
                           client-usage event; -1 when the module doesn't send it), v[1] = 1 the real camera is open,
                           v[2] = what the loopback shows (0 the idle card, 1 the camera, 2 the off card) */
};

/* MK_VBENCH's measurements, in order; lord-server maps them to its own ids (bench/lbvideo.c) */
enum { VB_H264_1080, VB_HEVC_1080, VB_VP9_1080, VB_AV1_1080, VB_H264_2160, VB_HEVC_2160, VB_ENC_H264, VB_ENC_VP8, VB_COUNT };
enum { VBR_STEP = 0, VBR_RESULT, VBR_SKIP };
enum { VBS_NOCODEC = 1, VBS_FAILED, VBS_MODE };

/* Frame formats of a ring (MM_RING v[4]). BGRA: premultiplied B, G, R, A bytes, w × h × 4 a slot. NV12 (4:2:0): the Y
 * plane (w × h bytes, stride w) then the interleaved U, V plane (w/2 × h/2 pairs, stride w) right after it, w and h even:
 * 1.5 bytes a pixel. Strides and offsets are never sent: both sides derive them from w and h. */
enum { MF_BGRA = 0, MF_NV12 = 1, MF_DMABUF = 2, MF_COUNT };   /* MF_DMABUF: MM_FORMAT only (MM_DMAFRAME, never a ring) */
/* An NV12 ring's colours (MM_RING v[5]): the matrix in the low byte (MC_601, MC_709, MC_2020), bit 8 full range */
enum { MC_601 = 0, MC_709 = 1, MC_2020 = 2, MC_MATRICES };
#define MC_FULL 0x100

typedef struct {
    int fmt, w, h, slots;
    size_t slot_bytes;                /* bytes of one slot */
    size_t y_off, uv_off;             /* NV12: where the planes start inside a slot (BGRA: 0, 0) */
    int y_stride, uv_stride;          /* bytes a row (BGRA: w × 4, 0) */
    int matrix, full;                 /* NV12's colours */
} MpRing;
/* A ring's layout from what MM_RING says (v[1..5]), checked strictly: a known format, 1..MP_MAX_DIM sides, even sides for
 * NV12, 1..MP_RING_SLOTS slots, a known matrix and no other bit in the colours, BGRA with colours 0. 0 ok (out filled),
 * -1 refused. The memfd must then hold slots × slot_bytes (map_sealed checks the size and the seal). */
int mp_ring_layout(int64_t fmt, int64_t w, int64_t h, int64_t slots, int64_t colors, MpRing *out);

/* A zero-copy frame's description (MM_DMAFRAME's payload, exactly this size). Two planes, each in one of the objects
 * (the fds that came with the message, in order): Y (R8, or R16 for 10 bits) and the interleaved U, V (GR88, or GR1616). */
#define MP_DMA_MAGIC 0x414d444cu          /* "LDMA" */
#define MP_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MP_FOURCC_R8     MP_FOURCC('R', '8', ' ', ' ')
#define MP_FOURCC_GR88   MP_FOURCC('G', 'R', '8', '8')
#define MP_FOURCC_R16    MP_FOURCC('R', '1', '6', ' ')
#define MP_FOURCC_GR1616 MP_FOURCC('G', 'R', '3', '2')
#define MP_DMA_MAX_OBJ 2
#define MP_DMA_MAX_PITCH 65536
#define MP_DMA_MAX_BYTES (512u << 20)     /* one object */
typedef struct {
    uint32_t magic;
    uint32_t w, h;                        /* the picture, even sides */
    uint32_t colors;                      /* MC_* | MC_FULL */
    uint32_t nobj;                        /* objects (fds) */
    uint32_t pad;
    uint64_t modifier[MP_DMA_MAX_OBJ];    /* each object's DRM format modifier */
    struct { uint32_t fourcc, obj, offset, pitch; } plane[2];
} MpDma;
/* Checks a description against what came with it: len its bytes, nfds the fds received, size[i] each object's real size
 * (lseek SEEK_END on lord-server's side, never the helper's word), max_w × max_h the largest picture accepted (the box
 * the player was asked for, rounded up a little; never more than MP_MAX_DIM), plane_ok(fourcc, modifier) the driver's
 * allowlist (NULL: any of the four formats). Every plane must lie inside its object (offset + pitch × rows, no overflow),
 * a pitch at least a row's bytes and at most MP_DMA_MAX_PITCH, Y and U-V of the same depth, two planes in one object
 * apart, every object used. 0 ok, -1 refused. */
int mp_dma_check(const void *payload, size_t len, int nfds, const int64_t size[MP_DMA_MAX_OBJ], int max_w, int max_h,
                 int (*plane_ok)(uint32_t fourcc, uint64_t modifier));

enum { MS_LOADING = 0, MS_PLAYING, MS_PAUSED, MS_ENDED, MS_RECORDING, MS_DONE, MS_FAILED };

/* Sends a message (doesn't block: MSG_DONTWAIT). 0 on success, -1 error (EAGAIN included). */
int mp_send(int sock, uint32_t type, uint32_t id, const int64_t v[6], const void *payload, size_t len,
            const int *fds, int nfds);
/* Shortcut without payload or descriptors. */
int mp_sendv(int sock, uint32_t type, uint32_t id, int64_t a, int64_t b, int64_t c, int64_t d);
/* Receives a message. payload receives the NUL-terminated payload (cap >= MP_PAYLOAD_MAX + 1).
 * fds: up to MP_MAX_FDS (with O_CLOEXEC), -1 in the ones that didn't arrive. Returns 1 if a
 * valid message arrived, 0 if the other side closed, -1 if there's nothing (EAGAIN) or the message is invalid
 * (in that case it already closed any descriptors that arrived). block: waits. */
int mp_recv(int sock, MediaMsg *m, char *payload, size_t cap, int fds[MP_MAX_FDS], int block);

/* "key=value\n" parameters: copies the value of key into out (0 if present). */
int mp_param(const char *params, const char *key, char *out, size_t n);
long long mp_param_int(const char *params, const char *key, long long def);
double mp_param_num(const char *params, const char *key, double def);

#endif
