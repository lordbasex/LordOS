/* aring.h — audio between axia-server and axia-media over shared memory (see docs/MEDIA.md).
 *
 * axia-media doesn't talk to PipeWire: the PipeWire socket (PulseAudio protocol) is a
 * very broad capability (loading modules, recording other streams, changing the output), and a
 * compromised decoder must not have it. So:
 *   - playback: axia-media writes PCM (s16, 48 kHz, stereo) into this ring; axia-server owns
 *     the PipeWire stream and copies from the ring when PipeWire asks (REQUEST), without waiting.
 *   - the clock: axia-server writes in the same page which byte of the ring is being heard now
 *     (read_index − output latency) and at what instant, with a seqlock. axia-media reads it
 *     without locks or round trips to sync the video.
 *   - recording (microphone): the other way around; axia-server owns the capture stream (after the
 *     permission) and writes; axia-media reads.
 * axia-server creates the memfd, sealed (it can't be shrunk or grown). Only one writer and one
 * reader per ring; positions are byte counters that never wrap to zero. */
#ifndef LORD_MEDIA_ARING_H
#define LORD_MEDIA_ARING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AR_MAGIC  0x474e5241u          /* "ARNG" */
#define AR_RATE   48000
#define AR_HDR    512                  /* the header takes this much; the data goes after */

typedef struct {
    uint32_t magic, rate, channels, size;       /* size: data bytes (power of 2) */
    _Atomic uint64_t wpos;                      /* bytes written (writer) */
    _Atomic uint64_t rpos;                      /* bytes read (reader) */
    /* playback: axia-media asks to discard what's before discard_to (seek) by bumping serial */
    _Atomic uint64_t discard_to;
    _Atomic uint32_t serial;
    _Atomic uint32_t overruns;                  /* recording: bytes lost because the ring was full (x1024) */
    /* clock (written by axia-server, seqlock: odd = writing) */
    _Atomic uint32_t clk_seq;
    _Atomic int32_t  clk_valid, clk_playing;
    _Atomic int64_t  clk_pos;                   /* ring byte heard at clk_mono_us */
    _Atomic int64_t  clk_mono_us;               /* CLOCK_MONOTONIC */
    _Atomic int64_t  clk_latency_us;            /* output latency (informational) */
} ARingHdr;

_Static_assert(sizeof(ARingHdr) <= AR_HDR, "ring header");

static inline uint8_t *ar_data(ARingHdr *h) { return (uint8_t *)h + AR_HDR; }
static inline size_t ar_bytes(uint32_t size) { return AR_HDR + (size_t)size; }

/* Bytes available to read / free room to write. */
static inline uint64_t ar_avail(ARingHdr *h) {
    return atomic_load_explicit(&h->wpos, memory_order_acquire) - atomic_load_explicit(&h->rpos, memory_order_acquire);
}
static inline uint64_t ar_space(ARingHdr *h) { return h->size - ar_avail(h); }

/* Writes up to n bytes (what fits). Returns how many. */
static inline size_t ar_write(ARingHdr *h, const void *p, size_t n) {
    uint64_t w = atomic_load_explicit(&h->wpos, memory_order_relaxed);
    uint64_t space = h->size - (w - atomic_load_explicit(&h->rpos, memory_order_acquire));
    if (n > space) n = (size_t)space;
    size_t off = (size_t)(w & (h->size - 1)), first = h->size - off < n ? h->size - off : n;
    memcpy(ar_data(h) + off, p, first);
    memcpy(ar_data(h), (const uint8_t *)p + first, n - first);
    atomic_store_explicit(&h->wpos, w + n, memory_order_release);
    return n;
}

