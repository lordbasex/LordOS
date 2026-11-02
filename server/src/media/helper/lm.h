/* lm.h — axia-media: the LordOS media helper (see docs/MEDIA.md).
 *
 * It's the only LordOS program that links FFmpeg (libavcodec, libavformat, libswscale,
 * libswresample) and libva, and only to decode and encode: the player, the
 * synchronization and the UI are our own. axia-server launches it on demand, with a fixed path and no
 * arguments; it runs as the user, in the helpers' shared sandbox
 * (sec/sandbox.c, sandbox_apply_helper): no network, no opening files (only the descriptors that
 * axia-server passes it, including PipeWire, the render node and the camera) and with seccomp. If it crashes, the
 * desktop goes on: axia-server only sees that the socket closed. */
#ifndef LORD_MEDIA_LM_H
#define LORD_MEDIA_LM_H

#include "../proto.h"
#include "../../errcode.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>

#define LM_SOCK 3                       /* the socket with axia-server */

extern int lm_debug;
#define LM_LOG(...) do { fprintf(stderr, "axia-media[%d]: ", (int)getpid()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LM_DBG(...) do { if (lm_debug) LM_LOG(__VA_ARGS__); } while (0)

double lm_now_ms(void);                 /* monotonic clock */


/* ---- I/O (lm_io.c) ---- */
/* Opens an input file that's already open (fd) with FFmpeg, without paths. */
AVFormatContext *lm_open_input(int fd, char *err, size_t errlen);
void lm_close_input(AVFormatContext **fmt);
/* Output context over an fd (with seek if the fd allows it). */
AVFormatContext *lm_open_output(int fd, const char *format);
int  lm_close_output(AVFormatContext *oc, int write_trailer);
/* sealed memfd (can't shrink) of n bytes, mapped RW. */
int  lm_memfd(size_t n, void **map, const char *name);
int  lm_write_all(int fd, const void *p, size_t n);

/* VA-API: FFmpeg device over the render node axia-server passed (or NULL). */
AVBufferRef *lm_vaapi_device(int render_fd);
int lm_hwaccel_is(const char *params, const char *name);
/* /dev/videoN and /dev/mediaN of the Pi 5's HEVC decoder (rpi-hevc-dec) from sysfs; 0 if found. */
int lm_hevc_dec_paths(char *video, size_t vn, char *media, size_t mn);
/* Acceleration by name (parameter hwaccel=vaapi|v4l2request|none): NULL = software. */
AVBufferRef *lm_hw_device(const char *accel, int fd);
/* The name requested in the parameters ("vaapi" if it doesn't say; "none" if hw=0). */
const char *lm_hw_accel(const char *params, char *buf, size_t n);

/* Minimal JSON (writing) */
typedef struct { char *d; size_t len, cap; } LmBuf;
void lm_buf_printf(LmBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void lm_buf_str(LmBuf *b, const char *s);      /* JSON string with escapes */
void lm_buf_free(LmBuf *b);

/* Replies to axia-server. A failed result's payload and every error are code lines (../../errcode.h),
 * never prose: the UI translates them (errors.media.*). Details go to the log (LM_LOG). */
void lm_result(uint32_t id, int status, const char *payload, int fd);
void lm_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- images (lm_image.c) ---- */
/* Decodes the first frame (image or video at pos_ms) to BGRA. */
AVFrame *lm_decode_still(AVFormatContext *fmt, int64_t pos_ms, int *orientation, int prefer_cover);
/* Scales/converts a frame to BGRA (premultiplied if it has alpha) of the requested size. */
AVFrame *lm_to_bgra(const AVFrame *src, int w, int h, int premultiply);
/* Encodes BGRA to png/jpeg into an fd. */
int  lm_encode_image(const AVFrame *bgra, const char *format, int quality, int out_fd);
void lm_probe_json(AVFormatContext *fmt, LmBuf *b);

/* ---- frame output to axia-server (lm_out.c) ---- */
void lm_out_init(int native);           /* native: original size (without scaling to the box) */
int  lm_out_present(AVFrame *f, int64_t pts);   /* 0 presented, 1 dropped (no slot), -1 error */
void lm_out_ack(uint32_t id);
int  lm_out_box(int w, int h);          /* MM_SIZE; 1 if it changed */
int  lm_out_redraw(void);
int  lm_out_has_box(void);
int  lm_out_has_last(void);
const AVFrame *lm_out_last(void);

/* ---- H.264/AAC encoder into MP4 (lm_enc.c) ---- */
typedef struct LmEnc LmEnc;
LmEnc *lm_enc_open(int out_fd, int w, int h, int fps, AVBufferRef *hw, int audio_rate, int audio_ch, char *err, size_t errlen);
int    lm_enc_video(LmEnc *e, const AVFrame *f, int64_t t_ms);  /* any format: it's converted */
int    lm_enc_audio(LmEnc *e, const int16_t *pcm, int samples);
int64_t lm_enc_close(LmEnc *e);         /* writes the trailer; returns the duration in ms (or <0) */
const char *lm_enc_name(const LmEnc *e);

/* ---- microphone: ring that axia-server fills (lm_io.c) ---- */
struct ARingHdr;
/* Maps and validates the ring (mono, 48 kHz). NULL if it's unusable. */
void *lm_mic_ring(int fd, size_t *len);
/* Calls fn with whatever has arrived (s16 mono). */
void lm_mic_drain(void *ring, void (*fn)(const int16_t *pcm, int samples));

/* ---- jobs ---- */
void lm_worker(void);                                   /* lm_job.c */
void lm_player(const char *params, int fd, int pulse, int render);      /* lm_play.c */
void lm_screenrec(const char *params, int out, int pulse, int render);  /* lm_rec.c */
void lm_camera(const char *params, int dev, int render);                /* lm_cam.c */
void lm_audiorec(const char *params, int out, int pulse);               /* lm_rec.c */
void lm_hwprobe(const int *render, int n_render);                       /* lm_hwprobe.c */
void lm_stream(const char *params, int ring, int render);               /* lm_stream.c */
/* the remote desktop's software encoder with its own settings (libx264, or libvpx for VP8), for LordBench */
AVCodecContext *lm_stream_sw_encoder(int vp8, int w, int h, int fps, unsigned kbps);
void lm_vbench(const char *params);                                     /* lm_vbench.c */

#endif
