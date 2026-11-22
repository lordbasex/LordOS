/* lm_cam.c — the camera (MK_CAMERA): our own V4L2 (no libv4l), MJPEG with libavcodec, the
 * preview through the same slots as the player (lm_out.c), JPEG photo and H.264 + AAC video.
 * lord-server opens the /dev/videoN (after checking the camera permission) and it arrives as an
 * fd: lord-media can't open another device. */
#include "lm.h"
#include "lm_link.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define NBUF 4

typedef struct { void *p; size_t len; } Buf;

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static enum AVPixelFormat av_fmt(uint32_t v4l) {
    switch (v4l) {
    case V4L2_PIX_FMT_YUYV: return AV_PIX_FMT_YUYV422;
    case V4L2_PIX_FMT_UYVY: return AV_PIX_FMT_UYVY422;
    case V4L2_PIX_FMT_NV12: return AV_PIX_FMT_NV12;
    case V4L2_PIX_FMT_YUV420: return AV_PIX_FMT_YUV420P;
    case V4L2_PIX_FMT_RGB24: return AV_PIX_FMT_RGB24;
    default: return AV_PIX_FMT_NONE;
    }
}

/* Picks a format: MJPEG (USB webcams give 720p at 30 frames that way) or a known raw one. */
static int choose_format(int fd, struct v4l2_format *fmt, int want_w, int want_h) {
    uint32_t best = 0;
    struct v4l2_fmtdesc d;
    memset(&d, 0, sizeof d);
    d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (d.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &d) == 0 && d.index < 64; d.index++) {
        if (d.pixelformat == V4L2_PIX_FMT_MJPEG) { best = d.pixelformat; break; }
        if (!best && av_fmt(d.pixelformat) != AV_PIX_FMT_NONE) best = d.pixelformat;
    }
    memset(fmt, 0, sizeof *fmt);
    fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, fmt)) return -1;
    if (!best) best = fmt->fmt.pix.pixelformat;
    fmt->fmt.pix.pixelformat = best;
    fmt->fmt.pix.width = (uint32_t)want_w;
    fmt->fmt.pix.height = (uint32_t)want_h;
    fmt->fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, fmt)) xioctl(fd, VIDIOC_G_FMT, fmt);     /* v4l2loopback: whoever writes sets the format */
    return (fmt->fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG || av_fmt(fmt->fmt.pix.pixelformat) != AV_PIX_FMT_NONE) ? 0 : -1;
}

static void flip_h(AVFrame *f) {
    for (int y = 0; y < f->height; y++) {
        uint32_t *row = (uint32_t *)(f->data[0] + (size_t)y * f->linesize[0]);
        for (int a = 0, b = f->width - 1; a < b; a++, b--) { uint32_t t = row[a]; row[a] = row[b]; row[b] = t; }
    }
}

static LmEnc *enc;
static int enc_fd = -1;              /* the video output (belongs to the encoder until it's closed) */
static int enc_mirror;
static int64_t enc_t0;

static void on_mic(const int16_t *pcm, int samples) {
    if (enc) lm_enc_audio(enc, pcm, samples);
}

/* A call (MM_LINK): the same pictures, encoded, to a packaged app's companion. It never waits for anyone: a picture that
 * finds the encoder busy or the companion behind is dropped. Light on purpose, as go-link's call is: 24 frames a second. */
#define LINK_FPS 24
static LmLink link_out = { .fd = -1 };
static AVCodecContext *link_enc;
static struct SwsContext *link_sws;
static AVFrame *link_yuv;
static AVPacket *link_pkt;
static int link_key;
static double link_last;

static void link_stop(void) {
    if (link_out.fd >= 0) lm_link_close(&link_out);
    avcodec_free_context(&link_enc);
    sws_freeContext(link_sws);
    link_sws = NULL;
    av_frame_free(&link_yuv);
    av_packet_free(&link_pkt);
}

static void link_start(int fd, int vp8, int kbps, int w, int h) {
    link_stop();
    if (kbps < 100 || kbps > 8000) kbps = 600;
    link_enc = lm_stream_sw_encoder(vp8, w, h, LINK_FPS, (unsigned)kbps);
    link_yuv = av_frame_alloc();
    link_pkt = av_packet_alloc();
    if (link_enc && link_yuv && link_pkt) {
        link_yuv->format = AV_PIX_FMT_YUV420P;
        link_yuv->width = w & ~1;
        link_yuv->height = h & ~1;
    }
    if (!link_enc || !link_yuv || !link_pkt || av_frame_get_buffer(link_yuv, 0) < 0) { lm_error("media.noVideoEncoder"); link_stop(); close(fd); return; }
    lm_link_init(&link_out, fd);
    link_key = 1;
}

