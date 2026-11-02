/* lm_rec.c — screen recording (MK_SCREENREC) and the voice recorder (MK_AUDIOREC).
 *
 * Screen: axia-server composites each frame as always and, while recording, copies the
 * result (with protected windows in black) into a free slot of a memfd it shares
 * with axia-media (MM_INRING / MM_INFRAME). Here it's encoded to H.264 and MM_SLOTFREE is announced. If
 * there's no free slot, axia-server drops that frame: it never waits.
 * Microphone: axia-server owns the PipeWire capture stream (after the permission) and
 * writes it into a shared-memory ring (aring.h); here it's only read. s16 mono at 48 kHz. */
#include "lm.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <libavutil/imgutils.h>

#define RATE 48000

typedef struct {
    LmEnc *enc;
    int out_fd;                     /* voice recorder: WAV */
    int64_t samples;
    int peak;                       /* 0..1000 since the last notification */
    int paused;
} Rec;

static Rec R;

static void on_mic(const int16_t *s, int k) {
    if (R.paused) return;
    size_t n = (size_t)k * 2;
    const uint8_t *d = (const uint8_t *)s;
    for (int i = 0; i < k; i++) { int v = abs(s[i]) * 1000 / 32768; if (v > R.peak) R.peak = v; }
    if (R.enc) lm_enc_audio(R.enc, s, k);
    else if (R.out_fd >= 0 && lm_write_all(R.out_fd, d, n & ~(size_t)1)) LM_LOG("could not write the recording");
    R.samples += k;
}

static void wav_header(int fd, uint32_t data_bytes) {
    uint8_t h[44];
    uint32_t rate = RATE, byte_rate = RATE * 2, chunk = 36 + data_bytes;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &chunk, 4); memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16; uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
    if (pwrite(fd, h, sizeof h, 0) != (ssize_t)sizeof h) LM_LOG("could not write the WAV header");
}

void lm_audiorec(const char *params, int out, int pulse) {
    (void)params;
    size_t ring_len = 0;
    void *ring = lm_mic_ring(pulse, &ring_len);
    close(pulse);
    R.out_fd = out;
    if (out < 0 || !ring) {
        lm_error("rec.noMicrophone");
        mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0);
        return;
    }
    wav_header(out, 0);
    lseek(out, 44, SEEK_SET);
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_RECORDING, 0, 0, 0);
    static char payload[MP_PAYLOAD_MAX + 1];
    double last = 0;
    int stop = 0;
    while (!stop) {
        struct pollfd pf[1] = { { LM_SOCK, POLLIN, 0 } };
        if (poll(pf, 1, 20) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS], r;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
                if (r == 0 || m.type == MM_STOP) { stop = 1; break; }
                if (m.type == MM_PAUSE) R.paused = 1;
                if (m.type == MM_PLAY) R.paused = 0;
            }
        }
        lm_mic_drain(ring, on_mic);
        if (lm_now_ms() - last >= 50) {
            mp_sendv(LM_SOCK, MM_LEVEL, 0, R.peak, R.samples * 1000 / RATE, R.paused, 0);
            R.peak = 0;
            last = lm_now_ms();
        }
    }
    lm_mic_drain(ring, on_mic);
    wav_header(out, (uint32_t)(R.samples * 2));
    fsync(out);
    munmap(ring, ring_len);
    char json[64];
    snprintf(json, sizeof json, "{\"duration\":%lld}", (long long)(R.samples * 1000 / RATE));
    mp_send(LM_SOCK, MM_RESULT, 0, (int64_t[6]){ 0 }, json, strlen(json), NULL, 0);
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_DONE, R.samples * 1000 / RATE, 0, 0);
}