/* Copies up to n bytes from the read position without advancing it. */
static inline size_t ar_peek(ARingHdr *h, void *p, size_t n) {
    uint64_t r = atomic_load_explicit(&h->rpos, memory_order_relaxed);
    uint64_t avail = atomic_load_explicit(&h->wpos, memory_order_acquire) - r;
    if (avail > h->size) avail = h->size;       /* a broken writer can't cause over-reading */
    if (n > avail) n = (size_t)avail;
    size_t off = (size_t)(r & (h->size - 1)), first = h->size - off < n ? h->size - off : n;
    memcpy(p, ar_data(h) + off, first);
    memcpy((uint8_t *)p + first, ar_data(h), n - first);
    return n;
}
static inline void ar_consume(ARingHdr *h, size_t n) { atomic_fetch_add_explicit(&h->rpos, n, memory_order_release); }
static inline size_t ar_read(ARingHdr *h, void *p, size_t n) { n = ar_peek(h, p, n); ar_consume(h, n); return n; }

/* Variants for axia-server: the other side (axia-media, or an app) can write the header,
 * so axia-server does NOT trust the other side's size or positions: it uses its own size and its
 * own cursor, and bounds everything. The worst the other side can do is lose or repeat audio. */
static inline size_t ar_write_t(ARingHdr *h, uint32_t size, uint64_t *wpos, const void *p, size_t n) {
    uint64_t r = atomic_load_explicit(&h->rpos, memory_order_acquire), w = *wpos;
    uint64_t used = (r > w || w - r > size) ? size : w - r;
    if (n > size - used) n = (size_t)(size - used);
    size_t off = (size_t)(w & (size - 1)), first = size - off < n ? size - off : n;
    memcpy(ar_data(h) + off, p, first);
    memcpy(ar_data(h), (const uint8_t *)p + first, n - first);
    *wpos = w + n;
    atomic_store_explicit(&h->wpos, *wpos, memory_order_release);
    return n;
}
static inline uint64_t ar_avail_t(ARingHdr *h, uint32_t size, uint64_t rpos) {
    uint64_t w = atomic_load_explicit(&h->wpos, memory_order_acquire);
    return (w < rpos || w - rpos > size) ? 0 : w - rpos;
}
static inline size_t ar_read_t(ARingHdr *h, uint32_t size, uint64_t *rpos, void *p, size_t n) {
    uint64_t avail = ar_avail_t(h, size, *rpos);
    if (n > avail) n = (size_t)avail;
    size_t off = (size_t)(*rpos & (size - 1)), first = size - off < n ? size - off : n;
    memcpy(p, ar_data(h) + off, first);
    memcpy((uint8_t *)p + first, ar_data(h), n - first);
    *rpos += n;
    atomic_store_explicit(&h->rpos, *rpos, memory_order_release);
    return n;
}

/* Clock: written by axia-server and read by axia-media. */
static inline void ar_clock_write(ARingHdr *h, int valid, int playing, int64_t pos, int64_t mono_us, int64_t lat_us) {
    uint32_t s = atomic_load_explicit(&h->clk_seq, memory_order_relaxed);
    atomic_store_explicit(&h->clk_seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&h->clk_valid, valid, memory_order_relaxed);
    atomic_store_explicit(&h->clk_playing, playing, memory_order_relaxed);
    atomic_store_explicit(&h->clk_pos, pos, memory_order_relaxed);
    atomic_store_explicit(&h->clk_mono_us, mono_us, memory_order_relaxed);
    atomic_store_explicit(&h->clk_latency_us, lat_us, memory_order_relaxed);
    atomic_store_explicit(&h->clk_seq, s + 2, memory_order_release);
}

static inline int ar_clock_read(ARingHdr *h, int *playing, int64_t *pos, int64_t *mono_us) {
    for (int tries = 0; tries < 100; tries++) {
        uint32_t s0 = atomic_load_explicit(&h->clk_seq, memory_order_acquire);
        if (s0 & 1) continue;
        int valid = atomic_load_explicit(&h->clk_valid, memory_order_relaxed);
        *playing = atomic_load_explicit(&h->clk_playing, memory_order_relaxed);
        *pos = atomic_load_explicit(&h->clk_pos, memory_order_relaxed);
        *mono_us = atomic_load_explicit(&h->clk_mono_us, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&h->clk_seq, memory_order_relaxed) == s0) return valid;
    }
    return 0;
}

#endif