static void link_frame(const AVFrame *f, int64_t ms) {
    if (link_out.fd < 0 || !link_enc) return;
    double t = lm_now_ms();
    if (t - link_last < 1000.0 / LINK_FPS - 2) return;
    link_last = t;
    link_sws = sws_getCachedContext(link_sws, f->width, f->height, f->format, link_yuv->width, link_yuv->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!link_sws || av_frame_make_writable(link_yuv) < 0) return;
    sws_scale(link_sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, link_yuv->data, link_yuv->linesize);
    link_yuv->pts = ms;
    link_yuv->pict_type = link_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (link_key) link_yuv->flags |= AV_FRAME_FLAG_KEY; else link_yuv->flags &= ~AV_FRAME_FLAG_KEY;
    link_key = 0;
    if (avcodec_send_frame(link_enc, link_yuv) < 0) return;
    while (avcodec_receive_packet(link_enc, link_pkt) >= 0) {
        lm_link_send(&link_out, LL_CAMERA, (link_pkt->flags & AV_PKT_FLAG_KEY) ? LF_KEY : 0, link_pkt->pts, link_pkt->data, (size_t)link_pkt->size);
        av_packet_unref(link_pkt);
    }
}

static void on_link(const LiveHdr *h, const uint8_t *d, void *arg) {
    (void)d;
    (void)arg;
    if (h->type == LL_KEYFRAME) link_key = 1;
}

/* ---- the capture itself (LmCam, lm.h): shared by the camera job and lord-studio's loopback output (../studio/stout.c) */
struct LmCam {
    int dev, W, H, nb;
    uint32_t pixfmt;
    Buf bufs[NBUF];
    AVCodecContext *mj;
    AVPacket *pkt;
    char card[64];
};

/* code: the error's code line (camera.*) when it fails; the fd stays the caller's */
LmCam *lm_cam_open(int dev, int want_w, int want_h, const char **code) {
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    *code = "camera.notACamera";
    if (dev < 0 || xioctl(dev, VIDIOC_QUERYCAP, &cap)) return NULL;
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    *code = "camera.noVideo";
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) return NULL;
    LmCam *c = calloc(1, sizeof *c);
    *code = "camera.startFailed";
    if (!c) return NULL;
    c->dev = dev;
    struct v4l2_format fmt;
    if (choose_format(dev, &fmt, want_w, want_h)) { *code = "camera.unsupportedFormat"; free(c); return NULL; }
    c->W = (int)fmt.fmt.pix.width;
    c->H = (int)fmt.fmt.pix.height;
    c->pixfmt = fmt.fmt.pix.pixelformat;
    struct v4l2_requestbuffers rb = { .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(dev, VIDIOC_REQBUFS, &rb) || rb.count < 2) { *code = "camera.busy"; free(c); return NULL; }
    c->nb = rb.count > NBUF ? NBUF : (int)rb.count;
    for (int i = 0; i < c->nb; i++) {
        struct v4l2_buffer b = { .index = (uint32_t)i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(dev, VIDIOC_QUERYBUF, &b)) continue;
        c->bufs[i].len = b.length;
        c->bufs[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev, b.m.offset);   /* the driver's size and offset */
        if (c->bufs[i].p == MAP_FAILED) { c->bufs[i].p = NULL; continue; }
        xioctl(dev, VIDIOC_QBUF, &b);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(dev, VIDIOC_STREAMON, &type)) { lm_cam_close(c); *code = "camera.startFailed"; return NULL; }
    if (c->pixfmt == V4L2_PIX_FMT_MJPEG) {
        const AVCodec *d = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        c->mj = d ? avcodec_alloc_context3(d) : NULL;
        if (c->mj) { c->mj->thread_count = 2; if (avcodec_open2(c->mj, d, NULL) < 0) avcodec_free_context(&c->mj); }
    }
    c->pkt = av_packet_alloc();
    snprintf(c->card, sizeof c->card, "%s", (const char *)cap.card);
    for (char *p = c->card; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    *code = NULL;
    return c;
}

