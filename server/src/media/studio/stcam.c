/* stcam.c — lord-studio's side of the camera (docs/CAMERA-STUDIO-PLAN.md 3.1; AGENTS.md, lord-studio).
 *
 * lord-studio is lord-media built with LM_STUDIO: the same camera job (helper/lm_cam.c, MK_CAMERA) with every decoded
 * picture passed through LordOS Studio's pipeline (st.h) before it goes to the preview slots, the photo, the video
 * and a companion's link. It receives only fds that lord-server opened: the camera, the render node (or none), the
 * segmenter's model (hash already checked) and, later, a background picture. Its messages are numbers: MM_STUDIO
 * (mode, blur, mirror), MM_STUDIO_BG (an fd, or none to clear) and, back, MM_STUDIO_STATS once a second.
 * With loopback=1 (phase 2, stcam_hub below) it is instead the session's one writer of the "LordOS Studio" V4L2
 * device: no camera at the start, the loopback's fd, and the camera only when lord-server sends it (MM_STUDIO_CAM). */
#ifdef LM_STUDIO
#include "../helper/lm.h"
#include "st.h"
#include "stout.h"
#include "models.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

static StFx *fx;
static AVFrame *in_bgra, *out_bgra;
static struct SwsContext *sws;
static int sws_w, sws_h, sws_fmt;
static double stats_at, vid_ms;
static int vid_frames;                      /* a background video's pictures shown (the self-test) */

/* reads the whole model (at most STUDIO_MODEL_MAX) from the fd lord-server checked */
static unsigned char *read_model(int fd, size_t *len) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > STUDIO_MODEL_MAX) return NULL;
    unsigned char *b = malloc((size_t)st.st_size);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t r = pread(fd, b + got, (size_t)st.st_size - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { free(b); b = NULL; break; }
        got += (size_t)r;
    }
    *len = got;
    return b;
}

int stcam_init(int model_fd, int face_fd, int render) {
    size_t len = 0;
    unsigned char *m = read_model(model_fd, &len);
    if (model_fd >= 0) close(model_fd);
    if (!m) { LM_LOG("no model"); if (face_fd >= 0) close(face_fd); return -1; }
    char err[96] = "";
    fx = stfx_new(m, len, render, err, sizeof err);
    free(m);
    if (!fx) { LM_LOG("the model was refused: %s", err); if (face_fd >= 0) close(face_fd); return -1; }
    /* BlazeFace: Auto Framing, Face Light's face and Retouch; without it they do nothing */
    if (face_fd >= 0) {
        m = read_model(face_fd, &len);
        close(face_fd);
        if (!m || stfx_face_model(fx, m, len, err, sizeof err)) LM_LOG("BlazeFace was refused: %s", m ? err : "unreadable");
        free(m);
    }
    return 0;
}

/* MM_STUDIO: every value is one byte of v[0..3] (proto.h), each checked again by stfx_config */
static StCfg cfg_now = { .mode = ST_MODE_BLUR, .blur = 50 };
static int bg_is_video;
/* A still background: the decoded picture kept as BGRA (at most ST_MAX_W×ST_MAX_H), cover-fitted by swscale (an area
 * average, or bicubic up) to the camera's size whenever that changes: a 3840×2160 catalog picture stays sharp behind
 * a 4K camera and is a clean average behind a 720p one (the composite then takes it as it is, with no resampling). */
static AVFrame *still;
static int still_w, still_h;                /* the size it was last fitted to */

static void apply_config(void) {
    StCfg c = cfg_now;
    /* a picture or a video: whatever the background file turned out to be */
    if (c.mode == ST_MODE_PICTURE || c.mode == ST_MODE_VIDEO) c.mode = bg_is_video ? ST_MODE_VIDEO : ST_MODE_PICTURE;
    if (fx) stfx_config(fx, &c);
}

