/* lm_encheck.c — the check of a hardware encoder's first keyframe (lm_encheck.h). */
#include "lm_encheck.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

int lm_encheck_ref(LmEncCheck *c, const uint8_t *y, int linesize, int w, int h) {
    memset(c, 0, sizeof *c);
    if (!y || w < LM_ENCHECK_STEP || h < LM_ENCHECK_STEP) return -1;
    c->w = w;
    c->h = h;
    c->gw = w / LM_ENCHECK_STEP;
    c->gh = h / LM_ENCHECK_STEP;
    c->ref = malloc((size_t)c->gw * c->gh);
    if (!c->ref) return -1;
    for (int j = 0; j < c->gh; j++)
        for (int i = 0; i < c->gw; i++)
            c->ref[(size_t)j * c->gw + i] = y[(size_t)(j * LM_ENCHECK_STEP + LM_ENCHECK_STEP / 2) * linesize + i * LM_ENCHECK_STEP + LM_ENCHECK_STEP / 2];
    return 0;
}

void lm_encheck_free(LmEncCheck *c) {
    free(c->ref);
    memset(c, 0, sizeof *c);
}

static int has_nal(const uint8_t *b, size_t n, int type) {
    for (size_t i = 0; i + 3 < n; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if ((b[i + 3] & 0x1f) == type) return 1;
            i += 2;
        }
    return 0;
}

int lm_encheck_packet(const LmEncCheck *c, const uint8_t *pkt, size_t n, const char **why) {
    static const char *dummy;
    if (!why) why = &dummy;
    *why = "";
#ifdef LM_ENCHECK_FAIL_TEST
    /* build/lord-media-encfail: every hardware keyframe fails, to run lm_stream.c's way out to libx264 */
    (void)pkt; (void)n;
    if (c) { *why = "test build: every check fails"; return -1; }
#endif
    if (!c->ref) { *why = "no reference frame"; return -1; }
    if (!pkt || !n || n > (64u << 20)) { *why = "empty packet"; return -1; }
    /* what a browser needs before anything else: without these it never makes a picture */
    if (!has_nal(pkt, n, 7) || !has_nal(pkt, n, 8)) { *why = "no SPS or PPS in the keyframe"; return -1; }
    if (!has_nal(pkt, n, 5)) { *why = "the keyframe has no IDR slice"; return -1; }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return 0;                                   /* nothing to check with: trust it */
    AVCodecContext *d = avcodec_alloc_context3(codec);
    AVPacket *p = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int ok = -1;
    *why = "out of memory";
    if (!d || !p || !f) goto out;
    d->thread_count = 1;
    d->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->err_recognition = AV_EF_EXPLODE;
    *why = "the H.264 decoder did not open";
    if (avcodec_open2(d, codec, NULL) < 0 || av_new_packet(p, (int)n) < 0) goto out;
    memcpy(p->data, pkt, n);
    p->flags |= AV_PKT_FLAG_KEY;
    *why = "the keyframe does not decode";
    if (avcodec_send_packet(d, p) < 0) goto out;
    avcodec_send_packet(d, NULL);                           /* drain: the one picture, now */
    if (avcodec_receive_frame(d, f) < 0) goto out;
    *why = "the decoded picture has another size";
    if (f->width != c->w || f->height != c->h || !f->data[0]) goto out;
    long sum = 0;
    for (int j = 0; j < c->gh; j++)
        for (int i = 0; i < c->gw; i++) {
            int v = f->data[0][(size_t)(j * LM_ENCHECK_STEP + LM_ENCHECK_STEP / 2) * f->linesize[0] + i * LM_ENCHECK_STEP + LM_ENCHECK_STEP / 2];
            int r = c->ref[(size_t)j * c->gw + i];
            sum += v > r ? v - r : r - v;
        }
    *why = "the decoded picture is not the frame that went in";
    if (sum > (long)LM_ENCHECK_MAX_DIFF * c->gw * c->gh) goto out;
    *why = "";
    ok = 0;
out:
    av_frame_free(&f);
    av_packet_free(&p);
    avcodec_free_context(&d);
    return ok;
}
