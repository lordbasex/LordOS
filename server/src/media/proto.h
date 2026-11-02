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
    MK_CAMERA,          /* V4L2 camera: fds = /dev/videoN, render node */
    MK_AUDIOREC,        /* audio recorder: fds = wav output, microphone ring */
    MK_HWPROBE,         /* which video codecs VA-API decodes: fds = render node → one MM_RESULT
                           with JSON {"vaapi":bool,"vendor":"…","vp9":bool,"av1":bool,"h264":bool,"hevc":bool} */
    MK_STREAM,          /* remote desktop live encoder (docs/REMOTE.md, "Video (WebRTC)"): fds = audio ring
                           (or -1), render node (or -1); params codec=h264|vp8, width, height, fps, kbps */
    MK_VBENCH,          /* LordBench's video measurement (lm_vbench.c, docs/BENCHMARK.md): no fds; params full=0|1.
                           Clips generated in memory; answers MM_BENCH records, then MM_STATE MS_DONE */
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
    MM_RECORD,          /* camera: fds = mp4 output, microphone ring (or no fd); no fds: finish */
    MM_LOOP,            /* v[0] = 1: start over when it ends */
    MM_KEYFRAME,        /* live encoder: the next frame is a keyframe */
    MM_RATE,            /* live encoder: v[0] = target kbit/s */

    /* lord-media → lord-server */
    MM_INFO = 64,       /* payload = JSON with the file's information */
    MM_RING,            /* fd = frames memfd; v[0] = gen, v[1] = width, v[2] = height, v[3] = slots */
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
};

/* MK_VBENCH's measurements, in order; lord-server maps them to its own ids (bench/lbvideo.c) */
enum { VB_H264_1080, VB_HEVC_1080, VB_VP9_1080, VB_AV1_1080, VB_H264_2160, VB_HEVC_2160, VB_ENC_H264, VB_ENC_VP8, VB_COUNT };
enum { VBR_STEP = 0, VBR_RESULT, VBR_SKIP };
enum { VBS_NOCODEC = 1, VBS_FAILED, VBS_MODE };

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