void stcam_config(const MediaMsg *m) {
    for (int i = 0; i < 4; i++) if (m->v[i] < 0 || m->v[i] > 0xffffffffLL) return;
#define B(v, k) ((int)(((v) >> (8 * (k))) & 0xff))
    StCfg c = { .mode = B(m->v[0], 0), .key = B(m->v[0], 1), .filter = B(m->v[0], 2), .mirror = B(m->v[0], 3) != 0,
                .blur = B(m->v[1], 0), .similarity = B(m->v[1], 1), .smoothness = B(m->v[1], 2), .spill = B(m->v[1], 3),
                .framing = B(m->v[2], 0), .light = B(m->v[2], 1), .match = B(m->v[2], 2), .filter_amt = B(m->v[2], 3),
                .retouch = B(m->v[3], 0) };
#undef B
    if (c.mode > ST_MODE_LAST) c.mode = ST_MODE_NONE;
    if (c.blur > ST_BLUR_MAX) c.blur = 50;
    cfg_now = c;
    apply_config();
}

/* ---- the background: a picture, or a short video that loops (decoded here, inside the sandbox, never a path) ---- */

static struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int stream, sw, sh, ow, oh, fd;
    double tb, start, next;       /* the stream's time base (s), when the loop started (ms), the next frame's time (ms) */
    uint8_t *bgra, *cover;        /* the picture at the camera's size; the scaled one before its crop (sw × sh) */
    int eof_twice;
} bv = { .stream = -1, .fd = -1 };

static void video_close(void) {
    avcodec_free_context(&bv.dec);
    av_packet_free(&bv.pkt);
    av_frame_free(&bv.frame);
    sws_freeContext(bv.sws);
    bv.sws = NULL;
    lm_close_input(&bv.fmt);
    if (bv.fd >= 0) close(bv.fd);
    bv.fd = -1;
    free(bv.bgra);
    free(bv.cover);
    bv.bgra = bv.cover = NULL;
    bv.stream = -1;
    bv.ow = bv.oh = bv.sw = bv.sh = 0;
}

/* a video if its best stream isn't a single picture (a still's codec: JPEG, PNG, WebP…) */
static int video_open(AVFormatContext *fmt) {
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) return -1;
    AVStream *st = fmt->streams[si];
    const AVCodecParameters *par = st->codecpar;
    if (par->codec_id == AV_CODEC_ID_MJPEG || par->codec_id == AV_CODEC_ID_PNG || par->codec_id == AV_CODEC_ID_WEBP ||
        par->codec_id == AV_CODEC_ID_BMP || par->codec_id == AV_CODEC_ID_TIFF || (st->disposition & AV_DISPOSITION_ATTACHED_PIC))
        return -1;
    if (par->width < 16 || par->height < 16 || par->width > ST_MAX_W || par->height > ST_MAX_H) return -1;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *dec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!dec || avcodec_parameters_to_context(dec, par) < 0) { avcodec_free_context(&dec); return -1; }
    dec->thread_count = 2;
    if (avcodec_open2(dec, codec, NULL) < 0) { avcodec_free_context(&dec); return -1; }
    bv.fmt = fmt;
    bv.dec = dec;
    bv.pkt = av_packet_alloc();
    bv.frame = av_frame_alloc();
    bv.stream = si;
    bv.tb = av_q2d(st->time_base);
    bv.start = lm_now_ms();
    bv.next = 0;
    bv.eof_twice = 0;
    if (!bv.pkt || !bv.frame) { bv.fmt = NULL; video_close(); return -1; }
    return 0;
}