void lm_screenrec(const char *params, int out, int pulse, int render) {
    int w = (int)mp_param_int(params, "width", 0), h = (int)mp_param_int(params, "height", 0);
    int fps = (int)mp_param_int(params, "fps", 30);
    char accel[24];
    AVBufferRef *hw = lm_hw_device(lm_hw_accel(params, accel, sizeof accel), render);
    size_t ring_len = 0;
    void *ring = lm_mic_ring(pulse, &ring_len);
    if (pulse >= 0) close(pulse);
    int mic = ring != NULL;
    if (!mic && pulse >= 0) LM_LOG("invalid microphone ring: recording without sound");
    char err[128];
    R.enc = lm_enc_open(out, w, h, fps, hw, mic ? RATE : 0, 1, err, sizeof err);
    if (!R.enc) { lm_error("%s", err); mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0); return; }
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_RECORDING, 0, 0, 0);
    static char payload[MP_PAYLOAD_MAX + 1];
    uint8_t *rmap = NULL;
    size_t ring_bytes = 0;
    int rw = 0, rh = 0, slots = 0;
    int stop = 0;
    long frames = 0;
    double last = 0, enc_ms = 0;
    int64_t last_t = 0;
    AVFrame *f = av_frame_alloc();
    while (!stop) {
        struct pollfd pf[1] = { { LM_SOCK, POLLIN, 0 } };
        if (poll(pf, 1, mic ? 20 : 100) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS], r;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                if (r == 0 || m.type == MM_STOP) { stop = 1; break; }
                if (m.type == MM_INRING && fds[0] >= 0 && !rmap) {
                    struct stat st;
                    rw = (int)m.v[1]; rh = (int)m.v[2]; slots = (int)m.v[0];
                    ring_bytes = (size_t)rw * rh * 4 * (size_t)slots;
                    if (rw > 0 && rh > 0 && rw <= MP_MAX_DIM && rh <= MP_MAX_DIM && slots > 0 && slots <= 8 && !fstat(fds[0], &st) &&
                        (size_t)st.st_size >= ring_bytes) {
                        void *mm = mmap(NULL, ring_bytes, PROT_READ, MAP_SHARED, fds[0], 0);
                        if (mm != MAP_FAILED) rmap = mm;
                    }
                } else if (m.type == MM_INFRAME && rmap && m.id < (uint32_t)slots) {
                    f->format = AV_PIX_FMT_BGRA;
                    f->width = rw;
                    f->height = rh;
                    f->data[0] = rmap + (size_t)m.id * rw * rh * 4;
                    f->linesize[0] = rw * 4;
                    double t0 = lm_now_ms();
                    if (!R.paused && lm_enc_video(R.enc, f, m.v[0]) >= 0) frames++;
                    enc_ms += lm_now_ms() - t0;
                    last_t = m.v[0];
                    mp_sendv(LM_SOCK, MM_SLOTFREE, m.id, 0, 0, 0, 0);
                } else if (m.type == MM_PAUSE) R.paused = 1;
                else if (m.type == MM_PLAY) R.paused = 0;
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            }
        }
        if (mic) lm_mic_drain(ring, on_mic);
        if (lm_now_ms() - last >= 250) {
            mp_sendv(LM_SOCK, MM_LEVEL, 0, R.peak, last_t, frames, frames ? (int64_t)(enc_ms * 100 / frames) : 0);
            R.peak = 0;
            last = lm_now_ms();
        }
    }
    f->data[0] = NULL;
    av_frame_free(&f);
    const char *name = lm_enc_name(R.enc);
    int64_t dur = lm_enc_close(R.enc);
    if (mic) munmap(ring, ring_len);
    if (rmap) munmap(rmap, ring_bytes);
    char json[160];
    snprintf(json, sizeof json, "{\"duration\":%lld,\"frames\":%ld,\"encoder\":\"%s\",\"encodeMs\":%.2f}", (long long)dur, frames, name,
             frames ? enc_ms / frames : 0.0);
    mp_send(LM_SOCK, MM_RESULT, 0, (int64_t[6]){ dur < 0 ? -1 : 0 }, json, strlen(json), NULL, 0);
    mp_sendv(LM_SOCK, MM_STATE, 0, dur < 0 ? MS_FAILED : MS_DONE, dur, 0, 0);
}
