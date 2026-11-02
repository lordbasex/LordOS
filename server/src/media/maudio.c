/* maudio.c — media audio in axia-server (see aring.h and docs/MEDIA.md).
 *
 * Playback: axia-media writes PCM into a shared-memory ring that axia-server creates;
 * here we own the PipeWire stream and, when PipeWire asks for bytes (REQUEST), they're copied from the
 * ring, without waiting for anyone. After each GET_PLAYBACK_LATENCY reply, which byte is being heard
 * and when is written to the ring (seqlock): the video clock in axia-media.
 * Recording: the capture stream belongs to axia-server (after the microphone permission) and it's
 * written into a ring that axia-media reads. That way axia-media never has the PipeWire socket. */
#include "media_int.h"
#include "aring.h"
#include "mpulse.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct MAudio {
    int rec;                        /* 0 playback, 1 recording */
    int ring_fd;
    ARingHdr *ring;
    size_t ring_len;
    uint32_t size;                  /* own data size (not the one in the shared header) */
    uint64_t pos;                   /* own cursor: write (recording) or read (playback) */
    LmPulse pa;
    int started, want_play;
    double volume;
    int volume_sent;
    uint32_t serial_seen;
    int64_t delta;                  /* ring byte = stream byte + delta */
    double flush_ms, last_lat_req, last_lat_at;
    char app[64], title[128], role[16], source[256];
};

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int make_ring(MAudio *a, int channels, uint32_t size) {
    size_t len = ar_bytes(size);
    int fd = memfd_create("lordos-audio", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { close(fd); return -1; }
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    ARingHdr *h = m;
    memset(h, 0, AR_HDR);
    h->rate = AR_RATE;
    h->channels = (uint32_t)channels;
    h->size = size;
    h->magic = AR_MAGIC;
    a->ring = h;
    a->size = size;
    a->ring_len = len;
    a->ring_fd = fd;
    return 0;
}

static void on_mic(void *ud, const uint8_t *d, size_t n) {
    MAudio *a = ud;
    size_t w = ar_write_t(a->ring, a->size, &a->pos, d, n & ~(size_t)1);
    if (w < n) atomic_fetch_add(&a->ring->overruns, (uint32_t)((n - w) / 1024 + 1));
}

static int start(MAudio *a) {
    a->started = 1;
    int fd = media_pulse_fd();
    if (fd < 0 || lmp_connect(&a->pa, fd, a->app, "org.lordos.Media")) {
        fprintf(stderr, "media: no PipeWire to %s\n", a->rec ? "record" : "play");
        a->pa.failed = 1;
        return -1;
    }
    if (a->rec) {
        lmp_record(&a->pa, a->source, AR_RATE, 1, a->title);
        a->pa.on_record = on_mic;
        a->pa.ud = a;
    } else lmp_playback(&a->pa, AR_RATE, 2, a->title, a->role);
    return 0;
}

MAudio *maudio_open(int rec, const char *app, const char *title, const char *role_or_source, int *ring_fd) {
    MAudio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->rec = rec;
    a->ring_fd = -1;
    a->pa.fd = -1;
    a->volume = 1;
    snprintf(a->app, sizeof a->app, "%s", app && *app ? app : "Axia DE");
    snprintf(a->title, sizeof a->title, "%s", title && *title ? title : a->app);
    if (rec) snprintf(a->source, sizeof a->source, "%s", role_or_source ? role_or_source : "");
    else snprintf(a->role, sizeof a->role, "%s", role_or_source && *role_or_source ? role_or_source : "video");
    /* playback: 1.4 s of stereo; recording: 2.7 s of mono */
    if (make_ring(a, rec ? 1 : 2, 1u << 18)) { free(a); return NULL; }
    if (rec) start(a);
    *ring_fd = a->ring_fd;
    return a;
}

void maudio_close(MAudio *a) {
    if (!a) return;
    lmp_close(&a->pa);
    if (a->ring) munmap(a->ring, a->ring_len);
    if (a->ring_fd >= 0) close(a->ring_fd);
    free(a);
}

int maudio_pollfd(const MAudio *a, short *events) {
    if (!a || a->pa.fd < 0) return -1;
    *events = POLLIN | (a->pa.out_len ? POLLOUT : 0);
    return a->pa.fd;
}

static void publish_clock(MAudio *a) {
    LmPulse *p = &a->pa;
    if (!p->lat_valid || p->lat_at_ms == a->last_lat_at) return;
    a->last_lat_at = p->lat_at_ms;
    if (p->lat_sent_ms < a->flush_ms) return;           /* reply from before the seek */
    double bps = AR_RATE * 4 / 1e6;                     /* bytes per microsecond */
    int64_t heard = p->read_index - (int64_t)(p->sink_usec * bps);
    ar_clock_write(a->ring, 1, p->lat_playing && !p->corked, heard + a->delta, (int64_t)(p->lat_at_ms * 1000), (int64_t)p->sink_usec);
}

/* What can be done now: handshake, seek, volume, pass PCM, ask for latency. */
void maudio_service(MAudio *a) {
    if (!a) return;
    if (!a->started) {
        if (a->rec) return;
        start(a);
    }
    LmPulse *p = &a->pa;
    if (p->fd >= 0 && lmp_dispatch(p)) { fprintf(stderr, "media: PipeWire connection dropped\n"); p->failed = 1; }
    if (a->rec || p->failed || !p->ready) return;
    /* seek: what's before discard_to isn't heard; PipeWire discards what it had */
    uint32_t serial = atomic_load(&a->ring->serial);
    if (serial != a->serial_seen) {
        a->serial_seen = serial;
        uint64_t to = atomic_load(&a->ring->discard_to);
        if (to > a->pos && to - a->pos <= ar_avail_t(a->ring, a->size, a->pos)) { a->pos = to; atomic_store(&a->ring->rpos, a->pos); }
        lmp_flush(p);
        p->requested = p->requested < 0 ? 0 : p->requested;
        a->delta = (int64_t)a->pos - p->written;
        a->flush_ms = now_ms();
        ar_clock_write(a->ring, 0, 0, 0, 0, 0);
    }
    if (!a->volume_sent) { lmp_volume(p, a->volume); a->volume_sent = 1; }
    if (a->want_play && p->corked) lmp_cork(p, 0);
    if (!a->want_play && !p->corked) lmp_cork(p, 1);
    static uint8_t buf[65536];
    while (p->requested >= 4 && ar_avail_t(a->ring, a->size, a->pos) >= 4) {
        size_t n = (size_t)(p->requested > (int64_t)sizeof buf ? (int64_t)sizeof buf : p->requested) & ~(size_t)3;
        uint64_t avail = ar_avail_t(a->ring, a->size, a->pos) & ~(uint64_t)3;
        if (n > avail) n = (size_t)avail;
        uint64_t keep = a->pos;
        n = ar_read_t(a->ring, a->size, &a->pos, buf, n);
        if (!n || lmp_write(p, buf, n)) { a->pos = keep; atomic_store(&a->ring->rpos, keep); break; }
    }
    if (!p->corked && now_ms() - a->last_lat_req > 40) { lmp_latency(p); a->last_lat_req = now_ms(); }
    publish_clock(a);
}

void maudio_playing(MAudio *a, int on) {
    if (!a || a->rec) return;
    a->want_play = on;
    maudio_service(a);
    if (!on && a->ring) {
        /* while paused the clock stays still where it was */
        int playing;
        int64_t pos, mono;
        if (ar_clock_read(a->ring, &playing, &pos, &mono)) {
            if (playing) pos += (int64_t)((now_ms() * 1000 - (double)mono) * AR_RATE * 4 / 1e6);
            ar_clock_write(a->ring, 1, 0, pos, (int64_t)(now_ms() * 1000), 0);
        }
    }
}

void maudio_volume(MAudio *a, double v) {
    if (!a) return;
    a->volume = v;
    a->volume_sent = 0;
    maudio_service(a);
}

int maudio_overruns(const MAudio *a) { return a && a->ring ? (int)atomic_load(&a->ring->overruns) : 0; }
void maudio_mic_drain(MAudio *a) { if (a && a->ring && a->rec) atomic_store(&a->ring->rpos, atomic_load(&a->ring->wpos)); }
int maudio_failed(const MAudio *a) { return !a || a->pa.failed; }