void lm_cam_size(const LmCam *c, int *w, int *h, uint32_t *pixfmt) { *w = c->W; *h = c->H; if (pixfmt) *pixfmt = c->pixfmt; }
const char *lm_cam_card(const LmCam *c) { return c->card; }

/* One picture, after POLLIN: 1 decoded into frame, 0 none this time, -1 the device went away (unplugged) */
int lm_cam_read(LmCam *c, AVFrame *frame) {
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(c->dev, VIDIOC_DQBUF, &b)) return errno == ENODEV ? -1 : 0;
    int got = 0;
    if (b.index < (unsigned)c->nb && c->bufs[b.index].p && b.bytesused > 0) {
        av_frame_unref(frame);
        if (c->pixfmt == V4L2_PIX_FMT_MJPEG && c->mj && c->pkt) {
            c->pkt->data = c->bufs[b.index].p;
            c->pkt->size = (int)b.bytesused;
            if (avcodec_send_packet(c->mj, c->pkt) >= 0) got = avcodec_receive_frame(c->mj, frame) >= 0;
            c->pkt->data = NULL;
            c->pkt->size = 0;
        } else if (c->pixfmt != V4L2_PIX_FMT_MJPEG) {
            /* raw: copied into our own frame (the buffer goes back to the driver right away) */
            frame->format = av_fmt(c->pixfmt);
            frame->width = c->W;
            frame->height = c->H;
            if (av_frame_get_buffer(frame, 0) >= 0) {
                uint8_t *src[4];
                int ls[4];
                if (av_image_fill_arrays(src, ls, c->bufs[b.index].p, frame->format, c->W, c->H, 1) > 0 &&
                    (size_t)av_image_get_buffer_size(frame->format, c->W, c->H, 1) <= b.bytesused) {
                    av_image_copy(frame->data, frame->linesize, (const uint8_t **)src, ls, frame->format, c->W, c->H);
                    got = 1;
                }
            }
        }
    }
    xioctl(c->dev, VIDIOC_QBUF, &b);
    return got;
}

/* stops the stream and frees the buffers; the fd stays the caller's */
void lm_cam_close(LmCam *c) {
    if (!c) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(c->dev, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < c->nb; i++) if (c->bufs[i].p) munmap(c->bufs[i].p, c->bufs[i].len);
    av_packet_free(&c->pkt);
    avcodec_free_context(&c->mj);
    free(c);
}

void lm_camera(const char *params, int dev, int render) {
    char accel[24];
    AVBufferRef *hw = lm_hw_device(lm_hw_accel(params, accel, sizeof accel), render);
    lm_out_init(0);
    const char *code = NULL;
    LmCam *cam = lm_cam_open(dev, (int)mp_param_int(params, "width", 1280), (int)mp_param_int(params, "height", 720), &code);
    if (!cam) { lm_error("%s", code); mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0); return; }
    int W, H;
    uint32_t pixfmt;
    lm_cam_size(cam, &W, &H, &pixfmt);
    char info[512];
    snprintf(info, sizeof info, "{\"camera\":\"%s\",\"width\":%d,\"height\":%d,\"format\":\"%.4s\",\"hwAvailable\":%s}", lm_cam_card(cam), W, H,
             (const char *)&pixfmt, hw ? "true" : "false");
    mp_send(LM_SOCK, MM_INFO, 0, NULL, info, strlen(info), NULL, 0);
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_PLAYING, 0, 0, 0);

    static char payload[MP_PAYLOAD_MAX + 1];
    AVFrame *frame = av_frame_alloc();
    void *ring = NULL;
    size_t ring_len = 0;
    int mic = 0, stop = 0;
    long nframes = 0;
    double t_start = lm_now_ms();
    while (!stop) {
        struct pollfd pf[3] = { { LM_SOCK, POLLIN, 0 }, { dev, POLLIN, 0 }, { link_out.fd, link_out.fd >= 0 ? lm_link_events(&link_out) : 0, 0 } };
        if (poll(pf, 3, 500) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        if (pf[2].revents & POLLOUT) lm_link_flush(&link_out);
        if ((pf[2].revents & (POLLIN | POLLHUP)) && lm_link_read(&link_out, on_link, NULL)) link_stop();
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS], r;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                if (r == 0 || m.type == MM_STOP) { stop = 1; for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]); break; }
                if (m.type == MM_LINK && fds[0] >= 0) { link_start(fds[0], m.v[0] != 0, (int)m.v[1], W, H); fds[0] = -1; }
                else if (m.type == MM_ACK) lm_out_ack(m.id);