/* the next decoded picture, or NULL; at the end it starts over */
static AVFrame *video_decode(void) {
    for (int tries = 0; tries < 256; tries++) {
        int r = avcodec_receive_frame(bv.dec, bv.frame);
        if (r == 0) { bv.eof_twice = 0; return bv.frame; }
        if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) {
            if (r == AVERROR_EOF) {
                if (++bv.eof_twice > 2) return NULL;          /* a file with no picture at all */
                av_seek_frame(bv.fmt, bv.stream, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(bv.dec);
                bv.start = lm_now_ms();
                bv.next = 0;
                continue;
            }
            int rr = av_read_frame(bv.fmt, bv.pkt);
            if (rr < 0) { avcodec_send_packet(bv.dec, NULL); continue; }
            if (bv.pkt->stream_index == bv.stream) avcodec_send_packet(bv.dec, bv.pkt);
            av_packet_unref(bv.pkt);
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* once per camera picture: the video's picture for now, at the camera's size (one decode at most per call) */
static void video_step(int w, int h) {
    if (bv.stream < 0 || !fx) return;
    double now = lm_now_ms() - bv.start;
    if (now < bv.next && bv.ow == w && bv.oh == h) return;
    AVFrame *f = video_decode();
    if (!f) { LM_LOG("the background video can't be decoded"); video_close(); bg_is_video = 0; stfx_background(fx, NULL, 0, 0, 0); apply_config(); return; }
    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    double t = pts != AV_NOPTS_VALUE ? pts * bv.tb * 1000.0 : bv.next + 33.3;
    double dur = f->duration > 0 ? f->duration * bv.tb * 1000.0 : 33.3;
    bv.next = t + (dur > 5 && dur < 200 ? dur : 33.3);
    if (now > bv.next + 500) bv.start = lm_now_ms() - t;   /* far behind (a slow machine): don't race to catch up */
    if (!bv.bgra || bv.ow != w || bv.oh != h) {
        free(bv.bgra);
        bv.bgra = malloc((size_t)w * h * 4);
        bv.ow = w;
        bv.oh = h;
        if (!bv.bgra) { video_close(); return; }
    }
    /* cover-fitted at the camera's size here (in swscale), so the composite takes it as it is: scaled to cover, then
     * the middle kept (the same shape as the camera, the usual case, needs no crop) */
    int sw = f->width, sh = f->height;
    double k = fmax((double)w / sw, (double)h / sh);
    int fw = (int)(sw * k + 0.5) & ~1, fh = (int)(sh * k + 0.5) & ~1;
    if (fw < w) fw = w;
    if (fh < h) fh = h;
    if (fw == w && fh == h) {
        bv.sws = sws_getCachedContext(bv.sws, sw, sh, f->format, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
        if (!bv.sws) return;
        uint8_t *dst[4] = { bv.bgra, NULL, NULL, NULL };
        int dls[4] = { w * 4, 0, 0, 0 };
        sws_scale(bv.sws, (const uint8_t *const *)f->data, f->linesize, 0, sh, dst, dls);
        stfx_background(fx, bv.bgra, w, h, w * 4);
        vid_frames++;
        return;
    }
    if (bv.sw != fw || bv.sh != fh) {
        free(bv.cover);
        bv.cover = malloc((size_t)fw * fh * 4);
        bv.sw = fw;
        bv.sh = fh;
        if (!bv.cover) { video_close(); return; }
    }
    bv.sws = sws_getCachedContext(bv.sws, sw, sh, f->format, fw, fh, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!bv.sws) return;
    uint8_t *dst[4] = { bv.cover, NULL, NULL, NULL };
    int dls[4] = { fw * 4, 0, 0, 0 };
    sws_scale(bv.sws, (const uint8_t *const *)f->data, f->linesize, 0, sh, dst, dls);
    int cx = (fw - w) / 2, cy = (fh - h) / 2;
    stfx_background(fx, bv.cover + ((size_t)cy * fw + cx) * 4, w, h, fw * 4);
    vid_frames++;
}

/* the still cover-fitted to w×h (even, the camera's size) and handed to the composite; 0 on success */
static int still_fit(int w, int h) {
    if (!still || !fx || w < 2 || h < 2) return -1;
    int sw = still->width, sh = still->height;
    double k = fmax((double)w / sw, (double)h / sh);
    int fw = (int)(sw * k + 0.5), fh = (int)(sh * k + 0.5);
    if (fw < w) fw = w;
    if (fh < h) fh = h;
    AVFrame *b = fw == sw && fh == sh ? NULL : lm_to_bgra(still, fw, fh, 0);
    const AVFrame *src = b ? b : still;
    if (src->width != fw || src->height != fh) { av_frame_free(&b); return -1; }
    int cx = (fw - w) / 2, cy = (fh - h) / 2;
    int r = stfx_background(fx, src->data[0] + (size_t)cy * src->linesize[0] + (size_t)cx * 4, w, h, src->linesize[0]);
    av_frame_free(&b);
    still_w = w;
    still_h = h;
    return r;
}

/* the picture or the video: decoded here (FFmpeg, inside the sandbox), a picture kept at most ST_MAX_W×ST_MAX_H and
 * fitted to the camera's size (still_fit), never a path */
void stcam_background(int fd) {
    video_close();
    av_frame_free(&still);
    still_w = still_h = 0;
    bg_is_video = 0;
    if (!fx) { if (fd >= 0) close(fd); return; }
    if (fd < 0) { stfx_background(fx, NULL, 0, 0, 0); apply_config(); return; }
    char err[64];
    AVFormatContext *fmt = lm_open_input(fd, err, sizeof err);
    if (fmt && !video_open(fmt)) {
        /* the fd stays open with the video (its I/O reads it); video_close closes both */
        bv.fd = fd;
        bg_is_video = 1;
        stfx_background(fx, NULL, 0, 0, 0);
        apply_config();
        return;
    }
    AVFrame *f = fmt ? lm_decode_still(fmt, 0, NULL, 0) : NULL;
    int ok = 0;
    if (f && f->width > 1 && f->height > 1) {
        double k = 1.0;
        if (f->width > ST_MAX_W) k = (double)ST_MAX_W / f->width;
        if (f->height * k > ST_MAX_H) k = (double)ST_MAX_H / f->height;
        int w = (int)(f->width * k) & ~1, h = (int)(f->height * k) & ~1;
        still = w >= 2 && h >= 2 ? lm_to_bgra(f, w, h, 0) : NULL;
        if (still) {
            /* the camera's size when frames already come (refitted in stcam_frame if it changes), else at most 1920×1080 */
            int cw = in_bgra ? in_bgra->width : 0, ch = in_bgra ? in_bgra->height : 0;
            if (cw < 64 || ch < 64) {
                double c = 1.0;
                if (w > 1920) c = 1920.0 / w;
                if (h * c > 1080) c = 1080.0 / h;
                cw = (int)(w * c) & ~1;
                ch = (int)(h * c) & ~1;
            }
            ok = !still_fit(cw, ch);
            if (!ok) av_frame_free(&still);
        }
    }
    av_frame_free(&f);
    lm_close_input(&fmt);
    close(fd);
    if (!ok) { lm_error("studio.badPicture"); stfx_background(fx, NULL, 0, 0, 0); }
    apply_config();
}

AVFrame *stcam_frame(AVFrame *f) {
    if (!fx || !f) return f;
    int w = f->width & ~1, h = f->height & ~1;
    if (w < 64 || h < 64 || w > ST_MAX_W || h > ST_MAX_H) return f;
    if (!in_bgra || in_bgra->width != w || in_bgra->height != h) {
        av_frame_free(&in_bgra);
        av_frame_free(&out_bgra);
        in_bgra = av_frame_alloc();
        out_bgra = av_frame_alloc();
        if (!in_bgra || !out_bgra) return f;
        in_bgra->format = out_bgra->format = AV_PIX_FMT_BGRA;
        in_bgra->width = out_bgra->width = w;
        in_bgra->height = out_bgra->height = h;
        if (av_frame_get_buffer(in_bgra, 64) < 0 || av_frame_get_buffer(out_bgra, 64) < 0) { av_frame_free(&in_bgra); av_frame_free(&out_bgra); return f; }
    }
    if (!sws || sws_w != f->width || sws_h != f->height || sws_fmt != f->format) {
        sws_freeContext(sws);
        sws = sws_getContext(f->width, f->height, f->format, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws) return f;
        int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
        sws_setColorspaceDetails(sws, sws_getCoefficients(cs), f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
                                 f->format == AV_PIX_FMT_YUVJ422P, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        sws_w = f->width;
        sws_h = f->height;
        sws_fmt = f->format;
    }
    if (av_frame_make_writable(in_bgra) < 0 || av_frame_make_writable(out_bgra) < 0) return f;
    sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, in_bgra->data, in_bgra->linesize);
    if (still && (still_w != w || still_h != h) && still_fit(w, h)) {
        av_frame_free(&still);                  /* out of memory: no picture rather than a stale one */
        stfx_background(fx, NULL, 0, 0, 0);
    }
    if (bg_is_video && (cfg_now.mode == ST_MODE_PICTURE || cfg_now.mode == ST_MODE_VIDEO)) {
        double t0 = lm_now_ms();
        video_step(w, h);
        vid_ms = lm_now_ms() - t0;
    } else vid_ms = 0;
    if (stfx_frame(fx, in_bgra->data[0], in_bgra->linesize[0], w, h, out_bgra->data[0], out_bgra->linesize[0])) return f;
    out_bgra->pts = f->pts;
    return out_bgra;
}

/* once a second: frames done, the steps' times (µs), whether the GPU composites, Auto Framing with Retouch and the
 * video background's decode (µs) */
void stcam_tick(void) {
    double now = lm_now_ms();
    if (!fx || now - stats_at < 1000) return;
    stats_at = now;
    StStats s;
    stfx_stats(fx, &s);
    int64_t v[6] = { s.frames, (int64_t)(s.seg_ms * 1000), (int64_t)((s.refine_ms + s.compose_ms) * 1000), s.gpu,
                     (int64_t)(s.frame_ms * 1000), (int64_t)(vid_ms * 1000) };
    mp_send(LM_SOCK, MM_STUDIO_STATS, 0, v, NULL, 0, NULL, 0);
}

/* probe=1: no camera; the pipeline on synthetic frames inside the sandbox, one MM_RESULT {"gl","gpu","segMs","composeMs",
 * "faces","looks","looksMs","video","videoFrames"} (gl: built with the GPU composite; gpu: it ran; faces: BlazeFace loaded;
 * looks: every phase 3 look ran; video: bg (a file's fd, or -1) was a video, and how many of its pictures were shown) */
void stcam_probe(int bg) {
    int w = 640, h = 360;
    uint8_t *in = malloc((size_t)w * h * 4), *out = malloc((size_t)w * h * 4);
    int ok = fx && in && out;
    for (int i = 0; ok && i < w * h; i++) { in[i * 4] = (uint8_t)(i % w); in[i * 4 + 1] = (uint8_t)(i / w); in[i * 4 + 2] = 128; in[i * 4 + 3] = 255; }
    StCfg c = { .mode = ST_MODE_BLUR, .blur = 50 };
    if (ok) stfx_config(fx, &c);
    for (int i = 0; ok && i < 10; i++) ok = !stfx_frame(fx, in, w * 4, w, h, out, w * 4);
    StStats s = { 0 };
    if (ok) stfx_stats(fx, &s);
    /* phase 3: every look at once (a green screen over a picture, Face Light, a filter, Auto Framing, Retouch) */
    int looks = ok;
    StStats s3 = { 0 };
    if (ok) {
        uint8_t *pic = malloc((size_t)w * h * 4);
        if (pic) memcpy(pic, in, (size_t)w * h * 4);
        StCfg c3 = { .mode = ST_MODE_PICTURE, .key = ST_KEY_GREEN, .similarity = 40, .smoothness = 20, .spill = 50, .match = 30,
                     .light = 50, .filter = ST_FILTER_FILM, .filter_amt = 80, .framing = 50, .retouch = 50 };
        looks = pic && !stfx_background(fx, pic, w, h, w * 4) && !stfx_config(fx, &c3);
        for (int i = 0; looks && i < 10; i++) looks = !stfx_frame(fx, in, w * 4, w, h, out, w * 4);
        if (looks) stfx_stats(fx, &s3);
        free(pic);
    }
    /* a background file: decoded here the way a camera's pictures use it, for about 40 frames */
    if (ok && bg >= 0) {
        cfg_now = (StCfg){ .mode = ST_MODE_VIDEO };
        stcam_background(bg);
        for (int i = 0; ok && i < 40; i++) {
            if (bg_is_video) video_step(w, h);
            ok = !stfx_frame(fx, in, w * 4, w, h, out, w * 4);
        }
    } else if (bg >= 0) close(bg);
#ifdef LORD_GPU
    const int gl = 1;                           /* built with the GPU composite */
#else
    const int gl = 0;
#endif
    char j[300];
    snprintf(j, sizeof j, "{\"gl\":%s,\"gpu\":%s,\"segMs\":%.2f,\"composeMs\":%.2f,\"faces\":%s,\"looks\":%s,\"looksMs\":%.2f,\"video\":%s,\"videoFrames\":%d}",
             gl ? "true" : "false", s.gpu ? "true" : "false", s.seg_ms, s.refine_ms + s.compose_ms, stfx_has_faces(fx) ? "true" : "false",
             looks ? "true" : "false", s3.seg_ms + s3.refine_ms + s3.compose_ms + s3.frame_ms, bg_is_video ? "true" : "false", vid_frames);
    lm_result(0, ok ? 0 : -1, ok ? j : "studio.failed", -1);
    free(in);
    free(out);
}

/* ---- the loopback: LordOS Studio's camera for every app (phase 2; docs/CAMERA-STUDIO-PLAN.md 2.2 and 8) ----------
 * One lord-studio per session writes the "LordOS Studio" device (v4l2loopback, /dev/video50, an fd lord-server opened
 * and checked). With nobody reading it writes the idle card once a second and has no camera at all; when a reader
 * starts streaming, v4l2loopback's client-usage event says so, lord-studio tells lord-server (MM_STUDIO_READERS), and
 * lord-server decides and sends the real camera's fd (MM_STUDIO_CAM). The camera is closed again as soon as lord-server
 * says so, which it does when the last reader and the popover's preview are gone: the camera's light tells the truth. */

static void hub_report(int readers, int cam_open, int what) {
    mp_sendv(LM_SOCK, MM_STUDIO_READERS, 0, readers, cam_open, what, 0);
}

static int hub_write(int loop, const uint8_t *p, size_t n) {
    ssize_t r;
    do r = write(loop, p, n);
    while (r < 0 && errno == EINTR);
    return r == (ssize_t)n ? 0 : -1;
}

void stcam_hub(const char *params, int loop, int render) {
    (void)render;                                   /* the GPU composite already has it (stcam_init) */
    int W = (int)mp_param_int(params, "width", 1280), H = (int)mp_param_int(params, "height", 720);
    st_out_size(&W, &H);
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    /* lord-server checked it already; the helper doesn't trust that either */
    if (loop < 0 || ioctl(loop, VIDIOC_QUERYCAP, &cap) || strcmp((const char *)cap.driver, "v4l2 loopback")) {
        LM_LOG("not a v4l2loopback device");
        lm_error("studio.failed");
        mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0);
        return;
    }
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width = (uint32_t)W;
    f.fmt.pix.height = (uint32_t)H;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.bytesperline = (uint32_t)W * 2;
    f.fmt.pix.sizeimage = (uint32_t)st_yuyv_size(W, H);
    f.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
    if (ioctl(loop, VIDIOC_S_FMT, &f) || f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV || (int)f.fmt.pix.width != W || (int)f.fmt.pix.height != H) {
        LM_LOG("the loopback refused YUYV %dx%d (another writer?)", W, H);
        lm_error("studio.failed");
        mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0);
        return;
    }
    /* the readers: v4l2loopback 0.13.0 and later; the initial event gives the count now */
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof sub);
    sub.type = ST_EVENT_CLIENT_USAGE;
    sub.flags = V4L2_EVENT_SUB_FL_SEND_INITIAL;
    int events = ioctl(loop, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0;
    if (!events) LM_LOG("no client-usage event from this v4l2loopback: lord-server counts the readers itself");
    size_t fsz = st_yuyv_size(W, H);
    uint8_t *cards[2] = { malloc(fsz), malloc(fsz) }, *out = malloc(fsz), *tmp = malloc((size_t)W * H * 4);
    if (!cards[0] || !cards[1] || !out || !tmp) { lm_error("studio.failed"); mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0); goto done; }
    for (int k = 0; k < 2; k++) { st_card_bgra(k, tmp, W, H); st_bgra_to_yuyv(tmp, W * 4, W, H, cards[k]); }
    free(tmp);
    tmp = NULL;
    lm_out_init(0);
    {
        char info[160];
        snprintf(info, sizeof info, "{\"loopback\":true,\"width\":%d,\"height\":%d,\"events\":%s}", W, H, events ? "true" : "false");
        mp_send(LM_SOCK, MM_INFO, 0, NULL, info, strlen(info), NULL, 0);
        mp_sendv(LM_SOCK, MM_STATE, 0, MS_PLAYING, 0, 0, 0);
    }
    static char payload[MP_PAYLOAD_MAX + 1];
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *osws = NULL;
    LmCam *cam = NULL;
    int cam_fd = -1, what = 0, preview = 0, readers = events ? 0 : -1, stop = 0;
    double next_card = 0, t_start = lm_now_ms();
    /* the first card at once: from now on v4l2loopback announces a camera that captures */
    if (hub_write(loop, cards[0], fsz)) LM_LOG("the first card wasn't written: %s", strerror(errno));
    next_card = lm_now_ms() + 1000;
    hub_report(readers, 0, 0);
    while (!stop) {
        double now = lm_now_ms();
        int timeout = cam ? 500 : (int)(next_card > now ? next_card - now : 0);
        struct pollfd pf[3] = { { LM_SOCK, POLLIN, 0 }, { loop, POLLPRI, 0 }, { cam ? cam_fd : -1, POLLIN, 0 } };
        if (poll(pf, 3, timeout) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        int changed = 0;
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS], r;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                if (r == 0 || m.type == MM_STOP) { stop = 1; for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]); break; }
                if (m.type == MM_ACK) lm_out_ack(m.id);
                else if (m.type == MM_SIZE) lm_out_box((int)m.v[0], (int)m.v[1]);
                else if (m.type == MM_STUDIO) stcam_config(&m);
                else if (m.type == MM_STUDIO_BG) { stcam_background(fds[0]); fds[0] = -1; }
                else if (m.type == MM_STUDIO_CAM) {
                    preview = m.v[1] == 1;
                    int want = m.v[0] == 1 ? 1 : m.v[0] == 2 ? 2 : 0;
                    if (want == 1 && fds[0] >= 0) {
                        if (cam) { lm_cam_close(cam); cam = NULL; close(cam_fd); cam_fd = -1; }
                        const char *code = NULL;
                        cam = lm_cam_open(fds[0], W, H, &code);
                        if (cam) { cam_fd = fds[0]; fds[0] = -1; what = 1; if (fx) stfx_reset(fx); }
                        else { lm_error("%s", code); what = 0; next_card = 0; }
                    } else if (want != 1) {
                        if (cam) { lm_cam_close(cam); cam = NULL; close(cam_fd); cam_fd = -1; }
                        what = want;
                        next_card = 0;                      /* the card at once, not the camera's last frame */
                    }
                    changed = 1;
                }
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            }
        }
        if (pf[1].revents & POLLPRI) {
            struct v4l2_event ev;
            for (int k = 0; k < 16; k++) {
                memset(&ev, 0, sizeof ev);
                if (ioctl(loop, VIDIOC_DQEVENT, &ev)) break;
                int n = st_client_usage(ev.type, ev.u.data, sizeof ev.u.data);
                if (n >= 0 && n != readers) { readers = n; changed = 1; }
            }
        }
        if (changed) hub_report(readers, cam != NULL, what);
        stcam_tick();
        if (cam && (pf[2].revents & (POLLIN | POLLERR | POLLHUP))) {
            int got = lm_cam_read(cam, frame);
            if (got < 0) {                                  /* unplugged: the card again, and lord-server hears it */
                LM_LOG("the camera went away");
                lm_cam_close(cam);
                cam = NULL;
                close(cam_fd);
                cam_fd = -1;
                what = 0;
                next_card = 0;
                lm_error("camera.failed");
                hub_report(readers, 0, what);
            } else if (got) {
                AVFrame *shown = stcam_frame(frame);
                osws = sws_getCachedContext(osws, shown->width, shown->height, shown->format, W, H, AV_PIX_FMT_YUYV422, SWS_BILINEAR, NULL, NULL, NULL);
                if (osws) {
                    uint8_t *dst[4] = { out, NULL, NULL, NULL };
                    int dls[4] = { W * 2, 0, 0, 0 };
                    sws_scale(osws, (const uint8_t *const *)shown->data, shown->linesize, 0, shown->height, dst, dls);
                    if (hub_write(loop, out, fsz) && errno != EAGAIN) LM_DBG("loopback write: %s", strerror(errno));
                }
                if (preview) lm_out_present(shown, (int64_t)(lm_now_ms() - t_start));
            }
        }
        if (!cam && lm_now_ms() >= next_card) {
            if (hub_write(loop, cards[what == 2 ? 1 : 0], fsz) && errno != EAGAIN) LM_DBG("loopback write: %s", strerror(errno));
            next_card = lm_now_ms() + 1000;
        }
    }
    if (cam) { lm_cam_close(cam); close(cam_fd); }
    sws_freeContext(osws);
    av_frame_free(&frame);
done:
    free(cards[0]);
    free(cards[1]);
    free(out);
    free(tmp);
}

void stcam_free(void) {
    video_close();
    av_frame_free(&still);
    stfx_free(fx);
    fx = NULL;
    av_frame_free(&in_bgra);
    av_frame_free(&out_bgra);
    sws_freeContext(sws);
    sws = NULL;
}
#endif
