/* rdesksnd.c — the remote computer's sound in lord-rdesk: an rdpsnd device of our own ("sys:lordos") that writes PCM
 * into the audio ring lord-server made for the session (media/aring.h, the same ring lord-media's players use). The
 * helper never talks to PipeWire, opens no socket and no file for it: lord-server owns the stream and plays what the
 * ring holds (docs/FILES.md, "Remote desktop"). Only 16-bit PCM at 44.1 or 48 kHz is offered to the server; the ring
 * is 48 kHz stereo, so 44.1 kHz is resampled here (linear) and mono doubled. */
#include "../media/aring.h"

#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/rdpsnd.h>
#include <freerdp/codec/audio.h>
#include <stdlib.h>
#include <string.h>

ARingHdr *rd_audio_ring;            /* rdeskhelper.c maps it; NULL: no sound here */

typedef struct {
    rdpsndDevicePlugin dev;
    AUDIO_FORMAT fmt;
    double phase;                   /* resampling: where the next output sample falls in the input */
    int16_t last[2];                /* the input's last frame, for interpolation across packets */
} LordSnd;

static BOOL supported(rdpsndDevicePlugin *d, const AUDIO_FORMAT *f) {
    (void)d;
    return f->wFormatTag == WAVE_FORMAT_PCM && f->wBitsPerSample == 16 && (f->nChannels == 1 || f->nChannels == 2) &&
           (f->nSamplesPerSec == 48000 || f->nSamplesPerSec == 44100);
}
static BOOL open_dev(rdpsndDevicePlugin *d, const AUDIO_FORMAT *f, UINT32 latency) {
    (void)latency;
    LordSnd *s = (LordSnd *)d;
    if (!supported(d, f)) return FALSE;
    s->fmt = *f;
    s->phase = 0;
    s->last[0] = s->last[1] = 0;
    return TRUE;
}
static UINT32 get_volume(rdpsndDevicePlugin *d) { (void)d; return 0xFFFFFFFFu; }
static BOOL set_volume(rdpsndDevicePlugin *d, UINT32 v) { (void)d; (void)v; return TRUE; }
static void close_dev(rdpsndDevicePlugin *d) { (void)d; }
static void free_dev(rdpsndDevicePlugin *d) { free(d); }

/* what is waiting in the ring, in ms: the server paces its packets by it */
static UINT queued_ms(void) {
    if (!rd_audio_ring) return 0;
    uint64_t n = ar_avail(rd_audio_ring);
    return (UINT)(n / (AR_RATE * 4 / 1000));
}

static UINT play(rdpsndDevicePlugin *d, const BYTE *data, size_t size) {
    LordSnd *s = (LordSnd *)d;
    if (!rd_audio_ring || !data) return 0;
    int ch = s->fmt.nChannels;
    size_t frames = size / (2u * (size_t)ch);
    const int16_t *in = (const int16_t *)data;
    int16_t out[2048 * 2];
    size_t o = 0;
    if (s->fmt.nSamplesPerSec == 48000) {
        for (size_t i = 0; i < frames; i++) {
            out[o++] = in[i * ch];
            out[o++] = in[i * ch + (ch - 1)];
            if (o == sizeof out / sizeof out[0]) { ar_write(rd_audio_ring, out, o * 2); o = 0; }
        }
    } else {
        /* 44.1 → 48 kHz: each output sample lies between input frames k-1 and k (k = 0: the last of the previous packet) */
        const double step = 44100.0 / 48000.0;
        double p = s->phase;
        while (p < (double)frames) {
            size_t k = (size_t)p;
            double f = p - (double)k;
            for (int c = 0; c < 2; c++) {
                int cc = c < ch ? c : 0;
                double a = k ? in[(k - 1) * ch + cc] : s->last[c];
                double b = in[k * ch + cc];
                out[o++] = (int16_t)(a + (b - a) * f);
            }
            if (o == sizeof out / sizeof out[0]) { ar_write(rd_audio_ring, out, o * 2); o = 0; }
            p += step;
        }
        s->phase = p - (double)frames;
        if (frames) for (int c = 0; c < 2; c++) s->last[c] = in[(frames - 1) * ch + (c < ch ? c : 0)];
    }
    if (o) ar_write(rd_audio_ring, out, o * 2);      /* a full ring drops the rest: the session never waits for sound */
    return queued_ms();
}

static UINT entry(PFREERDP_RDPSND_DEVICE_ENTRY_POINTS ep) {
    LordSnd *s = calloc(1, sizeof *s);
    if (!s) return CHANNEL_RC_NO_MEMORY;
    s->dev.FormatSupported = supported;
    s->dev.Open = open_dev;
    s->dev.GetVolume = get_volume;
    s->dev.SetVolume = set_volume;
    s->dev.Play = play;
    s->dev.Close = close_dev;
    s->dev.Free = free_dev;
    ep->pRegisterRdpsndDevice(ep->rdpsnd, &s->dev);
    return CHANNEL_RC_OK;
}

/* the library's own channels, plus our sound device */
PVIRTUALCHANNELENTRY rd_addin_provider(LPCSTR name, LPCSTR subsystem, LPCSTR type, DWORD flags) {
    if (name && subsystem && !strcmp(name, "rdpsnd") && !strcmp(subsystem, "lordos")) return (PVIRTUALCHANNELENTRY)(void *)entry;
    return freerdp_channels_load_static_addin_entry(name, subsystem, type, flags);
}
