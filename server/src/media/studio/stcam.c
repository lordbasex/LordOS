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
    stfx_free(fx);
    fx = NULL;
    av_frame_free(&in_bgra);
    av_frame_free(&out_bgra);
    sws_freeContext(sws);
    sws = NULL;
}
#endif
