/* stcam.c — lord-studio's side of the camera (docs/CAMERA-STUDIO-PLAN.md 3.1; AGENTS.md, lord-studio).
 *
 * lord-studio is lord-media built with LM_STUDIO: the same camera job (helper/lm_cam.c, MK_CAMERA) with every decoded
 * picture passed through LordOS Studio's pipeline (st.h) before it goes to the preview slots, the photo, the video
 * and a companion's link. It receives only fds that lord-server opened: the camera, the render node (or none), the
 * segmenter's model (hash already checked) and, later, a background picture. Its messages are numbers: MM_STUDIO
 * (mode, blur, mirror), MM_STUDIO_BG (an fd, or none to clear) and, back, MM_STUDIO_STATS once a second. */
#ifdef LM_STUDIO
#include "../helper/lm.h"
#include "st.h"
#include "models.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

static StFx *fx;
static AVFrame *in_bgra, *out_bgra;
static struct SwsContext *sws;
static int sws_w, sws_h, sws_fmt;
static double stats_at;

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

int stcam_init(int model_fd, int render) {
    size_t len = 0;
    unsigned char *m = read_model(model_fd, &len);
    if (model_fd >= 0) close(model_fd);
    if (!m) { LM_LOG("no model"); return -1; }
    char err[96] = "";
    fx = stfx_new(m, len, render, err, sizeof err);
    free(m);
    if (!fx) { LM_LOG("the model was refused: %s", err); return -1; }
    return 0;
}

void stcam_config(const MediaMsg *m) {
    StCfg c = { .mode = (int)m->v[0], .blur = (int)m->v[1], .mirror = m->v[2] != 0 };
    if (m->v[0] < 0 || m->v[0] > ST_MODE_LAST) c.mode = ST_MODE_NONE;
    if (m->v[1] < 0 || m->v[1] > ST_BLUR_MAX) c.blur = 50;
    if (fx) stfx_config(fx, &c);
}

/* the picture: decoded here (FFmpeg, inside the sandbox), at most 1920×1080, never a path */
void stcam_background(int fd) {
    if (!fx) { if (fd >= 0) close(fd); return; }
    if (fd < 0) { stfx_background(fx, NULL, 0, 0, 0); return; }
    char err[64];
    AVFormatContext *fmt = lm_open_input(fd, err, sizeof err);
    AVFrame *f = fmt ? lm_decode_still(fmt, 0, NULL, 0) : NULL;
    int ok = 0;
    if (f && f->width > 1 && f->height > 1) {
        double k = 1.0;
        if (f->width > 1920) k = 1920.0 / f->width;
        if (f->height * k > 1080) k = 1080.0 / f->height;
        int w = (int)(f->width * k) & ~1, h = (int)(f->height * k) & ~1;
        AVFrame *b = w >= 2 && h >= 2 ? lm_to_bgra(f, w, h, 0) : NULL;
        if (b) { ok = !stfx_background(fx, b->data[0], b->width, b->height, b->linesize[0]); av_frame_free(&b); }
    }
    av_frame_free(&f);
    lm_close_input(&fmt);
    close(fd);
    if (!ok) { lm_error("studio.badPicture"); stfx_background(fx, NULL, 0, 0, 0); }
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
    if (stfx_frame(fx, in_bgra->data[0], in_bgra->linesize[0], w, h, out_bgra->data[0], out_bgra->linesize[0])) return f;
    out_bgra->pts = f->pts;
    return out_bgra;
}

/* once a second: frames done, the steps' times (µs) and whether the GPU composites */
void stcam_tick(void) {
    double now = lm_now_ms();
    if (!fx || now - stats_at < 1000) return;
    stats_at = now;
    StStats s;
    stfx_stats(fx, &s);
    mp_sendv(LM_SOCK, MM_STUDIO_STATS, 0, s.frames, (int64_t)(s.seg_ms * 1000), (int64_t)((s.refine_ms + s.compose_ms) * 1000), s.gpu);
}

/* probe=1: no camera; the pipeline on synthetic frames inside the sandbox, one MM_RESULT {"gl","gpu","segMs","composeMs"}
 * (gl: built with the GPU composite; gpu: it ran) */
void stcam_probe(void) {
    int w = 640, h = 360;
    uint8_t *in = malloc((size_t)w * h * 4), *out = malloc((size_t)w * h * 4);
    int ok = fx && in && out;
    for (int i = 0; ok && i < w * h; i++) { in[i * 4] = (uint8_t)(i % w); in[i * 4 + 1] = (uint8_t)(i / w); in[i * 4 + 2] = 128; in[i * 4 + 3] = 255; }
    StCfg c = { .mode = ST_MODE_BLUR, .blur = 50 };
    if (ok) stfx_config(fx, &c);
    for (int i = 0; ok && i < 10; i++) ok = !stfx_frame(fx, in, w * 4, w, h, out, w * 4);
    StStats s = { 0 };
    if (ok) stfx_stats(fx, &s);
#ifdef LORD_GPU
    const int gl = 1;                           /* built with the GPU composite */
#else
    const int gl = 0;
#endif
    char j[160];
    snprintf(j, sizeof j, "{\"gl\":%s,\"gpu\":%s,\"segMs\":%.2f,\"composeMs\":%.2f}", gl ? "true" : "false", s.gpu ? "true" : "false", s.seg_ms,
             s.refine_ms + s.compose_ms);
    lm_result(0, ok ? 0 : -1, ok ? j : "studio.failed", -1);
    free(in);
    free(out);
}

void stcam_free(void) {
    stfx_free(fx);
    fx = NULL;
    av_frame_free(&in_bgra);
    av_frame_free(&out_bgra);
    sws_freeContext(sws);
    sws = NULL;
}
#endif
