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

/* How much sound may wait in the ring. The remote computer's clock and the output's never agree exactly (measured on
 * the Pi with a USB adapter: 48010 Hz in, 47897 Hz out, 0.24 %), and servers send a burst at the start. With no limit
 * the ring filled up (1.3 s behind the picture) and every packet lost its tail to a full ring: clicks. So the
 * resampler runs a little faster or slower to keep the ring between RD_LOW_MS and RD_HIGH_MS (RD_TRIM, 0.4 %, about
 * 7 cents: not heard), and only past RD_DROP_MS (the start's burst, a stall) are whole packets skipped, down to
 * RD_HIGH_MS. */
#define RD_LOW_MS  100
#define RD_HIGH_MS 200
#define RD_DROP_MS 500
#define RD_TRIM    0.004
static int skipping;

static UINT play(rdpsndDevicePlugin *d, const BYTE *data, size_t size) {
    LordSnd *s = (LordSnd *)d;
    if (!rd_audio_ring || !data) return 0;
    UINT q = queued_ms();
    if (q > RD_DROP_MS) skipping = 1;
    else if (q < RD_HIGH_MS) skipping = 0;
    if (skipping) return q;
    int ch = s->fmt.nChannels;
    size_t frames = size / (2u * (size_t)ch);
    const int16_t *in = (const int16_t *)data;
    int16_t out[2048 * 2];
    size_t o = 0;
    {
        /* to 48 kHz: each output sample lies between input frames k-1 and k (k = 0: the last of the previous packet);
         * a longer step uses up the input sooner, so fewer samples go into the ring */
        static double avg = -1;         /* packets come in bursts: steer by the average, not by each one */
        avg = avg < 0 ? q : avg + ((double)q - avg) * 0.05;
        double step = (double)s->fmt.nSamplesPerSec / 48000.0;
        if (avg > RD_HIGH_MS) step *= 1.0 + RD_TRIM;
        else if (avg < RD_LOW_MS) step *= 1.0 - RD_TRIM;
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