#ifdef LM_STUDIO
                else if (m.type == MM_STUDIO) stcam_config(&m);
                else if (m.type == MM_STUDIO_BG) { stcam_background(fds[0]); fds[0] = -1; }
#endif
                else if (m.type == MM_SIZE) { if (lm_out_box((int)m.v[0], (int)m.v[1])) lm_out_redraw(); }
                else if (m.type == MM_PHOTO) {
                    const AVFrame *last = lm_out_last();
                    int ok = -1;
                    if (last && fds[0] >= 0) {
                        AVFrame *b = lm_to_bgra(last, last->width, last->height, 0);
                        if (b) { if (m.v[0]) flip_h(b); ok = lm_encode_image(b, "jpeg", 92, fds[0]); av_frame_free(&b); }
                    }
                    char j[64];
                    snprintf(j, sizeof j, "{\"width\":%d,\"height\":%d}", last ? last->width : 0, last ? last->height : 0);
                    lm_result(m.id, ok ? -1 : 0, ok ? "camera.photoFailed" : j, -1);
                } else if (m.type == MM_RECORD && fds[0] >= 0 && !enc) {
                    char err[128];
                    ring = lm_mic_ring(fds[1], &ring_len);
                    mic = ring != NULL;
                    enc = lm_enc_open(fds[0], W, H, 30, hw, mic ? 48000 : 0, 1, err, sizeof err);
                    enc_mirror = m.v[0] != 0;
                    enc_t0 = (int64_t)lm_now_ms();
                    if (!enc) { lm_result(m.id, -1, err, -1); if (mic) { munmap(ring, ring_len); ring = NULL; mic = 0; } }
                    else { enc_fd = fds[0]; fds[0] = -1; mp_sendv(LM_SOCK, MM_STATE, 0, MS_RECORDING, 0, 0, 0); }
                } else if (m.type == MM_RECORD && enc) {             /* no fd: finish the video */
                    if (mic) { lm_mic_drain(ring, on_mic); munmap(ring, ring_len); ring = NULL; mic = 0; }
                    int64_t d = lm_enc_close(enc);
                    enc = NULL;
                    if (enc_fd >= 0) { close(enc_fd); enc_fd = -1; }
                    char j[64];
                    snprintf(j, sizeof j, "{\"duration\":%lld}", (long long)d);
                    lm_result(m.id, d < 0 ? -1 : 0, j, -1);
                    mp_sendv(LM_SOCK, MM_STATE, 0, MS_PLAYING, 0, 0, 0);
                }
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            }
        }
        if (mic) lm_mic_drain(ring, on_mic);
#ifdef LM_STUDIO
        stcam_tick();
#endif
        if (!(pf[1].revents & POLLIN)) continue;
        int got = lm_cam_read(cam, frame);
        if (got < 0) { LM_LOG("camera: the device went away"); break; }
        if (got) {
            int64_t t = (int64_t)(lm_now_ms() - t_start);
#ifdef LM_STUDIO
            AVFrame *shown = stcam_frame(frame);    /* LordOS Studio: the same picture with its effects */
#else
            AVFrame *shown = frame;
#endif
            lm_out_present(shown, t);
            link_frame(shown, t);
            nframes++;
            if (enc) {
                if (enc_mirror) {
                    AVFrame *bg = lm_to_bgra(shown, shown->width, shown->height, 0);
                    if (bg) { flip_h(bg); lm_enc_video(enc, bg, (int64_t)lm_now_ms() - enc_t0); av_frame_free(&bg); }
                } else lm_enc_video(enc, shown, (int64_t)lm_now_ms() - enc_t0);
                mp_sendv(LM_SOCK, MM_LEVEL, 0, 0, (int64_t)lm_now_ms() - enc_t0, 0, 0);
            }
        }
    }
    if (enc) { if (mic) munmap(ring, ring_len); lm_enc_close(enc); enc = NULL; }
    link_stop();
    lm_cam_close(cam);
    av_frame_free(&frame);
    LM_DBG("camera: %ld frames", nframes);
}
